#include "ps2/ee_ops.h"
#include <cassert>
#include <climits>
#include <immintrin.h>
#include <iostream>

static constexpr u32 op(unsigned rs=1,unsigned rt=2,unsigned rd=3,unsigned sa=0){return rs<<21|rt<<16|rd<<11|sa<<6;}
static constexpr u32 fp(unsigned fs=1,unsigned ft=2,unsigned fd=3){return ft<<16|fs<<11|fd<<6;}
static void packed(){
    EEContext c{};
    c.r[1].sw[0]=INT_MAX;c.r[2].sw[0]=1;
    c.r[1].sw[1]=INT_MIN;c.r[2].sw[1]=-1;
    mmi_paddsw(&c,op());assert(c.r[3].sw[0]==INT_MAX&&c.r[3].sw[1]==INT_MIN);
    mmi_psubsw(&c,op());assert(c.r[3].sw[0]==INT_MAX-1&&c.r[3].sw[1]==INT_MIN+1);
    c.r[1].sh[0]=SHRT_MAX;c.r[2].sh[0]=1;
    c.r[1].sh[1]=SHRT_MIN;c.r[2].sh[1]=1;
    mmi_paddsh(&c,op());assert(c.r[3].sh[0]==SHRT_MAX);
    mmi_psubsh(&c,op());assert(c.r[3].sh[1]==SHRT_MIN);
    c.r[1].sb[0]=SCHAR_MAX;c.r[2].sb[0]=1;
    c.r[1].sb[1]=SCHAR_MIN;c.r[2].sb[1]=1;
    mmi_paddsb(&c,op());assert(c.r[3].sb[0]==SCHAR_MAX);
    mmi_psubsb(&c,op());assert(c.r[3].sb[1]==SCHAR_MIN);
    c.r[1].w[0]=0xffffffff;c.r[2].w[0]=1;
    mmi_padduw(&c,op());assert(c.r[3].w[0]==0xffffffff);
    mmi_psubuw(&c,op(2,1));assert(c.r[3].w[0]==0);
    c.r[1].sw[0]=-1;c.r[2].sw[0]=1;c.r[1].sw[1]=7;c.r[2].sw[1]=7;
    mmi_pcgtw(&c,op());assert(c.r[3].w[0]==0&&c.r[3].w[1]==0);
    mmi_pceqw(&c,op());assert(c.r[3].w[0]==0&&c.r[3].w[1]==~0u);
    c.r[1].sh[0]=-1;c.r[2].sh[0]=1;
    mmi_pmaxh(&c,op());assert(c.r[3].sh[0]==1);
    mmi_pminh(&c,op());assert(c.r[3].sh[0]==-1);
    mmi_pcgth(&c,op(2,1));assert(c.r[3].h[0]==0xffff);
    c.r[1].sb[0]=-1;c.r[2].sb[0]=1;
    mmi_pcgtb(&c,op(2,1));assert(c.r[3].b[0]==255);
    mmi_pceqb(&c,op());assert(c.r[3].b[0]==0);
    for(int i=0;i<16;++i){c.r[1].b[i]=u8(0x80+i);c.r[2].b[i]=u8(i);}
    mmi_pextlb(&c,op());for(int i=0;i<8;++i){assert(c.r[3].b[2*i]==i&&c.r[3].b[2*i+1]==0x80+i);}
    mmi_ppacb(&c,op());for(int i=0;i<8;++i){assert(c.r[3].b[i]==2*i&&c.r[3].b[i+8]==0x80+2*i);}
    mmi_pinth(&c,op());for(int i=0;i<4;++i){assert(c.r[3].h[2*i]==c.r[2].h[i]&&c.r[3].h[2*i+1]==c.r[1].h[i+4]);}
    c.r[2].w[0]=10;c.r[2].w[1]=20;c.r[2].w[2]=30;c.r[2].w[3]=40;
    mmi_pexcw(&c,op());assert(c.r[3].w[0]==10&&c.r[3].w[1]==30&&c.r[3].w[2]==20&&c.r[3].w[3]==40);
    mmi_pexcw(&c,op(1,2,2));assert(c.r[2].w[1]==30&&c.r[2].w[2]==20);
    for(int i=0;i<4;++i)c.r[2].w[i]=0x8000|u32(i*5431);
    mmi_pext5(&c,op());mmi_ppac5(&c,op(1,3,4));
    for(int i=0;i<4;++i)assert(c.r[4].w[i]==c.r[2].w[i]);
    c.r[1].w[0]=0;c.r[1].w[1]=~0u;
    mmi_plzcw(&c,op());assert(c.r[3].w[0]==31&&c.r[3].w[1]==31);
    c.r[1].w[0]=0x00100000;mmi_plzcw(&c,op());assert(c.r[3].w[0]==10);
    c.r[1].w[0]=5;c.r[2].q=(u128(0xfedcba9876543210ULL)<<64)|0x0123456789abcdefULL;
    c.r[1].d[1]=0x1020304050607080ULL;
    for(unsigned s:{0u,1u,7u,8u,15u}) {
        c.r[4].w[0]=s;
        mmi_mtsab(&c,op(4,0,0,0));
        assert(c.sa==s);
        mmi_qfsrv(&c,op());
        u128 expected=s ? (c.r[2].q>>(s*8))|(c.r[1].q<<(128-s*8)) : c.r[2].q;
        assert(c.r[3].q==expected);
    }
    c.r[4].w[0]=3;mmi_mtsab(&c,op(4,0,0)|5);assert(c.sa==6);
    mmi_mtsah(&c,op(4,0,0)|2);assert(c.sa==2);
    mmi_mfsa(&c,op(0,0,5));assert(c.r[5].d[0]==2);
    c.r[0].q=0; mmi_paddw(&c,op(1,2,0));assert(c.r[0].q==0);
}
static void arithmetic(){
    EEContext c{};c.r[1].sw[0]=-3;c.r[2].sw[0]=5;
    alu_mult(&c,op());assert(c.lo.d[0]==u64(-15LL)&&c.hi.d[0]==~0ULL&&c.r[3].d[0]==u64(-15LL));
    alu_div(&c,op());assert(c.lo.d[0]==0&&c.hi.d[0]==u64(-3LL));
    c.r[2].w[0]=0;alu_div(&c,op());assert(c.lo.d[0]==1&&c.hi.d[0]==u64(-3LL));
    c.r[1].w[0]=0x80000000;c.r[2].w[0]=~0u;alu_div(&c,op());assert(c.lo.d[0]==0xffffffff80000000ULL&&c.hi.d[0]==0);
    c.r[1].sw[0]=-2;c.r[1].sw[2]=7;c.r[2].sw[0]=3;c.r[2].sw[2]=4;
    mmi_pmultw(&c,op());assert(c.r[3].sd[0]==-6&&c.r[3].sd[1]==28);
    assert(c.lo.d[0]==u64(-6LL)&&c.hi.d[0]==~0ULL&&c.lo.d[1]==28&&c.hi.d[1]==0);
    mmi_pdivw(&c,op());assert(c.lo.d[0]==0&&c.hi.d[0]==u64(-2LL)&&c.lo.d[1]==1&&c.hi.d[1]==3);
    c.lo.q=0;c.hi.q=0;c.r[1].sw[0]=2;c.r[2].sw[0]=3;c.r[1].sw[2]=4;c.r[2].sw[2]=5;
    mmi_pmaddw(&c,op());assert(c.lo.w[0]==6&&c.hi.w[0]==0&&c.r[3].w[0]==6&&c.lo.w[2]==20&&c.r[3].w[2]==20);
    c.r[2].sh[0]=3;c.r[1].sw[0]=-9;c.r[1].sw[1]=10;
    mmi_pdivbw(&c,op());assert(c.lo.sw[0]==-3&&c.hi.sw[0]==0&&c.lo.sw[1]==3&&c.hi.sw[1]==1);
    for(int i=0;i<8;++i){c.r[1].sh[i]=i+1;c.r[2].sh[i]=2;}
    mmi_pmulth(&c,op());assert(c.lo.w[0]==2&&c.lo.w[1]==4&&c.hi.w[0]==6&&c.r[3].w[1]==6);
    mmi_phmadh(&c,op());assert(c.lo.w[0]==6&&c.lo.w[1]==4&&c.hi.w[0]==14&&c.hi.w[1]==8);
    c.lo.w[0]=0;c.hi.w[0]=1;
    mmi_pmfhl_slw(&c,op());assert(c.r[3].sd[0]==INT_MAX);
    c.hi.w[0]=~0u;mmi_pmfhl_slw(&c,op());assert(c.r[3].sd[0]==INT_MIN);
}
static void floating(){
    EEContext c{};constexpr u32 w=fp();
    c.fpr[1].u=0x7f7fffff;c.fpr[2].f=2.0f;
    fpu_mul_s(&c,w);assert(c.fpr[3].u==0x7f7fffff);
    c.fpr[2].f=0;fpu_div_s(&c,w);assert(c.fpr[3].u==0x7f7fffff&&(c.fcr31&0x10020)==0x10020);
    c.fpr[2].f=-9;fpu_sqrt_s(&c,w);assert(c.fpr[3].f==3&&(c.fcr31&0x20040)==0x20040);
    c.fpr[1].f=-3.9f;fpu_cvt_w_s(&c,w);assert(c.fpr[3].s==-3);
    c.fpr[1].f=2147483648.0f;fpu_cvt_w_s(&c,w);assert(c.fpr[3].u==0x7fffffff);
    c.fpr[1].f=-2147483648.0f;fpu_cvt_w_s(&c,w);assert(c.fpr[3].u==0x80000000);
    c.fpr[1].u=0x80000000;c.fpr[2].u=0;
    fpu_c_lt_s(&c,w);assert(!(c.fcr31&0x800000));
    fpu_c_le_s(&c,w);assert(c.fcr31&0x800000);
    c.facc.f=10;c.fpr[1].f=3;c.fpr[2].f=2;
    fpu_madd_s(&c,w);assert(c.fpr[3].f==16);
    fpu_msub_s(&c,w);assert(c.fpr[3].f==4);
    assert(c.facc.f==10);
    fpu_madda_s(&c,w);assert(c.facc.f==16);
    fpu_msuba_s(&c,w);assert(c.facc.f==10);
    c.fpr[1].u=0xff800000;fpu_abs_s(&c,w);assert(c.fpr[3].u==0x7f800000);
}
int main(){const unsigned mxcsr=_mm_getcsr();_mm_setcsr((mxcsr&~0x603fu)|0xffc0u);packed();arithmetic();floating();_mm_setcsr(mxcsr);std::cout<<"ee_ops_tests passed\n";}
