#pragma once
// Asynchronous VU1 ("MTVU", after PCSX2 1.6 MTVU.cpp).
//
// The EE thread keeps parsing VIF1 and queues everything that touches VU1
// state, in order, to a dedicated host thread: micro-program starts, MPG
// uploads, VIF1 unpacks (with a copy of the unpack state) and ROW/COL
// updates.  The VU1 thread owns VU1's registers and memories plus that copy
// (vu1t_vif / vu1t_vifRegs); the EE side must vu1_sync() before touching
// them directly.  PATH1 output stays in EE submission order: every queued
// start leaves a placeholder in the GS stream that the GS thread fills with
// the program's XGKICK packets (gs::path1_program).
//
// Unlike 1.6, VU1 is not reported idle the moment a start is queued:
// VPU_STAT.VBS1 and VIF1 STAT.VEW stay set until the VU1 thread finished the
// program, so guest code that waits for VU1 still waits.
#include "ps2/types.h"

struct vifStruct;
struct VIFregisters;

// ---- EE side: queue work (only while g_vu_thread_active) ----
void vu1_queue_execute(u32 addr_bytes, u32 top, u32 itop, bool from_vif);
void vu1_queue_micro(u32 addr, const void* data, u32 bytes);   // addr + bytes <= micro size
void vu1_queue_row(const u32 row[4]);
void vu1_queue_col(const u32 col[4]);
// `bytes` of unpack input are copied, plus the 4 following bytes a V3 unpack
// may read past its last vector (PCSX2 queues (size + 4)).
void vu1_queue_unpack(const vifStruct& vif, const VIFregisters& regs, const u8* data, u32 bytes);

bool vu1_executing();        // a queued or running micro program (VBS1)
bool vu1_vif_executing();    // ... started by VIF1 (STAT.VEW)

// ---- VU1 thread side ----
void vu1_thread_irq();       // a D/T bit stopped VU1: the EE raises INTC_VU1
