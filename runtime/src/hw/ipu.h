#pragma once
// IPU (MPEG-2 macroblock decoder) used by Sony libmpeg for the PSS movies.
//
// Contract with the DMAC/hardware layer (runtime/src/hw/hwregs.cpp):
//  * registers 0x10002000-0x1000203F (IPU_CMD 0x00, IPU_CTRL 0x10, IPU_BP 0x20,
//    IPU_TOP 0x30) -> read_reg/write_reg (offset from 0x10002000; IPU_CMD and
//    IPU_TOP reads are 64-bit: read_reg64);
//  * FIFOs: 0x10007000 (out, EE reads) and 0x10007010 (in, EE writes);
//  * DMA channels 3 (fromIPU) and 4 (toIPU) are owned by this module: when the
//    EE sets CHCR.STR the DMAC calls dma_start(ch); the IPU moves data at its
//    own pace using hw::dma_chan(ch) (normal mode for ch3, normal/chain for
//    ch4), updating MADR/QWC/TADR/CHCR like the hardware, and calls
//    hw::dma_done(ch) when the transfer completes;
//  * command completion raises INTC_IPU via intc_raise().
#include "ps2/types.h"

namespace ipu {
void init();
u32 read_reg(u32 offset);
u64 read_reg64(u32 offset);
void write_reg(u32 offset, u32 value);
void write_reg64(u32 offset, u64 value);
void write_fifo(const qword& q);
void read_fifo(qword* q);
void dma_start(u32 ch);
} // namespace ipu
