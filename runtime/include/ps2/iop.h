#pragma once
// IOP subsystem interface (EE kernel <-> IOP).
//
// The IOP is not emulated: its kernel/system modules are HLE'd in C++ and the
// game's own IRX modules are statically recompiled (tools/recomp_iop.py).  IOP
// threads are fibers that run on the emulation thread, interleaved with the
// EE: the EE kernel calls iop::run() whenever the IOP may have work (after SIF
// traffic, when all EE threads are blocked, on IOP timer/vblank events).
//
// SIF model (see ps2sdk sifcmd/sifrpc):
//  * EE -> IOP: the EE kernel's sceSifSetDma syscall calls ee_sif_set_dma();
//    data is copied into IOP RAM immediately.  A copy that targets the IOP
//    command buffer (address published in SIF_REG_SUBADDR / SMCOM) is a SIF
//    command packet and is dispatched to the IOP command handlers.
//  * IOP -> EE: the IOP copies extra data straight into EE RAM and queues
//    command packets for the EE packet buffer (address received through
//    SIF_CMD_CHANGE_SADDR).  One queued packet is delivered at a time: it is
//    copied to the EE buffer and the EE DMAC SIF0 (channel 5) interrupt is
//    raised with dmac_raise(DMAC_SIF0).  The next packet is delivered when the
//    EE re-arms the channel (sceSifSetDChain -> ee_sif_set_dchain()).
#include "ps2/types.h"

namespace iop {

constexpr u32 IOP_RAM_SIZE = 2u << 20;
extern u8* g_iop_ram;   // 2 MiB IOP RAM (host pointer)

struct SifDmaXfer {     // EE layout of sceSifDmaData (16 bytes)
    u32 src;            // EE address
    u32 dest;           // IOP address
    u32 size;           // bytes
    u32 attr;           // SIF_DMA_INT_I / SIF_DMA_INT_O / SIF_DMA_ERT ...
};

void init();                                        // after iso::open()
void shutdown();

// EE kernel syscall backends.
u32 ee_sif_set_dma(const SifDmaXfer* xfers, u32 count);   // returns transfer id (> 0)
s32 ee_sif_dma_stat(u32 id);                       // <0 complete, >=0 pending (always complete here)
void ee_sif_set_dchain();                          // EE re-armed SIF0
u32 ee_sif_get_reg(u32 reg);
void ee_sif_set_reg(u32 reg, u32 value);

// EE hardware register window 0x1000F200-0x1000F2FF (MSCOM/SMCOM/MSFLG/SMFLG/CTRL...).
u32 sif_hw_read(u32 offset);
void sif_hw_write(u32 offset, u32 value);

// Scheduling.
void run();                 // run IOP threads / handlers until every IOP thread is blocked
bool has_work();            // something runnable or a due timer
u64 next_deadline_ns();     // earliest IOP timer/alarm deadline (platform::now_ns() clock), ~0 if none
void vblank(bool start);    // IOP VBLANK start / end (vblank module, Sirens tick)
void debug_dump();          // print IOP thread states to stderr (SIGQUIT diagnostics)

// Internal IOP memory/command transport used by the recompiled IRX runtime and
// HLE kernel. Addresses are guest IOP physical or kseg0/kseg1 aliases.
u8 read8(u32 address);
u16 read16(u32 address);
u32 read32(u32 address);
void write8(u32 address, u8 value);
void write16(u32 address, u16 value);
void write32(u32 address, u32 value);
void send_packet(u32 source, u32 bytes, u32 extra_source = 0,
                 u32 extra_destination = 0, u32 extra_bytes = 0);
// Queue a Sony FILEIO completion. slot selects an EE record at the registered
// base + slot * 0x440; the record is copied before delivering its SIF command.
void send_fileio_completion(u32 ee_destination, const void* data, u32 size, u32 slot = 0);
bool module_loaded(u32 base);
u32 dma_to_ee(u32 source, u32 destination, u32 bytes);

} // namespace iop
