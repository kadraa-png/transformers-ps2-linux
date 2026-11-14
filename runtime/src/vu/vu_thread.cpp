/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
 *  SPDX-License-Identifier: LGPL-3.0-or-later
 *
 *  Asynchronous VU1 after PCSX2 1.6 MTVU.cpp (see vu_thread.h for the
 *  contract): the same ordered event stream, on the runtime's SPSC ring.
 */
#include "../../../third_party/pcsx2_vif/compat.h"
#include "vu_thread.h"
#include "ps2/gs.h"
#include "ps2/log.h"
#include "ps2/platform.h"
#include "ps2/spsc_ring.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <xmmintrin.h>

void vu_set_vif_position_vu1(u32 top, u32 itop);   // vu.cpp, VU1 thread

bool g_vu_thread_active = false;
vifStruct vu1t_vif{};
VIFregisters vu1t_vifRegs{};

namespace {
enum class Cmd : u32 { Execute, Micro, Data, Row, Col, Unpack, Stop };
// Records are 16-byte aligned (header + payload rounded up), so contiguous
// payloads keep the alignment the unpackers expect.
struct Record {
    Cmd cmd;
    u32 bytes;   // payload
    u32 a, b;
};
static_assert(sizeof(Record) == 16);
// vifStruct from `tag` up to (excluding) StructEnd, as MTVU_VIF_UNPACK copies.
constexpr size_t kVifCopy = offsetof(vifStruct, StructEnd) - offsetof(vifStruct, tag);
struct UnpackState {
    u8 vif[kVifCopy];
    vifCycle cycle;
    u32 mode, num, mask, itop, top;
};

constexpr size_t kRingSize = 16u << 20;
constexpr u64 kPublishBatch = 64u << 10;
std::unique_ptr<SpscRing> ring;
std::thread worker;
u64 exec_seq = 0, vif_exec_seq = 0;   // EE thread: starts queued (all / by VIF1)
std::atomic<u64> done_seq{0};          // VU1 thread: starts finished
std::atomic<bool> irq_pending{false};
std::atomic<void (*)()> irq_wake{nullptr};

u64 padded(u64 bytes) { return (bytes + 15) & ~u64(15); }

void push(Cmd cmd, u32 a, u32 b, const void* p0, u32 n0, const void* p1 = nullptr, u32 n1 = 0,
          bool publish_now = false) {
    static const u8 zeros[16]{};
    const u32 bytes = n0 + n1;
    ring->reserve(sizeof(Record) + padded(bytes));
    const Record record{cmd, bytes, a, b};
    ring->write(&record, sizeof(record));
    if (n0) ring->write(p0, n0);
    if (n1) ring->write(p1, n1);
    if (padded(bytes) != bytes) ring->write(zeros, padded(bytes) - bytes);
    if (publish_now || ring->unpublished() >= kPublishBatch) ring->publish();
}

// MTVU_Unpack: the C unpackers read vu1t_vif / vu1t_vifRegs (MTVU_VifX).
void unpack(const u8* payload) {
    UnpackState state;
    std::memcpy(&state, payload, sizeof(state));
    std::memcpy(reinterpret_cast<u8*>(&vu1t_vif) + offsetof(vifStruct, tag), state.vif, kVifCopy);
    vu1t_vifRegs.cycle = state.cycle;
    vu1t_vifRegs.mode = state.mode;
    vu1t_vifRegs.num = state.num;
    vu1t_vifRegs.mask = state.mask;
    vu1t_vifRegs.itop = state.itop;
    vu1t_vifRegs.top = state.top;
    const u16 wl = vu1t_vifRegs.cycle.wl ? vu1t_vifRegs.cycle.wl : 256;
    _nVifUnpack(1, payload + sizeof(state), vu1t_vifRegs.mode, vu1t_vifRegs.cycle.cl < wl);
}

void run() {
    // The VU interpreter computes with the guest rounding/denormal mode the
    // EE thread uses (kernel.cpp set_guest_mxcsr).
    _mm_setcsr((_mm_getcsr() & ~0x603fu) | 0xffc0u);
    std::vector<u8> scratch;
    u64 position = 0;
    for (;;) {
        const u64 end = ring->wait_published(position);
        while (position != end) {
            Record record;
            ring->read(position, &record, sizeof(record));
            const u8* payload = nullptr;
            if (record.bytes) {
                payload = ring->contiguous(position + sizeof(record), record.bytes);
                if (!payload) {
                    if (scratch.size() < record.bytes) scratch.resize(record.bytes);
                    ring->read(position + sizeof(record), scratch.data(), record.bytes);
                    payload = scratch.data();
                }
            }
            switch (record.cmd) {
            case Cmd::Execute:
                vu_set_vif_position_vu1(record.b & 0xffff, record.b >> 16);
                vu_exec(1, record.a);
                gs::path1_program_end();
                done_seq.fetch_add(1, std::memory_order_release);
                break;
            case Cmd::Micro:
                std::memcpy(g_vu1.micro + record.a, payload, record.bytes);
                vu_micro_written(1, record.a, record.bytes);
                break;
            case Cmd::Data: std::memcpy(g_vu1.data + record.a, payload, record.bytes); break;
            case Cmd::Row: std::memcpy(vu1t_vif.MaskRow._u32, payload, 16); break;
            case Cmd::Col: std::memcpy(vu1t_vif.MaskCol._u32, payload, 16); break;
            case Cmd::Unpack: unpack(payload); break;
            case Cmd::Stop:
                ring->release(position + sizeof(record));
                return;
            }
            position += sizeof(record) + padded(record.bytes);
            ring->release(position);
        }
    }
}
} // namespace

void vu1_queue_execute(u32 addr_bytes, u32 top, u32 itop, bool from_vif) {
    ++exec_seq;
    if (from_vif) vif_exec_seq = exec_seq;
    // Published at once: the GS thread will wait for this program's PATH1
    // output as soon as it reaches the placeholder.
    push(Cmd::Execute, addr_bytes, (top & 0xffff) | (itop << 16), nullptr, 0, nullptr, 0, true);
    gs::path1_program();
}

void vu1_queue_micro(u32 addr, const void* data, u32 bytes) {
    if (bytes) push(Cmd::Micro, addr, 0, data, bytes);
}

bool vu1_queue_store(u32 offset, const void* data, u32 bytes) {
    if (!g_vu_thread_active) return false;
    if (offset < 0x4000) push(Cmd::Micro, offset, 0, data, bytes);
    else push(Cmd::Data, offset - 0x4000, 0, data, bytes);
    return true;
}

void vu1_queue_row(const u32 row[4]) { push(Cmd::Row, 0, 0, row, 16); }
void vu1_queue_col(const u32 col[4]) { push(Cmd::Col, 0, 0, col, 16); }

void vu1_queue_unpack(const vifStruct& vif, const VIFregisters& regs, const u8* data, u32 bytes) {
    UnpackState state;
    std::memcpy(state.vif, reinterpret_cast<const u8*>(&vif) + offsetof(vifStruct, tag), kVifCopy);
    state.cycle = regs.cycle;
    state.mode = regs.mode;
    state.num = regs.num;
    state.mask = regs.mask;
    state.itop = regs.itop;
    state.top = regs.top;
    // A V3 unpack reads its last vector's W from the next word (the sync
    // path reads it in place); no other format reads past `bytes`.
    const u32 overread = ((vif.cmd >> 2) & 3) == 2 ? 4 : 0;
    push(Cmd::Unpack, 0, 0, &state, sizeof(state), data, bytes + overread);
}

bool vu1_executing() {
    return g_vu_thread_active && done_seq.load(std::memory_order_acquire) < exec_seq;
}
bool vu1_vif_executing() {
    return g_vu_thread_active && done_seq.load(std::memory_order_acquire) < vif_exec_seq;
}

void vu1_sync() {
    if (!g_vu_thread_active || ring->drained()) return;
    gs::flush();   // PATH1 placeholders ahead of the GS thread must be visible
    const u64 t0 = PS2_UNLIKELY(g_log_mask & LOG_VU) ? platform::now_ns() : 0;
    ring->wait_consumed(ring->staged());
    // An EE load from VU1 memory etc. serializes the EE behind all queued VU1 work.
    if (t0) LOG(LOG_VU, "[vu] EE waited %.2f ms for VU1 (caller %p)\n", (platform::now_ns() - t0) / 1e6,
                __builtin_return_address(0));
}
void vu_port_sync_vu1() { vu1_sync(); }

void vu1_set_irq_wake(void (*wake)()) { irq_wake.store(wake, std::memory_order_release); }
bool vu1_take_irq() { return irq_pending.exchange(false, std::memory_order_acq_rel); }
// VU1 thread: D/T-bit interrupt.
void vu1_thread_irq() {
    irq_pending.store(true, std::memory_order_release);
    if (auto wake = irq_wake.load(std::memory_order_acquire)) wake();
}

bool vu_thread_start() {
    if (g_vu_thread_active) return true;
    const char* mode = std::getenv("PS2_VU_THREAD");
    if (mode && std::strcmp(mode, "0") == 0) return false;
    if (!gs::set_path1_async(true)) return false;   // no GS thread to order PATH1 on
    ring = std::make_unique<SpscRing>(kRingSize);
    vu1t_vif = vif1;
    vu1t_vifRegs = vif1Regs;
    exec_seq = vif_exec_seq = 0;
    done_seq.store(0, std::memory_order_relaxed);
    g_vu_thread_active = true;
    worker = std::thread(run);
    return true;
}

void vu_thread_stop() {
    if (!g_vu_thread_active) return;
    vu1_sync();
    push(Cmd::Stop, 0, 0, nullptr, 0, nullptr, 0, true);
    worker.join();
    g_vu_thread_active = false;
    gs::set_path1_async(false);
    ring.reset();
}

// Diagnostics: EE blocked on a full VU1 ring / VU1 thread idle (ns, cumulative).
void vu1_ring_waits(u64& producer, u64& consumer) {
    producer = ring ? ring->producer_wait_ns.load(std::memory_order_relaxed) : 0;
    consumer = ring ? ring->consumer_wait_ns.load(std::memory_order_relaxed) : 0;
}
