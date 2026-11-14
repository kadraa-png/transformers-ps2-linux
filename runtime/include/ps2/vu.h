#pragma once
// VU0 / VU1 vector units and the VIF0 / VIF1 interfaces.
//
// Contract used by other modules:
//  * Recompiled EE code includes "ps2/vu0_macro.h" and calls
//      vu0m_<mnemonic>(u32 insn_word)   for every COP2 macro instruction the
//                                       decoder (tools/r5900.py) names v*
//                                       (vaddbc, vmula, vclipw, vdiv, ... ,
//                                       vcallms, vcallmsr); the word is a
//                                       compile-time constant.
//      g_vu0.vf[n]                      for QMFC2/QMTC2/LQC2/SQC2 (vf0 is
//                                       read-only (0,0,0,1)).
//      vu0_cfc2(reg) / vu0_ctc2(reg,v)  CFC2/CTC2 (control regs 16..31 incl.
//                                       FBRST, CMSAR0, CMSAR1 (starts VU1)).
//      vu_cpcond2()                     BC2F/BC2T condition.
//  * The DMAC calls vif_dma(unit, data, qwc) for VIF0 (ch0) / VIF1 (ch1)
//    data and the hardware-register layer forwards VIF register / FIFO
//    accesses to vif_read_reg / vif_write_reg / vif_fifo_write.
//  * VU1 XGKICK calls gs::gif_path1(); VIF1 DIRECT/DIRECTHL calls
//    gs::gif_path2().
//  * VU0 execution is synchronous: VCALLMS / VIF0 MSCAL run the micro
//    program to its E-bit before returning.  VU1 is synchronous too unless
//    vu_thread_start() moved it to its own host thread (see
//    runtime/src/vu/vu_thread.h); then the EE side calls vu1_sync() before
//    it touches VU1 memory directly.
#include "ps2/types.h"

struct VURegs;
// The interpreter owns PCSX2's 16-byte VI register bank. This view retains
// the original public g_vuN.vi[index] syntax without duplicating registers.
struct VUIntView {
    VURegs* regs;
    u32& operator[](u32 index);
    const u32& operator[](u32 index) const;
};
struct VUState {
    qword* vf;       // aliases VURegs::VF[32], including read-only VF0
    VUIntView vi;    // aliases VURegs::VI[32].UL
    qword& acc;      // aliases VURegs::ACC
    u8* micro;       // micro (instruction) memory
    u8* data;        // data memory
    u32 micro_size;  // 4 KiB (VU0) / 16 KiB (VU1)
    u32 data_size;   // 4 KiB (VU0) / 16 KiB (VU1)
    void* impl;      // points to the corresponding PCSX2 VURegs
};

extern VUState g_vu0, g_vu1;

void vu_init();
void vu_reset(int unit);
void vu_exec(int unit, u32 start_pc_bytes);   // run until the E-bit program end
// Notify the VU dispatch cache after writes through VIF MPG or EE hardware
// micro-memory apertures. offset and bytes refer to the unit's micro memory.
void vu_micro_written(int unit, u32 offset, u32 bytes);
u32 vu0_cfc2(u32 reg);
void vu0_ctc2(u32 reg, u32 value);
void vu0_callms(u32 addr_bytes);
bool vu_cpcond2();
// Timing hooks remain part of the VU contract; they operate on the original
// PCSX2 VURegs pipeline through VUState::impl.
void vu_queue_q(VUState& vu, u32 bits, u32 latency);
void vu_queue_p(VUState& vu, u32 bits, u32 latency);
void vu_queue_flags(VUState& vu, u32 mac, u32 status);
void vu_queue_clip(VUState& vu, u32 clip);
void vu_wait_q(VUState& vu);
void vu_wait_p(VUState& vu);
void vu_set_vif_position(int unit, u32 top, u32 itop);

// VIF
void vif_init();
void vif_dma(int unit, const u8* data, u32 qwc);    // DMAC -> VIF (data consumed)
void vif_dma_words(int unit, const u32* words, u32 nwords); // DMAtag TTE upper 64 bits
u32 vif_read_reg(int unit, u32 offset);             // offset from 0x10003800 / 0x10003C00
void vif_write_reg(int unit, u32 offset, u32 value);
void vif_fifo_write(int unit, const qword& q);      // 0x10004000 / 0x10005000 FIFO writes
void vif_fifo_read(int unit, qword* q);
void vif_fifo_read_n(int unit, qword* q, u32 qwc);             // VIF1 FIFO read (GS -> EE download)
bool vif_path3_masked();                            // VIF1 MSKPATH3 latch for GIF/DMAC

// Asynchronous VU1 thread (after vu_init/vif_init and gs::init).  Disabled
// with PS2_VU_THREAD=0 and whenever the GS runs synchronously.
bool vu_thread_start();
void vu_thread_stop();                // drains queued work first
void vu1_sync();                      // EE: wait until VU1 finished everything queued
// EE store to VU1 micro (offset < 0x4000) / data (0x4000-0x7fff) memory,
// applied in order behind the queued work; false without the VU1 thread.
bool vu1_queue_store(u32 offset, const void* data, u32 bytes);
// VU1 D/T-bit interrupts raised on the VU1 thread are delivered by the EE:
// `wake` is called (from the VU1 thread) and the EE then polls vu1_take_irq().
void vu1_set_irq_wake(void (*wake)());
bool vu1_take_irq();
