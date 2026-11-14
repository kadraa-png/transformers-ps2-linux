#include <cstring>
/* PCSX2 - PS2 Emulator for PCs
 * Copyright (C) 2002-2010 PCSX2 Dev Team
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Adapter for the original PCSX2 1.6 VIF command/transfer/C-unpack sources.
 */
#include "../../../third_party/pcsx2_vif/compat.h"
#include "ps2/log.h"
#include "vu_thread.h"
#include <vector>

vifStruct vif0{}, vif1{};
VIFregisters vif0Regs{}, vif1Regs{};
VifDmaChannel vif0ch{}, vif1ch{};
u32 g_vif0Cycles=0,g_vif1Cycles=0;
VifLogger Console{},DevCon{};
VifGif gifUnit{};
VifGifRegs gifRegs{};
namespace {
std::vector<u32> deferred[2];
// Original VIF transfer takes word arrays; only IRQ-stalled tail data is copied.
void feed(int unit,const u32* data,u32 words) {
    vifStruct& v=unit?vif1:vif0;
    VifDmaChannel& ch=unit?vif1ch:vif0ch;
    if (!words || v.vifstalled.enabled || (v.irq && !v.cmd)) {
        if(words)deferred[unit].insert(deferred[unit].end(),data,data+words);
        return;
    }
    ch.chcr.STR=true;
    for(int idle_breaks=0;;){
        ch.qwc=(words+3)/4;
        u32* ptr=const_cast<u32*>(data);
        if(unit)VIF1transfer(ptr,static_cast<int>(words),true);
        else VIF0transfer(ptr,static_cast<int>(words),true);
        const u32 consumed=words-v.vifpacketsize;
        data+=consumed;words-=consumed;
        // PCSX2 breaks the transfer loop for timing (after DIRECT, before
        // MSKPATH3, ...) and resumes it from a later DMA event.  Here GIF/VU
        // work has already completed synchronously, so resume immediately.
        // Only an IRQ stall waits for the guest (FBRST.STC).
        if(!v.vifstalled.enabled||v.vifstalled.value!=VIF_TIMING_BREAK)break;
        v.vifstalled.enabled=false;
        if(!words)break;
        if(consumed==0&&++idle_breaks>2)break;   // no progress: retry on the next transfer
    }
    if(words)deferred[unit].insert(deferred[unit].end(),data,data+words);
    LOG(LOG_VIF,"[vif%d] feed left %u irq=%d stalled=%d/%d cmd=%02x code=%08x\n",
        unit,words,(int)v.irq,(int)v.vifstalled.enabled,(int)v.vifstalled.value,(unsigned)v.cmd,
        (unit?vif1Regs:vif0Regs).code);
    VIFregisters& regs=unit?vif1Regs:vif0Regs;
    if(v.irq && v.vifstalled.enabled && v.vifstalled.value==VIF_IRQ_STALL) {
        regs.stat.INT=true;
        if(((regs.code>>24)&0x7f)!=7)regs.stat.VIS=true;
        intc_raise(unit?INTC_VIF1:INTC_VIF0);
        --v.irq;
    }
    // PCSX2 vif0/vif1Interrupt at the end of the DMA: VPS reads idle, or
    // waiting for data while a command is still open.  (An IRQ stall keeps
    // its transferring state until FBRST.STC resumes it.)
    if(!v.vifstalled.enabled)regs.stat.VPS=v.cmd?VPS_WAITING:VPS_IDLE;
    ch.chcr.STR=false;
}
void drain(int unit) {
    auto& rest=deferred[unit];
    if(rest.empty())return;
    std::vector<u32> pending;
    pending.swap(rest);
    feed(unit,pending.data(),static_cast<u32>(pending.size()));
}
void reset(int unit,bool preserve_rows) {
    vifStruct& v=unit?vif1:vif0;
    VIFregisters& regs=unit?vif1Regs:vif0Regs;
    if(unit&&THREAD_VU1){   // the VU1 thread's copy holds the current ROW/COL
        vu1_sync();
        v.MaskRow=vu1t_vif.MaskRow;v.MaskCol=vu1t_vif.MaskCol;
    }
    VifVector row=v.MaskRow,col=v.MaskCol;
    v={};regs={};
    if(preserve_rows){v.MaskRow=row;v.MaskCol=col;}
    regs.cycle.cl=1;regs.cycle.wl=1;
    resetNewVif(unit);
    deferred[unit].clear();
    if(unit&&THREAD_VU1){vu1t_vif=v;vu1t_vifRegs=regs;}
}
// STAT.VEW of VIF1 starts handed to the VU1 thread clears once it ran them.
void refresh_vew() {
    if(THREAD_VU1&&vif1Regs.stat.VEW&&!vif1.queued_program&&!vu1_vif_executing())
        vif1Regs.stat.VEW=false;
}
}
u32 VifGif::TransferGSPacketData(int,u8* data,u32 bytes) {
    u32 offset=0;
    if(used){
        u32 n=std::min(bytes,16-used);
        std::memcpy(pending+used,data,n);used+=n;offset=n;
        if(used==16){gs::gif_path2(pending,1);used=0;}
    }
    u32 whole=(bytes-offset)&~15u;
    if(whole){gs::gif_path2(data+offset,whole/16);offset+=whole;}
    if(offset<bytes){used=bytes-offset;std::memcpy(pending,data+offset,used);}
    return bytes;
}
void vif_micro_end(int unit){if(unit==0||unit==1)(unit?vif1Regs:vif0Regs).stat.VEW=false;}
void vif_init(){reset(0,false);reset(1,false);gifUnit=VifGif{};gifRegs=VifGifRegs{};}
void vif_dma_words(int unit,const u32* words,u32 nwords){
    if(unit<0||unit>1||!words||!nwords)return;
    if(!deferred[unit].empty()&&!((unit?vif1:vif0).vifstalled.enabled))drain(unit);
    feed(unit,words,nwords);
}
void vif_dma(int unit,const u8* data,u32 qwc){
    if(unit<0||unit>1||!data||!qwc)return;
    // EE DMAC packets are word-aligned, but public VIF may receive an unaligned host buffer.
    if(reinterpret_cast<uintptr_t>(data)%alignof(u32)==0){vif_dma_words(unit,reinterpret_cast<const u32*>(data),qwc*4);return;}
    std::vector<u32> words(qwc*4);
    std::memcpy(words.data(),data,qwc*16);
    vif_dma_words(unit,words.data(),static_cast<u32>(words.size()));
}
u32 vif_read_reg(int unit,u32 offset){
    if(unit<0||unit>1||offset>0x170||(offset&15))return 0;
    if(unit&&offset==0)refresh_vew();
    if(offset>=0x100){
        const vifStruct* v=unit?&vif1:&vif0;
        if(unit&&THREAD_VU1){vu1_sync();v=&vu1t_vif;}   // difference-mode unpacks update ROW there
        return offset<0x140?v->MaskRow._u32[(offset-0x100)/16]:v->MaskCol._u32[(offset-0x140)/16];
    }
    return reinterpret_cast<const u32*>(&(unit?vif1Regs:vif0Regs))[offset/4];
}
void vif_write_reg(int unit,u32 offset,u32 value){
    if(unit<0||unit>1||offset>0x170||(offset&15))return;
    auto& v=unit?vif1:vif0;
    auto& regs=unit?vif1Regs:vif0Regs;
    if(offset>=0x100){
        if(offset<0x140){
            v.MaskRow._u32[(offset-0x100)/16]=value;
            if(unit&&THREAD_VU1)vu1_queue_row(v.MaskRow._u32);
        }else{
            v.MaskCol._u32[(offset-0x140)/16]=value;
            if(unit&&THREAD_VU1)vu1_queue_col(v.MaskCol._u32);
        }
        return;
    }
    switch(offset){
    case 0x00:if(unit)regs.stat.FDR=(value>>23)&1;return;
    case 0x10:
        if(value&1)reset(unit,true);
        if(value&2){regs.stat.VFS=true;v.vifstalled={true,VIF_IRQ_STALL};}
        if(value&4){regs.stat.VSS=true;v.vifstalled={true,VIF_IRQ_STALL};}
        if(value&8){
            bool resume=regs.stat.VSS||regs.stat.VFS||regs.stat.VIS||v.vifstalled.enabled;
            regs.stat.clear_flags(0x3f00);
            v.vifstalled.enabled=false;
            v.irq=0;
            if(resume)drain(unit);
        }
        regs.fbrst=value;return;
    case 0x30:regs.stat.MRK=false;regs.mark=value;return;
    case 0x20:regs.err._u32=value;return;
    case 0x50:regs.mode=value;return;
    default:return;
    }
}
void vif_fifo_write(int unit,const qword& q){if(unit==0||unit==1)vif_dma(unit,q.b,1);}
void vif_fifo_read(int unit,qword* q){if(q){if(unit==1)gs::read_fifo(q->b,1);else *q={};}}
// Bulk VIF1 FIFO download (DMA ch1 to memory): one GS round trip instead of
// one per quadword (the first read waits for the GS to finish drawing).
void vif_fifo_read_n(int unit,qword* q,u32 qwc){
    if(!q||!qwc)return;
    if(unit==1)gs::read_fifo(q->b,qwc);else std::memset(q,0,size_t(qwc)*16);
}
bool vif_path3_masked(){return vif1Regs.mskpath3!=0;}
