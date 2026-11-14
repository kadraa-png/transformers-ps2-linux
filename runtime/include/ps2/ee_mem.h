#pragma once
// EE guest address space.
//
// The whole 32-bit EE virtual address space is a 4 GiB host reservation at the
// fixed address EE_BASE, so guest address `a` lives at EE_BASE + a:
//   0x00000000-0x01FFFFFF  main RAM (32 MiB)
//   0x20000000-0x21FFFFFF  RAM mirror (uncached)            -> same pages
//   0x30000000-0x31FFFFFF  RAM mirror (uncached accelerated) -> same pages
//   0x70000000-0x70003FFF  scratchpad (16 KiB)
//   0x80000000-0x81FFFFFF  kseg0 RAM mirror                 -> same pages
//   0xA0000000-0xA1FFFFFF  kseg1 RAM mirror                 -> same pages
// Everything else is PROT_NONE (a stray access faults and is reported).
// Hardware registers (0x1000_0000-0x1FFF_FFFF) are never accessed through the
// reservation: every access with (a >> 28) == 1 takes the hw_* slow path.
#include "ps2/types.h"
#include <cstring>

#define EE_BASE ((u8*)0x0000200000000000ULL)
constexpr u32 EE_RAM_SIZE = 32u << 20;
constexpr u32 EE_RAM_MASK = EE_RAM_SIZE - 1;
constexpr u32 EE_SPR_ADDR = 0x70000000u;
constexpr u32 EE_SPR_SIZE = 16u << 10;

// Alias-agnostic access types: guest memory is accessed with every width.
typedef u8 __attribute__((may_alias)) u8a;
typedef u16 __attribute__((may_alias, aligned(1))) u16a;
typedef u32 __attribute__((may_alias, aligned(1))) u32a;
typedef u64 __attribute__((may_alias, aligned(1))) u64a;
typedef float __attribute__((may_alias, aligned(1))) f32a;
typedef qword qworda;  // qword itself is may_alias (ps2/types.h)

// Map the address space.  Must be called once before any guest code runs.
void ee_mem_init();

// Hardware register slow path (implemented in runtime/src/hw/hwregs.cpp).
u8 hw_read8(u32 a);
u16 hw_read16(u32 a);
u32 hw_read32(u32 a);
u64 hw_read64(u32 a);
void hw_read128(u32 a, qword* out);
void hw_write8(u32 a, u8 v);
void hw_write16(u32 a, u16 v);
void hw_write32(u32 a, u32 v);
void hw_write64(u32 a, u64 v);
void hw_write128(u32 a, const qword& v);

#define EE_IS_HW(a) (((a) >> 28) == 1u)

PS2_FORCEINLINE u8* ee_host(u32 a) { return EE_BASE + a; }

// Translate a DMA (physical) address: bit 31 selects scratchpad.
PS2_FORCEINLINE u8* ee_dma_ptr(u32 madr) {
    if (madr & 0x80000000u)
        return EE_BASE + EE_SPR_ADDR + (madr & (EE_SPR_SIZE - 1));
    return EE_BASE + (madr & EE_RAM_MASK);
}

// ---- loads -------------------------------------------------------------------
PS2_FORCEINLINE u8 ee_lb(u32 a) {
    if (PS2_UNLIKELY(EE_IS_HW(a))) return hw_read8(a);
    return *(const u8a*)(EE_BASE + a);
}
PS2_FORCEINLINE u16 ee_lh(u32 a) {
    if (PS2_UNLIKELY(EE_IS_HW(a))) return hw_read16(a);
    return *(const u16a*)(EE_BASE + a);
}
PS2_FORCEINLINE u32 ee_lw(u32 a) {
    if (PS2_UNLIKELY(EE_IS_HW(a))) return hw_read32(a);
    return *(const u32a*)(EE_BASE + a);
}
PS2_FORCEINLINE u64 ee_ld(u32 a) {
    if (PS2_UNLIKELY(EE_IS_HW(a))) return hw_read64(a);
    return *(const u64a*)(EE_BASE + a);
}
// LQ/SQ/LQC2/SQC2 ignore the low 4 address bits.
PS2_FORCEINLINE void ee_lq(u32 a, qword* out) {
    a &= ~15u;
    if (PS2_UNLIKELY(EE_IS_HW(a))) { hw_read128(a, out); return; }
    *out = *(const qworda*)(EE_BASE + a);
}

// ---- stores ------------------------------------------------------------------
PS2_FORCEINLINE void ee_sb(u32 a, u8 v) {
    if (PS2_UNLIKELY(EE_IS_HW(a))) { hw_write8(a, v); return; }
    *(u8a*)(EE_BASE + a) = v;
}
PS2_FORCEINLINE void ee_sh(u32 a, u16 v) {
    if (PS2_UNLIKELY(EE_IS_HW(a))) { hw_write16(a, v); return; }
    *(u16a*)(EE_BASE + a) = v;
}
PS2_FORCEINLINE void ee_sw(u32 a, u32 v) {
    if (PS2_UNLIKELY(EE_IS_HW(a))) { hw_write32(a, v); return; }
    *(u32a*)(EE_BASE + a) = v;
}
PS2_FORCEINLINE void ee_sd(u32 a, u64 v) {
    if (PS2_UNLIKELY(EE_IS_HW(a))) { hw_write64(a, v); return; }
    *(u64a*)(EE_BASE + a) = v;
}
PS2_FORCEINLINE void ee_sq(u32 a, const qword& v) {
    a &= ~15u;
    if (PS2_UNLIKELY(EE_IS_HW(a))) { hw_write128(a, v); return; }
    *(qworda*)(EE_BASE + a) = v;
}

// Host-side helpers for runtime code (no hardware path, RAM/scratchpad only).
PS2_FORCEINLINE u32 ee_read32_ram(u32 a) { return *(const u32a*)(EE_BASE + a); }
PS2_FORCEINLINE void ee_write32_ram(u32 a, u32 v) { *(u32a*)(EE_BASE + a) = v; }
inline const char* ee_cstr(u32 a) { return (const char*)(EE_BASE + a); }
