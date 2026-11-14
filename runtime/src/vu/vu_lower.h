#pragma once
#include "VUops.h"
#include <cstdint>

// Direct VU1 micro-memory and vector moves for the static block emitter.
// Addresses remain 16-bit VU byte addresses and wrap at VU1's 16 KiB aperture.
// Caller handles upper/lower overlap and pipe retirement before/after the op.
template<u32 Word>
inline void vu_lower_move(VURegs& vu) {
    constexpr unsigned source = (Word >> 11) & 31;
    constexpr unsigned target = (Word >> 16) & 31;
    constexpr unsigned lanes = (Word >> 21) & 15;
    if constexpr (target != 0) {
        if constexpr (lanes & 8) vu.VF[target].UL[0] = vu.VF[source].UL[0];
        if constexpr (lanes & 4) vu.VF[target].UL[1] = vu.VF[source].UL[1];
        if constexpr (lanes & 2) vu.VF[target].UL[2] = vu.VF[source].UL[2];
        if constexpr (lanes & 1) vu.VF[target].UL[3] = vu.VF[source].UL[3];
    }
}

template<u32 Word>
inline void vu_lower_lq(VURegs& vu) {
    constexpr unsigned source = (Word >> 11) & 15;
    constexpr unsigned target = (Word >> 16) & 31;
    constexpr unsigned lanes = (Word >> 21) & 15;
    constexpr int imm = (Word & 0x400u) ? int(Word & 0x3ffu) - 1024 : int(Word & 0x3ffu);
    if constexpr (target != 0) {
        const u16 byte_address = u16((imm + vu.VI[source].SS[0]) * 16);
        const auto* data = reinterpret_cast<const u32*>(vu.Mem + (byte_address & 0x3fffu));
        if constexpr (lanes & 8) vu.VF[target].UL[0] = data[0];
        if constexpr (lanes & 4) vu.VF[target].UL[1] = data[1];
        if constexpr (lanes & 2) vu.VF[target].UL[2] = data[2];
        if constexpr (lanes & 1) vu.VF[target].UL[3] = data[3];
    }
}

template<u32 Word>
inline void vu_lower_sq(VURegs& vu) {
    constexpr unsigned source = (Word >> 11) & 31;
    constexpr unsigned target = (Word >> 16) & 15;
    constexpr unsigned lanes = (Word >> 21) & 15;
    constexpr int imm = (Word & 0x400u) ? int(Word & 0x3ffu) - 1024 : int(Word & 0x3ffu);
    const u16 byte_address = u16((imm + vu.VI[target].SS[0]) * 16);
    auto* data = reinterpret_cast<u32*>(vu.Mem + (byte_address & 0x3fffu));
    if constexpr (lanes & 8) data[0] = vu.VF[source].UL[0];
    if constexpr (lanes & 4) data[1] = vu.VF[source].UL[1];
    if constexpr (lanes & 2) data[2] = vu.VF[source].UL[2];
    if constexpr (lanes & 1) data[3] = vu.VF[source].UL[3];
}

template<u32 Word>
inline void vu_lower_iaddiu(VURegs& vu) {
    constexpr unsigned source = (Word >> 11) & 15;
    constexpr unsigned target = (Word >> 16) & 15;
    constexpr int imm = ((Word >> 10) & 0x7800u) | (Word & 0x7ffu);
    if constexpr (target != 0) {
        vu.VIBackupCycles = 2;
        vu.VIRegNumber = target;
        vu.VIOldValue = vu.VI[target].US[0];
        vu.VI[target].SS[0] = vu.VI[source].SS[0] + imm;
    }
}
