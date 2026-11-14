#pragma once
// Hardware-module internals shared with the EE kernel.
#include "ps2/types.h"

namespace hw {

void init();
// Fold events produced on other host threads (GS SIGNAL/FINISH/LABEL) into
// the interrupt controller.  Emulation thread only.
void poll_async();
void debug_dump();   // DMAC / VIF / VU status to stderr (SIGQUIT diagnostics)

// INTC (0x1000F000 I_STAT, 0x1000F010 I_MASK)
extern u32 g_istat;
extern u32 g_imask;
// DMAC D_STAT (0x1000E010): bits 0-9 CIS, 13 SIS, 14 MEIS, 15 BEIS, 16-25 CIM, 29 SIM, 30 MEIM
extern u32 g_dstat;

inline u32 intc_pending() { return g_istat & g_imask; }
inline u32 dmac_pending() { return (g_dstat & 0x3FFu) & ((g_dstat >> 16) & 0x3FFu); }

// GS privileged-register bits owned by the runtime (VSINT/HSINT, IMR).
void gs_vblank_start();           // set CSR.VSINT, raise INTC_GS when VSMSK clear
u64 gs_read_csr();
void gs_write_imr(u64 v);
u64 gs_read_imr();

// EE timers 0-3 (0x10000000..0x10001830)
void timers_service(u64 now_ns);  // raise due timer interrupts
u64 timers_next_deadline_ns();    // ~0 if none armed


// DMA channel register block.  Channels 3 (fromIPU) and 4 (toIPU) are driven
// by the IPU module: the DMAC calls ipu::dma_start(ch) when CHCR.STR is set and
// the IPU module transfers at its own pace (updating the registers) and calls
// dma_done(ch) when the transfer is complete.
struct DmaChan {
    u32 chcr = 0, madr = 0, qwc = 0, tadr = 0, asr0 = 0, asr1 = 0, sadr = 0;
};
DmaChan& dma_chan(u32 ch);
void dma_done(u32 ch);   // clear STR, set D_STAT.CIS, re-evaluate interrupts
// D_PCR / D_STAT based condition for BC0F/BC0T.
bool cpcond0();

} // namespace hw
