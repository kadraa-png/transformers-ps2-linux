#pragma once
// Single-producer / single-consumer byte ring with batched publication.
//
// Cursors are monotonic byte positions; the storage wraps physically and
// copies are split, so records never need padding.  The producer stages
// records past `published` and makes them visible with publish(); the
// consumer releases what it has finished with release().
//
// Sleep/wake handshake: a sleeper raises its flag and re-checks its
// condition under the mutex; a waker changes its cursor, then clears the
// flag and notifies under the mutex.  The seq_cst fences order each side's
// store before its load of the other variable (store->load across two
// atomics needs a full fence), so a wakeup is never lost, and only the first
// change after a sleeper raised its flag pays for the mutex.
//
// The producer also records the consumer position that satisfies it
// (wake_at_), so release() only wakes it when that position is reached: a
// producer waiting on something else (a frame counter, via
// producer_wait/wake_producer) is not woken by every released record.
#include "ps2/types.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <vector>

class SpscRing {
public:
    explicit SpscRing(size_t size) : storage_(size), mask_(size - 1) {}   // size: power of two

    size_t capacity() const { return storage_.size(); }
    void reset() {
        staged_ = 0;
        published_.store(0, std::memory_order_relaxed);
        consumed_.store(0, std::memory_order_relaxed);
    }

    // ---- producer ----
    u64 staged() const { return staged_; }
    // Wait until `bytes` more fit.  Everything staged is published first: the
    // consumer must be able to see it to make room.
    void reserve(size_t bytes) {
        while (staged_ - consumed_.load(std::memory_order_acquire) + bytes > storage_.size()) {
            publish();
            const u64 target = staged_ + bytes - storage_.size();
            wait_released(target);
        }
    }
    // Append at the staged cursor (after reserve()).
    void write(const void* src, size_t bytes) {
        copy_in(staged_, src, bytes);
        staged_ += bytes;
    }
    u64 unpublished() const { return staged_ - published_.load(std::memory_order_relaxed); }
    void publish() {
        if (staged_ == published_.load(std::memory_order_relaxed)) return;
        published_.store(staged_, std::memory_order_release);
        wake(consumer_waiting_);
    }
    bool drained() const { return consumed_.load(std::memory_order_acquire) >= staged_; }
    u64 consumed() const { return consumed_.load(std::memory_order_acquire); }
    // Publish and wait until the consumer released `target`.
    void wait_consumed(u64 target) {
        if (consumed_.load(std::memory_order_acquire) >= target) return;
        publish();
        wait_released(target);
    }
    // Producer-side sleep on a condition the consumer signals through
    // wake_producer(); release() does not wake it.
    template <typename Ready>
    void producer_wait(Ready ready) {
        wake_at_.store(~0ull, std::memory_order_relaxed);
        sleep_until(producer_waiting_, ready);
    }

    // ---- consumer ----
    u64 published() const { return published_.load(std::memory_order_acquire); }
    // Sleep until something past `position` is published; returns the new end.
    u64 wait_published(u64 position) {
        u64 end = published_.load(std::memory_order_acquire);
        if (end != position) return end;
        sleep_until(consumer_waiting_, [&] {
            end = published_.load(std::memory_order_acquire);
            return end != position;
        });
        return end;
    }
    void read(u64 position, void* dst, size_t bytes) const {
        const size_t index = position & mask_;
        const size_t first = std::min(bytes, storage_.size() - index);
        std::memcpy(dst, storage_.data() + index, first);
        if (bytes > first) std::memcpy(static_cast<u8*>(dst) + first, storage_.data(), bytes - first);
    }
    // The bytes at `position` in place, or nullptr when they wrap.
    const u8* contiguous(u64 position, size_t bytes) const {
        const size_t index = position & mask_;
        return index + bytes <= storage_.size() ? storage_.data() + index : nullptr;
    }
    void release(u64 position) {
        consumed_.store(position, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (position >= wake_at_.load(std::memory_order_relaxed)) wake(producer_waiting_);
    }
    void wake_producer() { wake(producer_waiting_); }

private:
    // wake_at_ is stored before the sleeper raises its flag and read after
    // release()'s fence, so the existing handshake also covers it.
    void wait_released(u64 target) {
        wake_at_.store(target, std::memory_order_relaxed);
        sleep_until(producer_waiting_, [&] { return consumed_.load(std::memory_order_acquire) >= target; });
    }
    void copy_in(u64 position, const void* src, size_t bytes) {
        const size_t index = position & mask_;
        const size_t first = std::min(bytes, storage_.size() - index);
        std::memcpy(storage_.data() + index, src, first);
        if (bytes > first) std::memcpy(storage_.data(), static_cast<const u8*>(src) + first, bytes - first);
    }
    void wake(std::atomic<bool>& waiting) {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (waiting.exchange(false, std::memory_order_acq_rel)) {
            std::lock_guard lock(mutex_);
            cv_.notify_all();
        }
    }
    template <typename Ready>
    void sleep_until(std::atomic<bool>& waiting, Ready ready) {
        if (ready()) return;
        const auto t0 = std::chrono::steady_clock::now();
        sleep_until_inner(waiting, ready);
        const u64 ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - t0).count());
        (&waiting == &producer_waiting_ ? producer_wait_ns : consumer_wait_ns)
            .fetch_add(ns, std::memory_order_relaxed);
    }
    template <typename Ready>
    void sleep_until_inner(std::atomic<bool>& waiting, Ready ready) {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] {
            if (ready()) return true;
            waiting.store(true, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            return ready();
        });
        waiting.store(false, std::memory_order_relaxed);
    }

    std::vector<u8> storage_;
    size_t mask_;
    u64 staged_ = 0;                                     // producer only
    alignas(64) std::atomic<u64> published_{0};
    alignas(64) std::atomic<u64> consumed_{0};
    std::atomic<u64> wake_at_{0};                        // producer's wake position
    std::atomic<bool> producer_waiting_{false}, consumer_waiting_{false};
public:
    // Host time the producer spent blocked (ring full / waiting for the
    // consumer) and the consumer spent idle (ring empty); diagnostics.
    std::atomic<u64> producer_wait_ns{0}, consumer_wait_ns{0};
private:
    std::mutex mutex_;
    std::condition_variable cv_;
};
