#include "ps2/vu0_macro.h"
#include "ps2/gs.h"
#include "ps2/intc.h"
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static u32 load_word(const u8* src) { u32 value; std::memcpy(&value, src, 4); return value; }
static u32 kick = ~0u, interrupts = 0;
static std::vector<u8> direct;
// XGKICK start quadword + first packet word, in kick order (VU1 thread or not).
static std::vector<std::array<u32,2>> kicks;
static bool gs_thread_present = false;
static u32 path1_programs = 0, path1_program_ends = 0;
namespace gs {
void gif_path1(const u8* mem, u32 addr) {
    kick = addr;
    kicks.push_back({addr, load_word(mem + (addr & 1023) * 16)});
}
void gif_path2(const u8* data, u32 qwc) { direct.insert(direct.end(), data, data + 16*qwc); }
void read_fifo(u8* data, u32 qwc) { std::memset(data, 0x5a, 16*qwc); }
bool set_path1_async(bool async) { return !async || gs_thread_present; }
void path1_program() { ++path1_programs; }
void path1_program_end() { ++path1_program_ends; }
void flush() {}
void request_dump(const char*, int) {}   // F-key GS dump hook in platform_sdl
}
void intc_raise(u32 line) { interrupts |= 1u << line; }
static u32 word(u32 s, u32 t, u32 d, u32 mask=15) { return (s<<11)|(t<<16)|(d<<6)|(mask<<21); }
static void store(u8* dst, u32 v) { std::memcpy(dst, &v, 4); }
static u32 load(const u8* src) { u32 value; std::memcpy(&value, src, 4); return value; }
static void insn(u32 pc, u32 lower, u32 upper=0x2ff) {
    store(g_vu1.micro+pc, lower); store(g_vu1.micro+pc+4, upper);
}
static void stream(int unit, std::initializer_list<u32> words) {
    std::vector<u32> data(words); vif_dma_words(unit, data.data(), static_cast<u32>(data.size()));
}
static void branch_and_gif() {
    vu_reset(1);
    auto& v=g_vu1;
    v.vi[1]=4; v.vi[2]=8;
    insn(0,(0x24u<<25)|word(1,0,0)); // JR vi1 -> byte 32
    insn(8,0x80000000u|word(0,3,0)|0x32|(1<<6)); // delay: IADDI vi3=1
    insn(16,0x80000000u|word(0,3,0)|0x32|(7<<6)); // skipped
    insn(32,(0x25u<<25)|word(2,4,0)); // JALR vi4,vi2 -> byte 40
    insn(40,0x80000000u|word(3,3,0)|0x32|(1<<6)); // JALR delay slot
    insn(48,0x80000000u|word(0,3,0)|0x32|(7<<6)); // skipped
    insn(64,0,0x400002ff);insn(72,0);
    vu_exec(1,0);
    assert(v.vi[3]==2 && v.vi[4]==6);
    vu_reset(1);
    // A branch in the delay slot visits two pairs at the first branch's
    // target before committing its own branch (PCSX2 delaybranchpc rule).
    insn(0,(0x20u<<25)|3);
    insn(8,(0x20u<<25)|5);
    insn(32,0x80000000u|word(0,5,0)|0x32|(5<<6));
    insn(40,0x80000000u|word(0,5,0)|0x32|(6<<6));
    insn(56,0,0x400002ff);
    insn(64,0);
    vu_exec(1,0);assert(v.vi[5]==6);
    vu_reset(1);v.vi[1]=0x21;
    insn(0,0x80000000u|word(1,0,0,0)|(27<<6)|0x3c,0x400002ff);
    insn(8,0);
    kick=~0u;vu_exec(1,0);assert(kick==0x21);
}
static void q_and_flags() {
    vu_reset(1);auto& v=g_vu1;
    v.vi[22]=0x3f800000;v.vf[1].f[0]=8;v.vf[2].f[0]=2;
    // DIV, MULq before FDIV completion, WAITQ, MULq after completion.
    insn(0,0x80000000u|word(1,2,0,0)|(14<<6)|0x3c);
    insn(8,0,word(1,0,4,8)|0x1c);
    insn(16,0x80000000u|(14<<6)|0x3f);
    insn(24,0,word(1,0,5,8)|0x1c|0x40000000u);
    insn(32,0);
    vu_exec(1,0);
    assert(v.vf[4].f[0]==8 && v.vf[5].f[0]==32 && v.vi[22]==0x40800000);
    vu_reset(1);v.vi[22]=0x3f800000;v.vf[1].f[0]=8;v.vf[2].f[0]=2;
    insn(0,0x80000000u|word(1,2,0,0)|(14<<6)|0x3c);
    for(u32 pc=8;pc<48;pc+=8)insn(pc,0);
    insn(48,0,word(1,0,4,8)|0x1c);
    insn(56,0,word(1,0,5,8)|0x1c|0x40000000u);
    insn(64,0);vu_exec(1,0);
    assert(v.vf[4].f[0]==8 && v.vf[5].f[0]==32);
    vu_reset(1);v.vf[1].f[0]=-1;v.vf[2].f[0]=1;
    insn(0,0,word(1,2,3,8)|0x04); // SUB -> MAC negative
    insn(8,(22u<<25)|word(0,4,0)|2,0x2ff); // FSAND reads old status
    insn(16,0);insn(24,0);
    insn(32,(22u<<25)|word(0,5,0)|2,0x400002ff);
    insn(40,0);
    vu_exec(1,0);
    assert(v.vi[4]==0 && v.vi[5]==2 && (v.vi[17]&0x80));
    vu_reset(1);v.vi[6]=0x80;v.vf[1].f[0]=-1;v.vf[2].f[0]=1;
    insn(0,0,word(1,2,3,8)|0x04);
    insn(8,(26u<<25)|word(6,7,0));
    insn(16,0);insn(24,0);
    insn(32,(26u<<25)|word(6,8,0),0x400002ff);
    insn(40,0);
    vu_exec(1,0);assert(v.vi[7]==0 && v.vi[8]==0x80);
}
static void macros_and_controls() {
    vu_reset(0);auto& v=g_vu0;
    v.vf[1].f[0]=3;v.vf[2].f[0]=5;
    constexpr u32 cop2=(0x12u<<26)|(1u<<25);
    vu0m_vadd(cop2|word(1,2,3,8)|0x28);assert(v.vf[3].f[0]==8);
    vu0m_vmula(cop2|word(1,2,0,8)|(10u<<6)|0x3e);
    vu0m_vmadd(cop2|word(1,2,3,8)|0x29);
    assert(v.vf[3].f[0]==30 && vu0_cfc2(17)==v.vi[17]);
    v.vf[1].f[0]=20;v.vf[2].f[3]=10;
    vu0m_vclipw(cop2|word(1,2,0,8)|(7u<<6)|0x3f);
    assert((vu0_cfc2(18)&1)!=0);
    const auto status=vu0_cfc2(16);assert(status==v.vi[16]);
    vu0_ctc2(17,0xffffffff);assert(v.vi[17]!=0xffffffff);
    vu0_ctc2(26,0xffffffff);assert(v.vi[26]!=0xffffffff);
    vu0_ctc2(28,4);assert(v.vi[28]==4);
    vu0_ctc2(28,2);assert(v.vi[28]==0);
    store(g_vu0.micro,0x12345678);store(g_vu0.data,0x87654321);
    vu0_ctc2(28,2);
    assert(load(g_vu0.micro)==0x12345678 && load(g_vu0.data)==0x87654321);
    vif_init();assert(!vu_cpcond2());
}
static void vif_positions_and_streaming() {
    vif_init();vu_reset(1);
    insn(0,0,0x400002ff);insn(8,0);
    stream(1,{0x03000010,0x02000020,0x14000000});
    assert(vif_read_reg(1,0xa0)==0x10 && vif_read_reg(1,0xc0)==0x30);
    assert((vif_read_reg(1,0)&0x80)!=0);
    // UNPACK does not change TOPS.
    stream(1,{0x6c010000,1,2,3,4});
    assert(vif_read_reg(1,0xc0)==0x30);
    stream(1,{0x14000000});
    assert((vif_read_reg(1,0)&0x80)==0 && vif_read_reg(1,0xc0)==0x10);
    stream(1,{0x02000008}); // OFFSET clears DBF and initializes TOPS to BASE.
    assert(!(vif_read_reg(1,0)&0x80) && vif_read_reg(1,0xc0)==0x10);
    stream(1,{0x14000000});
    assert((vif_read_reg(1,0)&0x80) && vif_read_reg(1,0xc0)==0x18);
    stream(1,{0x0400002a,0x14000000});
    assert(vif_read_reg(1,0xd0)==0x2a && vif_read_reg(1,0xc0)==0x10);
    // PCSX2 copies TOPS/ITOPS into TOP/ITOP when MSCAL launches.
    insn(0,0x80000000u|word(0,3,0,0)|(26<<6)|0x3c,0x400002ff);
    insn(8,0x80000000u|word(0,4,0,0)|(26<<6)|0x3d);
    vu_exec(1,0);
    assert(g_vu1.vi[3]==vif_read_reg(1,0xe0) &&
           g_vu1.vi[4]==vif_read_reg(1,0xd0));
    vif_init();vu_reset(1);
    std::array<u32,4> a={0x6c020000,11,12,13};
    vif_dma(1,reinterpret_cast<const u8*>(a.data()),1);
    assert(load(g_vu1.data)==0);
    // An UNPACK still short of data leaves VPS "waiting"; once complete the
    // VIF reads idle (sceGsSyncPath spins on STAT.VPS/FQC).
    assert((vif_read_reg(1,0)&3)==1);
    std::array<u32,4> b={14,21,22,23};
    vif_dma(1,reinterpret_cast<const u8*>(b.data()),1);
    assert(load(g_vu1.data)==0);
    stream(1,{24});assert(load(g_vu1.data)==11 && load(g_vu1.data+16)==21);
    assert((vif_read_reg(1,0)&3)==0);
    direct.clear();stream(1,{0x50000001,1,2,3,4});
    assert(direct.size()==16 && load(direct.data())==1);
    qword fifo{};vif_fifo_read(1,&fifo);assert(fifo.b[0]==0x5a);
}
static void unpack_formats() {
    for(u32 format=0;format<16;++format) {
        if((format&3)==3&&format!=15)continue;
        vif_init();vu_reset(1);
        const u32 n=(format>>2)+1;
        std::vector<u32> data={(0x60u|format)<<24|1u<<16|4};
        if(format==15)data.push_back(0x8421);
        else if((format&3)==0)for(u32 i=0;i<n;++i)data.push_back(10+i);
        else if((format&3)==1){u32 packed=0;for(u32 i=0;i<n;++i){packed|=(10+i)<<((i&1)*16);if((i&1)||i+1==n){data.push_back(packed);packed=0;}}}
        else {u32 packed=0;for(u32 i=0;i<n;++i)packed|=(10+i)<<(i*8);data.push_back(packed);}
        vif_dma_words(1,data.data(),static_cast<u32>(data.size()));
        const u32* expected=nullptr;
        const std::array<u32,4> scalar={10,10,10,10}, two={10,11,10,11},
            three={10,11,12,0},four={10,11,12,13},packed={8,8,8,128};
        expected=(format==15?packed:n==1?scalar:n==2?two:n==3?three:four).data();
        for(u32 component=0;component<4;++component)
            assert(load(g_vu1.data+64+component*4)==expected[component]);
    }
    vif_init();vu_reset(1);
    // STMASK x=row, y=column, z=preserve, w=data; MODE=offset.
    store(g_vu1.data+8,0xdeadbeef);
    stream(1,{0x30000000,100,101,102,103,0x31000000,200,201,202,203,
              0x20000000,0x00000039,0x05000001,0x7c010000,1,2,3,4});
    assert(load(g_vu1.data)==100 && load(g_vu1.data+4)==200 && load(g_vu1.data+8)==0xdeadbeef);
    vif_init();vu_reset(1);
    // CL < WL repeats the previous source; CL > WL leaves skipped vectors untouched.
    stream(1,{0x01000301,0x6c030000,11,12,13,14});
    assert(load(g_vu1.data)==11 && load(g_vu1.data+16)==11 && load(g_vu1.data+32)==11);
    vif_init();vu_reset(1);
    stream(1,{0x01000103,0x6c020000,1,2,3,4,5,6,7,8});
    assert(load(g_vu1.data)==1 && load(g_vu1.data+16)==0 && load(g_vu1.data+48)==5);
    vif_init();vu_reset(1);interrupts=0;
    std::array<u32,4> first={0xec010000,7,8,9};
    vif_dma(1,reinterpret_cast<const u8*>(first.data()),1);
    assert(!(interrupts&(1u<<INTC_VIF1)));
    std::array<u32,4> second={10,0x30000000,50,51};
    vif_dma(1,reinterpret_cast<const u8*>(second.data()),1);
    assert((interrupts&(1u<<INTC_VIF1)) && load(g_vu1.data)==7);
    vif_write_reg(1,0x10,8);
    stream(1,{52,53});
    assert(vif_read_reg(1,0x100)==50 && vif_read_reg(1,0x130)==53);
}
// One VIF1 stream touching everything the VU1 thread takes over: ROW and
// difference-mode unpack, MPG, TOPS-relative unpacks, MSCAL with XTOP/XGKICK.
struct Vu1Outcome {
    std::vector<std::array<u32,2>> kicks;
    std::array<u32,8> data{};
    std::array<u32,4> row{};
    u32 vi1=0;
};
static Vu1Outcome run_vu1_stream() {
    kicks.clear();
    const u32 xtop=0x80000000u|word(0,1,0,0)|(26<<6)|0x3c, xgkick=0x80000000u|word(1,0,0,0)|(27<<6)|0x3c;
    stream(1,{0x01000101,                                  // STCYCL 1/1
              0x30000000,100,101,102,103,                  // STROW
              0x05000002,0x6c020000,1,2,3,4,5,6,7,8,       // STMOD difference, UNPACK V4-32 x2 @0
              0x05000000,
              0x4a040000,xtop,0x2ff,0,0x2ff,xgkick,0x400002ff,0,0x2ff,   // MPG 4 pairs @0
              0x03000020,0x02000010,                       // BASE 0x20, OFFSET 0x10 -> TOPS 0x20
              0x6c018000,0x00008000,0,0,0,                 // GIF tag (EOP) @TOPS
              0x14000000,                                  // MSCAL 0: kicks TOP 0x20
              0x6c018000,0x12348000,0,0,0,                 // marked tag @TOPS (0x30)
              0x14000000,                                  // MSCAL 0: kicks TOP 0x30
              0x00000000});                                // NOP flushes the queued start
    vu1_sync();
    Vu1Outcome outcome;
    outcome.kicks=kicks;
    for(u32 i=0;i<8;++i)outcome.data[i]=load(g_vu1.data+i*4);
    for(u32 i=0;i<4;++i)outcome.row[i]=vif_read_reg(1,0x100+i*16);
    outcome.vi1=g_vu1.vi[1];
    return outcome;
}
static void vu1_thread_matches_synchronous() {
    vif_init();vu_reset(1);
    assert(!vu_thread_start());   // no GS thread: PATH1 order could not be kept
    const Vu1Outcome expected=run_vu1_stream();
    assert(expected.kicks.size()==2 && expected.kicks[0]==(std::array<u32,2>{0x20,0x8000}) &&
           expected.kicks[1]==(std::array<u32,2>{0x30,0x12348000}));
    assert(expected.data[0]==101 && expected.data[7]==115 && expected.row[3]==115 && expected.vi1==0x30);
    vif_init();vu_reset(1);
    gs_thread_present=true;path1_programs=path1_program_ends=0;
    assert(vu_thread_start());
    const Vu1Outcome threaded=run_vu1_stream();
    assert(threaded.kicks==expected.kicks && threaded.data==expected.data &&
           threaded.row==expected.row && threaded.vi1==expected.vi1);
    // Every queued start leaves one PATH1 placeholder and closes it.
    assert(path1_programs==2 && path1_program_ends==2);
    // Finished starts no longer read as busy (VIF1 STAT.VEW / VPU_STAT.VBS1).
    assert(!vu_cpcond2() && !(vu0_cfc2(29)&0x100));
    vu_thread_stop();
    gs_thread_present=false;
}
static void benchmark() {
    vu_reset(1);
    insn(0,0,0x2ff);
    insn(8,0,0x400002ff);
    insn(16,0);
    constexpr u32 repeats=100000;
    const auto before=std::chrono::steady_clock::now();
    for(u32 i=0;i<repeats;++i)vu_exec(1,0);
    const auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-before).count();
    std::printf("VU1 interpreter: %.2f million instruction pairs/s (%g s)\n",(3.0*repeats)/seconds/1e6,seconds);
}
int main() {
    vu_init();
    branch_and_gif();q_and_flags();macros_and_controls();
    vif_positions_and_streaming();unpack_formats();vu1_thread_matches_synchronous();benchmark();
    std::puts("vu_tests passed");
}
