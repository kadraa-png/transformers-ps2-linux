#pragma once
// EE interrupt controller (INTC) and DMAC interrupt lines.
// Any module may raise these from the emulation thread; delivery to guest
// handlers happens at the next checkpoint.
#include "ps2/types.h"

enum IntcLine : u32 {
    INTC_GS = 0,
    INTC_SBUS = 1,
    INTC_VBLANK_START = 2,
    INTC_VBLANK_END = 3,
    INTC_VIF0 = 4,
    INTC_VIF1 = 5,
    INTC_VU0 = 6,
    INTC_VU1 = 7,
    INTC_IPU = 8,
    INTC_TIMER0 = 9,
    INTC_TIMER1 = 10,
    INTC_TIMER2 = 11,
    INTC_TIMER3 = 12,
    INTC_SFIFO = 13,
    INTC_VU0WD = 14,
};

enum DmacChannel : u32 {
    DMAC_VIF0 = 0,
    DMAC_VIF1 = 1,
    DMAC_GIF = 2,
    DMAC_FROM_IPU = 3,
    DMAC_TO_IPU = 4,
    DMAC_SIF0 = 5,
    DMAC_SIF1 = 6,
    DMAC_SIF2 = 7,
    DMAC_FROM_SPR = 8,
    DMAC_TO_SPR = 9,
};

void intc_raise(u32 line);      // sets I_STAT bit, interrupt delivered if unmasked
void dmac_raise(u32 channel);   // sets D_STAT CIS bit (channel interrupt status)
