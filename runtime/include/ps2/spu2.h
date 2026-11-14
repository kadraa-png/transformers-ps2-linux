#pragma once
// SPU2 sound processor (register-level), owned by runtime/src/spu2.
//
// Called from the emulation thread only:
//  * the IOP hardware layer forwards IOP accesses to 0x1F900000-0x1F9007FF
//    (core 0/1 registers, 0x1F900760+ shared registers) as 16-bit reads/writes;
//  * IOP DMA channel 4 (core 0) / 7 (core 1) transfers call dma_write/dma_read
//    (manual transfers to the current TSA, or AutoDMA streaming when the
//    core's ADMA register is enabled);
//  * SPU2 time follows the host clock at 48 kHz: frames_due() reports how
//    many output frames are owed, advance() produces them (output goes to
//    platform::audio_push()).  Every register/DMA access catches up first, so
//    the IOP sees the SPU2 progress between its accesses;
//  * SPU2 interrupts (IRQA hit, DMA completion, AutoDMA half/full buffer
//    requests) are reported through the callbacks below; the IOP raises the
//    corresponding IOP INTC lines (SPU2 = 9, DMA4/DMA7 completion).
#include "ps2/types.h"

namespace spu2 {

void init();
void reset();
void shutdown();

u16 read16(u32 iop_addr);
void write16(u32 iop_addr, u16 value);

// DMA between IOP RAM and SPU2 RAM.  `halfwords` counts 16-bit units.
// Completion is signalled with the dma callback (after the transfer finishes).
void dma_write(int core, const u16* data, u32 halfwords);
void dma_read(int core, u16* data, u32 halfwords);

// Produce `frames` stereo frames (48 kHz) and push them to the platform.
void advance(u32 frames);
// Frames owed by the host clock (stalls > 100 ms are skipped, not replayed).
u32 frames_due();
// Shift the clock by `frames` (positive = produce more): audio queue steering.
void nudge(s32 frames);
// Catch up to the host clock now (IOP polling of SPU2 DMA channel state).
void sync();

void set_irq_callback(void (*cb)(int core));     // IRQ address match on a core
// DMA transfer completed.  AutoDMA: once the transfer's last 512-halfword block
// has been fetched into the core's input buffer (upstream SPU2-X timing).
void set_dma_callback(void (*cb)(int core));

void debug_dump();   // voice/envelope state to stderr (SIGQUIT diagnostics)

} // namespace spu2
