// EE kernel HLE.
//
// Scheduling model (mirrors the PS2 kernel / Play!'s HLE):
//  * g_ready holds every runnable thread (including the running one), sorted
//    by priority (0 = highest), FIFO within a priority.
//  * The running thread is the first entry unless it has just been blocked.
//  * Switching happens on blocking syscalls, when a higher-priority thread
//    becomes ready (from a syscall or after interrupt handlers), on
//    RotateThreadReadyQueue and on thread exit.
//  * Each guest thread runs on its own Fiber.  When no thread is runnable the
//    scheduler fiber (the host thread's original stack) idles, delivering
//    interrupts and running the IOP until something becomes ready.
#include "kernel.h"
#include "fiber.h"
#include "hw/hw.h"

#include "ps2/gs.h"
#include "ps2/intc.h"
#include "ps2/iop.h"
#include "ps2/log.h"
#include "ps2/platform.h"
#include "ps2/spu2.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstring>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <xmmintrin.h>

void profiler_stop_and_report();
void debug_watch(u32 addr);
void debug_watch_report();

volatile u32 g_ee_events = 0;

void vu1_ring_waits(u64& producer, u64& consumer);
// Declared in ps2/ee_gen.h; read by the generated camera code.
float g_ee_camera_fov_scale = 1.0f;

// ---- mouse look (CAMERA_HOOKS in tools/recomp_ee.py) ------------------------
// The player camera controller f_0046d410 turns its camera object (s1) by the
// right stick at a capped rate.  The end hook adds the mouse motion straight to
// yaw (+284) and pitch (+280) with the game's own wrap and pitch limit, so the
// mouse is 1:1 and uncapped while a gamepad behaves exactly as before.
namespace {
constexpr float kMouseRadPerCount = 0.0025f;   // at --mouse-sens 1 (~0.14 deg)
u32 g_cam_object = 0;
float g_cam_invert = 1.0f;      // game "Invert Y axis": -1
float g_cam_pitch_before = 0.0f;
f32a& ee_f32(u32 addr) { return *reinterpret_cast<f32a*>(EE_BASE + addr); }
} // namespace

void ee_camera_hook_begin(EEContext* c) {
    g_cam_object = c->r[17].w[0];
    g_cam_invert = c->fpr[3].f;
    g_cam_pitch_before = ee_f32(g_cam_object + 280);
}

void ee_camera_hook_end(EEContext* c) {
    const u32 cam = c->r[17].w[0];
    if (cam != g_cam_object) return;
    float dx, dy;
    bool recent;
    platform::mouse_camera_take(dx, dy, recent);
    f32a& yaw = ee_f32(cam + 284);
    f32a& pitch = ee_f32(cam + 280);
    // With the stick idle the game levels the pitch again (auto-centre while
    // moving); with an active mouse that would fight the player.
    if (recent && ee_f32(cam + 392) == 0.0f && ee_f32(cam + 396) == 0.0f) pitch = g_cam_pitch_before;
    if (dx == 0.0f && dy == 0.0f) return;
    constexpr float two_pi = 6.28318530718f;
    const float turned = yaw - dx * kMouseRadPerCount;
    float wrap = turned / two_pi + 100.5f;   // as f_001ad450: [-pi, pi)
    wrap -= static_cast<float>(static_cast<int>(wrap));
    yaw = two_pi * (wrap - 0.5f);
    // Pitch limit as at 0046d79c: gp[-30116] * settings(s0)[+348].
    const float limit = ee_f32(c->r[28].w[0] - 30116) * ee_f32(c->r[16].w[0] + 348);
    const float tilted = pitch + dy * kMouseRadPerCount * g_cam_invert;
    pitch = std::max(-limit, std::min(limit, tilted));
}

namespace kernel {
namespace {

constexpr int MAX_THREADS = 256;
constexpr int MAX_SEMAS = 256;
constexpr int MAX_HANDLERS = 128;
constexpr int MAX_ALARMS = 64;
constexpr u32 INTR_STACK_TOP = 0x000F0000;  // guest kernel area, unused by the game
constexpr u32 STACKRES = 0x2A0;
constexpr u32 GUEST_RET = 0x00000000;        // ra value for runtime->guest calls
constexpr size_t FIBER_STACK = 8u << 20;

enum class TS : u8 { Free, Ready, Sleep, WaitSema, Suspended, SuspSleep, SuspWaitSema, Dormant };

struct Thread {
    TS st = TS::Free;
    u32 entry = 0, stack = 0, stack_size = 0, gp = 0, attr = 0, option = 0;
    s32 init_prio = 0, prio = 0;
    s32 wakeup = 0;
    s32 wait_sema = -1;
    s32 wait_result = 0;
    u32 start_arg = 0;
    Fiber* fiber = nullptr;
    bool need_start = false;    // fiber must (re)start at entry
    alignas(64) EEContext ctx;
};

struct Sema {
    bool used = false;
    s32 count = 0, max_count = 0, init_count = 0;
    u32 attr = 0, option = 0;
    std::vector<int> waiters;   // FIFO
};

struct Handler {
    bool used = false;
    bool enabled = true;
    u32 cause = 0;              // INTC cause or DMAC channel
    u32 func = 0, arg = 0, gp = 0;
};

struct Alarm {
    bool used = false;
    u64 due_ns = 0;
    u32 func = 0, arg = 0, gp = 0;
    u16 time = 0;
};

Config g_cfg;
Thread g_threads[MAX_THREADS];
Sema g_semas[MAX_SEMAS];
Handler g_intc_h[MAX_HANDLERS];
Handler g_dmac_h[MAX_HANDLERS];
std::vector<int> g_intc_order;   // handler ids in call order (all causes)
std::vector<int> g_dmac_order;
Alarm g_alarms[MAX_ALARMS];
std::vector<int> g_ready;
int g_cur = 0;                   // running thread id (0: scheduler / none)
Fiber g_sched_fiber;             // the host thread's own stack
Fiber* g_dead_fiber = nullptr;   // fiber of an exited thread, freed after switching away
bool g_in_intr = false;
bool g_need_resched = false;
bool g_dispatch_disabled = false;
alignas(64) EEContext g_ictx;     // context for interrupt handlers / runtime calls
u32 g_heap_end = 0;
u32 g_vsync_flag_addr = 0, g_vsync_csr_addr = 0;
// Kernel syscall table in guest kernel memory (physical 0x800, also visible
// at kseg0 0x80000800).  libkernel locates it by scanning kernel memory for
// handler addresses it installed with SetSyscall, then patches entries with
// the Copy syscall.  Entries still holding their marker are native syscalls.
constexpr u32 SYSCALL_TABLE = 0x00000800;
inline u32* syscall_table() { return (u32*)ee_host(SYSCALL_TABLE); }
inline u32 syscall_marker(u32 n) { return 0x8000F000u + n * 8; }
u32 g_osd_config = 0;
u32 g_osd_config2[2] = {};
u32 g_gs_hparam[3] = {};
u32 g_gs_vparam = 0;

// time
u64 g_t0_ns = 0;
u64 g_frame_ns = 20000000;       // PAL
u64 g_next_vbs_ns = 0;           // next vblank start
u64 g_next_vbe_ns = ~0ull;       // next vblank end
u32 g_vblanks = 0;
int g_field = 0;
std::atomic<u64> g_next_deadline{0};
std::atomic<u32> g_deadline_epoch{0};   // bumped by every publish_deadline()
std::atomic<bool> g_quit{false};
std::thread g_timer_thread;
int g_exit_code = 0;
u64 g_idle_ns = 0;               // time the scheduler spent waiting for events
u64 g_iop_ns = 0, g_spu_ns = 0;  // time in iop::run / spu2::advance (PS2_FPS only)

// PS2_FPS=1: once per second print vblanks, display flips and scheduler idle %.
// PS2_SHOT_DIR=<dir> [PS2_SHOT_EVERY=<vblanks>, default 50]: periodic PNGs.
struct DebugOutput {
    bool fps = false;
    const char* shot_dir = nullptr;
    u32 shot_every = 50;
    u32 shots = 0;
    u64 last_ns = 0, last_idle_ns = 0, last_iop_ns = 0, last_spu_ns = 0;
    u32 last_vblanks = 0, last_flips = 0;
} g_dbg;

// ---- helpers --------------------------------------------------------------
inline u32 A(EEContext* c, int n) { return c->r[4 + n].w[0]; }
inline u32 T(EEContext* c, int n) { return c->r[8 + n].w[0]; }   // t0..
inline void RET(EEContext* c, s64 v) { c->r[2].d[0] = (u64)v; }
inline u32* gp32(u32 a) { return (u32*)ee_host(a); }

u64 now_ns() { return platform::now_ns(); }

void run_iop() {
    if (!g_dbg.fps) { iop::run(); return; }
    const u64 t0 = now_ns();
    iop::run();
    g_iop_ns += now_ns() - t0;
}

void futex_wake_all(volatile u32* addr) {
    syscall(SYS_futex, (u32*)addr, FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
}

void set_guest_mxcsr() { _mm_setcsr((_mm_getcsr() & ~0x603fu) | 0xffc0u); }


void ready_insert(int id) {
    auto& t = g_threads[id];
    auto it = g_ready.begin();
    while (it != g_ready.end() && g_threads[*it].prio <= t.prio) ++it;
    g_ready.insert(it, id);
}
void ready_remove(int id) {
    auto it = std::find(g_ready.begin(), g_ready.end(), id);
    if (it != g_ready.end()) g_ready.erase(it);
}

void publish_deadline();

// ---- fibers / scheduling ----------------------------------------------------
void thread_main(void* p);

Fiber* thread_fiber(int id) {
    Thread& t = g_threads[id];
    if (t.need_start) {
        if (t.fiber && t.fiber != g_dead_fiber) fiber_destroy(t.fiber);
        t.fiber = fiber_create(FIBER_STACK, thread_main, (void*)(intptr_t)id);
        t.need_start = false;
    }
    return t.fiber;
}

void switch_to(int next) {
    if (next == g_cur) return;
    Fiber* from = g_cur ? g_threads[g_cur].fiber : &g_sched_fiber;
    LOG(LOG_THREAD, "[thread] switch %d -> %d\n", g_cur, next);
    g_cur = next;
    Fiber* to = next ? thread_fiber(next) : &g_sched_fiber;
    fiber_switch(from, to);
    // resumed
    if (g_dead_fiber && g_dead_fiber != (g_cur ? g_threads[g_cur].fiber : &g_sched_fiber)) {
        fiber_destroy(g_dead_fiber);
        g_dead_fiber = nullptr;
    }
}

// Pick the best runnable thread and switch to it (0 = idle scheduler).
void reschedule() {
    if (g_in_intr) {
        g_need_resched = true;
        return;
    }
    g_need_resched = false;
    if (g_dispatch_disabled && g_cur && g_threads[g_cur].st == TS::Ready) return;
    int next = g_ready.empty() ? 0 : g_ready.front();
    switch_to(next);
}

void deliver_interrupts(EEContext* c, u32 pc);
void process_time();

void idle_wait() {
    // Called on the scheduler fiber when nothing is runnable.
    for (;;) {
        if (g_quit.load()) return;
        u32 ev = __atomic_load_n(&g_ee_events, __ATOMIC_ACQUIRE);
        if (ev) return;
        if (iop::has_work()) return;
        publish_deadline();
        struct timespec ts = {0, 2000000};  // 2 ms cap
        const u64 t0 = now_ns();
        syscall(SYS_futex, (u32*)&g_ee_events, FUTEX_WAIT_PRIVATE, 0, &ts, nullptr, 0);
        g_idle_ns += now_ns() - t0;
    }
}

void scheduler_loop() {
    for (;;) {
        if (g_quit.load()) return;
        u32 ev = __atomic_exchange_n(&g_ee_events, 0, __ATOMIC_ACQ_REL);
        if (ev & EV_QUIT) return;
        process_time();
        hw::poll_async();
        run_iop();
        publish_deadline();   // the IOP may have started/finished DelayThread waits
        deliver_interrupts(&g_ictx, 0);
        if (!g_ready.empty()) {
            switch_to(g_ready.front());
            continue;
        }
        idle_wait();
    }
}

PS2_NORETURN void exit_current_thread(bool del);

void thread_main(void* p) {
    int id = (int)(intptr_t)p;
    Thread& t = g_threads[id];
    EEContext* c = &t.ctx;
    set_guest_mxcsr();
    ee_call(c, t.entry);
    LOG(LOG_THREAD, "[thread] %d returned from entry\n", id);
    exit_current_thread(false);
}

void reset_thread_ctx(Thread& t) {
    EEContext* c = &t.ctx;
    memset((void*)c, 0, sizeof(*c));
    c->thread = &t;
    u32 top = t.stack + t.stack_size;
    c->r[29].d[0] = SE32(top - 0x20);
    c->r[30].d[0] = SE32(top - 0x20);
    c->r[28].d[0] = SE32(t.gp);
    c->r[31].d[0] = GUEST_RET;
    c->cop0_status = 0x70030C13;  // EIE|IE|kernel-ish defaults; EIE (bit16) + IE (bit0)
    t.prio = t.init_prio;
    t.need_start = true;
}

// ---- interrupts ---------------------------------------------------------------
bool intr_enabled(EEContext* c) {
    if (g_in_intr) return false;
    if (c == &g_ictx || c == nullptr) return true;  // scheduler: always
    return (c->cop0_status & 0x10007) == 0x10001;    // EIE and IE, not EXL/ERL
}

s32 call_handler(const Handler& h, u32 cause, u32 epc) {
    EEContext* ic = &g_ictx;
    qword save[32];
    memcpy(save, ic->r, sizeof(save));  // handlers may nest only via runtime calls
    ic->r[4].d[0] = cause;
    ic->r[5].d[0] = SE32(h.arg);
    ic->r[6].d[0] = SE32(epc);
    ic->r[28].d[0] = SE32(h.gp);
    ic->r[29].d[0] = SE32(INTR_STACK_TOP - 0x100);
    ic->r[31].d[0] = GUEST_RET;
    ic->cop0_status = 0x70030C12;   // IE cleared inside handlers
    ic->intr_depth = 1;
    ee_call(ic, h.func);
    s32 r = (s32)ic->r[2].w[0];
    memcpy(ic->r, save, sizeof(save));
    return r;
}

const u32 kIntcOrder[] = {INTC_GS, INTC_SBUS, INTC_VBLANK_START, INTC_VBLANK_END, INTC_VIF0,
                          INTC_VIF1, INTC_VU0, INTC_VU1, INTC_IPU, INTC_TIMER0, INTC_TIMER1,
                          INTC_TIMER2, INTC_TIMER3, INTC_SFIFO, INTC_VU0WD};

void deliver_interrupts(EEContext* c, u32 pc) {
    if (!intr_enabled(c)) return;
    for (int guard = 0; guard < 64; ++guard) {
        u32 ip = hw::intc_pending();
        u32 dp = hw::dmac_pending();
        if (!ip && !dp) break;
        g_in_intr = true;
        if (ip) {
            for (u32 line : kIntcOrder) {
                if (!(ip & (1u << line))) continue;
                hw::g_istat &= ~(1u << line);
                LOG(LOG_INTC, "[intc] line %u\n", line);
                for (size_t k = 0; k < g_intc_order.size(); ++k) {
                    const Handler& h = g_intc_h[g_intc_order[k]];
                    if (!h.used || !h.enabled || h.cause != line) continue;
                    if (call_handler(h, line, pc) < 0) break;
                }
            }
        } else {
            for (int ch = 14; ch >= 0; --ch) {
                if (!(dp & (1u << ch))) continue;
                hw::g_dstat &= ~(1u << ch);
                LOG(LOG_INTC, "[intc] dmac ch %d\n", ch);
                for (size_t k = 0; k < g_dmac_order.size(); ++k) {
                    const Handler& h = g_dmac_h[g_dmac_order[k]];
                    if (!h.used || !h.enabled || h.cause != (u32)ch) continue;
                    if (call_handler(h, (u32)ch, pc) < 0) break;
                }
            }
        }
        g_in_intr = false;
    }
    if (g_need_resched) reschedule();
}

// ---- time -------------------------------------------------------------------------
// Produce the SPU2 output the host clock owes (spu2::frames_due), steering the
// clock +-5% of a video frame to keep the device queue near ~3 frames of
// latency.  The SPU2 runs in small steps with the IOP serviced in between so
// the sound driver can refill AutoDMA buffers / react to IRQs on time.
void audio_pump() {
    const u32 per_frame = g_cfg.pal ? 960 : 800;
    const u32 target = per_frame * 3;
    const u32 queued = platform::audio_queued_frames();
    if (queued < target) spu2::nudge((s32)(per_frame / 20));
    else if (queued > target * 2) spu2::nudge(-(s32)(per_frame / 20));
    for (int guard = 0; guard < 256; ++guard) {
        const u32 due = spu2::frames_due();
        if (!due) break;
        const u64 t0 = g_dbg.fps ? now_ns() : 0;
        spu2::advance(std::min<u32>(64, due));
        if (g_dbg.fps) g_spu_ns += now_ns() - t0;
        run_iop();
    }
}

void debug_vblank() {
    if (std::string control_path; platform::take_control_shot(control_path))
        if (!gs::screenshot(control_path.c_str())) LOG(LOG_GS, "[dbg] screenshot %s failed\n", control_path.c_str());
    for (const std::string& cmd : platform::take_control_debug()) {
        if (cmd == "watchreport") debug_watch_report();
        else if (cmd.rfind("watch ", 0) == 0) debug_watch(static_cast<u32>(strtoul(cmd.c_str() + 6, nullptr, 16)));
    }
    // "dump <path>": 32 MiB EE RAM then 16 KiB scratchpad, written atomically.
    if (std::string dump_path; platform::take_control_dump(dump_path)) {
        const std::string tmp = dump_path + ".tmp";
        bool ok = false;
        if (FILE* f = fopen(tmp.c_str(), "wb")) {
            ok = fwrite(EE_BASE, 1, EE_RAM_SIZE, f) == EE_RAM_SIZE &&
                 fwrite(EE_BASE + EE_SPR_ADDR, 1, EE_SPR_SIZE, f) == EE_SPR_SIZE;
            ok = (fclose(f) == 0) && ok;
        }
        if (!ok || rename(tmp.c_str(), dump_path.c_str()) != 0)
            fprintf(stderr, "[control] dump %s failed\n", dump_path.c_str());
    }
    if (g_dbg.shot_dir && g_vblanks % g_dbg.shot_every == 0) {
        char path[512];
        snprintf(path, sizeof(path), "%s/shot_%05u.png", g_dbg.shot_dir, g_dbg.shots++);
        if (!gs::screenshot(path)) LOG(LOG_GS, "[dbg] screenshot %s failed\n", path);
    }
    if (!g_dbg.fps) return;
    {
        // [hitch]: a game frame that took more than 2 vblanks, or a vblank
        // that reached us late on the host clock (the game itself slowed).
        static u32 hl_flips = 0, hl_vblank = 0;
        static u64 hl_ns = 0, hv_ns = 0;
        const u64 t = now_ns();
        const u32 f = gs::flip_count();
        if (hv_ns && t - hv_ns > 30000000ull)
            fprintf(stderr, "[hitch] vblank %u arrived %.1f ms after the previous one\n",
                    g_vblanks, (double)(t - hv_ns) * 1e-6);
        hv_ns = t;
        if (f != hl_flips) {
            if (hl_ns && g_vblanks - hl_vblank > 2)
                fprintf(stderr, "[hitch] vblank %u: frame took %u vblanks (%.1f ms)\n",
                        g_vblanks, g_vblanks - hl_vblank, (double)(t - hl_ns) * 1e-6);
            hl_flips = f; hl_vblank = g_vblanks; hl_ns = t;
        }
    }
    if (g_vblanks % (g_cfg.pal ? 50 : 60) != 0) return;
    const u64 now = now_ns();
    const u32 flips = gs::flip_count();
    if (g_dbg.last_ns) {
        const double secs = (double)(now - g_dbg.last_ns) * 1e-9;
        fprintf(stderr, "[fps] %.1f vblank/s  %.1f flips/s  idle %.0f%%  iop %.0f%%  spu2 %.0f%%\n",
                (g_vblanks - g_dbg.last_vblanks) / secs, (flips - g_dbg.last_flips) / secs,
                100.0 * (double)(g_idle_ns - g_dbg.last_idle_ns) * 1e-9 / secs,
                100.0 * (double)(g_iop_ns - g_dbg.last_iop_ns) * 1e-9 / secs,
                100.0 * (double)(g_spu_ns - g_dbg.last_spu_ns) * 1e-9 / secs);
        // Ring waits (% of the second): EE blocked on the VU1 / GS rings, VU1
        // blocked on PATH1, and the consumers' idle time.
        static u64 last[6];
        u64 cur[6];
        vu1_ring_waits(cur[0], cur[1]);
        u64 gsw[4];
        gs::ring_waits(gsw);
        cur[2] = gsw[0]; cur[3] = gsw[1]; cur[4] = gsw[2]; cur[5] = gsw[3];
        auto pct = [&](int i) { return 100.0 * (double)(cur[i] - last[i]) * 1e-9 / secs; };
        fprintf(stderr, "[wait] EE->VU1 full %.0f%%  VU1 idle %.0f%%  EE->GS full %.0f%%  GS idle %.0f%%  "
                        "VU1->PATH1 full %.0f%%  GS wait PATH1 %.0f%%\n",
                pct(0), pct(1), pct(2), pct(3), pct(4), pct(5));
        std::memcpy(last, cur, sizeof(last));
        static u64 last_wall[4], last_cpu[4];
        u64 wall[4], cpu[4];
        gs::op_times(wall, cpu);
        auto ms = [&](u64 a, u64 b) { return 100.0 * (double)(a - b) * 1e-9 / secs; };
        if (gs::op_timing_enabled())
            fprintf(stderr, "[gs] wall/cpu %%: draw %.0f/%.0f  present %.0f/%.0f  fifo %.0f/%.0f  other %.0f/%.0f\n",
                ms(wall[0], last_wall[0]), ms(cpu[0], last_cpu[0]), ms(wall[1], last_wall[1]), ms(cpu[1], last_cpu[1]),
                ms(wall[2], last_wall[2]), ms(cpu[2], last_cpu[2]), ms(wall[3], last_wall[3]), ms(cpu[3], last_cpu[3]));
        std::memcpy(last_wall, wall, sizeof(wall));
        std::memcpy(last_cpu, cpu, sizeof(cpu));
    }
    g_dbg.last_ns = now;
    g_dbg.last_flips = flips;
    g_dbg.last_vblanks = g_vblanks;
    g_dbg.last_idle_ns = g_idle_ns;
    g_dbg.last_iop_ns = g_iop_ns;
    g_dbg.last_spu_ns = g_spu_ns;
}

// Returns the host time the EE spent blocked in gs::vsync on GS/VU1 backpressure.
u64 vblank_start() {
    ++g_vblanks;
    g_field ^= 1;
    const u64 t0 = now_ns();
    gs::vsync(g_field);   // the GS adapter switches MXCSR itself
    const u64 stall = now_ns() - t0;
    if (!platform::poll_events()) request_quit();
    hw::gs_vblank_start();
    if (g_vsync_flag_addr) {
        *gp32(g_vsync_flag_addr) = 1;
        u64 csr = hw::gs_read_csr();
        memcpy(ee_host(g_vsync_csr_addr), &csr, 8);
    }
    intc_raise(INTC_VBLANK_START);
    iop::vblank(true);
    audio_pump();
    debug_vblank();
    return stall;
}

void vblank_end() {
    intc_raise(INTC_VBLANK_END);
    iop::vblank(false);
}

void process_alarms(u64 now) {
    for (int i = 0; i < MAX_ALARMS; ++i) {
        Alarm& a = g_alarms[i];
        if (!a.used || a.due_ns > now) continue;
        a.used = false;
        Handler h;
        h.func = a.func;
        h.arg = a.arg;
        h.gp = a.gp;
        bool was = g_in_intr;
        g_in_intr = true;
        EEContext* ic = &g_ictx;
        ic->r[4].d[0] = (u64)(i + 1);
        ic->r[5].d[0] = a.time;
        ic->r[6].d[0] = SE32(a.arg);
        ic->r[28].d[0] = SE32(a.gp);
        ic->r[29].d[0] = SE32(INTR_STACK_TOP - 0x100);
        ic->r[31].d[0] = GUEST_RET;
        ic->intr_depth = 1;
        LOG(LOG_INTC, "[alarm] %d fires -> %08x\n", i + 1, a.func);
        ee_call(ic, a.func);
        g_in_intr = was;
    }
}

void process_time() {
    u64 now = now_ns();
    int burst = 0;
    while (now >= g_next_vbs_ns && burst < 4) {
        // Time blocked on GS/VU1 backpressure is not guest time: the real EE
        // keeps computing while VU1/GS work through a heavy frame.  Without this
        // the host-clocked vblanks fall due again at once and starve EE threads
        // (e.g. the level loader's inflate behind the VU1 loading animation).
        const u64 stall = vblank_start();
        if (stall > 1000000) {
            g_next_vbs_ns += stall;
            now = now_ns();
        }
        g_next_vbe_ns = g_next_vbs_ns + 1600000;
        g_next_vbs_ns += g_frame_ns;
        ++burst;
        if (now >= g_next_vbe_ns) {
            vblank_end();
            g_next_vbe_ns = ~0ull;
        }
    }
    if (now >= g_next_vbs_ns) g_next_vbs_ns = now + g_frame_ns;  // fell far behind: resync
    if (now >= g_next_vbe_ns) {
        vblank_end();
        g_next_vbe_ns = ~0ull;
    }
    process_alarms(now);
    hw::timers_service(now);
    publish_deadline();
}

void publish_deadline() {
    u64 d = std::min(g_next_vbs_ns, g_next_vbe_ns);
    for (const auto& a : g_alarms)
        if (a.used) d = std::min(d, a.due_ns);
    d = std::min(d, hw::timers_next_deadline_ns());
    d = std::min(d, iop::next_deadline_ns());
    g_next_deadline.store(d, std::memory_order_release);
    g_deadline_epoch.fetch_add(1, std::memory_order_release);
}

void timer_thread_main() {
    while (!g_quit.load()) {
        u64 now = now_ns();
        const u32 epoch = g_deadline_epoch.load(std::memory_order_acquire);
        u64 d = g_next_deadline.load(std::memory_order_acquire);
        if (now >= d) {
            __atomic_or_fetch(&g_ee_events, EV_TIMER, __ATOMIC_RELEASE);
            futex_wake_all(&g_ee_events);
            // Wait until the emulation thread has consumed the event and
            // republished.  The epoch (not the value) is compared: a deadline
            // can legitimately be republished unchanged, e.g. an IOP thread
            // whose DelayThread expired is only woken by the following iop::run.
            for (int i = 0; i < 2000 && g_deadline_epoch.load(std::memory_order_acquire) == epoch &&
                            !g_quit.load(); ++i)
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            continue;
        }
        u64 wait = std::min<u64>(d - now, 1000000);
        std::this_thread::sleep_for(std::chrono::nanoseconds(wait));
    }
}

// ---- thread operations ---------------------------------------------------------
int alloc_thread() {
    for (int i = 1; i < MAX_THREADS; ++i)
        if (g_threads[i].st == TS::Free) return i;
    return -1;
}

bool valid_thread(u32 id) { return id > 0 && id < MAX_THREADS && g_threads[id].st != TS::Free; }

void make_ready(int id) {
    Thread& t = g_threads[id];
    t.st = TS::Ready;
    ready_insert(id);
    if (g_cur == 0 || t.prio < g_threads[g_cur].prio || g_threads[g_cur].st != TS::Ready)
        g_need_resched = true;
}

// Block the current thread (already marked with a wait state) and run others.
void block_current() {
    ready_remove(g_cur);
    reschedule();
}

void wake_from_wait(int id, s32 result) {
    Thread& t = g_threads[id];
    t.wait_result = result;
    if (t.st == TS::Sleep || t.st == TS::WaitSema) {
        make_ready(id);
    } else if (t.st == TS::SuspSleep || t.st == TS::SuspWaitSema) {
        t.st = TS::Suspended;
    }
    t.wait_sema = -1;
}

void exit_current_thread(bool del) {
    Thread& t = g_threads[g_cur];
    int id = g_cur;
    LOG(LOG_THREAD, "[thread] exit %d\n", id);
    ready_remove(id);
    t.st = del ? TS::Free : TS::Dormant;
    if (!del) t.prio = t.init_prio;
    g_dead_fiber = t.fiber;
    t.fiber = nullptr;
    t.need_start = !del;
    if (!del) reset_thread_ctx(t);
    int next = g_ready.empty() ? 0 : g_ready.front();
    // switch away for good
    Fiber* to = next ? thread_fiber(next) : &g_sched_fiber;
    g_cur = next;
    Fiber dummy;
    fiber_switch(&dummy, to);
    __builtin_unreachable();
}

void refer_thread(u32 id, u32 addr) {
    Thread& t = g_threads[id];
    u32 st = 0, wt = 0;
    switch (t.st) {
    case TS::Ready: st = ((int)id == g_cur) ? 0x01 : 0x02; break;
    case TS::Sleep: st = 0x04; wt = 1; break;
    case TS::WaitSema: st = 0x04; wt = 2; break;
    case TS::Suspended: st = 0x08; break;
    case TS::SuspSleep: st = 0x0C; wt = 1; break;
    case TS::SuspWaitSema: st = 0x0C; wt = 2; break;
    case TS::Dormant: st = 0x10; break;
    default: break;
    }
    if (addr) {
        u32* p = gp32(addr);
        p[0] = st;
        p[1] = t.entry;
        p[2] = t.stack;
        p[3] = t.stack_size;
        p[4] = t.gp;
        p[5] = (u32)t.init_prio;
        p[6] = (u32)t.prio;
        p[7] = t.attr;
        p[8] = t.option;
        p[9] = wt;
        p[10] = wt == 2 ? (u32)t.wait_sema : 0;
        p[11] = (u32)t.wakeup;
    }
}

// ---- handlers ------------------------------------------------------------------------
s32 add_handler(Handler* tab, std::vector<int>& order, u32 cause, u32 func, s32 next, u32 arg, u32 gp) {
    int id = -1;
    for (int i = 1; i < MAX_HANDLERS; ++i)
        if (!tab[i].used) { id = i; break; }
    if (id < 0) return -1;
    tab[id] = Handler{true, true, cause, func, arg, gp};
    if (next == 0) order.insert(order.begin(), id);
    else if (next == -1) order.push_back(id);
    else {
        auto it = std::find(order.begin(), order.end(), next);
        order.insert(it, id);
    }
    return id;
}
s32 remove_handler(Handler* tab, std::vector<int>& order, u32 id) {
    if (id == 0 || id >= (u32)MAX_HANDLERS || !tab[id].used) return -1;
    tab[id].used = false;
    order.erase(std::remove(order.begin(), order.end(), (int)id), order.end());
    return 0;
}

// ---- syscalls ---------------------------------------------------------------------
void sys_setup_thread(EEContext* c) {
    u32 gp = A(c, 0), stack = A(c, 1), stack_size = A(c, 2), args = A(c, 3);
    (void)gp;
    u32 stack_top = (stack == 0xFFFFFFFFu) ? (EE_RAM_SIZE - 4096) : stack + stack_size;
    // argc/argv
    u32 argc = (u32)g_cfg.argv.size();
    *gp32(args) = argc;
    u32 ptrs = args + 4;
    u32 payload = ptrs + (argc + 1) * 4;
    for (u32 i = 0; i < argc; ++i) {
        *gp32(ptrs + i * 4) = payload;
        const auto& s = g_cfg.argv[i];
        memcpy(ee_host(payload), s.c_str(), s.size() + 1);
        payload += (u32)s.size() + 1;
    }
    *gp32(ptrs + argc * 4) = 0;
    Thread& t = g_threads[g_cur];
    t.stack = stack_top - stack_size;
    t.stack_size = stack_size;
    t.gp = gp;
    RET(c, (s32)(stack_top - STACKRES));
}

s32 sys_create_thread(u32 param) {
    const u32* p = gp32(param);
    int id = alloc_thread();
    if (id < 0) return -1;
    Thread& t = g_threads[id];
    t = Thread{};
    t.st = TS::Dormant;
    t.entry = p[1];
    t.stack = p[2];
    t.stack_size = p[3];
    t.gp = p[4];
    t.init_prio = (s32)p[5];
    t.attr = p[7];
    t.option = p[8];
    reset_thread_ctx(t);
    LOG(LOG_THREAD, "[thread] create %d entry=%08x stack=%08x+%x prio=%d\n", id, t.entry, t.stack,
        t.stack_size, t.init_prio);
    return id;
}

s32 sys_start_thread(u32 id, u32 arg) {
    if (!valid_thread(id) || g_threads[id].st != TS::Dormant) return -1;
    Thread& t = g_threads[id];
    reset_thread_ctx(t);
    t.ctx.r[4].d[0] = SE32(arg);
    t.start_arg = arg;
    t.wakeup = 0;
    make_ready((int)id);
    LOG(LOG_THREAD, "[thread] start %d arg=%08x\n", id, arg);
    return (s32)id;
}

s32 sys_terminate_thread(u32 id) {
    if (!valid_thread(id) || (int)id == g_cur) return -1;
    Thread& t = g_threads[id];
    if (t.st == TS::Dormant) return -1;
    if (t.st == TS::WaitSema || t.st == TS::SuspWaitSema) {
        if (t.wait_sema >= 0) {
            auto& w = g_semas[t.wait_sema].waiters;
            w.erase(std::remove(w.begin(), w.end(), (int)id), w.end());
        }
    }
    ready_remove((int)id);
    t.st = TS::Dormant;
    if (t.fiber) {
        fiber_destroy(t.fiber);
        t.fiber = nullptr;
    }
    reset_thread_ctx(t);
    return (s32)id;
}

s32 sys_change_prio(u32 id, s32 prio) {
    if (id == 0) id = (u32)g_cur;
    if (!valid_thread(id)) return -1;
    Thread& t = g_threads[id];
    s32 old = t.prio;
    t.prio = prio;
    if (t.st == TS::Ready) {
        ready_remove((int)id);
        ready_insert((int)id);
    }
    g_need_resched = true;
    return old;
}

s32 sys_rotate(s32 prio) {
    if (prio == 0x80 && g_cur) prio = g_threads[g_cur].prio;  // TH_SELF-ish
    for (size_t i = 0; i < g_ready.size(); ++i) {
        int id = g_ready[i];
        if (g_threads[id].prio == prio) {
            g_ready.erase(g_ready.begin() + (long)i);
            ready_insert(id);
            break;
        }
    }
    g_need_resched = true;
    return prio;
}

s32 sys_release_wait(u32 id) {
    if (!valid_thread(id)) return -1;
    Thread& t = g_threads[id];
    if (t.st == TS::WaitSema || t.st == TS::SuspWaitSema) {
        auto& w = g_semas[t.wait_sema].waiters;
        w.erase(std::remove(w.begin(), w.end(), (int)id), w.end());
        wake_from_wait((int)id, -1);
    } else if (t.st == TS::Sleep || t.st == TS::SuspSleep) {
        wake_from_wait((int)id, -1);
    } else {
        return -1;
    }
    return (s32)id;
}

s32 sys_wakeup(u32 id) {
    if (id == 0) id = (u32)g_cur;
    if (!valid_thread(id)) return -1;
    Thread& t = g_threads[id];
    if (t.st == TS::Sleep || t.st == TS::SuspSleep) wake_from_wait((int)id, 0);
    else if (t.st != TS::Dormant) t.wakeup++;
    else return -1;
    return (s32)id;
}

s32 sys_suspend(u32 id) {
    if (!valid_thread(id) || (int)id == g_cur) return -1;
    Thread& t = g_threads[id];
    switch (t.st) {
    case TS::Ready: ready_remove((int)id); t.st = TS::Suspended; break;
    case TS::Sleep: t.st = TS::SuspSleep; break;
    case TS::WaitSema: t.st = TS::SuspWaitSema; break;
    default: return -1;
    }
    return (s32)id;
}

s32 sys_resume(u32 id) {
    if (!valid_thread(id)) return -1;
    Thread& t = g_threads[id];
    switch (t.st) {
    case TS::Suspended: make_ready((int)id); break;
    case TS::SuspSleep: t.st = TS::Sleep; break;
    case TS::SuspWaitSema: t.st = TS::WaitSema; break;
    default: return -1;
    }
    return (s32)id;
}

s32 sys_create_sema(u32 param) {
    const s32* p = (const s32*)gp32(param);
    for (int i = 1; i < MAX_SEMAS; ++i) {
        if (g_semas[i].used) continue;
        Sema& s = g_semas[i];
        s = Sema{};
        s.used = true;
        s.count = p[2];
        s.init_count = p[2];
        s.max_count = p[1];
        s.attr = (u32)p[4];
        s.option = (u32)p[5];
        LOG(LOG_THREAD, "[sema] create %d count=%d max=%d\n", i, s.count, s.max_count);
        return i;
    }
    return -1;
}

bool valid_sema(u32 id) { return id > 0 && id < MAX_SEMAS && g_semas[id].used; }

s32 sys_signal_sema(u32 id) {
    if (!valid_sema(id)) return -1;
    Sema& s = g_semas[id];
    if (!s.waiters.empty()) {
        int tid = s.waiters.front();
        s.waiters.erase(s.waiters.begin());
        wake_from_wait(tid, (s32)id);
    } else {
        s.count++;
    }
    return (s32)id;
}

s32 sys_delete_sema(u32 id) {
    if (!valid_sema(id)) return -1;
    Sema& s = g_semas[id];
    for (int tid : s.waiters) wake_from_wait(tid, -1);
    s.waiters.clear();
    s.used = false;
    g_need_resched = true;
    return (s32)id;
}

s32 sys_wait_sema(u32 id) {
    if (!valid_sema(id)) return -1;
    Sema& s = g_semas[id];
    if (s.count > 0) {
        s.count--;
        return (s32)id;
    }
    if (!g_cur) FATAL("WaitSema outside a thread");
    Thread& t = g_threads[g_cur];
    t.st = TS::WaitSema;
    t.wait_sema = (s32)id;
    t.wait_result = (s32)id;
    s.waiters.push_back(g_cur);
    block_current();
    return t.wait_result;
}

void refer_sema(u32 id, u32 addr) {
    Sema& s = g_semas[id];
    s32* p = (s32*)gp32(addr);
    p[0] = s.count;
    p[1] = s.max_count;
    p[2] = s.init_count;
    p[3] = (s32)s.waiters.size();
    p[4] = (s32)s.attr;
    p[5] = (s32)s.option;
}

s32 sys_set_alarm(u32 time, u32 func, u32 arg, u32 gp) {
    for (int i = 0; i < MAX_ALARMS; ++i) {
        if (g_alarms[i].used) continue;
        Alarm& a = g_alarms[i];
        a.used = true;
        u64 hsync_ns = g_cfg.pal ? 64000 : 63556;
        a.due_ns = now_ns() + (u64)time * hsync_ns;
        a.func = func;
        a.arg = arg;
        a.gp = gp;
        a.time = (u16)time;
        publish_deadline();
        return i + 1;
    }
    return -1;
}

s32 sys_release_alarm(u32 id) {
    if (id == 0 || id > (u32)MAX_ALARMS || !g_alarms[id - 1].used) return -1;
    g_alarms[id - 1].used = false;
    return 0;
}

void set_gs_crt(u32 interlace, u32 mode, u32 ffmd) {
    // SMODE1 values the BIOS programs (CMOD=3 PAL, 2 NTSC)
    u64 smode1 = (mode == 3) ? 0x740836504ull : 0x740834504ull;
    u64 smode2 = (interlace ? 1 : 0) | (ffmd ? 2 : 0);
    gs::write_priv64(0x0010, smode1);
    gs::write_priv64(0x0020, smode2);
    g_cfg.pal = (mode == 3);
    g_frame_ns = g_cfg.pal ? 20000000 : 16683333;
    LOG(LOG_SYS, "[sys] SetGsCrt interlace=%u mode=%u ffmd=%u\n", interlace, mode, ffmd);
}

// DECI2 (debugger channel) as libkernel's TTY uses it: Open/Send/Poll/kPuts.
// Layouts follow Play!: handler buffer {u32 ?, s32 status0, s32 ?, u32 status1, u32 dataAddr},
// send block {u32 size, u32 [2], data...}.
struct Deci2Handler {
    bool used = false;
    u32 device = 0, buffer = 0;
};
Deci2Handler g_deci2[16];

void deci2_print(const char* s, u32 n) {
    std::string text(s, strnlen(s, n));
    if (!text.empty()) LOG(LOG_TTY, "%s", text.c_str());
}

s32 sys_deci2(u32 fn, u32 param) {
    switch (fn) {
    case 0x01: {  // Open
        for (int i = 1; i < 16; ++i) {
            if (g_deci2[i].used) continue;
            g_deci2[i] = Deci2Handler{true, gp32(param)[0], gp32(param)[1]};
            return i;
        }
        return -1;
    }
    case 0x02: {  // Close
        u32 id = gp32(param)[0];
        if (id < 16) g_deci2[id].used = false;
        return 1;
    }
    case 0x03: {  // Send
        u32 id = gp32(param)[0];
        if (id >= 16 || !g_deci2[id].used) return -1;
        u32* buf = gp32(g_deci2[id].buffer);
        u32 data = buf[4];
        if (data) {
            u32 size = *gp32(data);
            if (size >= 0x0C) deci2_print(ee_cstr(data + 0x0C), size - 0x0C);
            buf[1] = 0;
        } else {
            buf[1] = (u32)-1;
        }
        return 1;
    }
    case 0x04: {  // Poll
        u32 id = gp32(param)[0];
        if (id < 16 && g_deci2[id].used) gp32(g_deci2[id].buffer)[3] = 0;
        return 1;
    }
    case 0x10:  // kPuts
        deci2_print(ee_cstr(*gp32(param)), 0x1000);
        return 1;
    default:
        LOG(LOG_SYS, "[sys] Deci2Call fn %x unsupported\n", fn);
        return -1;
    }
}

void do_exit(int code) {
    g_exit_code = code;
    request_quit();
}

} // namespace

// ============================================================================
void post_events(u32 bits) {
    __atomic_or_fetch(&g_ee_events, bits, __ATOMIC_RELEASE);
    futex_wake_all(&g_ee_events);
}

void request_quit() {
    if (g_quit.exchange(true)) return;
    post_events(EV_QUIT);
    // A guest thread spinning in a polling loop (e.g. waiting for VU1) may not
    // reach a scheduler checkpoint; never leave the window/Ctrl-C unanswered.
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::fprintf(stderr, "[sys] quit did not unwind in 2 s; exiting\n");
        // _exit, not std::exit, and no stdio flush: the main thread may already
        // be inside exit(), waiting on a stdio lock that the PS2_CONTROL reader
        // holds while blocked in fgets.  Memcard writes are fsync'ed already.
        profiler_stop_and_report();
        _exit(0);
    }).detach();
}

u64 cycles() { return (now_ns() - g_t0_ns) * 294912ull / 1000000ull; }
u32 vblank_count() { return g_vblanks; }

void debug_dump() {
    static const char* const names[] = {"free", "ready", "sleep", "wait-sema", "suspended", "susp-sleep",
                                        "susp-wait", "dormant"};
    fprintf(stderr, "--- EE threads (current %d, in interrupt %d, ready queue:", g_cur, (int)g_in_intr);
    for (int id : g_ready) fprintf(stderr, " %d", id);
    fprintf(stderr, ") ---\n");
    for (int i = 1; i < MAX_THREADS; ++i) {
        const Thread& t = g_threads[i];
        if (t.st == TS::Free) continue;
        fprintf(stderr, "  t%-3d prio %3d %-10s entry %08x ra %08x wakeups %d", i, t.prio,
                names[static_cast<int>(t.st)], t.entry, t.ctx.r[31].w[0], t.wakeup);
        if (t.wait_sema >= 0)
            fprintf(stderr, " sema %d (count %d, %zu waiting)", t.wait_sema, g_semas[t.wait_sema].count,
                    g_semas[t.wait_sema].waiters.size());
        fprintf(stderr, "\n");
    }
}

// The guest is provably just waiting (a side-effect-free polling loop, or a
// burst of COP0 Count reads): give the IOP its turn, or sleep until the next
// event/deadline (at most 250 us) instead of burning the host core.  Count and
// the timers follow host time, so the guest loop only observes time passing.
void guest_waiting() {
    if (g_in_intr) return;   // an interrupt handler must finish promptly
    gs::flush();             // the GS thread can work while the guest waits
    if (__atomic_load_n(&g_ee_events, __ATOMIC_ACQUIRE)) return;
    const u64 now = now_ns();
    if (iop::has_work()) {
        run_iop();
        publish_deadline();
        return;
    }
    const u64 deadline = g_next_deadline.load(std::memory_order_acquire);
    if (deadline <= now) return;
    struct timespec ts = {0, (long)std::min<u64>(deadline - now, 250'000)};
    syscall(SYS_futex, (u32*)&g_ee_events, FUTEX_WAIT_PRIVATE, 0, &ts, nullptr, 0);
    g_idle_ns += now_ns() - now;
}

void count_poll() {
    static u64 window_start = 0;
    static u32 reads = 0;
    if (++reads < 512) return;
    const u64 now = now_ns();
    const u64 elapsed = now - window_start;
    window_start = now;
    reads = 0;
    if (elapsed <= 1'000'000) guest_waiting();   // >= 512 reads/ms: spinning, not working
}

void interrupt_state_changed() {
    if (hw::intc_pending() || hw::dmac_pending()) __atomic_or_fetch(&g_ee_events, EV_INTR, __ATOMIC_RELEASE);
}

u32 call_guest(u32 func, u32 gp, u32 a0, u32 a1, u32 a2, u32 a3) {
    EEContext* ic = &g_ictx;
    qword save[32];
    memcpy(save, ic->r, sizeof(save));
    ic->r[4].d[0] = SE32(a0);
    ic->r[5].d[0] = SE32(a1);
    ic->r[6].d[0] = SE32(a2);
    ic->r[7].d[0] = SE32(a3);
    ic->r[28].d[0] = SE32(gp);
    ic->r[29].d[0] = SE32(INTR_STACK_TOP - 0x2000);
    ic->r[31].d[0] = GUEST_RET;
    ee_call(ic, func);
    u32 r = ic->r[2].w[0];
    memcpy(ic->r, save, sizeof(save));
    return r;
}

void init(const Config& cfg) {
    g_cfg = cfg;
    g_frame_ns = cfg.pal ? 20000000 : 16683333;
    for (u32 n = 0; n < 0x100; ++n) syscall_table()[n] = syscall_marker(n);
    // OSDCONFIGPARAM: jpLanguage bit 4, version bits 13-15 (1 = V2: libscf reads
    // the language field bits 16-20; with version 0 it only sees jpLanguage),
    // screen type (bits 1-2: 0 = 4:3, 2 = 16:9), RGB.  The game reads the
    // screen type at boot and switches its own projection to 16:9.
    g_osd_config = ((u32)cfg.language & 0x1F) << 16 | 1u << 13 |
                   (cfg.language != 0 ? 1u << 4 : 0u) |
                   (cfg.widescreen ? 2u << 1 : 0u);
    g_osd_config2[0] = 0;
    ::g_ee_camera_fov_scale = cfg.widescreen ? 4.0f / 3.0f : 1.0f;
    memset((void*)&g_ictx, 0, sizeof(g_ictx));
    g_ictx.cop0_status = 0x70030C13;
    if (const char* e = getenv("PS2_FPS")) g_dbg.fps = *e && *e != '0';
    if (const char* e = getenv("PS2_SHOT_DIR"); e && *e) g_dbg.shot_dir = e;
    if (const char* e = getenv("PS2_SHOT_EVERY")) g_dbg.shot_every = std::max(1u, (u32)strtoul(e, nullptr, 10));
}

int run(u32 entry) {
    set_guest_mxcsr();
    g_t0_ns = now_ns();
    g_next_vbs_ns = g_t0_ns + g_frame_ns;
    publish_deadline();
    g_timer_thread = std::thread(timer_thread_main);

    // Boot thread: id 1, priority 0 (crt0 calls SetupThread which re-uses it).
    Thread& t = g_threads[1];
    t = Thread{};
    t.st = TS::Ready;
    t.entry = entry;
    t.init_prio = t.prio = 0;
    t.stack = 0x01F00000;
    t.stack_size = 0x00010000;
    t.gp = 0;
    reset_thread_ctx(t);
    ready_insert(1);
    switch_to(1);
    scheduler_loop();
    g_quit.store(true);
    if (g_timer_thread.joinable()) g_timer_thread.join();
    return g_exit_code;
}

// ---- syscall dispatch --------------------------------------------------------------
void syscall_dispatch(EEContext* c, u32 pc) {
    s32 num = (s32)c->r[3].w[0];
    bool is_i = num < 0;
    u32 n = (u32)(is_i ? -num : num);
    LOG(LOG_SYS, "[sys] %s%02x(%08x, %08x, %08x, %08x) pc=%08x thread=%d\n", is_i ? "i" : "", n,
        A(c, 0), A(c, 1), A(c, 2), A(c, 3), pc, g_cur);
    if (n < 0x100) {
        // Kernel patches installed by libkernel (SetSyscall or direct writes
        // into the table) run as guest code, except the alarm syscalls which
        // stay native because their guest versions poke kernel internals.
        u32 h = syscall_table()[n];
        bool native_only = n >= 0xFC;
        if (h != syscall_marker(n) && !native_only && ee_lookup(h)) {
            ee_call(c, h);
            return;
        }
    }
    switch (n) {
    case 0x01: RET(c, 0); break;                                         // ResetEE
    case 0x02: set_gs_crt(A(c, 0), A(c, 1), A(c, 2)); RET(c, 0); break;  // SetGsCrt
    case 0x04: do_exit((s32)A(c, 0)); RET(c, 0); break;                  // Exit
    case 0x06:                                                           // LoadExecPS2
    case 0x07:                                                           // ExecPS2
        LOG(LOG_SYS, "[sys] LoadExec/ExecPS2 '%s' -> exiting\n", ee_cstr(A(c, 0)));
        do_exit(0);
        RET(c, 0);
        break;
    case 0x10:  // AddIntcHandler(cause, handler, next, arg)
        RET(c, add_handler(g_intc_h, g_intc_order, A(c, 0), A(c, 1), (s32)A(c, 2), A(c, 3), c->r[28].w[0]));
        break;
    case 0x11: RET(c, remove_handler(g_intc_h, g_intc_order, A(c, 1))); break;
    case 0x12:  // AddDmacHandler
        RET(c, add_handler(g_dmac_h, g_dmac_order, A(c, 0), A(c, 1), (s32)A(c, 2), A(c, 3), c->r[28].w[0]));
        break;
    case 0x13: RET(c, remove_handler(g_dmac_h, g_dmac_order, A(c, 1))); break;
    case 0x14: case 0x1A: {  // (i)EnableIntc
        u32 bit = 1u << (A(c, 0) & 31);
        s32 r = (hw::g_imask & bit) ? 0 : 1;
        hw::g_imask |= bit;
        RET(c, r);
        interrupt_state_changed();
        break;
    }
    case 0x15: case 0x1B: {  // (i)DisableIntc
        u32 bit = 1u << (A(c, 0) & 31);
        s32 r = (hw::g_imask & bit) ? 1 : 0;
        hw::g_imask &= ~bit;
        RET(c, r);
        break;
    }
    case 0x16: case 0x1C: {  // (i)EnableDmac
        u32 bit = 1u << (16 + (A(c, 0) & 15));
        s32 r = (hw::g_dstat & bit) ? 0 : 1;
        hw::g_dstat |= bit;
        RET(c, r);
        interrupt_state_changed();
        break;
    }
    case 0x17: case 0x1D: {  // (i)DisableDmac
        u32 bit = 1u << (16 + (A(c, 0) & 15));
        s32 r = (hw::g_dstat & bit) ? 1 : 0;
        hw::g_dstat &= ~bit;
        RET(c, r);
        break;
    }
    case 0x18: case 0x1E: case 0xFC: case 0xFD:  // (i)SetAlarm(time, handler, arg)
        RET(c, sys_set_alarm(A(c, 0) & 0xFFFF, A(c, 1), A(c, 2), c->r[28].w[0]));
        break;
    case 0x19: case 0x1F: case 0xFE: case 0xFF: RET(c, sys_release_alarm(A(c, 0))); break;
    case 0x20: RET(c, sys_create_thread(A(c, 0))); break;
    case 0x21: {  // DeleteThread
        u32 id = A(c, 0);
        if (!valid_thread(id) || (int)id == g_cur || g_threads[id].st != TS::Dormant) { RET(c, -1); break; }
        if (g_threads[id].fiber) fiber_destroy(g_threads[id].fiber);
        g_threads[id] = Thread{};
        RET(c, (s32)id);
        break;
    }
    case 0x22: RET(c, sys_start_thread(A(c, 0), A(c, 1))); if (!is_i) reschedule(); break;
    case 0x23: exit_current_thread(false);
    case 0x24: exit_current_thread(true);
    case 0x25: case 0x26: RET(c, sys_terminate_thread(A(c, 0))); break;
    case 0x27: g_dispatch_disabled = true; RET(c, 0); break;
    case 0x28: g_dispatch_disabled = false; RET(c, 0); if (!is_i) reschedule(); break;
    case 0x29: case 0x2A: RET(c, sys_change_prio(A(c, 0), (s32)A(c, 1))); if (!is_i) reschedule(); break;
    case 0x2B: case 0x2C: RET(c, sys_rotate((s32)A(c, 0))); if (!is_i) reschedule(); break;
    case 0x2D: case 0x2E: RET(c, sys_release_wait(A(c, 0))); if (!is_i) reschedule(); break;
    case 0x2F: RET(c, g_cur); break;  // GetThreadId / iGetThreadId
    case 0x30: case 0x31: {
        u32 id = A(c, 0) ? A(c, 0) : (u32)g_cur;
        if (!valid_thread(id)) { RET(c, id < (u32)MAX_THREADS ? 0 : -1); break; }
        refer_thread(id, A(c, 1));
        RET(c, (s32)gp32(A(c, 1) ? A(c, 1) : 0)[0]);
        if (!A(c, 1)) RET(c, 0);
        break;
    }
    case 0x32: {  // SleepThread
        Thread& t = g_threads[g_cur];
        if (t.wakeup > 0) {
            t.wakeup--;
            RET(c, g_cur);
            break;
        }
        t.st = TS::Sleep;
        t.wait_result = g_cur;
        block_current();
        RET(c, t.wait_result);
        break;
    }
    case 0x33: case 0x34: RET(c, sys_wakeup(A(c, 0))); if (!is_i) reschedule(); break;
    case 0x35: case 0x36: {  // CancelWakeupThread
        u32 id = A(c, 0) ? A(c, 0) : (u32)g_cur;
        if (!valid_thread(id)) { RET(c, -1); break; }
        s32 w = g_threads[id].wakeup;
        g_threads[id].wakeup = 0;
        RET(c, w);
        break;
    }
    case 0x37: case 0x38: RET(c, sys_suspend(A(c, 0))); break;
    case 0x39: case 0x3A: RET(c, sys_resume(A(c, 0))); if (!is_i) reschedule(); break;
    case 0x3C: sys_setup_thread(c); break;
    case 0x3D: {  // SetupHeap(start, size)
        u32 start = A(c, 0), size = A(c, 1);
        g_heap_end = (size == 0xFFFFFFFFu) ? g_threads[g_cur ? g_cur : 1].stack : start + size;
        RET(c, (s32)g_heap_end);
        break;
    }
    case 0x3E: RET(c, (s32)g_heap_end); break;  // EndOfHeap
    case 0x40: RET(c, sys_create_sema(A(c, 0))); break;
    case 0x41: case 0x49: RET(c, sys_delete_sema(A(c, 0))); if (!is_i) reschedule(); break;
    case 0x42: case 0x43:
        RET(c, sys_signal_sema(A(c, 0)));
        if (!is_i) reschedule();
        break;
    case 0x44: RET(c, sys_wait_sema(A(c, 0))); break;
    case 0x45: case 0x46: {  // PollSema
        u32 id = A(c, 0);
        if (!valid_sema(id) || g_semas[id].count <= 0) { RET(c, -1); break; }
        g_semas[id].count--;
        RET(c, (s32)id);
        break;
    }
    case 0x47: case 0x48:
        if (!valid_sema(A(c, 0))) { RET(c, -1); break; }
        refer_sema(A(c, 0), A(c, 1));
        RET(c, (s32)A(c, 0));
        break;
    case 0x4A: g_osd_config = *gp32(A(c, 0)); RET(c, 0); break;   // SetOsdConfigParam
    case 0x4B: *gp32(A(c, 0)) = g_osd_config; RET(c, 0); break;   // GetOsdConfigParam
    case 0x4C:  // GetGsHParam(p1, p2, p3)
        if (A(c, 0)) *gp32(A(c, 0)) = g_gs_hparam[0];
        if (A(c, 1)) *gp32(A(c, 1)) = g_gs_hparam[1];
        if (A(c, 2)) *gp32(A(c, 2)) = g_gs_hparam[2];
        RET(c, 0);
        break;
    case 0x4D: RET(c, (s32)g_gs_vparam); break;
    case 0x4E: g_gs_hparam[0] = A(c, 0); g_gs_hparam[1] = A(c, 1); g_gs_hparam[2] = A(c, 2); RET(c, 0); break;
    case 0x4F: g_gs_vparam = A(c, 0); RET(c, 0); break;
    case 0x5C: case 0x5D: case 0x5E: case 0x5F: {  // Enable/Disable Intc/Dmac handler
        bool en = (n == 0x5C || n == 0x5E);
        Handler* tab = (n <= 0x5D) ? g_intc_h : g_dmac_h;
        u32 id = A(c, 1);
        if (id && id < (u32)MAX_HANDLERS && tab[id].used) { tab[id].enabled = en; RET(c, 0); }
        else RET(c, -1);
        break;
    }
    case 0x60: RET(c, (s32)A(c, 0)); break;                 // KSeg0
    case 0x61: case 0x62: RET(c, 0); break;                 // Enable/DisableCache
    case 0x63: case 0x67: RET(c, (s32)ee_mfc0(c, A(c, 0))); break;  // GetCop0
    case 0x64: case 0x68: RET(c, 0); break;                 // FlushCache
    case 0x66: case 0x6A: RET(c, 0); break;                 // CpuConfig
    case 0x6B: RET(c, 0); break;                            // SifStopDma
    case 0x6E: memcpy(ee_host(A(c, 0)), g_osd_config2, 8); (void)0; RET(c, 0); break;
    case 0x6F: memcpy(ee_host(A(c, 0)), g_osd_config2, 8); RET(c, 0); break;
    case 0x70: RET(c, (s64)hw::gs_read_imr()); break;       // GsGetIMR
    case 0x71: hw::gs_write_imr(c->r[4].d[0]); RET(c, 0); break;  // GsPutIMR
    case 0x73: g_vsync_flag_addr = A(c, 0); g_vsync_csr_addr = A(c, 1); RET(c, 0); break;
    case 0x74: {  // SetSyscall(number, address): write the guest-visible table
        u32 num = A(c, 0) & 0xFF, addr = A(c, 1);
        syscall_table()[num] = addr;
        LOG(LOG_SYS, "[sys] SetSyscall %x -> %08x\n", num, addr);
        RET(c, 0);
        break;
    }
    case 0x75: LOG(LOG_TTY, "%s", ee_cstr(A(c, 0))); RET(c, 0); break;  // _print
    case 0x76: RET(c, iop::ee_sif_dma_stat(A(c, 0))); break;
    case 0x77:
        RET(c, (s32)iop::ee_sif_set_dma((const iop::SifDmaXfer*)ee_host(A(c, 0)), A(c, 1)));
        __atomic_or_fetch(&g_ee_events, EV_IOP, __ATOMIC_RELEASE);  // let the IOP react promptly
        break;
    case 0x78: iop::ee_sif_set_dchain(); RET(c, 0); break;
    case 0x79: iop::ee_sif_set_reg(A(c, 0), A(c, 1)); RET(c, 0); break;
    case 0x7A: RET(c, (s32)iop::ee_sif_get_reg(A(c, 0))); break;
    case 0x7B: do_exit(0); RET(c, 0); break;   // ExecOSD
    case 0x7C: RET(c, sys_deci2(A(c, 0), A(c, 1))); break;  // Deci2Call (debug TTY)
    case 0x7D: RET(c, 0); break;               // PSMode
    case 0x7E: RET(c, 0); break;               // MachineType (retail)
    case 0x7F: RET(c, (s32)EE_RAM_SIZE); break;
    default:
        LOG(LOG_SYS, "[sys] unimplemented syscall %s0x%x at %08x\n", is_i ? "-" : "", n, pc);
        RET(c, 0);
        break;
    }
    // Syscalls are preemption points; interrupts raised by the call are delivered here.
    if (!is_i && !g_in_intr && (g_ee_events || hw::intc_pending() || hw::dmac_pending())) ee_service_events(c, pc);
}

} // namespace kernel

// ---- hooks used by generated code ----------------------------------------------------
void ee_syscall(EEContext* c, u32 pc, u32 /*code*/) { kernel::syscall_dispatch(c, pc); }

void ee_service_events(EEContext* c, u32 pc) {
    using namespace kernel;
    u32 ev = __atomic_exchange_n(&g_ee_events, 0, __ATOMIC_ACQ_REL);
    if (ev & EV_QUIT) {
        __atomic_or_fetch(&g_ee_events, EV_QUIT, __ATOMIC_RELEASE);
        if (g_cur) {
            // unwind to the scheduler which returns from run()
            Fiber* from = g_threads[g_cur].fiber;
            g_cur = 0;
            fiber_switch(from, &g_sched_fiber);
        }
        return;
    }
    if (g_in_intr) {  // never nest: keep the bits for later
        __atomic_or_fetch(&g_ee_events, ev, __ATOMIC_RELEASE);
        return;
    }
    if (ev & EV_TIMER) process_time();
    hw::poll_async();
    run_iop();
    publish_deadline();   // the IOP may have started/finished DelayThread waits
    deliver_interrupts(c, pc);
    if (g_need_resched) reschedule();
}

void kernel_ee_ei(EEContext* c) {
    c->cop0_status |= 0x10001;
    if (hw::intc_pending() || hw::dmac_pending() || g_ee_events) ee_service_events(c, c->pc);
}

u32 g_ee_spin = 0;
void ee_spin_wait(EEContext*) {
    g_ee_spin = 0;
    kernel::guest_waiting();
}
