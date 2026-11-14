#pragma once
// GS (Graphics Synthesizer) backend interface.
//
// The runtime feeds the backend the exact data the real GIF would deliver to
// the GS.  The backend owns GS emulation state (local memory, registers,
// rendering) and presents the displayed frame into the platform window.
// All functions are called from the emulation thread unless noted.
#include "ps2/types.h"

namespace gs {

struct Config {
    int renderer = 0;     // 0 = software (accurate), 1 = OpenGL hardware
    int upscale = 1;      // HW renderer internal resolution multiplier
    int sw_threads = -1;  // software rasterizer worker threads (-1 = auto, 0 = none)
    bool vsync = true;    // host vsync on present
    bool bilinear_present = true;
    bool widescreen = false;  // present the picture at 16:9 instead of 4:3
    int sharpen = 0;          // contrast-adaptive sharpening of the output, 0 (off) - 100
    bool skip_deflicker = false;  // HW: skip the game's 1-line anti-flicker blend (sharper on monitors)
};

// `sdl_window` is an SDL_Window* created by the platform layer (without an
// SDL renderer).  The backend creates whatever GL context it needs on it.
bool init(void* sdl_window, const Config& cfg);
void shutdown();

// Privileged registers live at guest 0x12000000-0x12001FFF.  The runtime
// forwards every EE access in that range here (offset = addr - 0x12000000).
void write_priv64(u32 offset, u64 value);
u64 read_priv64(u32 offset);

// GIF paths.  `qwc` counts 128-bit quadwords.  Data is consumed before return.
void gif_path1(const u8* vu1_mem, u32 start_qw);   // XGKICK: packet in VU1 data memory (16 KiB, wraps)
void gif_path2(const u8* data, u32 qwc);           // VIF1 DIRECT / DIRECTHL
void gif_path3(const u8* data, u32 qwc);           // GIF DMA channel / GIF FIFO

// Asynchronous VU1 (ps2/vu.h vu_thread_start): gif_path1 is then called on
// the VU1 thread.  The emulation thread calls path1_program() when it queues
// a micro-program start; that program's PATH1 packets are inserted there in
// the GS stream, and the VU1 thread calls path1_program_end() after its E-bit.
// set_path1_async(true) fails without a GS thread (PS2_GS_SYNC=1).
bool set_path1_async(bool async);
void path1_program();
void path1_program_end();                          // VU1 thread

// Display registers to stderr without waiting for the GS thread (SIGQUIT).
void debug_dump();

// Local->host transfer (BITBLTBUF + TRXDIR=1): copy `qwc` quadwords of the
// pending download into dst.
void read_fifo(u8* dst, u32 qwc);

// Called once per vertical blank (field = 0/1 for interlaced field parity).
// Presents the current display output.
void vsync(int field);

// Called by the backend (possibly from another thread) when a SIGNAL, FINISH
// or LABEL event sets a CSR bit.  The runtime raises the GS interrupt.
void set_event_callback(void (*cb)(u32 csr_bits));

// Save the most recently presented frame as a PNG (debug / verification).
bool screenshot(const char* path);
// Record a raw GSdx dump (for gs_replay) of the next `vsyncs` vsyncs.
void request_dump(const char* path, int vsyncs);

// Number of DISPFB1/DISPFB2 changes so far (display buffer flips = game frames).
u32 flip_count();

// Make queued GS work visible to the GS thread now (commands are otherwise
// published in batches; the runtime calls this on every event/idle pass).
void flush();

void ring_waits(u64 out[4]);   // diagnostics, see gs.cpp
void op_times(u64 wall[4], u64 cpu[4]);   // GS thread wall/CPU ns per command class (PS2_GS_TIMING=1)
bool op_timing_enabled();
} // namespace gs
