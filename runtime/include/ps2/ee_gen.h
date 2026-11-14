#pragma once
// Included by every generated EE translation unit (build/generated/ee_*.cpp).
// Small helpers whose semantics are part of the code-generator contract.
#include "ps2/ee_context.h"
#include "ps2/ee_mem.h"
#include "ps2/ee_ops.h"
#include "ps2/vu.h"
#include "ps2/vu0_macro.h"

// Debug aid: verify that every call returned to the instruction after its
// delay slot (catches longjmp-style control flow the recompiler cannot model).
#ifndef EE_CHECK_RETURNS
#define EE_CHECK_RETURNS 1
#endif
void ee_bad_return(EEContext* c, u32 call_pc, u32 expected);
#if EE_CHECK_RETURNS
#define EE_RET_CHECK(c, call_pc, expected)                                   \
    do {                                                                     \
        if (PS2_UNLIKELY((c)->r[31].w[0] != (u32)(expected)))                \
            ee_bad_return((c), (call_pc), (expected));                       \
    } while (0)
#else
#define EE_RET_CHECK(c, call_pc, expected) do { } while (0)
#endif

// Widescreen: generated code multiplies the engine camera's tan(fov / 2) by
// this (1 for 4:3, 4/3 for --widescreen); see CAMERA_TAN_CALLS in recomp_ee.py.
extern float g_ee_camera_fov_scale;

// Mouse look: hooks in the game's camera controller (CAMERA_HOOKS in
// recomp_ee.py) add the mouse motion straight to the camera yaw/pitch.
void ee_camera_hook_begin(EEContext* c);
void ee_camera_hook_end(EEContext* c);

// Function-entry trace ring (debugging): every generated function records its
// guest address; PS2_BREAK=<hex addr> dumps the ring when that function is entered.
#ifndef EE_TRACE_CALLS
#define EE_TRACE_CALLS 1
#endif
extern u32 g_ee_trace[4096];
extern u32 g_ee_trace_pos;
extern u32 g_ee_break;
void ee_break_hit(EEContext* c, u32 addr);
#if EE_TRACE_CALLS
#define EE_TRACE(c, addr)                                                    \
    do {                                                                     \
        g_ee_trace[g_ee_trace_pos++ & 4095] = (addr);                        \
        if (PS2_UNLIKELY((addr) == g_ee_break)) ee_break_hit((c), (addr));    \
    } while (0)
#else
#define EE_TRACE(c, addr) do { } while (0)
#endif

bool ee_cpcond0();  // BC0F/BC0T condition (DMA completion, see D_PCR/D_STAT)

// ---- RAM-only accessors (stack / gp relative: never hardware) ----------------
PS2_FORCEINLINE u32 ee_lw_ram(u32 a) { return *(const u32a*)(EE_BASE + a); }
PS2_FORCEINLINE u64 ee_ld_ram(u32 a) { return *(const u64a*)(EE_BASE + a); }
PS2_FORCEINLINE void ee_sw_ram(u32 a, u32 v) { *(u32a*)(EE_BASE + a) = v; }
PS2_FORCEINLINE void ee_sd_ram(u32 a, u64 v) { *(u64a*)(EE_BASE + a) = v; }
PS2_FORCEINLINE void ee_lq_ram(u32 a, qword* o) { *o = *(const qworda*)(EE_BASE + (a & ~15u)); }
PS2_FORCEINLINE void ee_sq_ram(u32 a, const qword& v) { *(qworda*)(EE_BASE + (a & ~15u)) = v; }

// ---- kernel-mode accessors ----------------------------------------------------
// Functions that run with ERL/EXL set (libkernel ERET helpers) may address the
// hardware through kseg0/kseg1 (0x9xxxxxxx / 0xBxxxxxxx); the code generator
// uses these variants inside such functions.
PS2_FORCEINLINE bool ee_is_hw_k(u32 a) {
    u32 n = a >> 28;
    return n == 1 || n == 9 || n == 0xB;
}
PS2_FORCEINLINE u8 ee_lb_k(u32 a) { return ee_is_hw_k(a) ? hw_read8(a) : ee_lb(a); }
PS2_FORCEINLINE u16 ee_lh_k(u32 a) { return ee_is_hw_k(a) ? hw_read16(a) : ee_lh(a); }
PS2_FORCEINLINE u32 ee_lw_k(u32 a) { return ee_is_hw_k(a) ? hw_read32(a) : ee_lw(a); }
PS2_FORCEINLINE u64 ee_ld_k(u32 a) { return ee_is_hw_k(a) ? hw_read64(a) : ee_ld(a); }
PS2_FORCEINLINE void ee_lq_k(u32 a, qword* o) {
    if (ee_is_hw_k(a)) hw_read128(a & ~15u, o);
    else ee_lq(a, o);
}
PS2_FORCEINLINE void ee_sb_k(u32 a, u8 v) { if (ee_is_hw_k(a)) hw_write8(a, v); else ee_sb(a, v); }
PS2_FORCEINLINE void ee_sh_k(u32 a, u16 v) { if (ee_is_hw_k(a)) hw_write16(a, v); else ee_sh(a, v); }
PS2_FORCEINLINE void ee_sw_k(u32 a, u32 v) { if (ee_is_hw_k(a)) hw_write32(a, v); else ee_sw(a, v); }
PS2_FORCEINLINE void ee_sd_k(u32 a, u64 v) { if (ee_is_hw_k(a)) hw_write64(a, v); else ee_sd(a, v); }
PS2_FORCEINLINE void ee_sq_k(u32 a, const qword& v) {
    if (ee_is_hw_k(a)) hw_write128(a & ~15u, v);
    else ee_sq(a, v);
}

// ---- unaligned load/store pairs (little endian, PCSX2 semantics) -------------
PS2_FORCEINLINE void ee_lwl(EEContext* c, int rt, u32 addr) {
    static const u32 mask[4] = {0x00ffffff, 0x0000ffff, 0x000000ff, 0x00000000};
    static const u8 shift[4] = {24, 16, 8, 0};
    u32 s = addr & 3;
    u32 mem = ee_lw(addr & ~3u);
    u32 v = (c->r[rt].w[0] & mask[s]) | (mem << shift[s]);
    if (rt) c->r[rt].d[0] = SE32(v);
}
PS2_FORCEINLINE void ee_lwr(EEContext* c, int rt, u32 addr) {
    static const u32 mask[4] = {0x00000000, 0xff000000, 0xffff0000, 0xffffff00};
    static const u8 shift[4] = {0, 8, 16, 24};
    u32 s = addr & 3;
    u32 mem = ee_lw(addr & ~3u);
    u32 v = (c->r[rt].w[0] & mask[s]) | (mem >> shift[s]);
    if (!rt) return;
    if (s == 0) c->r[rt].d[0] = SE32(v);
    else c->r[rt].w[0] = v;
}
PS2_FORCEINLINE void ee_ldl(EEContext* c, int rt, u32 addr) {
    u32 s = addr & 7;
    u64 mem = ee_ld(addr & ~7u);
    u64 mask = s == 7 ? 0 : (~0ull >> ((s + 1) * 8));
    u64 v = (c->r[rt].d[0] & mask) | (mem << ((7 - s) * 8));
    if (rt) c->r[rt].d[0] = v;
}
PS2_FORCEINLINE void ee_ldr(EEContext* c, int rt, u32 addr) {
    u32 s = addr & 7;
    u64 mem = ee_ld(addr & ~7u);
    u64 mask = s == 0 ? 0 : (~0ull << ((8 - s) * 8));
    u64 v = (c->r[rt].d[0] & mask) | (mem >> (s * 8));
    if (rt) c->r[rt].d[0] = v;
}
PS2_FORCEINLINE void ee_swl(EEContext* c, int rt, u32 addr) {
    static const u32 mask[4] = {0xffffff00, 0xffff0000, 0xff000000, 0x00000000};
    static const u8 shift[4] = {24, 16, 8, 0};
    u32 s = addr & 3;
    u32 mem = ee_lw(addr & ~3u);
    ee_sw(addr & ~3u, (c->r[rt].w[0] >> shift[s]) | (mem & mask[s]));
}
PS2_FORCEINLINE void ee_swr(EEContext* c, int rt, u32 addr) {
    static const u32 mask[4] = {0x00000000, 0x000000ff, 0x0000ffff, 0x00ffffff};
    static const u8 shift[4] = {0, 8, 16, 24};
    u32 s = addr & 3;
    u32 mem = ee_lw(addr & ~3u);
    ee_sw(addr & ~3u, (c->r[rt].w[0] << shift[s]) | (mem & mask[s]));
}
PS2_FORCEINLINE void ee_sdl(EEContext* c, int rt, u32 addr) {
    u32 s = addr & 7;
    u64 mem = ee_ld(addr & ~7u);
    u64 mask = s == 7 ? 0 : (~0ull << ((s + 1) * 8));
    ee_sd(addr & ~7u, (c->r[rt].d[0] >> ((7 - s) * 8)) | (mem & mask));
}
PS2_FORCEINLINE void ee_sdr(EEContext* c, int rt, u32 addr) {
    u32 s = addr & 7;
    u64 mem = ee_ld(addr & ~7u);
    u64 mask = s == 0 ? 0 : (~0ull >> ((8 - s) * 8));
    ee_sd(addr & ~7u, (c->r[rt].d[0] << (s * 8)) | (mem & mask));
}
