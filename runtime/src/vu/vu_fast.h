#pragma once
#include "vu_compiled.h"
#include "vu_simd.h"
#include <cstring>
#ifndef VU_IMAGE_UNIT
#define VU_IMAGE_UNIT 1
#endif
#ifndef VUI
#define VUI vuRegs[VU_IMAGE_UNIT]
#endif

// The PCSX2 VU1 pair-retirement order with constant opcode words and direct
// VUops calls. Included after VUops.cpp's instruction bodies so they can inline
// with a constant VU.code, including lane selection and immediate decoding.
extern void vu_port_intc_irq(unsigned idx);
extern void vu_port_vif_vew_end(unsigned idx);
extern int vu1branch;
extern void _vuFlushAll(VURegs* VU);

template<u32 Word, vu_simd::Op Operation, vu_simd::Source Src,
         vu_simd::Target Dest, void(*Scalar)(), bool WithFlags = true,
         bool ClampA = true, bool ClampB = true, bool ClampAcc = true>
static inline void vu_fast_math() {
    if (vu_simd::available<Operation, Src>())
        vu_simd::arithmetic<Word, Operation, Src, Dest,
                            WithFlags ? vu_simd::FlagsFull : vu_simd::FlagsNone,
                            ClampA, ClampB, ClampAcc>(VU1);
    else
        Scalar();
}

// The authoritative fmac[] descriptors remain in VURegs. This transient
// occupancy mask only avoids scanning disabled slots on every instruction;
// recompute it when a generic interpreter pair or XGKICK touched those slots.
static inline void vu_fast_ensure_pipes(VUFastPipes& state, VURegs* VU) {
    if (state.valid) return;
    state.fmac_mask = 0;
    for (unsigned i = 0; i < 8; ++i)
        if (VU->fmac[i].enable) state.fmac_mask |= 1u << i;
    state.valid = true;
}

static inline void vu_fast_flush_pipes(VUFastPipes& state, VURegs* VU) {
    u32 remaining = state.fmac_mask;
    while (remaining) {
        const unsigned slot = __builtin_ctz(remaining);
        const u32 bit = 1u << slot;
        remaining &= ~bit;
        auto& pipe = VU->fmac[slot];
        if ((VU->cycle - pipe.sCycle) >= pipe.Cycle) {
            pipe.enable = 0;
            state.fmac_mask &= ~bit;
            VU->VI[REG_MAC_FLAG].UL = pipe.macflag;
            VU->VI[REG_STATUS_FLAG].UL = pipe.statusflag;
            VU->VI[REG_CLIP_FLAG].UL = pipe.clipflag;
        }
    }
    _vuFDIVflush(VU);
    _vuEFUflush(VU);
}

static inline void vu_fast_stall_fmac(VUFastPipes& state, VURegs* VU,
                                     int reg, int lanes) {
    u32 remaining = state.fmac_mask;
    while (remaining) {
        const unsigned slot = __builtin_ctz(remaining);
        const u32 bit = 1u << slot;
        remaining &= ~bit;
        auto& pipe = VU->fmac[slot];
        if (pipe.reg != reg || !(pipe.xyzw & lanes)) continue;
        pipe.enable = 0;
        state.fmac_mask &= ~bit;
        VU->VI[REG_MAC_FLAG].UL = pipe.macflag;
        VU->VI[REG_STATUS_FLAG].UL = pipe.statusflag;
        VU->VI[REG_CLIP_FLAG].UL = pipe.clipflag;
        VU->cycle = pipe.Cycle + pipe.sCycle + 1;
        vu_fast_flush_pipes(state, VU);
        return;
    }
}

template<const _VURegsNum& Analysis>
static inline void vu_fast_test_stalls(VUFastPipes& state, VURegs* VU) {
    if constexpr (Analysis.pipe == VUPIPE_FMAC) {
        if constexpr (Analysis.VFread0)
            vu_fast_stall_fmac(state, VU, Analysis.VFread0, Analysis.VFr0xyzw);
        if constexpr (Analysis.VFread1)
            vu_fast_stall_fmac(state, VU, Analysis.VFread1, Analysis.VFr1xyzw);
    } else if constexpr (Analysis.pipe == VUPIPE_FDIV) {
        _vuFlushFDIV(VU);
    } else if constexpr (Analysis.pipe == VUPIPE_EFU) {
        _vuFlushEFU(VU);
    }
}

static inline void vu_fast_add_fmac(VUFastPipes& state, VURegs* VU,
                                    int reg, int lanes) {
    const u32 free = (~state.fmac_mask) & 0xffu;
    if (!free) return; // PCSX2 drops the write when all eight pipes are full.
    const unsigned slot = __builtin_ctz(free);
    state.fmac_mask |= 1u << slot;
    auto& pipe = VU->fmac[slot];
    pipe.enable = 1;
    pipe.sCycle = VU->cycle;
    pipe.Cycle = 3;
    pipe.reg = reg;
    pipe.xyzw = lanes;
    pipe.macflag = VU->macflag;
    pipe.statusflag = VU->statusflag;
    pipe.clipflag = VU->clipflag;
}

template<const _VURegsNum& Analysis>
static inline void vu_fast_add_stalls(VUFastPipes& state, VURegs* VU) {
    if constexpr (Analysis.pipe == VUPIPE_FMAC) {
        if constexpr (Analysis.VFwrite)
            vu_fast_add_fmac(state, VU, Analysis.VFwrite, Analysis.VFwxyzw);
        else if constexpr (Analysis.VIwrite & (1 << REG_CLIP_FLAG))
            vu_fast_add_fmac(state, VU, -REG_CLIP_FLAG, 0);
        else
            vu_fast_add_fmac(state, VU, 0, 0);
    } else if constexpr (Analysis.pipe == VUPIPE_FDIV) {
        if constexpr (Analysis.VIwrite & (1 << REG_Q))
            _vuFDIVAdd(VU, Analysis.cycles);
    } else if constexpr (Analysis.pipe == VUPIPE_EFU) {
        if constexpr (Analysis.VIwrite & (1 << REG_P))
            _vuEFUAdd(VU, Analysis.cycles);
    }
}

template<u32 Index, u32 Lower, u32 Upper, void(*UpperOp)(), void(*LowerOp)(),
         const _VURegsNum& uregs, const _VURegsNum& lregs>
static inline void vu_fast_pair(VUFastPipes& state) {
    VURegs* VU = &VU1;
    vu_fast_ensure_pipes(state, VU);
    VECTOR old_vf, new_vf;
    REG_VI old_vi, new_vi;
    int vfreg = 0, vireg = 0, discard = 0;

    VU->VI[REG_TPC].UL += 8;
    if constexpr (Upper & 0x40000000u) VU->ebit = 2;
    if constexpr (Upper & 0x10000000u) {
        if (VU0.VI[REG_FBRST].UL & 0x400) {
            vpu_stat_set(0x200);
            vu_port_intc_irq(1);
            VU->ebit = 1;
        }
    }
    if constexpr (Upper & 0x08000000u) {
        if (VU0.VI[REG_FBRST].UL & 0x800) {
            vpu_stat_set(0x400);
            vu_port_intc_irq(1);
            VU->ebit = 1;
        }
    }

    VU->code = Upper;
#ifndef INT_VUSTALLHACK
    vu_fast_test_stalls<uregs>(state, VU);
#endif
    if constexpr (Upper & 0x80000000u) {
        VU->code = Upper;
        UpperOp();
        VU->VI[REG_I].UL = Lower;
    } else {
        VU->code = Lower;
#ifndef INT_VUSTALLHACK
        vu_fast_test_stalls<lregs>(state, VU);
#endif
        vu1branch = lregs.pipe == VUPIPE_BRANCH;
        if (uregs.VFwrite) {
            if (lregs.VFwrite == uregs.VFwrite) discard = 1;
            if (lregs.VFread0 == uregs.VFwrite || lregs.VFread1 == uregs.VFwrite) {
                old_vf = VU->VF[uregs.VFwrite];
                vfreg = uregs.VFwrite;
            }
        }
        if (uregs.VIread & (1 << REG_CLIP_FLAG)) {
            if (lregs.VIwrite & (1 << REG_CLIP_FLAG)) discard = 1;
            if (lregs.VIread & (1 << REG_CLIP_FLAG)) {
                old_vi = VU->VI[REG_CLIP_FLAG];
                vireg = REG_CLIP_FLAG;
            }
        }
        VU->code = Upper;
        UpperOp();
        if (discard == 0) {
            if (vfreg) {
                new_vf = VU->VF[vfreg];
                VU->VF[vfreg] = old_vf;
            }
            if (vireg) {
                new_vi = VU->VI[vireg];
                VU->VI[vireg] = old_vi;
            }
            VU->code = Lower;
            LowerOp();
            if constexpr (LowerOp == VU1MI_XGKICK) {
                state.valid = false;
                vu_fast_ensure_pipes(state, VU);
            }
            if (vfreg) VU->VF[vfreg] = new_vf;
            if (vireg) VU->VI[vireg] = new_vi;
        }
    }
    vu_fast_add_stalls<uregs>(state, VU);
    vu_fast_add_stalls<lregs>(state, VU);
    vu_fast_flush_pipes(state, VU);
    if (VU->VIBackupCycles > 0) --VU->VIBackupCycles;
    if (VU->branch > 0) {
        if (VU->branch-- == 1) {
            VU->VI[REG_TPC].UL = VU->branchpc;
            if (VU->takedelaybranch) {
                VU->branch = 2;
                VU->branchpc = VU->delaybranchpc;
                VU->delaybranchpc = 0;
                VU->takedelaybranch = false;
            }
        }
    }
    if (VU->ebit > 0) {
        if (VU->ebit-- == 1) {
            VU->VIBackupCycles = 0;
            _vuFlushAll(VU);
            vpu_stat_clear(0x100);
            vu_port_vif_vew_end(1);
        }
    }
    ++VU->cycle;
}

// Image graphs only (VUI: the image unit, see vu_graph.h): flatten pulls the arithmetic (and its clamp/flag
// helpers) into this wrapper, which itself inlines into the graph node.
template<u32 Word, vu_simd::Op Operation, vu_simd::Source Src,
         vu_simd::Target Dest, void(*Scalar)(), int FlagMode,
         bool ClampA, bool ClampB, bool ClampAcc>
[[gnu::always_inline, gnu::flatten]] static inline void vu_lean_math() {
    if (vu_simd::available<Operation, Src>())
        vu_simd::arithmetic<Word, Operation, Src, Dest, FlagMode, ClampA, ClampB, ClampAcc>(VUI);
    else
        Scalar();
}
