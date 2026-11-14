#include "ps2/gs.h"
#include "ps2/log.h"
#include "ps2/platform.h"
#include "ps2/spsc_ring.h"
#include "GSdx.h"
#include "Window/GSWndSDL.h"
#include <SDL3/SDL.h>

#include <png.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <xmmintrin.h>
#include <ctime>

extern "C" {
int GSinit();
void GSshutdown();
void GSclose();
int GSopen2(void**, unsigned int);
void GSsetBaseMem(unsigned char*);
void GSirqCallback(void (*)());
void GSwriteCSR(unsigned int);
void GSreset();
void GSgifTransfer1(unsigned char*, unsigned int);
void GSgifTransfer1Packet(const unsigned char*, unsigned int);
void GSgifTransfer2(unsigned char*, unsigned int);
void GSgifTransfer3(unsigned char*, unsigned int);
void GSinitReadFIFO2(unsigned char*, unsigned int);
void GSreadFIFO2(unsigned char*, unsigned int);
void GSvsync(int);
void GSvsyncNoSwap(int);
void GSdebugTransferRegs(unsigned long long* bitbltbuf, unsigned long long* trxpos, unsigned long long* trxreg);
void GSsetVsync(int);
}

void ee_dump_trace(int count);   // core/dispatch.cpp (diagnostics)
namespace {
// The EE runs with RZ/FTZ/DAZ; GSdx's SIMD and JIT expect host rounding.
struct HostMXCSR {
    unsigned int previous = _mm_getcsr();
    HostMXCSR() { if (previous != 0x1f80) _mm_setcsr(0x1f80); }
    ~HostMXCSR() { if (previous != 0x1f80) _mm_setcsr(previous); }
};

alignas(16) std::array<u8, 0x2000> regs{};
SDL_Window* window = nullptr;
std::atomic<void (*)(u32)> event_callback{nullptr};
bool initialized = false;
bool synchronous = false;
bool presented = false;
int last_field = 0;
u32 reported_events = 0;
// CSR as the EE sees it, without draining the GS (and, behind it, VU1):
// the GS thread publishes CSR after every command; the EE masks the bits its
// own not-yet-applied CSR writes clear, and supplies FIELD itself (it toggles
// at the EE's vblank, which the GS thread only reaches later).
std::atomic<u32> csr_seen{0};
u64 last_bitbltbuf = 0, last_trxreg = 0;   // diagnostics only (EE side copies)
struct CsrClear { u64 end; u32 bits; };   // ring position after the write
std::deque<CsrClear> csr_clears;          // EE thread
u32 ee_field = 0;                         // EE thread
void publish_csr() {
    u32 csr;
    std::memcpy(&csr, regs.data() + 0x1000, sizeof(csr));
    csr_seen.store(csr, std::memory_order_release);
}

// The producer is exclusively the EE thread. The consumer is exclusively the
// GS thread; a command is made visible only after its entire payload is copied.
constexpr size_t ring_size = 32u << 20;
constexpr size_t max_transfer = 1u << 20;
std::unique_ptr<SpscRing> ring;
std::atomic<u64> frames_done{0};
u64 frames_sent = 0;
std::thread gs_thread;

enum class Op : u32 { Write, Path1, Path1Packet, Path1Program, Path2, Path3, Fifo, Vsync, Screenshot, Stop };
struct Command {
    Op op;
    u32 arg;
    u32 bytes;
    u32 reserved;
    u64 value;
};
static_assert(sizeof(Command) == 24);
struct ScreenshotRequest { const char* path; bool result; };

// PATH1 of the asynchronous VU1: the VU1 thread streams each program's
// XGKICK packets here, the GS thread replays them at that program's
// Op::Path1Program placeholder in the main ring (EE submission order).
enum class P1 : u32 { Packet, Wrap, End };
struct P1Record {
    P1 kind;
    u32 qwc;     // Packet: quadwords
    u32 start;   // Wrap: start quadword in the 16 KiB VU1 memory snapshot
    u32 bytes;   // payload
};
static_assert(sizeof(P1Record) == 16);
constexpr size_t p1_ring_size = 16u << 20;
constexpr u64 p1_batch = 64u << 10;   // publish (VU1) / release (GS) granularity
std::unique_ptr<SpscRing> p1_ring;
bool path1_async = false;             // EE thread decides; VU1 thread only while set

// ---- early FINISH (as PCSX2's EE-side GIF unit) ----------------------------
// The game paces its frames on the GS FINISH interrupt.  Waiting until the GS
// thread has *drawn* up to the FINISH serializes game, VU1 and rendering, so
// the queueing side reports FINISH as soon as the packet is queued (rendering
// still happens in order; reads of GS state drain the queue as before).
// PS2_GS_LATE_FINISH=1 restores reporting from the GS thread.
struct GifScan { u32 left = 0, nreg = 0, reg = 0, flg = 0; u64 regs = 0; };
GifScan scan_path2, scan_path3;
bool early_finish = true;
std::atomic<u32> early_csr{0};   // FINISH raised early and not yet cleared by the EE
u32 scan_gif(GifScan& g, const u8* data, u32 qwc, u32 wrap_qw = 0, u32 start = 0) {
    u32 bits = 0;
    for (u32 q = 0; q < qwc; ++q) {
        const u8* d = wrap_qw ? data + ((start + q) % wrap_qw) * 16 : data + q * 16;
        if (!g.left) {   // GIF tag
            u64 tag;
            std::memcpy(&tag, d, 8);
            std::memcpy(&g.regs, d + 8, 8);
            const u32 nloop = static_cast<u32>(tag & 0x7fff);
            g.flg = static_cast<u32>(tag >> 58) & 3;
            g.nreg = (static_cast<u32>(tag >> 60) & 15) ? (static_cast<u32>(tag >> 60) & 15) : 16;
            g.reg = 0;
            g.left = g.flg == 0 ? nloop * g.nreg : g.flg == 1 ? (nloop * g.nreg + 1) / 2 : nloop;
            continue;
        }
        if (g.flg == 0) {
            if (((g.regs >> (4 * g.reg)) & 15) == 0xe && d[8] == 0x61) bits |= 2;   // A+D FINISH
            if (++g.reg == g.nreg) g.reg = 0;
        }
        --g.left;
    }
    return bits;
}
void report_early(u32 bits) {
    if (!(bits & 2)) return;
    early_csr.fetch_or(2, std::memory_order_acq_rel);
    if (auto cb = event_callback.load(std::memory_order_acquire)) cb(2);
}

void irq() {
    u32 csr;
    std::memcpy(&csr, regs.data() + 0x1000, sizeof(csr));
    const u32 new_bits = (csr & 0x1f) & ~reported_events;
    reported_events |= new_bits;
    // LABEL has no CSR status bit, so GSdx calls us with new_bits == 0.
    const u32 report = early_finish ? new_bits & ~2u : new_bits;   // FINISH went out early
    if (auto cb = event_callback.load(std::memory_order_acquire)) cb(report);
}

bool backend_init(const gs::Config& cfg) {
    regs.fill(0);
    publish_csr();
    reported_events = 0;
    presented = false;
    last_field = 0;
    theApp.Init();
    theApp.SetConfig("Renderer", cfg.renderer ? 12 : 13);
    theApp.SetConfig("upscale_multiplier", std::max(1, cfg.upscale));
    const int sw_threads = cfg.sw_threads >= 0 ? cfg.sw_threads
        : std::clamp(static_cast<int>(std::thread::hardware_concurrency() / 2), 2, 8);
    theApp.SetConfig("extrathreads", sw_threads);
    theApp.SetConfig("interlace", 7);
    theApp.SetConfig("AspectRatio", cfg.widescreen ? 2 : 1);   // GSdx: 1 = 4:3, 2 = 16:9
    theApp.SetConfig("linear_present", cfg.bilinear_present ? 1 : 0);
    theApp.SetConfig("vsync", cfg.vsync ? 1 : 0);
    theApp.SetConfig("ps2_sharpen", std::clamp(cfg.sharpen, 0, 100));
    theApp.SetConfig("ps2_skip_deflicker", cfg.skip_deflicker ? 1 : 0);
    theApp.SetConfig("osd_log_enabled", 0);
    theApp.SetConfig("osd_monitor_enabled", 0);
    // Upscaled HW rendering: GSdx's "normal vertex" half-pixel offset keeps the
    // game's 8-pixel channel shuffles and full-screen passes aligned (without
    // it the depth haze and shadow passes smear; measured against the software
    // renderer, 3x: 16% -> 5% of pixels off).  The other user hacks keep their
    // stock defaults.
    if (cfg.renderer && cfg.upscale > 1) {
        theApp.SetConfig("UserHacks", 1);
        theApp.SetConfig("UserHacks_HalfPixelOffset", 1);
    }
    // PS2_GSDX="key=value,key=value": set any GSdx option (see GSdx.cpp
    // m_default_configuration), e.g. PS2_GSDX=accurate_blending_unit=3.
    if (const char* extra = std::getenv("PS2_GSDX")) {
        std::string s(extra);
        size_t start = 0;
        while (start < s.size()) {
            size_t end = s.find(',', start);
            if (end == std::string::npos) end = s.size();
            const std::string item = s.substr(start, end - start);
            const size_t eq = item.find('=');
            if (eq != std::string::npos)
                theApp.SetConfig(item.substr(0, eq).c_str(), std::atoi(item.c_str() + eq + 1));
            start = end + 1;
        }
    }
    if (GSinit() != 0) return false;
    GSsetBaseMem(regs.data());
    GSirqCallback(irq);
    GSsetVsync(cfg.vsync ? 1 : 0);
    void* display[2] = {window, window};
    if (GSopen2(display, 0) == 0) return true;
    GSshutdown();
    return false;
}

void backend_write(u32 offset, u64 value) {
    if (offset == 0x1000) {
        u32 csr;
        std::memcpy(&csr, regs.data() + offset, 4);
        const u32 cleared = static_cast<u32>(value) & 0x1f;
        csr &= ~cleared;
        reported_events &= ~cleared;
        if (value & (1u << 9)) {
            GSreset();
            std::fill(regs.begin(), regs.begin() + 0x1000, 0);
            csr &= ~0x1f;
            reported_events = 0;
        }
        std::memcpy(regs.data() + offset, &csr, 4);
        GSwriteCSR(static_cast<u32>(value));
    } else {
        std::memcpy(regs.data() + offset, &value, 8);
    }
}

// present=false: the guest did not flip since the last present, so the
// picture is unchanged.  GSdx still merges and deinterlaces (its weave/blend
// buffers keep the other field's lines) but skips the swap.
void backend_vsync(int field, bool present = true) {
    u32 csr;
    std::memcpy(&csr, regs.data() + 0x1000, 4);
    csr = (csr & ~(1u << 13)) | (static_cast<u32>(field & 1) << 13);
    std::memcpy(regs.data() + 0x1000, &csr, 4);
    last_field = field & 1;
    if (!present) { GSvsyncNoSwap(last_field); return; }
    GSvsync(last_field);
    presented = true;
}

bool backend_screenshot(const char* path) {
    if (!presented || !path) return false;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (w <= 0 || h <= 0) return false;
    std::vector<u8> pixels(static_cast<size_t>(w) * h * 4);
    GSSDL_CaptureNextFlip(pixels.data(), w, h);
    // The EGL/Wayland front buffer is not readable after a swap. Capture the
    // finished backbuffer during an on-demand extra present, not each frame.
    GSvsync(last_field);
    if (!GSSDL_FinishCapture()) return false;
    // Write to a temporary name and rename, so pollers never read a partial PNG.
    const std::string tmp = std::string(path) + ".tmp";
    FILE* file = std::fopen(tmp.c_str(), "wb");
    if (!file) return false;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png ? png_create_info_struct(png) : nullptr;
    if (!info) {
        if (png) png_destroy_write_struct(&png, nullptr);
        std::fclose(file);
        return false;
    }
    bool success = false;
    if (!setjmp(png_jmpbuf(png))) {
        png_init_io(png, file);
        png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                     PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
        png_write_info(png, info);
        for (int y = h - 1; y >= 0; --y)
            png_write_row(png, pixels.data() + static_cast<size_t>(y) * w * 4);
        png_write_end(png, info);
        success = true;
    }
    png_destroy_write_struct(&png, &info);
    std::fclose(file);
    if (success) success = std::rename(tmp.c_str(), path) == 0;
    else std::remove(tmp.c_str());
    return success;
}

void wait_for(u64 target) { ring->wait_consumed(target); }
void wait_for_frame(u64 target) {
    if (frames_done.load(std::memory_order_acquire) >= target) return;
    ring->producer_wait([target] { return frames_done.load(std::memory_order_acquire) >= target; });
}
// Commands are staged in the ring past `published` and made visible in
// batches: per-command publication (fence + flag exchange) dominated scenes
// with ~10^5 XGKICKs per frame.  Anything that waits on the consumer, plus
// gs::flush() from the runtime's event/idle paths, publishes first.
constexpr u64 publish_batch = 256u << 10;
void publish() { ring->publish(); }
u64 push(Command cmd, const u8* payload = nullptr, bool publish_now = false) {
    ring->reserve(sizeof(cmd) + cmd.bytes);
    ring->write(&cmd, sizeof(cmd));
    if (cmd.bytes) ring->write(payload, cmd.bytes);
    if (publish_now || ring->unpublished() >= publish_batch) publish();
    return ring->staged();
}

// VU1 thread: append one PATH1 record (payload given in up to two pieces).
void p1_push(P1 kind, u32 qwc, u32 start, const u8* a, u32 na, const u8* b = nullptr, u32 nb = 0) {
    const P1Record record{kind, qwc, start, na + nb};
    p1_ring->reserve(sizeof(record) + na + nb);
    p1_ring->write(&record, sizeof(record));
    if (na) p1_ring->write(a, na);
    if (nb) p1_ring->write(b, nb);
    if (p1_ring->unpublished() >= p1_batch) p1_ring->publish();
}
// GS thread: replay one program's PATH1 packets, up to its End record.
void replay_path1_program(u64& position, std::vector<u8>& scratch) {
    u64 released = position;
    for (;;) {
        if (position == p1_ring->published()) {
            p1_ring->release(position);   // never sleep on space the VU1 thread waits for
            released = position;
            p1_ring->wait_published(position);
        }
        P1Record record;
        p1_ring->read(position, &record, sizeof(record));
        position += sizeof(record);
        if (record.kind == P1::End) break;
        const u8* payload = p1_ring->contiguous(position, record.bytes);
        if (!payload) {
            if (scratch.size() < record.bytes) scratch.resize(record.bytes);
            p1_ring->read(position, scratch.data(), record.bytes);
            payload = scratch.data();
        }
        if (record.kind == P1::Packet) GSgifTransfer1Packet(payload, record.qwc);
        else GSgifTransfer1(const_cast<u8*>(payload), record.start * 16);
        position += record.bytes;
        if (position - released >= p1_batch) {
            p1_ring->release(position);
            released = position;
        }
    }
    p1_ring->release(position);
}

// PS2_GS_TIMING=1 diagnostics (printed with PS2_FPS=1): GS thread wall / CPU
// time per command class (0 drawing paths, 1 present, 2 FIFO readback,
// 3 other).  Wall time well above CPU time means the thread blocked inside
// the GL driver.  Opt-in: the thread CPU clock is a system call per command.
std::atomic<u64> op_wall_ns[4], op_cpu_ns[4];
const bool op_timing = [] { const char* f = std::getenv("PS2_GS_TIMING"); return f && *f == '1'; }();
inline u64 clock_ns(clockid_t id) {
    timespec t;
    clock_gettime(id, &t);
    return u64(t.tv_sec) * 1000000000ull + u64(t.tv_nsec);
}

void consume() {
    std::vector<u8> scratch, p1_scratch;
    u64 position = 0, end = 0, p1_position = 0;
    for (;;) {
        if (position == end) end = ring->wait_published(position);
        Command cmd;
        ring->read(position, &cmd, sizeof(cmd));
        u8* payload = nullptr;
        if (cmd.bytes) {
            // Read contiguous payloads in place; copy only those that wrap.
            payload = const_cast<u8*>(ring->contiguous(position + sizeof(cmd), cmd.bytes));
            if (!payload) {
                if (scratch.size() < cmd.bytes) scratch.resize(cmd.bytes);
                ring->read(position + sizeof(cmd), scratch.data(), cmd.bytes);
                payload = scratch.data();
            }
        }
        u64 wall0 = 0, cpu0 = 0;
        if (op_timing) { wall0 = clock_ns(CLOCK_MONOTONIC); cpu0 = clock_ns(CLOCK_THREAD_CPUTIME_ID); }
        switch (cmd.op) {
        case Op::Write: backend_write(cmd.arg, cmd.value); break;
        case Op::Path1: GSgifTransfer1(payload, cmd.arg * 16); break;
        case Op::Path1Packet: GSgifTransfer1Packet(payload, cmd.arg); break;
        case Op::Path1Program: replay_path1_program(p1_position, p1_scratch); break;
        case Op::Path2: GSgifTransfer2(payload, cmd.arg); break;
        case Op::Path3: GSgifTransfer3(payload, cmd.arg); break;
        case Op::Fifo: {
            auto* dst = reinterpret_cast<u8*>(static_cast<uintptr_t>(cmd.value));
            GSinitReadFIFO2(dst, cmd.arg);
            GSreadFIFO2(dst, cmd.arg);
            if (PS2_UNLIKELY(g_log_mask & LOG_GS)) {
                unsigned long long bitbltbuf, trxpos, trxreg;
                GSdebugTransferRegs(&bitbltbuf, &trxpos, &trxreg);
                last_bitbltbuf = bitbltbuf;
                last_trxreg = trxreg;
                static u64 shown = 0;
                if (shown++ < 4)
                    LOG(LOG_GS, "[gs] FIFO download %u qw: BITBLTBUF %016llx TRXPOS %016llx TRXREG %016llx\n", cmd.arg,
                        (unsigned long long)bitbltbuf, (unsigned long long)trxpos, (unsigned long long)trxreg);
            }
            break;
        }
        case Op::Vsync:
            backend_vsync(static_cast<int>(cmd.arg), cmd.value == 0);
            frames_done.fetch_add(1, std::memory_order_release);
            ring->wake_producer();
            break;
        case Op::Screenshot: {
            auto* request = reinterpret_cast<ScreenshotRequest*>(static_cast<uintptr_t>(cmd.value));
            request->result = backend_screenshot(request->path);
            break;
        }
        case Op::Stop: break;
        }
        if (op_timing) {
            const unsigned cls = cmd.op == Op::Vsync || cmd.op == Op::Screenshot ? 1
                               : cmd.op == Op::Fifo ? 2 : cmd.op == Op::Write || cmd.op == Op::Stop ? 3 : 0;
            op_wall_ns[cls].fetch_add(clock_ns(CLOCK_MONOTONIC) - wall0, std::memory_order_relaxed);
            op_cpu_ns[cls].fetch_add(clock_ns(CLOCK_THREAD_CPUTIME_ID) - cpu0, std::memory_order_relaxed);
        }
        publish_csr();
        position += sizeof(cmd) + cmd.bytes;
        ring->release(position);
        if (cmd.op == Op::Stop) return;
    }
}

// Quadwords of the PATH1 packet starting at `start` in VU1 data memory (1024
// qwords, wrapping) up to and including its EOP tag's data; 0 when the tags
// do not reach EOP within one revolution (then the whole memory is sent).
u32 path1_packet_qwc(const u8* vu1, u32 start) {
    u32 total = 0;
    while (total < 1024) {
        u64 tag;
        std::memcpy(&tag, vu1 + ((start + total) & 1023) * 16, sizeof(tag));
        ++total;
        const u32 nloop = static_cast<u32>(tag & 0x7fff);
        const u32 flg = static_cast<u32>(tag >> 58) & 3;
        const u32 nreg = (static_cast<u32>(tag >> 60) & 15) ? (static_cast<u32>(tag >> 60) & 15) : 16;
        total += flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
        if (tag & 0x8000) return total <= 1024 ? total : 0;   // EOP
    }
    return 0;
}
}

// Defined in GSdx's GSRenderer.cpp (F10 dumps).
extern std::mutex g_gsdx_dump_mutex;
extern std::string g_gsdx_dump_path;
extern std::atomic<long> g_gsdx_dump_vsyncs;

namespace gs {
bool init(void* sdl_window, const Config& cfg) {
    if (initialized || !sdl_window || (cfg.renderer != 0 && cfg.renderer != 1)) return false;
    window = static_cast<SDL_Window*>(sdl_window);
    synchronous = std::getenv("PS2_GS_SYNC") && std::strcmp(std::getenv("PS2_GS_SYNC"), "1") == 0;
    early_finish = !synchronous && !(std::getenv("PS2_GS_LATE_FINISH") &&
                                     std::strcmp(std::getenv("PS2_GS_LATE_FINISH"), "1") == 0);
    if (synchronous) {
        HostMXCSR mxcsr;
        if (!backend_init(cfg)) { window = nullptr; return false; }
    } else {
        ring = std::make_unique<SpscRing>(ring_size);
        frames_done.store(0, std::memory_order_relaxed);
        frames_sent = 0;
        csr_clears.clear();
        ee_field = 0;
        std::promise<bool> started;
        auto ready = started.get_future();
        gs_thread = std::thread([cfg, start = std::move(started)]() mutable {
            HostMXCSR mxcsr;
            const bool opened = backend_init(cfg);
            start.set_value(opened);
            if (!opened) return;
            consume();
            GSclose();
            GSshutdown();
        });
        if (!ready.get()) {
            gs_thread.join();
            ring.reset();
            window = nullptr;
            return false;
        }
    }
    initialized = true;
    return true;
}

void shutdown() {
    if (!initialized) return;
    if (synchronous) {
        HostMXCSR mxcsr;
        GSclose();
        GSshutdown();
    } else {
        push({Op::Stop, 0, 0, 0, 0}, nullptr, true);
        gs_thread.join();
        ring.reset();
        p1_ring.reset();
        path1_async = false;
    }
    initialized = false;
    presented = false;
    event_callback.store(nullptr, std::memory_order_release);
    window = nullptr;
}

u32 flips = 0;
bool flip_pending = false;
u64 dispfb[2] = {~0ull, ~0ull};

void write_priv64(u32 offset, u64 value) {
    if (!initialized || offset > 0x1ff8 || (offset & 7)) return;
    // PS2_LOG_DISPLAY=1: every change of PMODE/SMODE2/DISPFB/DISPLAY/BGCOLOR,
    // stamped with the flip count (debugging output placement).
    static const bool log_display = [] { const char* e = getenv("PS2_LOG_DISPLAY"); return e && *e == '1'; }();
    if (PS2_UNLIKELY(log_display) && offset <= 0xe0) {
        static u64 last_value[29];
        static bool seen[29];
        const u32 index = offset / 8;
        if (!seen[index] || last_value[index] != value) {
            seen[index] = true;
            last_value[index] = value;
            fprintf(stderr, "[disp] flip %u priv %02x = %016llx\n", flips, offset, (unsigned long long)value);
        }
    }
    if (offset == 0x70 || offset == 0x90) {   // DISPFB1 / DISPFB2
        u64& last = dispfb[offset == 0x90];
        if (value != last) { last = value; flip_pending = true; }
    }
    if (synchronous) {
        HostMXCSR mxcsr;
        backend_write(offset, value);
    } else {
        const u64 end = push({Op::Write, offset, 0, 0, value});
        // SIGNAL/FINISH/... write-1-to-clear; RESET clears them all.
        const u32 cleared = (value & (1u << 9)) ? 0x1fu : static_cast<u32>(value) & 0x1f;
        if (offset == 0x1000 && cleared) {
            csr_clears.push_back({end, cleared});
            early_csr.fetch_and(~cleared, std::memory_order_acq_rel);
        }
    }
}

u64 read_priv64(u32 offset) {
    u64 value = 0;
    if (!initialized || offset > 0x1ff8 || (offset & 7)) return 0;
    if (!synchronous && offset == 0x1000) {
        publish();   // the GS thread must reach pending SIGNAL/FINISH work
        const u64 done = ring->consumed();
        while (!csr_clears.empty() && csr_clears.front().end <= done) csr_clears.pop_front();
        u32 pending = 0;
        for (const CsrClear& clear : csr_clears) pending |= clear.bits;
        const u32 csr = (csr_seen.load(std::memory_order_acquire) & ~pending) |
                        early_csr.load(std::memory_order_acquire);
        return (csr & ~(1u << 13)) | (ee_field << 13);
    }
    if (!synchronous) {
        const u64 t0 = platform::now_ns();
        wait_for(ring->staged());
        if (PS2_UNLIKELY(g_log_mask & LOG_GS)) {
            static u64 total[0x400], count[0x400];
            total[offset >> 3] += platform::now_ns() - t0;
            if ((++count[offset >> 3] & 255) == 1)
                LOG(LOG_GS, "[gs] EE read priv %04x drained the GS queue: %llu reads, %.1f ms total\n", offset,
                    (unsigned long long)count[offset >> 3], total[offset >> 3] / 1e6);
        }
    }
    std::memcpy(&value, regs.data() + offset, offset == 0x1000 ? 4 : 8);
    return value;
}
void debug_dump() {
    // Racy reads of the GS thread's register mirror: diagnostics only.
    auto reg = [](u32 offset) {
        u64 value;
        std::memcpy(&value, regs.data() + offset, sizeof(value));
        return static_cast<unsigned long long>(value);
    };
    std::fprintf(stderr, "--- GS: PMODE %016llx SMODE2 %016llx BGCOLOR %016llx CSR %08x\n"
                         "    DISPFB1 %016llx DISPLAY1 %016llx\n    DISPFB2 %016llx DISPLAY2 %016llx\n",
                 reg(0x00), reg(0x20), reg(0xe0), csr_seen.load(std::memory_order_relaxed), reg(0x70), reg(0x80),
                 reg(0x90), reg(0xa0));
}

void gif_path1(const u8* vu1_mem, u32 start_qw) {
    if (!initialized || !vu1_mem) return;
    if (PS2_UNLIKELY(g_log_mask & LOG_GS)) {   // sample XGKICK packets: tag + first data qwords
        static u32 seen = 0;
        if (++seen % 50000 == 1) {
            const u32 s = start_qw & 1023;
            u64 q[8];
            for (int k = 0; k < 4; ++k) std::memcpy(&q[2 * k], vu1_mem + ((s + k) & 1023) * 16, 16);
            LOG(LOG_GS, "[gs] path1 #%u at qw %u: tag %016llx %016llx data %016llx %016llx %016llx %016llx\n",
                seen, s, (unsigned long long)q[0], (unsigned long long)q[1], (unsigned long long)q[2],
                (unsigned long long)q[3], (unsigned long long)q[4], (unsigned long long)q[5]);
        }
    }
    if (synchronous) {
        HostMXCSR mxcsr;
        GSgifTransfer1(const_cast<u8*>(vu1_mem), (start_qw & 1023) * 16);
    } else if (path1_async) {   // VU1 thread
        const u32 start = start_qw & 1023;
        if (const u32 qwc = path1_packet_qwc(vu1_mem, start)) {
            if (early_finish) {
                GifScan g;
                report_early(scan_gif(g, vu1_mem, qwc, 1024, start));
            }
            const u32 head = std::min(qwc, 1024 - start);
            p1_push(P1::Packet, qwc, 0, vu1_mem + start * 16, head * 16, vu1_mem, (qwc - head) * 16);
        } else {
            p1_push(P1::Wrap, 0, start, vu1_mem, 16384);
        }
    } else if (const u32 qwc = path1_packet_qwc(vu1_mem, start_qw & 1023)) {
        // Copy just the packet (XGKICK traffic is frequent); unwrap it if it
        // crosses the end of VU1 memory.
        const u32 start = start_qw & 1023;
        if (start + qwc <= 1024) {
            push({Op::Path1Packet, qwc, qwc * 16, 0, 0}, vu1_mem + start * 16);
        } else {
            alignas(16) static u8 unwrapped[16384];
            const u32 head = 1024 - start;
            std::memcpy(unwrapped, vu1_mem + start * 16, head * 16);
            std::memcpy(unwrapped + head * 16, vu1_mem, (qwc - head) * 16);
            push({Op::Path1Packet, qwc, qwc * 16, 0, 0}, unwrapped);
        }
    } else {
        // No EOP within one revolution (huge IMAGE data): snapshot all of VU1
        // and let GSgifTransfer1 wrap.
        push({Op::Path1, start_qw & 1023, 16384, 0, 0}, vu1_mem);
    }
}
// Diagnostics: producer blocked / consumer idle on the GS command ring and the
// VU1->GS PATH1 ring (ns, cumulative).
bool op_timing_enabled() { return op_timing; }
void op_times(u64 wall[4], u64 cpu[4]) {
    for (int i = 0; i < 4; ++i) {
        wall[i] = op_wall_ns[i].load(std::memory_order_relaxed);
        cpu[i] = op_cpu_ns[i].load(std::memory_order_relaxed);
    }
}
void ring_waits(u64 out[4]) {
    out[0] = ring ? ring->producer_wait_ns.load(std::memory_order_relaxed) : 0;
    out[1] = ring ? ring->consumer_wait_ns.load(std::memory_order_relaxed) : 0;
    out[2] = p1_ring ? p1_ring->producer_wait_ns.load(std::memory_order_relaxed) : 0;
    out[3] = p1_ring ? p1_ring->consumer_wait_ns.load(std::memory_order_relaxed) : 0;
}
bool set_path1_async(bool async) {
    if (!initialized || synchronous) return false;
    // The ring outlives every async period: the GS thread keeps its cursor.
    if (async && !p1_ring) p1_ring = std::make_unique<SpscRing>(p1_ring_size);
    path1_async = async;
    return true;
}
void path1_program() {
    if (initialized && path1_async) push({Op::Path1Program, 0, 0, 0, 0}, nullptr, true);
}
void path1_program_end() {
    if (!initialized || !path1_async) return;
    p1_push(P1::End, 0, 0, nullptr, 0);
    p1_ring->publish();
}
void gif_path2(const u8* data, u32 qwc) {
    if (!initialized || !data || !qwc) return;
    if (synchronous) {
        HostMXCSR mxcsr;
        GSgifTransfer2(const_cast<u8*>(data), qwc);
    } else {
        const u32 early = early_finish ? scan_gif(scan_path2, data, qwc) : 0;
        while (qwc) {
            const u32 count = std::min(qwc, static_cast<u32>(max_transfer / 16));
            push({Op::Path2, count, count * 16, 0, 0}, data);
            data += count * 16;
            qwc -= count;
        }
        report_early(early);
    }
}
void gif_path3(const u8* data, u32 qwc) {
    if (!initialized || !data || !qwc) return;
    if (synchronous) {
        HostMXCSR mxcsr;
        GSgifTransfer3(const_cast<u8*>(data), qwc);
    } else {
        const u32 early = early_finish ? scan_gif(scan_path3, data, qwc) : 0;
        while (qwc) {
            const u32 count = std::min(qwc, static_cast<u32>(max_transfer / 16));
            push({Op::Path3, count, count * 16, 0, 0}, data);
            data += count * 16;
            qwc -= count;
        }
        report_early(early);
    }
}
// Repeated per-frame GS->host downloads (Transformers reads its 64x64 sun
// lens-flare OcclusionBuffer back every frame) would drain the whole GS queue
// each time and serialize the EE with the GS thread.  Once the same bulk
// download (same destination and size) repeats within half a second, it is
// answered with the data of the previous request, and the new download is
// queued in order on the GS thread without waiting: the result is one request
// late, as with occlusion queries on PC.  First, one-off and small reads stay
// synchronous.  PS2_GS_SYNC_READBACK=1 disables this.
struct DeferredRead {
    u8* dst = nullptr;
    u32 qwc = 0;
    u64 frame = 0;        // frames_sent at the last request
    u64 end = 0;          // ring position after the queued download
    bool pending = false; // a queued download fills `data`
    std::vector<u8> data;
};
std::array<DeferredRead, 4> deferred_reads;
unsigned deferred_next = 0;

// Each request makes exactly one download (GSdx reads the transfer
// rectangle once).  Returns true when it handled the request.
bool read_fifo_deferred(u8* dst, u32 qwc) {
    static const bool sync_only = [] { const char* e = std::getenv("PS2_GS_SYNC_READBACK"); return e && *e == '1'; }();
    if (sync_only || qwc < 16) return false;
    DeferredRead* entry = nullptr;
    for (auto& e : deferred_reads) if (e.dst == dst && e.qwc == qwc) entry = &e;
    if (!entry) {
        entry = &deferred_reads[deferred_next++ % deferred_reads.size()];
        *entry = DeferredRead{dst, qwc, frames_sent, 0, false, {}};
        return false;   // first request: synchronous, straight into dst
    }
    // frames_sent counts vblanks: at 10 fps a per-frame read repeats every 6.
    const bool recent = frames_sent - entry->frame <= 30;
    entry->frame = frames_sent;
    if (!recent) { entry->pending = false; return false; }
    const size_t bytes = size_t(qwc) * 16;
    entry->data.resize(bytes);
    if (entry->pending) {
        // Answer with the previous request's download (queued a frame ago,
        // normally finished), then queue this request's download.
        wait_for(entry->end);
        std::memcpy(dst, entry->data.data(), bytes);
        entry->end = push({Op::Fifo, qwc, 0, 0, reinterpret_cast<uintptr_t>(entry->data.data())}, nullptr, true);
        return true;
    }
    // Second request of a run: synchronous through the staging buffer, which
    // then holds the data the next request is answered with.
    entry->end = push({Op::Fifo, qwc, 0, 0, reinterpret_cast<uintptr_t>(entry->data.data())}, nullptr, true);
    wait_for(entry->end);
    std::memcpy(dst, entry->data.data(), bytes);
    entry->pending = true;
    return true;
}

void read_fifo(u8* dst, u32 qwc) {
    if (!initialized || !dst || !qwc) return;
    if (PS2_UNLIKELY(g_log_mask & LOG_GS)) {
        static int shown = 0;
        if (shown++ < 3) {
            LOG(LOG_GS, "[gs] FIFO download of %u qw requested by:\n", qwc);
            ee_dump_trace(48);
        }
    }
    if (synchronous) {
        HostMXCSR mxcsr;
        GSinitReadFIFO2(dst, qwc);
        GSreadFIFO2(dst, qwc);
    } else {
        if (read_fifo_deferred(dst, qwc)) return;
        const u64 t0 = platform::now_ns();
        const u64 end = push({Op::Fifo, qwc, 0, 0, reinterpret_cast<uintptr_t>(dst)}, nullptr, true);
        wait_for(end);
        if (PS2_UNLIKELY(g_log_mask & LOG_GS)) {   // GS->host downloads drain the whole queue
            static u64 calls = 0, qws = 0, ns = 0, window = 0;
            ++calls; qws += qwc; ns += platform::now_ns() - t0;
            if (t0 - window > 1000000000ull) {
                LOG(LOG_GS, "[gs] FIFO downloads/s: %llu calls, %llu qw, %.1f ms waiting (BITBLTBUF %016llx TRXREG %016llx)\n",
                    (unsigned long long)calls, (unsigned long long)qws, ns / 1e6,
                    (unsigned long long)last_bitbltbuf, (unsigned long long)last_trxreg);
                calls = qws = ns = 0; window = t0;
            }
        }
    }
}
// PS2_GS_SKIP_SWAP=1: a game that double-buffers changes DISPFB once per
// frame, so vblanks without a flip show the same picture; those then skip
// the swap (GSdx still merges/deinterlaces).  Content drawn straight into the
// displayed buffer (movies, loading screens) swaps every vblank again once no
// flip happened for 8 vblanks.  Off by default: measured per level it was
// mixed (+1 fps in Mountain Ruin, -2 in Deep Amazon), the per-vblank swap
// apparently also paces the driver's queue.
// PS2_GS_AHEAD=<n> (default 2): vblanks the GS thread may lag behind.
void vsync(int field) {
    if (!initialized) return;
    static const bool present_all = [] { const char* e = std::getenv("PS2_GS_SKIP_SWAP"); return !(e && *e == '1'); }();
    static const u64 ahead = [] { const char* e = std::getenv("PS2_GS_AHEAD"); const int n = e ? std::atoi(e) : 2; return u64(n < 1 ? 1 : n > 16 ? 16 : n); }();
    static u32 since_flip = ~0u;
    const bool flipped = flip_pending;
    if (flip_pending) { ++flips; flip_pending = false; }
    since_flip = flipped ? 0 : since_flip == ~0u ? since_flip : since_flip + 1;
    const bool present = present_all || flipped || since_flip >= 8;
    if (synchronous) {
        HostMXCSR mxcsr;
        backend_vsync(field, present);
    } else {
        ++frames_sent;
        ee_field = static_cast<u32>(field & 1);
        push({Op::Vsync, static_cast<u32>(field & 1), 0, 0, present ? 0u : 1u}, nullptr, true);
        if (frames_sent > ahead) wait_for_frame(frames_sent - ahead);
    }
}
void set_event_callback(void (*cb)(u32)) {
    event_callback.store(cb, std::memory_order_release);
}
void request_dump(const char* path, int vsyncs) {
    {
        std::lock_guard lock(g_gsdx_dump_mutex);
        g_gsdx_dump_path = path;
    }
    g_gsdx_dump_vsyncs.store(vsyncs);
}

bool screenshot(const char* path) {
    if (!initialized || !path) return false;
    if (synchronous) {
        HostMXCSR mxcsr;
        return backend_screenshot(path);
    }
    ScreenshotRequest request{path, false};
    const u64 end = push({Op::Screenshot, 0, 0, 0, reinterpret_cast<uintptr_t>(&request)}, nullptr, true);
    wait_for(end);
    return request.result;
}
u32 flip_count() { return flips; }
void flush() {
    if (initialized && !synchronous) publish();
}
} // namespace gs
