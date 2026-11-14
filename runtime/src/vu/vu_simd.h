#pragma once

#include "../../../third_party/pcsx2_vu/VU.h"
#include <immintrin.h>
#include <type_traits>

// Arithmetic only: the caller owns instruction retirement, stalls, FMAC pipe
// insertion/flush, and VI flag visibility. Compile with FP contraction off:
// MADD rounds multiplication separately before adding ACC, never fused FMA.
// No MXCSR modification is needed; operands are clamped before arithmetic,
// and results are classified by bits.
namespace vu_simd {

enum class Op { Add, Sub, Mul, Madd, Msub };
enum class Source { Vector, X, Y, Z, W, I, Q };
enum class Target { VF, ACC };
struct Flags { u32 mac, status; };

// How much MAC/status work an arithmetic op does (tools/vu_image_emit.py
// proves per graph node which part any later reader can observe).
//   FlagsNone: nothing.
//   FlagsZero: only the four MAC zero bits, which a later op's update keeps
//              for lanes that overflow; the other MAC bits and status stay stale.
//   FlagsMac:  the exact MAC flag; status stays stale.
//   FlagsFull: exact MAC and status (the interpreter's behaviour).
constexpr int FlagsNone = 0, FlagsZero = 1, FlagsMac = 2, FlagsFull = 3;

// ADDi has a game-specific interpreter exception controlled at runtime.
// The generated caller must select its original scalar body in that mode.
template<Op Operation, Source Src>
inline bool available() {
    if constexpr (Operation == Op::Add && Src == Source::I)
        return !CHECK_VUADDSUBHACK;
    return true;
}

[[gnu::always_inline]] inline u32 update_status(VURegs& vu) {
    const u32 old_status = vu.statusflag;
    u32 new_status = 0;
    if (vu.macflag & 0x000f) new_status |= 1;
    if (vu.macflag & 0x00f0) new_status |= 2;
    if (vu.macflag & 0x0f00) new_status |= 4;
    if (vu.macflag & 0xf000) new_status |= 8;
    return vu.statusflag = (old_status & 0xc30) | new_status | ((old_status & 0xf) << 6);
}

// PCSX2's vuDouble: subnormal input -> signed zero, NaN/Inf input ->
// signed largest finite float. Integer comparisons also preserve NaN sign.
[[gnu::always_inline]] inline __m128 clamp(__m128 v) {
    const __m128i bits = _mm_castps_si128(v);
    const __m128i sign = _mm_and_si128(bits, _mm_set1_epi32(0x80000000u));
    const __m128i magnitude = _mm_and_si128(bits, _mm_set1_epi32(0x7fffffffu));
    const __m128i exponent = _mm_and_si128(magnitude, _mm_set1_epi32(0x7f800000));
    const __m128i zero = _mm_cmpeq_epi32(exponent, _mm_setzero_si128());
    const __m128i huge = _mm_cmpeq_epi32(exponent, _mm_set1_epi32(0x7f800000));
    const __m128i finite = _mm_or_si128(sign, _mm_set1_epi32(0x7f7fffff));
    const __m128i result = _mm_blendv_epi8(bits, finite, huge);
    return _mm_castsi128_ps(_mm_blendv_epi8(result, sign, zero));
}

// x/y/z/w in memory correspond to SIMD lanes 0/1/2/3, but the MAC
// register records them in bits 3/2/1/0 respectively.
// FlagMode: see FlagsNone..FlagsFull above.
template<u32 Word, Op Operation, Source Src = Source::Vector,
         Target Dst = Target::VF, int FlagMode = FlagsFull,
         bool ClampA = true, bool ClampB = true, bool ClampAcc = true>
inline Flags arithmetic(VURegs& vu) {
    constexpr unsigned fs = (Word >> 11) & 31;
    constexpr unsigned ft = (Word >> 16) & 31;
    constexpr unsigned fd = (Word >> 6) & 31;
    constexpr unsigned xyzw = (Word >> 21) & 15;
    constexpr unsigned lanes = ((xyzw & 8) ? 1 : 0) | ((xyzw & 4) ? 2 : 0) |
                               ((xyzw & 2) ? 4 : 0) | ((xyzw & 1) ? 8 : 0);
    if constexpr (xyzw == 0 && FlagMode == FlagsNone) {
        return {0, 0};
    } else if constexpr (xyzw == 0 && FlagMode == FlagsZero) {
        vu.macflag &= ~0xfu;
        return {vu.macflag, vu.statusflag};
    } else if constexpr (xyzw == 0) {
        vu.macflag &= ~0xffffu;
        if constexpr (FlagMode == FlagsFull) update_status(vu);
        return {vu.macflag, vu.statusflag};
    }
    constexpr int lane = Src == Source::X ? 0 : Src == Source::Y ? 1 :
                         Src == Source::Z ? 2 : 3;
    constexpr bool accumulate = Operation == Op::Madd || Operation == Op::Msub;
    constexpr bool uses_ft = Src != Source::I && Src != Source::Q;
    // Clamp*=false: the image graph proved those lanes already clean (no
    // denormal/Inf/NaN), where clamp() is the identity.
    const auto maybe = [](auto enabled, __m128 v) {
        if constexpr (decltype(enabled)::value) return clamp(v); else return v;
    };
    const __m128 a = maybe(std::bool_constant<ClampA>{}, _mm_loadu_ps(vu.VF[fs].F));
    __m128 b;
    if constexpr (Src == Source::Vector) {
        b = maybe(std::bool_constant<ClampB>{}, _mm_loadu_ps(vu.VF[ft].F));
    } else if constexpr (uses_ft) {
        // Broadcast before clamping, as in the interpreter's scalar bc path.
        b = maybe(std::bool_constant<ClampB>{}, _mm_castsi128_ps(_mm_set1_epi32(vu.VF[ft].UL[lane])));
    } else {
        constexpr unsigned reg = Src == Source::I ? REG_I : REG_Q;
        b = clamp(_mm_castsi128_ps(_mm_set1_epi32(vu.VI[reg].UL)));
    }
    __m128 result;
    if constexpr (Operation == Op::Add) result = _mm_add_ps(a, b);
    if constexpr (Operation == Op::Sub) result = _mm_sub_ps(a, b);
    if constexpr (Operation == Op::Mul) result = _mm_mul_ps(a, b);
    if constexpr (Operation == Op::Madd)
        result = _mm_add_ps(maybe(std::bool_constant<ClampAcc>{}, _mm_loadu_ps(vu.ACC.F)), _mm_mul_ps(a, b));
    if constexpr (Operation == Op::Msub)
        result = _mm_sub_ps(maybe(std::bool_constant<ClampAcc>{}, _mm_loadu_ps(vu.ACC.F)), _mm_mul_ps(a, b));

    const __m128i raw = _mm_castps_si128(result);
    const __m128i sign = _mm_srai_epi32(raw, 31);
    const __m128i exponent = _mm_and_si128(raw, _mm_set1_epi32(0x7f800000));
    const __m128i zero_exp = _mm_cmpeq_epi32(exponent, _mm_setzero_si128());
    const __m128i overflow = _mm_cmpeq_epi32(exponent, _mm_set1_epi32(0x7f800000));
    // VU_MAC_UPDATE distinguishes signed zero from subnormal results.
    const __m128i is_zero = _mm_cmpeq_epi32(
        _mm_and_si128(raw, _mm_set1_epi32(0x7fffffff)), _mm_setzero_si128());
    const __m128i underflow = _mm_andnot_si128(is_zero, zero_exp);
    const __m128i zero_flag = zero_exp;
    const __m128i output = _mm_blendv_epi8(
        _mm_blendv_epi8(raw, _mm_or_si128(_mm_and_si128(raw, _mm_set1_epi32(0x80000000u)),
                                           _mm_set1_epi32(0x7f7fffff)), overflow),
        _mm_and_si128(raw, _mm_set1_epi32(0x80000000u)), underflow);
    // MAC bit order: x is bit 3, w bit 0, the reverse of SIMD lanes 0..3.
    // Reversing the lanes before movemask yields that order directly.
    const auto flag_bits = [](__m128i v) -> u32 {
        return u32(_mm_movemask_ps(_mm_castsi128_ps(_mm_shuffle_epi32(v, 0x1b)))) & xyzw;
    };
    if constexpr (FlagMode == FlagsZero) {
        // Zero bits only: cleared for inactive and normal lanes, set for
        // zero/underflow results, unchanged for overflowing lanes.
        const u32 keep = flag_bits(overflow);
        vu.macflag = (vu.macflag & ~(15u & ~keep)) | flag_bits(zero_flag);
    }
    if constexpr (FlagMode >= FlagsMac) {
    // Match the PCSX2 per-lane MAC transitions and status history.
    constexpr u32 active = xyzw;
    const u32 zeros = flag_bits(zero_flag), unders = flag_bits(underflow);
    const u32 overs = flag_bits(overflow), signs = flag_bits(sign);
    const u32 normal = active & ~(zeros | overs);
    constexpr u32 inactive = (~active) & 15;
    const u32 clear = (inactive | normal) |
                      ((inactive | normal | zeros | overs) << 8) |
                      ((inactive | normal | zeros | unders) << 12) |
                      (15u << 4);
    vu.macflag = (vu.macflag & ~clear) | zeros | (signs << 4) |
                 (unders << 8) | (overs << 12);
    if constexpr (FlagMode == FlagsFull) update_status(vu);
    }

    if constexpr (Dst == Target::ACC) {
        const __m128i mask = _mm_setr_epi32((lanes & 1) ? -1 : 0,
                                           (lanes & 2) ? -1 : 0,
                                           (lanes & 4) ? -1 : 0,
                                           (lanes & 8) ? -1 : 0);
        const __m128i prior = _mm_loadu_si128(reinterpret_cast<const __m128i*>(vu.ACC.UL));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(vu.ACC.UL),
                         _mm_blendv_epi8(prior, output, mask));
    } else if constexpr (fd != 0) {
        const __m128i mask = _mm_setr_epi32((lanes & 1) ? -1 : 0,
                                           (lanes & 2) ? -1 : 0,
                                           (lanes & 4) ? -1 : 0,
                                           (lanes & 8) ? -1 : 0);
        const __m128i prior = _mm_loadu_si128(reinterpret_cast<const __m128i*>(vu.VF[fd].UL));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(vu.VF[fd].UL),
                         _mm_blendv_epi8(prior, output, mask));
    }
    return {vu.macflag, vu.statusflag};
}
} // namespace vu_simd
