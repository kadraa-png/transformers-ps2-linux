#pragma once
// Basic fixed-width types shared by every runtime module and the generated code.
#include <cstdint>
#include <cstddef>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef unsigned __int128 u128;
typedef __int128 s128;

// One 128-bit PS2 quadword with every lane view the hardware uses.
// may_alias: guest memory is read/written through qword and narrower views.
union alignas(16) qword {
    u128 q;
    u64 d[2];
    s64 sd[2];
    u32 w[4];
    s32 sw[4];
    u16 h[8];
    s16 sh[8];
    u8 b[16];
    s8 sb[16];
    float f[4];
} __attribute__((may_alias));
static_assert(sizeof(qword) == 16, "qword must be 16 bytes");

#define PS2_LIKELY(x) __builtin_expect(!!(x), 1)
#define PS2_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define PS2_NOINLINE __attribute__((noinline))
#define PS2_FORCEINLINE inline __attribute__((always_inline))
#define PS2_NORETURN [[noreturn]]
