#include "kernel.h"
#include "recompiled.h"
#include "kernel/fiber.h"
#include "ps2/iop.h"
#include "ps2/iso.h"
#include "ps2/log.h"
#include "ps2/platform.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <deque>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <map>

namespace iop::kernel {
namespace {
constexpr u64 NEVER = std::numeric_limits<u64>::max();
constexpr u32 HEAP_START = 0x0b0000, HEAP_END = IOP_RAM_SIZE;
constexpr size_t HOST_STACK_SIZE = 256 * 1024;
enum class State { Dormant, Ready, Sleeping, VblankStart, VblankEnd, Delayed, Sema, Event, Rpc, Dead };
struct Thread {
    int id;
    u32 entry = 0, priority = 64, guest_stack = 0, stack_size = 0, arg = 0;
    Context cpu{};
    State state = State::Dormant;
    u64 wake_ns = NEVER;
    u32 wait_id = 0, wait_bits = 0, wait_mode = 0, wait_result = 0;
    int wakeups = 0;
    bool suspended = false;
    Fiber* fiber = nullptr;   // host stack; switched with the EE kernel's asm fibers
    Thread() = default;
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;
    ~Thread() { fiber_destroy(fiber); }
};
struct Sema { int count, maximum; u32 attr = 0, option = 0; int initial = 0; };
struct Event { u32 bits; };
struct Timer {
    u32 handler = 0, arg = 0, gp = 0, compare = 0, source = 1, prescale = 1;
    u64 period = 0, next = NEVER;
    bool running = false;
};
struct File { iso::Entry entry; u64 offset = 0; };
struct RpcServer {
    u32 address = 0, handler = 0, buffer = 0, cfunc = 0, cbuf = 0, queue = 0, gp = 0;
};
struct RpcRequest {
    u32 server = 0, fno = 0, token = 0;
    std::vector<u8> data;
};
struct RpcQueue {
    int thread_id = 0;
    std::deque<RpcRequest> waiting;
};
std::unordered_map<u32, RpcQueue> rpc_queues;
std::unordered_map<u32, RpcRequest> rpc_active;
struct CommandHandler { u32 handler = 0, arg = 0, gp = 0; };
struct IntrHandler { u32 handler, arg, gp; };
struct Alarm { u32 handler = 0, arg = 0, gp = 0; u64 next = NEVER; };
struct PendingCd { u32 handler, gp; int thread_id, checkpoints; bool ready; };
std::unordered_map<int, std::unique_ptr<Thread>> threads;
std::unordered_map<int, Sema> semas;
std::unordered_map<int, Event> events;
std::unordered_map<int, Timer> timers;
std::unordered_map<int, File> files;
std::vector<Alarm> alarms;
std::deque<PendingCd> pending_cd;
std::unordered_map<u32, RpcServer> rpc_servers;
std::unordered_map<u32, CommandHandler> commands;
std::unordered_map<u32, IntrHandler> interrupts;   // RegisterIntrHandler
// EnableIntr/DisableIntr mask (INTC lines 0-31, DMA channel lines 0x20+ch):
// independent of handler registration, which may come before or after.
std::array<bool, 64> irq_enabled{};
bool irq_is_enabled(u32 irq) { return irq < irq_enabled.size() && irq_enabled[irq]; }
struct Allocation { u32 requested; u32 extent; };
std::map<u32, Allocation> allocations;
std::map<u32, u32> free_blocks;
std::array<u32, 2> vblank_handlers{};
std::array<u32, 2> vblank_args{};
std::array<u32, 2> vblank_gp{};
Fiber scheduler{};   // whatever host stack called step()
// Back-edges between scheduling checks in checkpoint(), and the longest a busy
// thread runs before control returns to the scheduler (and the EE).
constexpr u32 CHECKPOINT_BATCH = 1024;
constexpr u64 SLICE_NS = 500'000;
u32 checkpoint_countdown = CHECKPOINT_BATCH;
u64 slice_end_ns = 0;
Thread* current = nullptr;
int next_id = 1, next_fd = 3;
int last_run_id = 0;
u32 cd_callback = 0, cd_callback_gp = 0, cd_error = 0;
u32 callback_stack = 0;
int callback_depth = 0;              // > 0 while interrupt-context guest code runs
Context* callback_context = nullptr;  // innermost running callback (live sp for nesting)
bool intr_enabled = true;
int critical_thread = 0;             // yielded with interrupts masked: resumes first
u32 command_buffer = 0;
u32 stream_lba = 0;

u32 arg(const Context& c, unsigned n) { return n < 4 ? c.r[4 + n] : iop::read32(c.r[29] + n * 4); }
void copy_guest(u32 dest, u32 src, u32 n) {
    for (u32 i = 0; i < n; ++i) iop::write8(dest + i, iop::read8(src + i));
}
std::string guest_string(u32 addr, size_t limit = 4096) {
    std::string result;
    for (size_t i = 0; i < limit; ++i) {
        char ch = static_cast<char>(iop::read8(addr + static_cast<u32>(i)));
        if (!ch) return result;
        result.push_back(ch);
    }
    FATAL("IOP unterminated string at %08x", addr);
}
void put_string(u32 address, const std::string& s) {
    for (size_t i = 0; i <= s.size(); ++i) iop::write8(address + static_cast<u32>(i), i == s.size() ? 0 : s[i]);
}
u32 allocate_at(u32 address, u32 size, u32 align) {
    if (!size) size = 1;
    const u64 extent64 = (static_cast<u64>(size) + 15) & ~15ull;
    if (!align || (align & (align - 1))) FATAL("IOP invalid heap alignment %u", align);
    if ((address & (align - 1)) || address < HEAP_START || address >= HEAP_END ||
        extent64 > HEAP_END - address) return 0;
    const u32 extent = static_cast<u32>(extent64);
    auto it = free_blocks.upper_bound(address);
    if (it == free_blocks.begin()) return 0;
    --it;
    const u32 begin = it->first, end = begin + it->second;
    if (address < begin || extent > end - address) return 0;
    free_blocks.erase(it);
    if (address > begin) free_blocks.emplace(begin, address - begin);
    if (address + extent < end) free_blocks.emplace(address + extent, end - address - extent);
    allocations.emplace(address, Allocation{size, extent});
    return address;
}
u32 allocate(u32 size, u32 align = 16) {
    if (!align || (align & (align - 1))) FATAL("IOP invalid heap alignment %u", align);
    for (const auto& [begin, extent] : free_blocks) {
        const u64 address = (static_cast<u64>(begin) + align - 1) & ~(static_cast<u64>(align) - 1);
        if (address <= HEAP_END && address - begin <= extent &&
            ((static_cast<u64>(std::max(size, 1u)) + 15) & ~15ull) <= extent - (address - begin))
            return allocate_at(static_cast<u32>(address), size, align);
    }
    return 0;
}
bool release(u32 address) {
    auto used = allocations.find(address);
    if (used == allocations.end()) return false;
    u32 begin = address, end = begin + used->second.extent;
    allocations.erase(used);
    auto after = free_blocks.lower_bound(begin);
    if (after != free_blocks.begin()) {
        auto before = std::prev(after);
        if (before->first + before->second == begin) {
            begin = before->first;
            free_blocks.erase(before);
        }
    }
    if (after != free_blocks.end() && after->first == end) {
        end += after->second;
        free_blocks.erase(after);
    }
    free_blocks.emplace(begin, end - begin);
    return true;
}
Thread* thread(int id) {
    auto it = threads.find(id);
    return it == threads.end() ? nullptr : it->second.get();
}
void yield(State state) {
    if (!current) FATAL("IOP blocking kernel operation outside a thread");
    if (callback_depth) FATAL("IOP blocking kernel operation inside an interrupt handler");
    current->state = state;
    fiber_switch(current->fiber, &scheduler);
}
void fiber_entry(void* arg) {
    const int id = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    Thread* t = thread(id);
    if (!t) FATAL("IOP missing thread %d", id);
    t->cpu.r[4] = t->arg;
    t->cpu.r[29] = t->guest_stack + t->stack_size - 16;
    iop::invoke(t->entry, t->cpu);
    t->state = State::Dead;
    fiber_switch(t->fiber, &scheduler);   // a dead thread's fiber is never resumed
    FATAL("IOP dead thread %d resumed", id);
}
void start(Thread& t) {
    if (t.state != State::Dormant) FATAL("IOP StartThread on active thread %d", t.id);
    if (!t.guest_stack) {
        t.stack_size = std::max(t.stack_size, 256u);
        t.guest_stack = allocate(t.stack_size);
        if (!t.guest_stack) FATAL("IOP thread stack allocation exhausted");
    }
    fiber_destroy(t.fiber);   // a previous run's fiber is parked for good
    t.fiber = fiber_create(HOST_STACK_SIZE, fiber_entry, reinterpret_cast<void*>(static_cast<intptr_t>(t.id)));
    t.state = State::Ready;
}
// Interrupt-context guest code (interrupt/timer/alarm/vblank/SIF-command
// handlers, CD callbacks).  Nested callbacks continue below the outer
// callback's live stack pointer instead of reusing the stack top.
u32 callback(u32 address, u32 gp, u32 a0 = 0, u32 a1 = 0, u32 a2 = 0) {
    if (!address) return 0;
    if (!callback_stack) {
        callback_stack = allocate(0x4000);
        if (!callback_stack) FATAL("IOP callback stack allocation exhausted");
    }
    Context c{};
    c.r[4] = a0; c.r[5] = a1; c.r[6] = a2;
    c.r[28] = gp;
    c.r[29] = callback_context ? ((callback_context->r[29] - 64) & ~15u) : callback_stack + 0x4000 - 16;
    if (c.r[29] < callback_stack + 0x400) FATAL("IOP interrupt callbacks nested too deeply");
    Context* const outer = callback_context;
    callback_context = &c;
    ++callback_depth;
    iop::invoke(address, c);
    --callback_depth;
    callback_context = outer;
    return c.r[2];
}
// Interrupts raised while masked (CpuSuspendIntr / DisableIntr / inside
// another handler) stay latched, as I_STAT does, until unmasked.
std::vector<u32> pending_irqs;
std::array<bool, 2> pending_vblank{};
bool interrupts_deliverable() { return intr_enabled && !callback_depth; }
void deliver_pending() {
    if (!interrupts_deliverable()) return;
    for (size_t i = 0; i < pending_irqs.size();) {
        auto it = interrupts.find(pending_irqs[i]);
        if (it == interrupts.end()) { pending_irqs.erase(pending_irqs.begin() + static_cast<long>(i)); continue; }
        if (!irq_is_enabled(pending_irqs[i])) { ++i; continue; }
        const IntrHandler handler = it->second;
        pending_irqs.erase(pending_irqs.begin() + static_cast<long>(i));
        callback(handler.handler, handler.gp, handler.arg);
        if (!interrupts_deliverable()) return;
    }
    for (int k = 0; k < 2; ++k) {
        if (!pending_vblank[k]) continue;
        pending_vblank[k] = false;
        callback(vblank_handlers[k], vblank_gp[k], static_cast<u32>(k), vblank_args[k]);
        if (!interrupts_deliverable()) return;
    }
}
bool matches(const Event& event, const Thread& t) {
    return (t.wait_mode & 1) ? ((event.bits & t.wait_bits) != 0)
                             : ((event.bits & t.wait_bits) == t.wait_bits);
}
void wake_waiters() {
    u64 now = platform::now_ns();
    for (auto& [id, t] : threads) {
        if (t->state == State::Delayed && t->wake_ns <= now) t->state = State::Ready;
        if (t->state == State::Sema) {
            auto it = semas.find(t->wait_id);
            if (it != semas.end() && it->second.count > 0) {
                --it->second.count;
                t->state = State::Ready;
            }
        }
        if (t->state == State::Event) {
            auto it = events.find(t->wait_id);
            if (it != events.end() && matches(it->second, *t)) {
                t->wait_result = it->second.bits;
                if (t->wait_mode & 0x10) it->second.bits = 0;
                t->state = State::Ready;
            }
        }
    }
    // Timer/alarm handlers are interrupts: they wait while interrupts are
    // masked or another handler runs (they stay due and fire later).
    if (!interrupts_deliverable()) return;
    std::vector<Timer> due_timers;
    for (auto& [id, timer] : timers) {
        if (!timer.running || timer.next > now) continue;
        timer.next = timer.period ? now + timer.period : NEVER;
        if (!timer.period) timer.running = false;
        due_timers.push_back(timer);
    }
    for (const Timer& timer : due_timers) callback(timer.handler, timer.gp, timer.arg);
    for (size_t i = 0; i < alarms.size();) {
        if (alarms[i].next > now) { ++i; continue; }
        auto alarm = alarms[i];
        alarms.erase(alarms.begin() + i);
        u32 ticks = callback(alarm.handler, alarm.gp, alarm.arg);
        if (ticks) {
            alarm.next = now + static_cast<u64>(ticks) * 1000000000ull / 36864000;
            alarms.push_back(alarm);
        }
    }
}
int next_handle() { return next_id++; }
[[noreturn]] void unsupported(const char* lib, u32 ordinal) { FATAL("unsupported IOP import %s:%u", lib, ordinal); }

u32 thbase(Context& c, u32 op) {
    switch (op) {
    case 4: { // iop_thread_t {attr,option,entry,stack_size,priority}
        u32 p = arg(c, 0);
        int id = next_handle();
        auto t = std::make_unique<Thread>();
        t->id = id; t->entry = iop::read32(p + 8);
        t->stack_size = iop::read32(p + 12); t->priority = iop::read32(p + 16);
        t->cpu.r[28] = c.r[28];
        threads.emplace(id, std::move(t));
        return id;
    }
    case 5: { int id = arg(c, 0); auto t = thread(id); if (!t || (t->state != State::Dormant && t->state != State::Dead)) return -1; threads.erase(id); return 0; }
    case 6: { auto t = thread(arg(c, 0)); if (!t || t->state != State::Dormant) return -1; t->arg = arg(c, 1); start(*t); return 0; }
    case 8: yield(State::Dead); return 0;
    case 9: if (!current) FATAL("IOP ExitDeleteThread outside fiber"); yield(State::Dead); return 0;
    case 10: case 11: { auto t = thread(arg(c, 0)); if (!t || t->state == State::Dead || t->state == State::Dormant) return -1; if (t == current) yield(State::Dead); else t->state = State::Dead; return 0; }
    case 14: case 15: { auto t = thread(arg(c, 0) ? arg(c, 0) : current ? current->id : 0); if (!t) return -1; t->priority = arg(c, 1); return 0; }
    case 16: case 17: last_run_id = current ? current->id : last_run_id; return 0;
    case 18: case 19: { auto t = thread(arg(c, 0)); if (!t || t->state == State::Dormant || t->state == State::Dead) return -1; t->state = State::Ready; return 0; }
    case 27: case 28: { auto t = thread(arg(c, 0)); if (!t) return -1; int old = t->wakeups; t->wakeups = 0; return old; }
    case 29: case 30: { auto t = thread(arg(c, 0) ? arg(c, 0) : current ? current->id : 0); if (!t) return -1; t->suspended = true; if (t == current) yield(State::Ready); return 0; }
    case 31: case 32: { auto t = thread(arg(c, 0)); if (!t) return -1; t->suspended = false; return 0; }
    case 20: return current ? current->id : 0;
    case 22: case 23: case 45: {
        Thread* t = thread(arg(c, 0)); if (!t) return -1;
        u32 p = arg(c, 1);
        u32 state = t->state == State::Dormant || t->state == State::Dead ? 0x10 :
                    t->suspended ? (t->state == State::Ready ? 8 : 0xc) :
                    t == current ? 1 : t->state == State::Ready ? 2 : 4;
        u32 wait = t->state == State::Sleeping ? 1 : t->state == State::Delayed ? 2 :
                   t->state == State::Sema ? 3 : t->state == State::Event ? 4 : 0;
        if (op == 45) {
            iop::write32(p, state); iop::write32(p + 4, t->priority);
            iop::write32(p + 8, wait); iop::write32(p + 12, t->wait_id);
            iop::write32(p + 16, t->wakeups); return 0;
        }
        iop::write32(p, 0); iop::write32(p + 4, 0);
        iop::write32(p + 8, state); iop::write32(p + 12, t->entry);
        iop::write32(p + 16, t->guest_stack); iop::write32(p + 20, t->stack_size);
        iop::write32(p + 24, t->cpu.r[28]); iop::write32(p + 28, t->priority);
        iop::write32(p + 32, t->priority); iop::write32(p + 36, wait);
        iop::write32(p + 40, t->wait_id); iop::write32(p + 44, t->wakeups); return 0;
    }
    case 24: {
        if (!current) FATAL("IOP SleepThread outside fiber");
        if (current->wakeups) { --current->wakeups; return 0; }
        yield(State::Sleeping); return 0;
    }
    case 25: case 26: { auto t = thread(arg(c, 0)); if (!t) return -1; ++t->wakeups; if (t->state == State::Sleeping) { --t->wakeups; t->state = State::Ready; } return 0; }
    case 33: if (!current) FATAL("IOP DelayThread outside fiber"); current->wake_ns = platform::now_ns() + static_cast<u64>(arg(c, 0)) * 1000; yield(State::Delayed); return 0;
    case 34: { u64 ticks = platform::now_ns() * 36864 / 1000000; iop::write32(arg(c, 0), ticks); iop::write32(arg(c, 0) + 4, ticks >> 32); return 0; }
    case 39: { u64 ticks = static_cast<u64>(arg(c, 0)) * 36864 / 1000; iop::write32(arg(c, 1), ticks); iop::write32(arg(c, 1) + 4, ticks >> 32); return 0; }
    case 40: { u64 ticks = iop::read32(arg(c, 0)) | (static_cast<u64>(iop::read32(arg(c, 0) + 4)) << 32); iop::write32(arg(c, 1), ticks / 36864000); iop::write32(arg(c, 2), (ticks % 36864000) * 1000000 / 36864000); return 0; }
    case 35: case 36: {
        u32 clock = arg(c, 0), handler = arg(c, 1), userdata = arg(c, 2);
        u64 ticks = iop::read32(clock) | (static_cast<u64>(iop::read32(clock + 4)) << 32);
        alarms.push_back(Alarm{handler, userdata, c.r[28],
                               platform::now_ns() + ticks * 1000000000ull / 36864000});
        return 0;
    }
    case 37: case 38: {
        u32 handler = arg(c, 0), userdata = arg(c, 1);
        auto before = alarms.size();
        std::erase_if(alarms, [&](const Alarm& a) { return a.handler == handler && a.arg == userdata; });
        return before != alarms.size() ? 0 : -1;
    }
    default: unsupported("thbase", op);
    }
}
u32 thsemap(Context& c, u32 op) {
    int id = arg(c, 0);
    switch (op) {
    case 4: {   // CreateSema(iop_sema_t{attr, option, initial, max})
        const u32 p = static_cast<u32>(id);
        const int handle = next_handle();
        const int initial = static_cast<int>(iop::read32(p + 8));
        semas.emplace(handle, Sema{initial, static_cast<int>(iop::read32(p + 12)), iop::read32(p), iop::read32(p + 4), initial});
        return handle;
    }
    case 5: return semas.erase(id) ? 0 : -1;
    case 6: case 7: { auto it = semas.find(id); if (it == semas.end()) return -1; if (it->second.count >= it->second.maximum) return -1; ++it->second.count; wake_waiters(); return 0; }
    case 8: { auto it = semas.find(id); if (it == semas.end()) return -1; if (it->second.count) { --it->second.count; return 0; } if (!current) FATAL("IOP WaitSema outside fiber"); current->wait_id = id; yield(State::Sema); return 0; }
    case 9: { auto it = semas.find(id); if (it == semas.end() || !it->second.count) return -1; --it->second.count; return 0; }
    case 11: {   // ReferSemaStatus -> iop_sema_info_t{attr, option, initial, max, current, numWaitThreads}
        auto it = semas.find(id); if (it == semas.end()) return -1;
        const u32 p = arg(c, 1);
        u32 waiting = 0;
        for (const auto& [tid, t] : threads) if (t->state == State::Sema && t->wait_id == static_cast<u32>(id)) ++waiting;
        iop::write32(p, it->second.attr); iop::write32(p + 4, it->second.option);
        iop::write32(p + 8, it->second.initial); iop::write32(p + 12, it->second.maximum);
        iop::write32(p + 16, it->second.count); iop::write32(p + 20, waiting);
        return 0;
    }
    default: unsupported("thsemap", op);
    }
}
u32 thevent(Context& c, u32 op) {
    int id = arg(c, 0);
    switch (op) {
    case 4: { int handle = next_handle(); events.emplace(handle, Event{iop::read32(id + 8)}); return handle; }
    case 5: return events.erase(id) ? 0 : -1;
    case 6: case 7: { auto it = events.find(id); if (it == events.end()) return -1; it->second.bits |= arg(c, 1); wake_waiters(); return 0; }
    case 8: case 9: { auto it = events.find(id); if (it == events.end()) return -1; it->second.bits &= arg(c, 1); return 0; }
    case 10: case 11: {
        auto it = events.find(id); if (it == events.end()) return -1;
        u32 bits = arg(c, 1), mode = arg(c, 2), out = arg(c, 3);
        Thread temp{}; temp.wait_bits = bits; temp.wait_mode = mode;
        if (!matches(it->second, temp)) {
            if (op == 11) return -1;
            if (!current) FATAL("IOP WaitEventFlag outside fiber");
            current->wait_id = id; current->wait_bits = bits; current->wait_mode = mode;
            yield(State::Event);
            if (out) iop::write32(out, current->wait_result);
            return 0;
        }
        if (out) iop::write32(out, it->second.bits);
        if (mode & 0x10) it->second.bits = 0;
        return 0;
    }
    case 13: case 14: {
        auto it = events.find(id); if (it == events.end()) return -1;
        u32 p = arg(c, 1);
        iop::write32(p + 8, it->second.bits);
        iop::write32(p + 12, it->second.bits);
        u32 waiting = 0;
        for (const auto& [tid, t] : threads) if (t->state == State::Event && t->wait_id == static_cast<u32>(id)) ++waiting;
        iop::write32(p + 16, waiting);
        return 0;
    }
    default: unsupported("thevent", op);
    }
}
std::string format_guest(Context& c, u32 fmt_addr, unsigned first_arg, u32 va_addr = 0);
u32 sysmem(Context& c, u32 op) {
    switch (op) {
    case 4: {
        const u32 mode = arg(c, 0), size = arg(c, 1);
        if (mode == 0) return allocate(size);
        if (mode == 2) return allocate_at(arg(c, 2), size, 16);
        if (mode == 1) {
            const u64 extent = (static_cast<u64>(std::max(size, 1u)) + 15) & ~15ull;
            for (auto it = free_blocks.rbegin(); it != free_blocks.rend(); ++it) {
                if (extent > it->second) continue;
                const u32 address = (it->first + it->second - extent) & ~15u;
                if (address >= it->first) return allocate_at(address, size, 16);
            }
        }
        return 0;
    }
    case 5: return release(arg(c, 0)) ? 0 : static_cast<u32>(-1);
    case 6: return IOP_RAM_SIZE;
    case 7: {
        u32 maximum = 0;
        for (const auto& [begin, size] : free_blocks) {
            const u32 aligned = (begin + 15) & ~15u;
            if (aligned - begin <= size) maximum = std::max(maximum, size - (aligned - begin));
        }
        return maximum;
    }
    case 8: {
        u32 total = 0;
        for (const auto& [begin, size] : free_blocks) {
            const u32 aligned = (begin + 15) & ~15u;
            if (aligned - begin <= size) total += size - (aligned - begin);
        }
        return total;
    }
    case 9: return allocations.count(arg(c, 0)) ? arg(c, 0) : 0;
    case 10: { auto it = allocations.find(arg(c, 0)); return it == allocations.end() ? static_cast<u32>(-1) : it->second.requested; }
    case 14: { auto text = format_guest(c, arg(c, 0), 1, 0); LOG(LOG_TTY, "IOP Kprintf: %s", text.c_str()); return text.size(); }
    default: unsupported("sysmem", op);
    }
}
std::string format_guest(Context& c, u32 fmt_addr, unsigned first_arg, u32 va_addr) {
    auto fmt = guest_string(fmt_addr);
    std::string out;
    size_t cursor = 0;
    unsigned index = first_arg;
    auto next = [&]() -> u32 {
        u32 value = va_addr ? iop::read32(va_addr + (index++ - first_arg) * 4) : arg(c, index++);
        return value;
    };
    while (cursor < fmt.size()) {
        if (fmt[cursor] != '%') { out.push_back(fmt[cursor++]); continue; }
        size_t start = cursor++;
        if (cursor < fmt.size() && fmt[cursor] == '%') { out.push_back('%'); ++cursor; continue; }
        while (cursor < fmt.size() && std::strchr("-+ #0", fmt[cursor])) ++cursor;
        if (cursor < fmt.size() && fmt[cursor] == '*') ++cursor;
        else while (cursor < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[cursor]))) ++cursor;
        if (cursor < fmt.size() && fmt[cursor] == '.') {
            ++cursor;
            if (cursor < fmt.size() && fmt[cursor] == '*') ++cursor;
            else while (cursor < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[cursor]))) ++cursor;
        }
        size_t modifiers = cursor;
        while (cursor < fmt.size() && std::strchr("hlzjt", fmt[cursor])) ++cursor;
        if (cursor >= fmt.size() || cursor - start > 24) FATAL("IOP malformed printf format at %08x", fmt_addr);
        char spec = fmt[cursor++];
        if (cursor - modifiers > 2) FATAL("IOP unsupported 64-bit printf format");
        std::string token = fmt.substr(start, cursor - start);
        size_t pos = token.find('*');
        while (pos != std::string::npos) {
            s32 width = static_cast<s32>(next());
            if (width < -4096 || width > 4096) FATAL("IOP printf width out of range");
            token.replace(pos, 1, std::to_string(width));
            pos = token.find('*', pos + 1);
        }
        if (cursor - modifiers == 2 && spec != 's') token.erase(token.size() - 2, 1);
        char text[8192];
        int length = -1;
        switch (spec) {
        case 's': { auto str = guest_string(next()); length = std::snprintf(text, sizeof(text), token.c_str(), str.c_str()); break; }
        case 'd': case 'i': length = std::snprintf(text, sizeof(text), token.c_str(), static_cast<s32>(next())); break;
        case 'u': case 'x': case 'X': case 'o': case 'c': length = std::snprintf(text, sizeof(text), token.c_str(), next()); break;
        case 'p': { u32 value = next(); length = std::snprintf(text, sizeof(text), "0x%08x", value); break; }
        default: FATAL("IOP unsupported printf format %%%c", spec);
        }
        if (length < 0 || length >= static_cast<int>(sizeof(text)) || out.size() + length > 65535)
            FATAL("IOP printf output too large");
        out.append(text, length);
    }
    return out;
}
u32 print_guest(Context& c, u32 format_addr, unsigned first_arg) {
    auto text = format_guest(c, format_addr, first_arg);
    LOG(LOG_TTY, "%s", text.c_str());
    return text.size();
}
u32 sysclib(Context& c, u32 op) {
    u32 a = arg(c, 0), b = arg(c, 1), n = arg(c, 2);
    switch (op) {
    case 6: return std::toupper(static_cast<unsigned char>(a));
    case 8: {
        unsigned char ch = static_cast<unsigned char>(a);
        u32 bits = 0;
        if (std::isupper(ch)) bits |= 1;
        if (std::islower(ch)) bits |= 2;
        if (std::isdigit(ch)) bits |= 4;
        if (std::isspace(ch)) bits |= 8;
        if (std::ispunct(ch)) bits |= 16;
        if (std::iscntrl(ch)) bits |= 32;
        if (std::isxdigit(ch)) bits |= 64;
        if (ch == ' ' || ch == '\t') bits |= 128;
        return bits;
    }
    case 7: return std::tolower(static_cast<unsigned char>(a));
    case 11: for (u32 i = 0; i < n; ++i) if (iop::read8(a+i) != iop::read8(b+i)) return static_cast<s32>(iop::read8(a+i)) - iop::read8(b+i); return 0;
    case 12: case 13: {
        if (op == 13 && b < a && static_cast<u64>(b) + n > a) for (u32 i = n; i--;) iop::write8(a+i, iop::read8(b+i));
        else copy_guest(a, b, n);
        return a;
    }
    case 14: for (u32 i = 0; i < n; ++i) iop::write8(a+i, b); return a;
    case 17: for (u32 i = 0; i < b; ++i) iop::write8(a + i, 0); return 0;
    case 19: { auto text = format_guest(c, b, 2); put_string(a, text); return text.size(); }
    case 20: { u32 i = 0; while (iop::read8(a+i)) ++i; auto s = guest_string(b); put_string(a+i, s); return a; }
    case 21: for (u32 i = 0;; ++i) { u8 x = iop::read8(a+i); if (x == static_cast<u8>(b)) return a+i; if (!x) return 0; }
    case 22: case 29: {
        for (u32 i = 0; op == 22 || i < n; ++i) { u8 x = iop::read8(a+i), y = iop::read8(b+i); if (x != y) return static_cast<s32>(x) - y; if (!x) return 0; }
        return 0;
    }
    case 23: put_string(a, guest_string(b)); return a;
    case 27: return guest_string(a).size();
    case 28: {
        u32 end = a;
        while (iop::read8(end)) ++end;
        u32 copied = 0;
        while (copied < n && iop::read8(b + copied)) {
            iop::write8(end + copied, iop::read8(b + copied));
            ++copied;
        }
        iop::write8(end + copied, 0);
        return a;
    }
    case 30: { u32 i = 0; for (; i < n && iop::read8(b+i); ++i) iop::write8(a+i, iop::read8(b+i)); for (; i < n; ++i) iop::write8(a+i, 0); return a; }
    case 32: { u32 result = 0; for (u32 i = 0;; ++i) { u8 x = iop::read8(a+i); if (x == static_cast<u8>(b)) result = a+i; if (!x) return result; } }
    case 34: { auto hay = guest_string(a), needle = guest_string(b); auto p = hay.find(needle); return p == std::string::npos ? 0 : a + p; }
    case 36: case 38: {
        auto str = guest_string(a); char* end = nullptr;
        unsigned base = n; unsigned long result = op == 36 ? static_cast<unsigned long>(std::strtol(str.c_str(), &end, base)) : std::strtoul(str.c_str(), &end, base);
        if (b) iop::write32(b, a + (end - str.c_str()));
        return static_cast<u32>(result);
    }
    case 42: { auto text = format_guest(c, b, 0, n); put_string(a, text); return text.size(); }
    default: unsupported("sysclib", op);
    }
}
u32 intrman(Context& c, u32 op) {
    u32 irq = arg(c, 0);
    switch (op) {
    case 4: {   // RegisterIntrHandler(irq, mode, handler, arg)
        if (interrupts.count(irq)) return -104;   // KE_FOUND_HANDLER
        interrupts.emplace(irq, IntrHandler{arg(c, 2), arg(c, 3), c.r[28]});
        return 0;
    }
    case 5: return interrupts.erase(irq) ? 0 : -105;   // ReleaseIntrHandler; KE_NOTFOUND_HANDLER
    case 6:   // EnableIntr
        if (irq >= irq_enabled.size()) return -101;   // KE_ILLEGAL_INTRCODE
        irq_enabled[irq] = true;
        deliver_pending();
        return 0;
    case 7: {   // DisableIntr(irq, &previous)
        if (irq >= irq_enabled.size()) return -101;
        const bool was = irq_enabled[irq];
        if (arg(c, 1)) iop::write32(arg(c, 1), was ? irq : 0);
        irq_enabled[irq] = false;
        return was ? 0 : -103;   // KE_INTRDISABLE
    }
    case 17: if (irq) iop::write32(irq, intr_enabled); intr_enabled = false; return 0;
    case 18: intr_enabled = irq != 0; deliver_pending(); return 0;
    case 23: return callback_depth > 0;   // QueryIntrContext
    default: unsupported("intrman", op);
    }
}
u32 timrman(Context& c, u32 op) {
    int id = arg(c, 0);
    switch (op) {
    case 4: { int handle = next_handle(); timers.emplace(handle, Timer{}); return handle; }
    case 6: return timers.erase(id) ? 0 : -1;
    case 10: {
        auto it = timers.find(id); if (it == timers.end() || !it->second.running) return 0;
        const Timer& t = it->second;
        u64 frequency = t.source == 4 ? 15625 : t.source == 2 ? 13500000 : 36864000;
        u64 elapsed = t.period - std::min<u64>(t.period, t.next - std::min(t.next, platform::now_ns()));
        return elapsed * frequency / (static_cast<u64>(std::max(t.prescale, 1u)) * 1000000000ull);
    }
    case 20: {
        auto it = timers.find(id); if (it == timers.end()) return -1;
        it->second.compare = arg(c, 1);
        it->second.handler = arg(c, 2);
        it->second.arg = arg(c, 3);
        it->second.gp = c.r[28];
        return 0;
    }
    case 22: {
        auto it = timers.find(id); if (it == timers.end()) return -1;
        Timer& t = it->second;
        t.source = arg(c, 1);
        t.prescale = arg(c, 3);
        u64 frequency = t.source == 4 ? 15625 : t.source == 2 ? 13500000 : 36864000;
        t.period = std::max<u64>(1, static_cast<u64>(t.compare) * std::max(t.prescale, 1u) * 1000000000ull / frequency);
        return 0;
    }
    case 23: { auto it = timers.find(id); if (it == timers.end()) return -1; it->second.running = true; it->second.next = platform::now_ns() + (it->second.period ? it->second.period : 1000000); return 0; }
    case 24: { auto it = timers.find(id); if (it == timers.end()) return -1; it->second.running = false; return 0; }
    default: unsupported("timrman", op);
    }
}
u32 ioman(Context& c, u32 op) {
    int fd = arg(c, 0);
    switch (op) {
    case 4: { iso::Entry e{}; if (!iso::find(guest_string(fd), &e) || e.is_dir) return -2; int handle = next_fd++; files.emplace(handle, File{e, 0}); return handle; }
    case 5: return files.erase(fd) ? 0 : -1;
    case 6: {
        auto it = files.find(fd); if (it == files.end()) return -1;
        u32 n = std::min<u64>(arg(c, 2), it->second.entry.size - std::min<u64>(it->second.offset, it->second.entry.size));
        std::vector<u8> bytes(n);
        if (n && !iso::read_bytes(static_cast<u64>(it->second.entry.lba) * iso::SECTOR_SIZE + it->second.offset, n, bytes.data())) return -1;
        for (u32 i = 0; i < n; ++i) iop::write8(arg(c, 1)+i, bytes[i]);
        it->second.offset += n; return n;
    }
    case 7: return -1; // read-only disc
    case 8: { auto it = files.find(fd); if (it == files.end()) return -1; s64 base = arg(c, 2) == 0 ? 0 : arg(c, 2) == 1 ? it->second.offset : it->second.entry.size; s64 pos = base + static_cast<s32>(arg(c, 1)); if (pos < 0) return -1; it->second.offset = pos; return pos; }
    case 9: return -1;
    default: unsupported("ioman", op);
    }
}
u32 cdvdman(Context& c, u32 op) {
    switch (op) {
    case 4: cd_error = 0; return 1;
    case 10: {
        iso::Entry entry;
        const std::string name = guest_string(arg(c, 1));
        const bool found = iso::find(name, &entry);
        LOG(LOG_CDVD, "[cdvdman] SearchFile '%s' -> %s lba %u size %u\n", name.c_str(), found ? "found" : "missing",
            found ? entry.lba : 0, found ? entry.size : 0);
        if (!found) return 0;
        u32 file = arg(c, 0);
        iop::write32(file, entry.lba);
        iop::write32(file + 4, entry.size);
        return 1;
    }
    case 6: {
        u32 lba = arg(c, 0), count = arg(c, 1), dest = arg(c, 2);
        if (count > IOP_RAM_SIZE / iso::SECTOR_SIZE || static_cast<u64>(dest) + static_cast<u64>(count) * iso::SECTOR_SIZE > IOP_RAM_SIZE) FATAL("IOP sceCdRead outside RAM");
        cd_error = iso::read_sectors(lba, count, g_iop_ram + dest) ? 0 : 0x30;
        LOG(LOG_CDVD, "[cdvdman] Read lba %u x%u -> %06x%s\n", lba, count, dest, cd_error ? " FAILED" : "");
        if (cd_callback) pending_cd.push_back(PendingCd{cd_callback, cd_callback_gp,
                                                         current ? current->id : 0, 16, !current});
        return cd_error ? 0 : 1;
    }
    case 8: return cd_error;
    case 11: return 0;
    case 12: return 0x14; // PS2 DVD
    case 13: return 2; // ready
    case 28: return 0;
    case 37: { u32 old = cd_callback; cd_callback = arg(c, 0); cd_callback_gp = c.r[28]; return old; }
    case 56: stream_lba = 0; return 1;
    case 57: {
        u32 count = arg(c, 0), dest = arg(c, 1);
        if (count > IOP_RAM_SIZE / iso::SECTOR_SIZE || static_cast<u64>(dest) + static_cast<u64>(count) * iso::SECTOR_SIZE > IOP_RAM_SIZE)
            FATAL("IOP sceCdStRead outside RAM");
        bool ok = iso::read_sectors(stream_lba, count, g_iop_ram + dest);
        LOG(LOG_CDVD, "[cdvdman] StRead lba %u x%u -> %06x%s\n", stream_lba, count, dest, ok ? "" : " FAILED");
        if (arg(c, 3)) iop::write32(arg(c, 3), ok ? 0 : 0x30);
        if (ok) stream_lba += count;
        return ok ? count : 0;
    }
    case 58: stream_lba = arg(c, 0); return 1;
    case 59: stream_lba = arg(c, 0); return 1;
    case 60: return 0;
    case 61: return 1;
    default: unsupported("cdvdman", op);
    }
}
u32 next_rpc_request(u32 queue_addr) {
    auto q = rpc_queues.find(queue_addr);
    if (q == rpc_queues.end()) FATAL("sceSifGetNextRequest on unknown queue %08x", queue_addr);
    if (q->second.waiting.empty()) {
        iop::write32(queue_addr + 4, 0);
        return 0;
    }
    RpcRequest request = std::move(q->second.waiting.front());
    q->second.waiting.pop_front();
    const u32 address = request.server;
    if (rpc_active.contains(address))
        FATAL("IOP RPC server %08x already executing a request", address);
    auto server = std::find_if(rpc_servers.begin(), rpc_servers.end(),
                               [address](const auto& entry) { return entry.second.address == address; });
    if (server == rpc_servers.end()) FATAL("IOP RPC server %08x was removed", address);
    const u32 buffer = server->second.buffer;
    for (size_t n = 0; n < request.data.size(); ++n)
        iop::write8(buffer + static_cast<u32>(n), request.data[n]);
    iop::write32(address + 12, static_cast<u32>(request.data.size()));
    iop::write32(address + 36, request.fno);
    iop::write32(queue_addr + 4, 1);
    rpc_active.emplace(address, std::move(request));
    return address;
}
void exec_rpc_request(u32 address, u32 stack) {
    if (!current) FATAL("sceSifExecRequest outside IOP thread");
    auto pending = rpc_active.find(address);
    if (pending == rpc_active.end()) FATAL("sceSifExecRequest without pending request %08x", address);
    RpcRequest request = std::move(pending->second);
    rpc_active.erase(pending);
    auto server = std::find_if(rpc_servers.begin(), rpc_servers.end(),
                               [address](const auto& entry) { return entry.second.address == address; });
    if (server == rpc_servers.end()) FATAL("IOP RPC server %08x was removed", address);
    Context c{};
    c.r[4] = request.fno;
    c.r[5] = server->second.buffer;
    c.r[6] = static_cast<u32>(request.data.size());
    c.r[28] = server->second.gp;
    c.r[29] = stack;
    LOG(LOG_SIF, "[rpc] exec sid=%08x fno=%u size=%zu token=%u\n", server->first, request.fno,
        request.data.size(), request.token);
    iop::invoke(server->second.handler, c);
    LOG(LOG_SIF, "[rpc] done token=%u result=%08x\n", request.token, c.r[2]);
    iop::complete_rpc(request.token, c.r[2]);
}
u32 sifcmd(Context& c, u32 op) {
    switch (op) {
    case 4: case 14: return 0;
    case 8: command_buffer = arg(c, 0); return 0;
    case 10: commands[arg(c, 0)] = CommandHandler{arg(c, 1), arg(c, 2), c.r[28]}; return 0;
    case 12: case 13: case 28: case 29: {
        u32 src = arg(c, 1), size = arg(c, 2), extra = arg(c, 3), dest = arg(c, 4), extra_size = arg(c, 5);
        if (size < 16 || size > 112) FATAL("IOP invalid SIF packet size %u", size);
        iop::write32(src, (extra_size << 8) | size);
        iop::write32(src + 4, extra_size ? dest : 0);
        iop::write32(src + 8, arg(c, 0));
        iop::send_packet(src, size, extra, dest, extra_size);
        if ((op == 28 || op == 29) && arg(c, 6)) callback(arg(c, 6), c.r[28], arg(c, 7));
        return 1;
    }
    case 17: { // sceSifRegisterRpc(server, sid, handler, buff, callback, cbuff, queue)
        const u32 address = arg(c, 0), sid = arg(c, 1), queue = arg(c, 6);
        if (!rpc_queues.contains(queue)) FATAL("IOP RPC server %08x has no registered queue %08x", sid, queue);
        rpc_servers[sid] = RpcServer{address, arg(c, 2), arg(c, 3),
                                     arg(c, 4), arg(c, 5), queue, c.r[28]};
        iop::write32(address, sid);
        iop::write32(address + 4, arg(c, 2));
        iop::write32(address + 8, arg(c, 3));
        iop::write32(address + 16, arg(c, 4));
        iop::write32(address + 20, arg(c, 5));
        iop::write32(address + 64, queue);
        return address;
    }
    case 19: { // sceSifSetRpcQueue(qd, thread_id)
        const u32 queue = arg(c, 0);
        rpc_queues[queue] = RpcQueue{static_cast<int>(arg(c, 1)), {}};
        for (u32 offset = 0; offset < 24; offset += 4) iop::write32(queue + offset, 0);
        iop::write32(queue, arg(c, 1));
        return 0;
    }
    case 20: return next_rpc_request(arg(c, 0));
    case 21: exec_rpc_request(arg(c, 0), c.r[29]); return 0;
    case 22: {
        if (!current) FATAL("sceSifRpcLoop outside fiber");
        const u32 queue = arg(c, 0);
        for (;;) {
            const u32 server = next_rpc_request(queue);
            if (server) exec_rpc_request(server, c.r[29]);
            else yield(State::Rpc);
        }
    }
    default: unsupported("sifcmd", op);
    }
}
u32 sifman(Context& c, u32 op) {
    switch (op) {
    case 5: return 0;
    case 7: {
        u32 transfer = arg(c, 0), count = arg(c, 1);
        u32 id = 0;
        if (count > 64) FATAL("IOP SIF DMA count %u", count);
        for (u32 i = 0; i < count; ++i) {
            u32 descriptor = transfer + 16 * i;
            id = iop::dma_to_ee(iop::read32(descriptor), iop::read32(descriptor + 4),
                                iop::read32(descriptor + 8));
        }
        return id;
    }
    case 8: return -1;
    default: unsupported("sifman", op);
    }
}
u32 modload(Context& c, u32 op) {
    size_t count = 0;
    const Module* const* mods = generated_modules(count);
    if (op == 23) {
        u32 address = arg(c, 0);
        for (size_t i = 0; i < count; ++i)
            if (iop::module_loaded(mods[i]->base) && address >= mods[i]->base &&
                address - mods[i]->base < mods[i]->size) return static_cast<u32>(i + 17);
        return -1;
    }
    if (op == 17) {
        u32 id = arg(c, 0), dest = arg(c, 1);
        if (id < 17 || id - 17 >= count || !iop::module_loaded(mods[id - 17]->base)) return -1;
        const Module& m = *mods[id - 17];
        iop::write32(dest, m.base);
        iop::write32(dest + 4, m.size);
        iop::write32(dest + 8, m.entry);
        iop::write32(dest + 12, m.gp);
        return 0;
    }
    unsupported("modload", op);
}
} // namespace

void reset() {
    threads.clear(); semas.clear(); events.clear(); timers.clear(); files.clear(); alarms.clear(); pending_cd.clear();
    rpc_servers.clear(); rpc_queues.clear(); rpc_active.clear(); commands.clear();
    allocations.clear(); free_blocks.clear(); interrupts.clear(); intr_enabled = true;
    pending_irqs.clear(); pending_vblank = {}; critical_thread = 0; irq_enabled = {};
    free_blocks.emplace(HEAP_START, HEAP_END - HEAP_START);
    vblank_handlers = {}; vblank_args = {}; vblank_gp = {}; current = nullptr;
    next_id = 1; next_fd = 3; last_run_id = 0;
    cd_callback = cd_callback_gp = cd_error = command_buffer = callback_stack = stream_lba = 0;
}
void heap_rpc(u32 fno, u32 send_addr, u32 send_size, u32 recv_addr, u32 recv_size) {
    const u32 send = send_addr & 0x1fffffff, recv = recv_addr & 0x1fffffff;
    if (send > IOP_RAM_SIZE || send_size > IOP_RAM_SIZE - send ||
        recv > IOP_RAM_SIZE || recv_size > IOP_RAM_SIZE - recv || recv_size < 4)
        FATAL("IOP heap RPC packet out of bounds");
    auto input = [&](u32 bytes) {
        if (send_size < bytes) FATAL("short IOP heap RPC function %u packet", fno);
    };
    auto arg_word = [&](u32 offset) { return iop::read32(send_addr + offset); };
    u32 value = 0;
    switch (fno) {
    case 1: input(4); value = allocate(arg_word(0)); break;
    case 2: input(4); value = release(arg_word(0)) ? 0 : static_cast<u32>(-1); break;
    case 3: {
        input(5);
        const char* path = reinterpret_cast<const char*>(g_iop_ram + send + 4);
        const size_t length = strnlen(path, send_size - 4);
        if (length == send_size - 4) FATAL("unterminated IOP heap RPC file path");
        iso::Entry file;
        if (!iso::find(std::string(path, length), &file) || file.is_dir) {
            value = static_cast<u32>(-2);
            break;
        }
        const u32 dest = arg_word(0) & 0x1fffffff;
        auto block = allocations.upper_bound(dest);
        if (block == allocations.begin()) { value = static_cast<u32>(-1); break; }
        --block;
        if (dest < block->first || dest - block->first > block->second.extent ||
            file.size > block->second.extent - (dest - block->first)) {
            value = static_cast<u32>(-1);
            break;
        }
        value = iso::read_bytes(u64(file.lba) * iso::SECTOR_SIZE, file.size, g_iop_ram + dest)
            ? 0 : static_cast<u32>(-5);
        break;
    }
    case 4: { // Sony EE wrapper 0x1266e0: {size, mode, fixed-address}.
        input(12);
        Context c{};
        c.r[4] = arg_word(4); c.r[5] = arg_word(0); c.r[6] = arg_word(8);
        value = sysmem(c, 4);
        break;
    }
    case 5: value = IOP_RAM_SIZE; break;
    case 6: case 7: { Context c{}; value = sysmem(c, fno == 6 ? 7 : 8); break; }
    default: FATAL("Unsupported IOP heap RPC function %u", fno);
    }
    iop::write32(recv_addr, value);
    LOG(LOG_SIF, "[iop heap] function %u -> %08x\n", fno, value);
}
void interrupt(u32 irq) {
    auto it = interrupts.find(irq);
    if (it == interrupts.end()) { LOG(LOG_SPU, "[iop irq] %u (no handler)\n", irq); return; }
    if (!interrupts_deliverable() || !irq_is_enabled(irq)) {
        LOG(LOG_SPU, "[iop irq] %u latched (intr %d depth %d enabled %d)\n", irq, (int)intr_enabled,
            callback_depth, (int)irq_is_enabled(irq));
        if (std::find(pending_irqs.begin(), pending_irqs.end(), irq) == pending_irqs.end()) pending_irqs.push_back(irq);
        return;
    }
    LOG(LOG_SPU, "[iop irq] %u -> %08x\n", irq, it->second.handler);
    const IntrHandler handler = it->second;
    callback(handler.handler, handler.gp, handler.arg);
}
void start_module(u32 entry, const Context& context) {
    int id = next_handle();
    auto t = std::make_unique<Thread>();
    t->id = id;
    t->entry = entry;
    t->cpu = context;
    t->arg = context.r[4];
    t->priority = 0;
    t->stack_size = 0x4000;
    start(*t);
    threads.emplace(id, std::move(t));
}
void checkpoint() {
    if (PS2_UNLIKELY(g_log_mask & LOG_IOP)) {   // where busy IOP threads spend their time
        static u32 samples = 0;
        if (++samples % 20000 == 0 && current && !callback_depth)
            LOG(LOG_IOP, "[iopspin] t%d pc=%08x ra=%08x\n", current->id, current->cpu.pc, current->cpu.r[31]);
    }
    if (!current || current->state != State::Ready || callback_depth) return;
    if (!pending_cd.empty()) {
        bool due = false;
        for (auto& request : pending_cd)
            if (request.thread_id == current->id && !request.ready && --request.checkpoints == 0)
                request.ready = due = true;
        if (due) {   // deliver the CD callback; the thread may poll for it
            if (!intr_enabled) critical_thread = current->id;
            yield(State::Ready);
            return;
        }
    }
    if (--checkpoint_countdown) return;
    checkpoint_countdown = CHECKPOINT_BATCH;
    // A real IOP thread runs until it blocks.  Preempt only for a higher-priority
    // thread readied by a timer/alarm/delay expiry, or at the end of the slice so
    // the EE (same host thread) keeps running.
    if (platform::now_ns() < slice_end_ns) {
        wake_waiters();
        bool higher = false;
        for (const auto& [id, t] : threads)
            if (t->state == State::Ready && !t->suspended && t->priority < current->priority) { higher = true; break; }
        if (!higher) return;
    }
    // With interrupts masked no other thread may run before this one resumes.
    if (!intr_enabled) critical_thread = current->id;
    yield(State::Ready);
}
void deliver_cd_callbacks() {
    while (interrupts_deliverable() && !pending_cd.empty() && pending_cd.front().ready) {
        PendingCd request = pending_cd.front();
        pending_cd.pop_front();
        callback(request.handler, request.gp, 1);
    }
}
Thread* pick_thread() {
    if (critical_thread) {   // it yielded with interrupts masked: nothing else may run first
        Thread* t = thread(critical_thread);
        critical_thread = 0;
        if (t && t->state == State::Ready && !t->suspended) return t;
    }
    Thread* best = nullptr;
    for (auto& [id, t] : threads)
        if (t->state == State::Ready && !t->suspended &&
            (!best || t->priority < best->priority ||
             (t->priority == best->priority &&
              ((id > last_run_id && best->id <= last_run_id) ||
               ((id > last_run_id) == (best->id > last_run_id) && id < best->id)))))
            best = t.get();
    return best;
}
void step() {
    wake_waiters();
    deliver_cd_callbacks();
    deliver_pending();
    Thread* best = pick_thread();
    if (!best) return;
    last_run_id = best->id;
    current = best;
    checkpoint_countdown = CHECKPOINT_BATCH;
    slice_end_ns = platform::now_ns() + SLICE_NS;
    fiber_switch(&scheduler, best->fiber);
    current = nullptr;
    for (auto& request : pending_cd)
        if (request.thread_id == best->id && best->state != State::Ready) request.ready = true;
    deliver_cd_callbacks();
}
bool has_work() {
    wake_waiters();
    if (!pending_cd.empty() && pending_cd.front().ready) return true;
    for (const auto& [id, t] : threads) if (t->state == State::Ready && !t->suspended) return true;
    return false;
}
u64 deadline() {
    u64 result = NEVER;
    // Runnable threads want the CPU again one slice-pair after the last
    // iop::run (which itself runs at most ~1 ms): a busy IOP gets ~half the time.
    for (const auto& [id, t] : threads)
        if (t->state == State::Ready && !t->suspended) { result = platform::now_ns() + 2 * SLICE_NS; break; }
    for (const auto& [id, t] : threads) if (t->state == State::Delayed) result = std::min(result, t->wake_ns);
    for (const auto& [id, timer] : timers) if (timer.running) result = std::min(result, timer.next);
    for (const auto& alarm : alarms) result = std::min(result, alarm.next);
    return result;
}
void dump_threads() {
    static const char* const names[] = {"dormant", "ready", "sleeping", "vblank-start", "vblank-end",
                                        "delayed", "sema", "event", "rpc", "dead"};
    const u64 now = platform::now_ns();
    fprintf(stderr, "--- IOP threads (current t%d) ---\n", current ? current->id : 0);
    for (const auto& [id, t] : threads) {
        fprintf(stderr, "  t%-3d pri %3u %-12s%s wait_id=%x bits=%x mode=%x", id, t->priority,
                names[static_cast<int>(t->state)], t->suspended ? " (suspended)" : "", t->wait_id,
                t->wait_bits, t->wait_mode);
        if (t->state == State::Delayed) fprintf(stderr, " wake in %lld us", (long long)(t->wake_ns - now) / 1000);
        if (t->state == State::Sema) {
            auto it = semas.find(static_cast<int>(t->wait_id));
            if (it != semas.end()) fprintf(stderr, " sema count=%d", it->second.count);
        }
        if (t->state == State::Event) {
            auto it = events.find(static_cast<int>(t->wait_id));
            if (it != events.end()) fprintf(stderr, " flag bits=%x", it->second.bits);
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "  pending rpc: %zu active, timers %zu, alarms %zu, cd %zu\n", rpc_active.size(),
            timers.size(), alarms.size(), pending_cd.size());
}
void vblank(bool start) {
    State waiting = start ? State::VblankStart : State::VblankEnd;
    for (auto& [id, t] : threads) if (t->state == waiting) t->state = State::Ready;
    const int k = start ? 0 : 1;
    if (!interrupts_deliverable()) { pending_vblank[k] = true; return; }
    callback(vblank_handlers[k], vblank_gp[k], static_cast<u32>(k), vblank_args[k]);
}
void command(u32 addr, u32 size) {
    if (size < 16) FATAL("IOP short SIF command packet (%u bytes)", size);
    u32 id = iop::read32(addr + 8);
    LOG(LOG_SIF, "[sif] ee->iop cmd %08x size %u\n", id, size);
    auto it = commands.find(id);
    if (it == commands.end()) FATAL("IOP unregistered SIF command %08x", id);
    callback(it->second.handler, it->second.gp, addr, it->second.arg);
}
bool has_rpc_server(u32 id) { return rpc_servers.count(id) != 0; }
u32 rpc_buffer(u32 id, u32& cbuf) {
    auto it = rpc_servers.find(id);
    if (it == rpc_servers.end()) return 0;
    cbuf = it->second.cbuf;
    return it->second.buffer;
}
void rpc_request(u32 id, u32 fno, u32 send_addr, u32 send_size, u32 token) {
    auto it = rpc_servers.find(id);
    if (it == rpc_servers.end()) FATAL("IOP unregistered RPC server %08x", id);
    const RpcServer& server = it->second;
    if (!server.handler) FATAL("IOP RPC server %08x has no handler", id);
    auto queue = rpc_queues.find(server.queue);
    if (queue == rpc_queues.end()) FATAL("IOP RPC server %08x has no queue", id);
    RpcRequest request{server.address, fno, token, std::vector<u8>(send_size)};
    for (u32 n = 0; n < send_size; ++n) request.data[n] = iop::read8(send_addr + n);
    queue->second.waiting.push_back(std::move(request));
    Thread* worker = thread(queue->second.thread_id);
    if (!worker) FATAL("IOP RPC queue %08x has no worker thread %d", server.queue, queue->second.thread_id);
    LOG(LOG_SIF, "[rpc] queue sid=%08x fno=%u size=%u token=%u worker=%d state=%d\n", id, fno,
        send_size, token, worker->id, (int)worker->state);
    if (worker->state == State::Rpc || worker->state == State::Sleeping)
        worker->state = State::Ready;
}
bool supports(const char* lib, u32 op) {
    auto name = [lib](const char* other) { return std::strcmp(lib, other) == 0; };
    if (name("thbase")) return op == 4 || op == 5 || op == 6 || op == 8 || op == 9 ||
        op == 10 || op == 11 || (op >= 14 && op <= 20) || (op >= 22 && op <= 32) ||
        op == 33 || (op >= 34 && op <= 40) || op == 45;
    if (name("thsemap")) return op == 4 || op == 5 || op == 6 || op == 7 || op == 8 || op == 9 || op == 11;
    if (name("thevent")) return op == 4 || op == 5 || op == 6 || op == 7 || op == 8 ||
        op == 9 || op == 10 || op == 11 || op == 13 || op == 14;
    if (name("sysmem")) return (op >= 4 && op <= 10) || op == 14;
    if (name("sysclib")) return op == 6 || op == 7 || op == 8 || op == 11 || op == 12 ||
        op == 13 || op == 14 || op == 17 || op == 19 || (op >= 20 && op <= 23) ||
        (op >= 27 && op <= 30) || op == 32 || op == 34 || op == 36 || op == 38 || op == 42;
    if (name("intrman")) return op == 4 || op == 5 || op == 6 || op == 7 ||
        op == 17 || op == 18 || op == 23;
    if (name("timrman")) return op == 4 || op == 6 || op == 10 || op == 20 || op == 22 || op == 23 || op == 24;
    if (name("vblank")) return op == 4 || op == 5 || op == 8 || op == 9;
    if (name("ioman")) return op >= 4 && op <= 9;
    if (name("cdvdman")) return op == 4 || op == 6 || op == 8 || op == 10 ||
        op == 11 || op == 12 || op == 13 || op == 28 || op == 37 ||
        (op >= 56 && op <= 61);
    if (name("sifcmd")) return op == 4 || op == 8 || op == 10 || op == 12 ||
        op == 13 || op == 14 || op == 17 || op == 19 || op == 20 ||
        op == 21 || op == 22 || op == 28 || op == 29;
    if (name("sifman")) return op == 5 || op == 7 || op == 8;
    if (name("loadcore")) return op == 5 || op == 6;
    if (name("modload")) return op == 17 || op == 23;
    if (name("stdio")) return op == 4;
    return false;
}
u32 dispatch(Context& c, const char* library, u32 op) {
    u32 result = 0;
    if (!std::strcmp(library, "thbase")) result = thbase(c, op);
    else if (!std::strcmp(library, "thsemap")) result = thsemap(c, op);
    else if (!std::strcmp(library, "thevent")) result = thevent(c, op);
    else if (!std::strcmp(library, "sysmem")) result = sysmem(c, op);
    else if (!std::strcmp(library, "sysclib")) result = sysclib(c, op);
    else if (!std::strcmp(library, "stdio")) { if (op != 4) unsupported(library, op); result = print_guest(c, arg(c, 0), 1); }
    else if (!std::strcmp(library, "loadcore")) { if (op != 5 && op != 6) unsupported(library, op); result = 0; }
    else if (!std::strcmp(library, "modload")) result = modload(c, op);
    else if (!std::strcmp(library, "sifman")) result = sifman(c, op);
    else if (!std::strcmp(library, "intrman")) result = intrman(c, op);
    else if (!std::strcmp(library, "timrman")) result = timrman(c, op);
    else if (!std::strcmp(library, "vblank")) {
        switch (op) {
        case 4: case 5: if (!current) FATAL("IOP WaitVblank outside fiber"); yield(op == 4 ? State::VblankStart : State::VblankEnd); result = 0; break;
        case 8: { u32 index = arg(c, 0) & 1; vblank_handlers[index] = arg(c, 2); vblank_args[index] = arg(c, 3); vblank_gp[index] = c.r[28]; result = 0; break; }
        case 9: { u32 index = arg(c, 0) & 1; vblank_handlers[index] = 0; result = 0; break; }
        default: unsupported(library, op);
        }
    }
    else if (!std::strcmp(library, "ioman")) result = ioman(c, op);
    else if (!std::strcmp(library, "cdvdman")) result = cdvdman(c, op);
    else if (!std::strcmp(library, "sifcmd")) result = sifcmd(c, op);
    else unsupported(library, op);
    LOG(LOG_CALL, "[iopcall] t%d %s:%u(%08x,%08x,%08x,%08x) -> %08x\n", current ? current->id : 0, library, op,
        arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), result);
    c.r[2] = result;
    return result;
}
} // namespace iop::kernel
