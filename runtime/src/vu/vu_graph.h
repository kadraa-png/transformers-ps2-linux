#pragma once
#include "vu_compiled.h"
#include "ps2/types.h"
#include "vu_simd.h"
#include "vu_lower.h"
#include <cstring>

// Whole-image graphs are compiled once per unit: an image shard defines
// VU_IMAGE_UNIT 0 for VU0 programs; everything else targets VU1.
// VU0 images never use vu_lean_kick or vu_graph_lower_lq/sq (VU1 addressing).
#ifndef VU_IMAGE_UNIT
#define VU_IMAGE_UNIT 1
#endif
#ifndef VUI
#define VUI vuRegs[VU_IMAGE_UNIT]
#endif
extern int vu0branch;

// A generated graph node has already proved the physical slot and cycle of
// each hazard. Payloads and flag values stay in VURegs and remain exact.
static PS2_FORCEINLINE void vu_graph_flush_fmac(unsigned slot) {
    auto& pipe = VUI.fmac[slot];
    pipe.enable = 0;
    VUI.VI[REG_MAC_FLAG].UL = pipe.macflag;
    VUI.VI[REG_STATUS_FLAG].UL = pipe.statusflag;
    VUI.VI[REG_CLIP_FLAG].UL = pipe.clipflag;
}
static PS2_FORCEINLINE void vu_graph_add_fmac(unsigned slot, int reg, int lanes) {
    auto& pipe = VUI.fmac[slot];
    pipe.enable = 1;
    pipe.sCycle = VUI.cycle;
    pipe.Cycle = 3;
    pipe.reg = reg;
    pipe.xyzw = lanes;
    pipe.macflag = VUI.macflag;
    pipe.statusflag = VUI.statusflag;
    pipe.clipflag = VUI.clipflag;
}
// Graphs start from empty pipelines.  A pending VI backup is checked
// separately (vu_graph_backup_entry) because it is cheap to prove harmless.
static inline bool vu_graph_empty_entry_of(const VURegs& vu) {
    if (vu.fdiv.enable || vu.efu.enable || vu.branch || vu.ebit) return false;
    for (const auto& pipe : vu.fmac) if (pipe.enable) return false;
    return true;
}
static inline bool vu_graph_empty_entry() { return vu_graph_empty_entry_of(VUI); }
// Safe: the graph's first two pairs hold no conditional branch, the only
// readers of the backed-up VI value, so the backup may expire at once.
template<bool Safe>
static inline bool vu_graph_backup_entry_of(VURegs& vu) {
    if (!vu.VIBackupCycles) return true;
    if constexpr (!Safe) return false;
    vu.VIBackupCycles = 0;
    return true;
}
template<bool Safe>
static inline bool vu_graph_backup_entry() { return vu_graph_backup_entry_of<Safe>(VUI); }
static inline bool vu_graph_pair_matches(u32 pc, u64 expected) {
    u64 actual;
    std::memcpy(&actual, VUI.Micro + pc, sizeof(actual));
    return actual == expected;
}

template<u32 Word> static inline void vu_graph_lower_move() { vu_lower_move<Word>(VUI); }
template<u32 Word> static inline void vu_graph_lower_lq() { vu_lower_lq<Word>(VUI); }
template<u32 Word> static inline void vu_graph_lower_sq() { vu_lower_sq<Word>(VUI); }
template<u32 Word> static inline void vu_graph_lower_iaddiu() { vu_lower_iaddiu<Word>(VUI); }

// Tpc=false: whole-image graphs keep TPC static and store it only before
// the operations that read it (branches, links) and at exits.
template<u32 Lower, u32 Upper, void(*UpperOp)(),
         void(*LowerOp)(), const _VURegsNum& uregs, const _VURegsNum& lregs,
         bool Tpc = true>
static PS2_FORCEINLINE void vu_graph_ops() {
    VURegs* VU = &VUI;
    VECTOR old_vf, new_vf;
    REG_VI old_vi, new_vi;
    int vfreg = 0, vireg = 0, discard = 0;
    if constexpr (Tpc) VU->VI[REG_TPC].UL += 8;
    if constexpr (Upper & 0x40000000u) VU->ebit = 2;
    if constexpr (Upper & 0x20000000u) VU->flags |= VUFLAG_MFLAGSET;
    // D/T-flagged nodes have an unknown runtime FBRST decision and must not
    // be included in the static graph.
    static_assert(!(Upper & 0x18000000u));
    VU->code = Upper;
    if constexpr (Upper & 0x80000000u) {
        UpperOp();
        VU->VI[REG_I].UL = Lower;
    } else {
        (VU_IMAGE_UNIT ? vu1branch : vu0branch) = lregs.pipe == VUPIPE_BRANCH;
        if constexpr (uregs.VFwrite) {
            if constexpr (lregs.VFwrite == uregs.VFwrite) discard = 1;
            if constexpr (lregs.VFread0 == uregs.VFwrite || lregs.VFread1 == uregs.VFwrite) {
                old_vf = VU->VF[uregs.VFwrite];
                vfreg = uregs.VFwrite;
            }
        }
        if constexpr (uregs.VIread & (1 << REG_CLIP_FLAG)) {
            if constexpr (lregs.VIwrite & (1 << REG_CLIP_FLAG)) discard = 1;
            if constexpr (lregs.VIread & (1 << REG_CLIP_FLAG)) {
                old_vi = VU->VI[REG_CLIP_FLAG];
                vireg = REG_CLIP_FLAG;
            }
        }
        UpperOp();
        if (!discard) {
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
            if (vfreg) VU->VF[vfreg] = new_vf;
            if (vireg) VU->VI[vireg] = new_vi;
        } else {
            // In the interpreter _vu1ExecLower is not called after a VF/VI
            // write conflict, leaving the upper opcode as the current code.
        }
    }
}

template<bool MayBackup, bool MayBranch, bool MayEnd>
static PS2_FORCEINLINE void vu_graph_retire() {
    if constexpr (MayBackup)
        if (VUI.VIBackupCycles > 0) --VUI.VIBackupCycles;
    if constexpr (MayBranch)
        if (VUI.branch > 0 && VUI.branch-- == 1) {
            VUI.VI[REG_TPC].UL = VUI.branchpc;
            if (VUI.takedelaybranch) {
                VUI.branch = 2;
                VUI.branchpc = VUI.delaybranchpc;
                VUI.delaybranchpc = 0;
                VUI.takedelaybranch = false;
            }
        }
    if constexpr (MayEnd)
        if (VUI.ebit > 0 && VUI.ebit-- == 1) {
            VUI.VIBackupCycles = 0;
            _vuFlushAll(&VUI);
            vpu_stat_clear(VU_IMAGE_UNIT ? 0x100u : 0x1u);
            vu_port_vif_vew_end(VU_IMAGE_UNIT);
        }
    ++VUI.cycle;
}

// ---- whole-image graphs (tools/vu_image_emit.py) ----------------------------
// Interpreter pipeline state at a graph exit; remaining cycles are relative
// to the next pair (-1 = empty), branch/delayed are pair pcs (-2 = none,
// -1 = the runtime target already in VUI.branchpc).
struct VULeanExit {
    s16 pc, branch, delayed;
    s8 ebit, fdiv_r;
    u8 fdiv_cycles;
    s8 efu_r;
    u8 efu_cycles;
    s8 fr[8], freg[8];
    u8 flanes[8];
};
// Slot payloads the interpreter would hold in fmac[]/fdiv/efu.
struct VULeanPayload {
    u32 sm[8], ss[8], sc[8];
    u32 qv, qs, pv;
};
[[gnu::noinline, gnu::cold]] static u32 vu_lean_ret(const VULeanExit& e, u32 cyc,
                                                   const VULeanPayload& p, u32 done) {
    VUI.VI[REG_TPC].UL = u32(e.pc & 2047) * 8;
    VUI.cycle = cyc;
    for (unsigned i = 0; i < 8; ++i) {
        auto& pipe = VUI.fmac[i];
        if (e.fr[i] < 0) { pipe.enable = 0; continue; }
        pipe.enable = 1;
        pipe.sCycle = cyc - (3 - e.fr[i]);
        pipe.Cycle = 3;
        pipe.reg = e.freg[i];
        pipe.xyzw = e.flanes[i];
        pipe.macflag = p.sm[i];
        pipe.statusflag = p.ss[i];
        pipe.clipflag = p.sc[i];
    }
    if (e.fdiv_r < 0) VUI.fdiv.enable = 0;
    else {
        VUI.fdiv.enable = 1;
        VUI.fdiv.Cycle = e.fdiv_cycles;
        VUI.fdiv.sCycle = cyc - (e.fdiv_cycles - e.fdiv_r);
        VUI.fdiv.reg.UL = p.qv;
        VUI.fdiv.statusflag = p.qs;
    }
    if (e.efu_r < 0) VUI.efu.enable = 0;
    else {
        VUI.efu.enable = 1;
        VUI.efu.Cycle = e.efu_cycles;
        VUI.efu.sCycle = cyc - (e.efu_cycles - e.efu_r);
        VUI.efu.reg.UL = p.pv;
    }
    if (e.branch == -2) VUI.branch = 0;
    else {
        VUI.branch = 1;
        if (e.branch >= 0) VUI.branchpc = u32(e.branch) * 8;
    }
    if (e.delayed == -2) VUI.takedelaybranch = false;
    else {
        VUI.takedelaybranch = true;
        VUI.delaybranchpc = u32(e.delayed) * 8;
    }
    VUI.ebit = e.ebit;
    return done;
}
static void vu_lean_nop() {}
// XGKICK after the graph published the drained pipes (the interpreter body
// runs _vuFlushAll first).
template<u32 Lower>
static PS2_FORCEINLINE void vu_lean_kick() {
    constexpr unsigned is = (Lower >> 11) & 15;
    vu_port_xgkick(1, VUI.Mem, (VUI.VI[is].US[0] & 0x3ffu) * 16);
}

// Calls a PCSX2 opcode body (all are __fi) inline right after the graph's
// constant store to VUI.code, so its field decoding folds to constants.
template<void(*Body)(VURegs*)>
[[gnu::always_inline, gnu::flatten]] static inline void vu_inl() { Body(&VUI); }
