/* PCSX2 - PS2 Emulator for PCs
 * Copyright (C) 2002-2010 PCSX2 Dev Team
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Compatibility boundary for the 1.6 VIF interpreter; DMA and JIT are
 * replaced by synchronous runtime hooks, not by alternative VIF handlers.
 * MTVU follows 1.6: VIF1 unpacks, MPG and ROW/COL updates are queued to the
 * VU1 thread (runtime/src/vu/vu_thread.h), which owns VU1 memory and its own
 * copy of the unpack state.
 */
#pragma once
#include "ps2/vu.h"
#include "../pcsx2_vu/VU.h"
#include "ps2/gs.h"
#include "ps2/intc.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <type_traits>
using uptr = uintptr_t;
#define _vifT template<int idx>
#define CHECK_VIF1STALLHACK false
#define VIF_LOG(...) ((void)0)
#define VifCodeLog(...) ((void)0)
#define GUNIT_LOG(...) ((void)0)
#define GUNIT_WARN(...) ((void)0)
#define pxAssert(x) assert(x)
#define pxAssume(x) assert(x)
#define jNO_DEFAULT default: break;
#define BIAS 1
struct VifLogger {
 template<class... T> void WriteLn(const char* fmt,T... args){std::fprintf(stderr,fmt,args...);std::fputc('\n',stderr);}
 template<class... T> void Warning(const char* fmt,T... args){WriteLn(fmt,args...);}
 template<class... T> void Error(const char* fmt,T... args){WriteLn(fmt,args...);}
};
extern VifLogger Console, DevCon;
// Deliberately keep the 1.6 state field names and register bit layout.
enum { VPS_IDLE=0, VPS_WAITING=1, VPS_DECODING=2, VPS_TRANSFERRING=3, VIF_TIMING_BREAK=1, VIF_IRQ_STALL=2 };
union tVIF_STAT {
 struct {u32 VPS:2,VEW:1,VGW:1,_reserved:2,MRK:1,DBF:1,VSS:1,VFS:1,VIS:1,INT:1,ER0:1,ER1:1,_reserved2:9,FDR:1,FQC:5;};
 u32 _u32;
 bool test(u32 flags) const {return !!(_u32&flags);} void clear_flags(u32 flags){_u32 &= ~flags;}
};
union tVIF_ERR {struct {u32 MII:1,ME0:1,ME1:1,_reserved:29;};u32 _u32; void reset(){_u32=0;}};
struct vifCycle {u8 cl,wl,pad[2];};
struct VIFregisters {
 tVIF_STAT stat;u32 _pad0[3];u32 fbrst,_pad1[3];tVIF_ERR err;u32 _pad2[3];u32 mark,_pad3[3];
 vifCycle cycle;u32 _pad4[3];u32 mode,_pad5[3],num,_pad6[3],mask,_pad7[3],code,_pad8[3],itops,_pad9[3];
 u32 base,_pad10[3],ofst,_pad11[3],tops,_pad12[3],itop,_pad13[3],top,_pad14[3],mskpath3,_pad15[3];
 u32 r0,_pad16[3],r1,_pad17[3],r2,_pad18[3],r3,_pad19[3],c0,_pad20[3],c1,_pad21[3],c2,_pad22[3],c3,_pad23[3],offset,addr;
};
static_assert(offsetof(VIFregisters,r0)==0x100);
struct vifCode {u32 addr,size,cmd;u16 wl,cl;};
struct tVIF_CTRL {bool enabled;u32 value;};
struct VifVector {u32 _u32[4];};
struct vifStruct {
 VifVector MaskRow,MaskCol;
 vifCode tag;int cmd,pass,cl;u8 usn,start_aligned,StructEnd;
 int irq;bool done;tVIF_CTRL vifstalled;bool stallontag,waitforvu;int unpackcalls;
 tVIF_CTRL irqoffset;u32 vifpacketsize;u8 inprogress,dmamode;bool queued_program;u32 queued_pc;
};
extern vifStruct vif0,vif1;
extern VIFregisters vif0Regs,vif1Regs;
#define GetVifX (idx ? vif1 : vif0)
#define vifXRegs (idx ? vif1Regs : vif0Regs)
// 1.6 THREAD_VU1: fixed at startup (vu_thread_start()).
extern bool g_vu_thread_active;
#define THREAD_VU1 g_vu_thread_active
extern vifStruct vu1t_vif;          // owned by the VU1 thread
extern VIFregisters vu1t_vifRegs;   // only cycle/mode/num/mask/itop/top are used
#define MTVU_VifX (idx ? (THREAD_VU1 ? vu1t_vif : vif1) : vif0)
#define MTVU_VifXRegs (idx ? (THREAD_VU1 ? vu1t_vifRegs : vif1Regs) : vif0Regs)
struct VifDmaChannel {struct {bool STR;} chcr;u32 qwc,madr;};
extern VifDmaChannel vif0ch,vif1ch;
#define vifXch (idx ? vif1ch : vif0ch)
#define VifStallEnable(ch) ((ch).chcr.STR)
extern u32 g_vif0Cycles,g_vif1Cycles;
extern void vifExecQueue(int idx);
bool VIF0transfer(u32* data,int size,bool TTE);
bool VIF1transfer(u32* data,int size,bool TTE);
inline int _limit(int a,int b){return std::min(a,b);}
using FnType_VifCmdHandler = int (int,const u32*);
extern FnType_VifCmdHandler* const vifCmdHandler[2][128];
using UNPACKFUNCTYPE = void (*)(void*,const void*);
#define create_unpack_u_type(bits) using UNPACKFUNCTYPE_u##bits = void (*)(u32*,const u##bits*);
#define create_unpack_s_type(bits) using UNPACKFUNCTYPE_s##bits = void (*)(u32*,const s##bits*);
create_unpack_u_type(32) create_unpack_u_type(16) create_unpack_u_type(8)
create_unpack_s_type(32) create_unpack_s_type(16) create_unpack_s_type(8)
extern const u8 nVifT[16];
extern const UNPACKFUNCTYPE VIFfuncTable[2][4][64];
template<int idx> int nVifUnpack(const u8*);
template<int idx> void vifUnpackSetup(const u32*);
struct nVifStruct {alignas(16) u8 buffer[4096];u32 bSize;};
extern nVifStruct nVif[2];
void resetNewVif(int idx);
void _nVifUnpack(int idx,const u8* data,uint mode,bool isFill);
void vu0ExecMicro(u32 pc);void vu1ExecMicro(u32 pc);
struct VifGif {u8 pending[16]{};u32 used=0;u32 TransferGSPacketData(int type,u8* ptr,u32 bytes);};
extern VifGif gifUnit;
enum {GIF_TRANS_DIRECT=0,GIF_TRANS_DIRECTHL=1};
using GIF_TRANSFER_TYPE = int;
struct VifGifRegs {struct {u32 M3P=0;} stat;};extern VifGifRegs gifRegs;
