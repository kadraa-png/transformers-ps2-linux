/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010 PCSX2 Dev Team
 *
 *  This runtime adapter connects the original PCSX2 1.6 VURegs interpreter,
 *  VUops.cpp, VUflags.cpp and VU0/VU1microInterp.cpp to the native port.
 *  SPDX-License-Identifier: LGPL-3.0-or-later
 */
#include "ps2/vu.h"
#include "ps2/gs.h"
#include "ps2/intc.h"
#include "ps2/log.h"
#include "ps2/platform.h"
#include "VUops.h"
#include "vu_compiled.h"
#include "vu_capture.h"
#include "vu_thread.h"
#include <array>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <type_traits>

extern bool g_vu_thread_active;

namespace {
alignas(16) std::array<u8,4096> micro0{}, data0{};
alignas(16) std::array<u8,16384> micro1{}, data1{};
struct VifPosition { u32 top=0, itop=0; };
VifPosition positions[2]{};
constexpr u32 execution_limit=10'000'000;
static_assert(sizeof(VECTOR)==sizeof(qword) && alignof(VECTOR)>=alignof(qword));
static_assert(std::is_trivially_copyable_v<VURegs>);
}

VUPortCpuRegs cpuRegs{};
bool vu_port_addsub_hack=false;
VUState g_vu0{reinterpret_cast<qword*>(vuRegs[0].VF),{&vuRegs[0]},
              reinterpret_cast<qword&>(vuRegs[0].ACC),micro0.data(),data0.data(),4096,4096,&vuRegs[0]};
VUState g_vu1{reinterpret_cast<qword*>(vuRegs[1].VF),{&vuRegs[1]},
              reinterpret_cast<qword&>(vuRegs[1].ACC),micro1.data(),data1.data(),16384,16384,&vuRegs[1]};

u32& VUIntView::operator[](u32 index){return regs->VI[index&31].UL;}
const u32& VUIntView::operator[](u32 index) const{return regs->VI[index&31].UL;}

void vu_compiled_reset(int unit);
void vu_reset(int unit){
    if(unit<0||unit>1)return;
    VURegs& r=vuRegs[unit];
    std::memset(static_cast<void*>(&r),0,sizeof(r));
    VUState& v=unit?g_vu1:g_vu0;
    r.idx=static_cast<uint>(unit);
    r.Micro=v.micro;r.Mem=v.data;
    std::memset(v.micro,0,v.micro_size);
    std::memset(v.data,0,v.data_size);
    r.VF[0].f.w=1.f;
    positions[unit]={};
    vu_capture_written(static_cast<unsigned>(unit));
    vu_compiled_reset(unit);
}
void vu_init(){vu_reset(0);vu_reset(1);}

// Both PCSX2 interpreters execute one upper/lower pair at a time and clear the
// corresponding running bit only after the E-bit delay pair has retired.
u32 vu_port_run_micro(unsigned idx,u32 start_pc_bytes,u32 max_pairs);
void vu_exec(int unit,u32 start_pc_bytes){
    if(unit<0||unit>1)return;
    VURegs& r=vuRegs[unit];
    const u32 active=unit?0x100u:1u;
    vpu_stat_set(active);
    const u32 start=start_pc_bytes==~0u?r.VI[REG_TPC].UL:start_pc_bytes;
    vu_capture_launch(static_cast<unsigned>(unit), start);
    static const bool forced = [] {
        const char* const mode = std::getenv("PS2_VU_INTERP");
        return mode && std::strcmp(mode, "1") == 0;
    }();
    const u32 pairs=forced || !vu_compiled_at(static_cast<unsigned>(unit),start)
        ? vu_port_run_micro(static_cast<unsigned>(unit),start,execution_limit)
        : vu_port_run_compiled(static_cast<unsigned>(unit),start,execution_limit);
    if(PS2_UNLIKELY(g_log_mask&LOG_VU)){
        static std::mutex log_mutex;   // VU0 runs on the EE thread, VU1 maybe on its own
        std::lock_guard lock(log_mutex);
        if(pairs>100000){   // long-running program: where and why
            LOG(LOG_VU,"[vu] VU%d long run: start %04x -> tpc %04x, %u pairs; VI:",unit,start,
                r.VI[REG_TPC].UL,pairs);
            for(int i=1;i<16;++i) LOG(LOG_VU," %x",r.VI[i].US[0]);
            LOG(LOG_VU,"  TOP %x ITOP %x\n",positions[unit].top,positions[unit].itop);
        }
        // Workload statistics: executed pairs per unit, once per second.
        static u64 counts[2],calls[2],window_start=0;
        static u32 starts[2][2048];   // launches per start pair this window
        static u64 start_pairs[2048];  // VU1 pairs executed per start pair this window
        counts[unit]+=pairs;++calls[unit];
        ++starts[unit][(start>>3)&2047];
        if(unit) start_pairs[(start>>3)&2047]+=pairs;
        const u64 now=platform::now_ns();
        if(!window_start)window_start=now;
        if(now-window_start>=1000000000ull){
            LOG(LOG_VU,"[vu] last %.2fs: VU0 %llu pairs/%llu runs, VU1 %llu pairs/%llu runs; VU1 starts:",
                (now-window_start)/1e9,(unsigned long long)counts[0],(unsigned long long)calls[0],
                (unsigned long long)counts[1],(unsigned long long)calls[1]);
            for(u32 pc=0;pc<2048;++pc) if(starts[1][pc]) LOG(LOG_VU," %04x:%u(%lluk pairs)",pc*8,starts[1][pc],(unsigned long long)(start_pairs[pc]/1000));
            LOG(LOG_VU,"\n");
            extern u64 g_vu1_tier_pairs[4]; extern u64 g_vu1_graph_exits[2048];
            LOG(LOG_VU,"[vu] VU1 tiers: graph %llu fast %llu step %llu interp %llu; graph exits:",
                (unsigned long long)g_vu1_tier_pairs[0],(unsigned long long)g_vu1_tier_pairs[1],
                (unsigned long long)g_vu1_tier_pairs[2],(unsigned long long)g_vu1_tier_pairs[3]);
            for(u32 pc=0;pc<2048;++pc) if(g_vu1_graph_exits[pc]>100) LOG(LOG_VU," %04x:%llu",pc*8,(unsigned long long)g_vu1_graph_exits[pc]);
            LOG(LOG_VU,"\n");
            extern u64 g_vu0_tier_pairs[2];
            LOG(LOG_VU,"[vu] VU0 tiers: graph %llu step %llu\n",
                (unsigned long long)g_vu0_tier_pairs[0],(unsigned long long)g_vu0_tier_pairs[1]);
            std::memset(g_vu0_tier_pairs,0,sizeof(g_vu0_tier_pairs));
            std::memset(g_vu1_tier_pairs,0,sizeof(g_vu1_tier_pairs));std::memset(g_vu1_graph_exits,0,sizeof(g_vu1_graph_exits));
            std::memset(starts,0,sizeof(starts));std::memset(start_pairs,0,sizeof(start_pairs));
            counts[0]=counts[1]=calls[0]=calls[1]=0;window_start=now;
        }
    }
    // A runaway micro program must not leave the port permanently VU-busy.
    vpu_stat_clear(active);
}
void vu0ExecMicro(u32 addr){vu_exec(0,addr);}
// PCSX2 VU1micro.cpp::vu1ExecMicro (VIF1 MSCAL/MSCALF/MSCNT): with the VU1
// thread the start is queued with the VIF1 TOP/ITOP of this program.
void vu1ExecMicro(u32 addr){
    if(g_vu_thread_active)vu1_queue_execute(addr,vif_read_reg(1,0xe0),vif_read_reg(1,0xd0),true);
    else vu_exec(1,addr);
}
void vu0_callms(u32 addr_bytes){vu_exec(0,addr_bytes);}

// PCSX2 VU0.cpp::CFC2 / CTC2; EE SE32 of the return value is done by the
// recompiled COP2 instruction, not by the VU register bank.
u32 vu0_cfc2(u32 reg){
    reg&=31;
    if(reg==REG_TPC)return VU0.VI[reg].UL>>3;
    if(reg==REG_VPU_STAT){
        const u32 stat=vpu_stat();
        // VBS1 covers starts still queued for the VU1 thread.
        return g_vu_thread_active?(stat&~0x100u)|(vu1_executing()?0x100u:0u):stat;
    }
    return VU0.VI[reg].UL;
}
void vu0_ctc2(u32 reg,u32 value){
    reg&=31;
    if(!reg)return;
    switch(reg){
    case REG_MAC_FLAG: case REG_TPC: case REG_VPU_STAT: return;
    case REG_FBRST:
        vu1_sync();   // VU1 reads FBRST's D/T enables while it runs
        VU0.VI[REG_FBRST].UL=value&0x0c0c;
        if(value&2){vpu_stat_clear(0xffu);VU0.VI[REG_FBRST].UL&=~0xffu;}
        if(value&0x200){vpu_stat_clear(0xff00u);VU0.VI[REG_FBRST].UL&=~0xff00u;}
        return;
    case REG_CMSAR1:
        // A start while VU1 runs is ignored.  The synchronous VU1 is never
        // running here; the VU1 thread still is while earlier starts run.
        if(vu0_cfc2(REG_VPU_STAT)&0x100)return;
        if(g_vu_thread_active)vu1_queue_execute((value&0xffff)*8,vif_read_reg(1,0xe0),vif_read_reg(1,0xd0),false);
        else vu_exec(1,(value&0xffff)*8);
        return;
    default:
        VU0.VI[reg].UL=reg<16?value&0xffff:value;
        return;
    }
}
// COP2.cpp uses VIF1 STAT.VEW (not VPU_STAT) as CP2COND.
bool vu_cpcond2(){return (vif_read_reg(1,0)&4)!=0;}

void _vuFlushAll(VURegs* vu);
namespace {
VURegs& regs(VUState& vu){return *static_cast<VURegs*>(vu.impl);}
void queue_fmac(VURegs& vu,int reg,u32 mac,u32 status,u32 clip){
    unsigned slot=0;
    while(slot<8&&vu.fmac[slot].enable)++slot;
    if(slot==8){_vuFlushAll(&vu);slot=0;}
    auto& pipe=vu.fmac[slot];
    pipe.enable=1;pipe.reg=reg;pipe.xyzw=0;
    pipe.sCycle=vu.cycle;pipe.Cycle=3;
    pipe.macflag=mac;pipe.statusflag=status;pipe.clipflag=clip;
}
}
void vu_queue_q(VUState& vu,u32 bits,u32 latency){
    VURegs& r=regs(vu);
    if(r.fdiv.enable)vu_wait_q(vu);
    r.q.UL=bits;r.fdiv.enable=1;r.fdiv.sCycle=r.cycle;r.fdiv.Cycle=latency;
    r.fdiv.reg.UL=bits;r.fdiv.statusflag=r.statusflag;
}
void vu_queue_p(VUState& vu,u32 bits,u32 latency){
    VURegs& r=regs(vu);
    if(r.efu.enable)vu_wait_p(vu);
    r.p.UL=bits;r.efu.enable=1;r.efu.sCycle=r.cycle;r.efu.Cycle=latency;
    r.efu.reg.UL=bits;
}
void vu_queue_flags(VUState& vu,u32 mac,u32 status){
    VURegs& r=regs(vu);r.macflag=mac;r.statusflag=status;
    queue_fmac(r,0,mac,status,r.clipflag);
}
void vu_queue_clip(VUState& vu,u32 clip){
    VURegs& r=regs(vu);r.clipflag=clip;
    queue_fmac(r,-REG_CLIP_FLAG,r.macflag,r.statusflag,clip);
}
void vu_wait_q(VUState& vu){
    VURegs& r=regs(vu);
    if(r.fdiv.enable){u32 ready=r.fdiv.sCycle+r.fdiv.Cycle;
        if(r.cycle<ready)r.cycle=ready;_vuTestPipes(&r);}
}
void vu_wait_p(VUState& vu){
    VURegs& r=regs(vu);
    if(r.efu.enable){u32 ready=r.efu.sCycle+r.efu.Cycle;
        if(r.cycle<ready)r.cycle=ready;_vuTestPipes(&r);}
}
void vu_set_vif_position(int unit,u32 top,u32 itop){
    // The VU1 thread takes each start's TOP/ITOP from its queued record.
    if(unit==0||(unit==1&&!g_vu_thread_active))positions[unit]={top,itop};
}
void vu_set_vif_position_vu1(u32 top,u32 itop){positions[1]={top,itop};}
u32 vu_port_vif_reg(unsigned idx,unsigned offset){
    if(idx>1)return 0;
    return offset==0?positions[idx].itop:positions[idx].top;
}
void vu_port_xgkick(unsigned idx,const u8* data,unsigned address){
    if(idx==1)gs::gif_path1(data,address>>4);
}
// Called from the JR/JALR body after TPC was advanced past the jumping pair.
void vu_port_indirect_branch(unsigned idx,u32 target){vu_capture_indirect(idx,target,vuRegs[idx].VI[REG_TPC].UL-8);}
void vu_port_intc_irq(unsigned idx){
    if(idx&&g_vu_thread_active)vu1_thread_irq();
    else intc_raise(idx?INTC_VU1:INTC_VU0);
}
void vif_micro_end(int unit);
// VIF1 STAT.VEW of a queued start is derived from the VU1 thread's progress.
void vu_port_vif_vew_end(unsigned idx){if(!(idx&&g_vu_thread_active))vif_micro_end(static_cast<int>(idx));}
