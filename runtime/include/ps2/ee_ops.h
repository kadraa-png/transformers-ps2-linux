#pragma once
// R5900 interpreter semantics (PCSX2 1.6). All packed operations snapshot their inputs
// before writing: rd may equal rs or rt, and r0 is immutable.
// FPU arithmetic assumes MXCSR=0xffc0 (RC=truncate, FTZ, DAZ, all exceptions
// masked). This is PCSX2 1.6's DEFAULT_sseMXCSR in pcsx2/Config.h: the EE
// rounds toward zero, flushes denormals, and must not fuse intermediate ops.
#include "ps2/ee_context.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <type_traits>

namespace ee_ops_detail {
PS2_FORCEINLINE unsigned rs(u32 w) { return (w >> 21) & 31; }
PS2_FORCEINLINE unsigned rt(u32 w) { return (w >> 16) & 31; }
PS2_FORCEINLINE unsigned rd(u32 w) { return (w >> 11) & 31; }
PS2_FORCEINLINE unsigned sa(u32 w) { return (w >> 6) & 31; }
PS2_FORCEINLINE void put(EEContext* c, u32 w, qword v) { if (rd(w)) c->r[rd(w)] = v; }
PS2_FORCEINLINE void low(EEContext* c, u32 w, u64 v) { if (rd(w)) c->r[rd(w)].d[0] = v; }
PS2_FORCEINLINE u64 sx(u32 x) { return u64(s64(s32(x))); }
PS2_FORCEINLINE void hilo(EEContext* c, unsigned i, u32 lo, u32 hi) {
    c->lo.d[i] = sx(lo); c->hi.d[i] = sx(hi);
}
PS2_FORCEINLINE void divide(EEContext* c, unsigned i, u32 a, u32 b, bool sign) {
    u32 q, r;
    if (!b) { q = sign && s32(a) < 0 ? 1 : ~0u; r = a; }
    else if (sign && a == 0x80000000 && b == ~0u) { q = a; r = 0; }
    else if (sign) { q = u32(s32(a) / s32(b)); r = u32(s32(a) % s32(b)); }
    else { q = a / b; r = a % b; }
    hilo(c, i, q, r);
}
template<typename T> PS2_FORCEINLINE T clamp(s64 v) {
    return T(std::clamp(v, s64(std::numeric_limits<T>::min()), s64(std::numeric_limits<T>::max())));
}
PS2_FORCEINLINE void packed_words(EEContext* c, u32 w, int mode) {
    const qword a = c->r[rs(w)], b = c->r[rt(w)]; qword v{};
    for (int i=0;i<4;++i) {
        switch(mode) {
        case 0: v.w[i]=a.w[i]+b.w[i]; break;
        case 1: v.w[i]=a.w[i]-b.w[i]; break;
        case 2: v.w[i]=a.sw[i]>b.sw[i] ? ~0u : 0; break;
        case 3: v.w[i]=std::max(a.sw[i], b.sw[i]); break;
        case 4: v.w[i]=u32(clamp<s32>(s64(a.sw[i])+b.sw[i])); break;
        case 5: v.w[i]=u32(clamp<s32>(s64(a.sw[i])-b.sw[i])); break;
        case 6: v.w[i]=a.sw[i]==b.sw[i] ? ~0u : 0; break;
        case 7: v.w[i]=std::min(a.sw[i], b.sw[i]); break;
        case 8: v.w[i]=a.w[i]>~0u-b.w[i] ? ~0u : a.w[i]+b.w[i]; break;
        case 9: v.w[i]=a.w[i]<b.w[i] ? 0 : a.w[i]-b.w[i]; break;
        case 10: v.w[i]=b.w[i]==0x80000000 ? 0x7fffffff : u32(b.sw[i]<0 ? -s64(b.sw[i]) : b.sw[i]); break;
        }
    }
    put(c,w,v);
}
PS2_FORCEINLINE void packed_half(EEContext* c, u32 w, int mode) {
    const qword a=c->r[rs(w)], b=c->r[rt(w)]; qword v{};
    for (int i=0;i<8;++i) switch(mode) {
    case 0: v.h[i]=a.h[i]+b.h[i]; break;
    case 1: v.h[i]=a.h[i]-b.h[i]; break;
    case 2: v.h[i]=a.sh[i]>b.sh[i] ? 0xffff : 0; break;
    case 3: v.h[i]=std::max(a.sh[i],b.sh[i]); break;
    case 4: v.h[i]=u16(clamp<s16>(s64(a.sh[i])+b.sh[i])); break;
    case 5: v.h[i]=u16(clamp<s16>(s64(a.sh[i])-b.sh[i])); break;
    case 6: v.h[i]=b.h[i]==0x8000 ? 0x7fff : u16(b.sh[i]<0 ? -int(b.sh[i]) : b.sh[i]); break;
    case 7: v.h[i]=a.h[i]==b.h[i] ? 0xffff : 0; break;
    case 8: v.h[i]=std::min(a.sh[i],b.sh[i]); break;
    case 9: v.h[i]=std::min(65535, int(a.h[i])+int(b.h[i])); break;
    case 10: v.h[i]=a.h[i]<b.h[i] ? 0 : a.h[i]-b.h[i]; break;
    case 11: v.h[i]= i<4 ? a.h[i]-b.h[i] : a.h[i]+b.h[i]; break;
    } put(c,w,v);
}
PS2_FORCEINLINE void packed_byte(EEContext* c, u32 w, int mode) {
    const qword a=c->r[rs(w)], b=c->r[rt(w)]; qword v{};
    for (int i=0;i<16;++i) switch(mode) {
    case 0: v.b[i]=a.b[i]+b.b[i]; break;
    case 1: v.b[i]=a.b[i]-b.b[i]; break;
    case 2: v.b[i]=a.sb[i]>b.sb[i] ? 255 : 0; break;
    case 3: v.b[i]=u8(clamp<s8>(s64(a.sb[i])+b.sb[i])); break;
    case 4: v.b[i]=u8(clamp<s8>(s64(a.sb[i])-b.sb[i])); break;
    case 5: v.b[i]=a.b[i]==b.b[i] ? 255 : 0; break;
    case 6: v.b[i]=std::min(255,int(a.b[i])+int(b.b[i])); break;
    case 7: v.b[i]=a.b[i]<b.b[i] ? 0 : a.b[i]-b.b[i]; break;
    } put(c,w,v);
}
PS2_FORCEINLINE void interleave(EEContext* c,u32 w,int size,int start) {
    const qword a=c->r[rs(w)],b=c->r[rt(w)]; qword v{};
    for(int i=0;i<16/size/2;++i) for(int j=0;j<size;++j) {
        v.b[(i*2)*size+j]=b.b[(start+i)*size+j];
        v.b[(i*2+1)*size+j]=a.b[(start+i)*size+j];
    } put(c,w,v);
}
PS2_FORCEINLINE void pack(EEContext* c,u32 w,int size) {
    const qword a=c->r[rs(w)],b=c->r[rt(w)]; qword v{};
    for(int i=0;i<16/size/2;++i) for(int j=0;j<size;++j) {
        v.b[i*size+j]=b.b[2*i*size+j];
        v.b[(i+8/size)*size+j]=a.b[2*i*size+j];
    } put(c,w,v);
}
PS2_FORCEINLINE void product_words(EEContext* c,u32 w,int mode) {
    const qword a=c->r[rs(w)],b=c->r[rt(w)]; qword v{};
    for(unsigned i=0;i<2;++i) {
        const unsigned n=i*2;
        const u64 p= mode==3 || mode==4 ? u64(a.w[n])*b.w[n] : u64(s64(a.sw[n])*s64(b.sw[n]));
        if (mode==1 || mode==2) {
            // PCSX2's signed PMADDW/PMSUBW high word uses its observed
            // /0xffffffff approximation, not an ordinary 64-bit carry.
            s128 upper=(s128(c->hi.sw[n])*s128(0x100000000ULL));
            upper += mode==1 ? s64(p) : -s128(s64(p));
            if (mode==1 && !i &&
                ((b.sw[n]&0x7fffffff)==0 || (b.sw[n]&0x7fffffff)==0x7fffffff) &&
                a.sw[n]!=b.sw[n]) upper+=0x70000000;
            const u32 h=u32(upper/0xffffffffLL);
            const u32 l=mode==1 ? c->lo.w[n]+u32(p) : c->lo.w[n]-u32(p);
            hilo(c,i,l,h);
            v.w[n]=l;v.w[n+1]=h;
        } else {
            const u64 total=mode==3 ? p+((u64(c->hi.w[n])<<32)|c->lo.w[n]) : p;
            hilo(c,i,u32(total),u32(total>>32));
            v.d[i]=total;
        }
    } put(c,w,v);
}
PS2_FORCEINLINE void product_half(EEContext* c,u32 w,int mode) {
    const qword a=c->r[rs(w)],b=c->r[rt(w)]; qword v{};
    for(int i=0;i<8;++i) {
        const u32 p=u32(int(a.sh[i])*int(b.sh[i]));
        u32& dst = (i%4<2 ? c->lo : c->hi).w[(i/4)*2+(i%2)];
        if(mode==1) dst+=p;
        else if(mode==2) dst-=p;
        else if(mode==3 || mode==4) {
            if(i%2) dst=mode==3 ? p : ~p;
            else {
                const u32 next=u32(int(a.sh[i+1])*int(b.sh[i+1]));
                dst=mode==3 ? p+next : next-p;
            }
        } else dst=p;
    }
    for(int i=0;i<4;++i) v.w[i]=(i%2 ? c->hi : c->lo).w[(i/2)*2];
    put(c,w,v);
}
PS2_FORCEINLINE void permute(EEContext* c,u32 w,const int* map,int size) {
    const qword b=c->r[rt(w)]; qword v{};
    for(int i=0;i<16/size;++i) for(int j=0;j<size;++j) v.b[i*size+j]=b.b[map[i]*size+j];
    put(c,w,v);
}
} // namespace ee_ops_detail

#define EE_REGS const qword a=c->r[ee_ops_detail::rs(w)], b=c->r[ee_ops_detail::rt(w)]; qword v{}
#define EE_PUT ee_ops_detail::put(c,w,v)
#define EE_WORD(name,mode) PS2_FORCEINLINE void mmi_##name(EEContext* c,u32 w){ ee_ops_detail::packed_words(c,w,mode); }
#define EE_HALF(name,mode) PS2_FORCEINLINE void mmi_##name(EEContext* c,u32 w){ ee_ops_detail::packed_half(c,w,mode); }
#define EE_BYTE(name,mode) PS2_FORCEINLINE void mmi_##name(EEContext* c,u32 w){ ee_ops_detail::packed_byte(c,w,mode); }
#define EE_INTER(name,size,start) PS2_FORCEINLINE void mmi_##name(EEContext* c,u32 w){ ee_ops_detail::interleave(c,w,size,start); }
#define EE_PACK(name,size) PS2_FORCEINLINE void mmi_##name(EEContext* c,u32 w){ ee_ops_detail::pack(c,w,size); }
#define EE_PRODW(name,mode) PS2_FORCEINLINE void mmi_##name(EEContext* c,u32 w){ ee_ops_detail::product_words(c,w,mode); }

PS2_FORCEINLINE void alu_mult(EEContext* c,u32 w){ const u64 p=u64(s64(c->r[ee_ops_detail::rs(w)].sw[0])*c->r[ee_ops_detail::rt(w)].sw[0]); ee_ops_detail::hilo(c,0,u32(p),u32(p>>32)); ee_ops_detail::low(c,w,c->lo.d[0]); }
PS2_FORCEINLINE void alu_multu(EEContext* c,u32 w){ const u64 p=u64(c->r[ee_ops_detail::rs(w)].w[0])*c->r[ee_ops_detail::rt(w)].w[0]; ee_ops_detail::hilo(c,0,u32(p),u32(p>>32)); ee_ops_detail::low(c,w,c->lo.d[0]); }
PS2_FORCEINLINE void alu_div(EEContext* c,u32 w){ ee_ops_detail::divide(c,0,c->r[ee_ops_detail::rs(w)].w[0],c->r[ee_ops_detail::rt(w)].w[0],true); }
PS2_FORCEINLINE void alu_divu(EEContext* c,u32 w){ ee_ops_detail::divide(c,0,c->r[ee_ops_detail::rs(w)].w[0],c->r[ee_ops_detail::rt(w)].w[0],false); }
#define EE_SCALAR1(name,base) PS2_FORCEINLINE void mmi_##name(EEContext* c,u32 w){ const u64 p=base ? u64(c->r[ee_ops_detail::rs(w)].w[0])*c->r[ee_ops_detail::rt(w)].w[0] : u64(s64(c->r[ee_ops_detail::rs(w)].sw[0])*c->r[ee_ops_detail::rt(w)].sw[0]); ee_ops_detail::hilo(c,1,u32(p),u32(p>>32)); ee_ops_detail::low(c,w,c->lo.d[1]); }
EE_SCALAR1(mult1,false) EE_SCALAR1(multu1,true)
PS2_FORCEINLINE void mmi_div1(EEContext* c,u32 w){ ee_ops_detail::divide(c,1,c->r[ee_ops_detail::rs(w)].w[0],c->r[ee_ops_detail::rt(w)].w[0],true); }
PS2_FORCEINLINE void mmi_divu1(EEContext* c,u32 w){ ee_ops_detail::divide(c,1,c->r[ee_ops_detail::rs(w)].w[0],c->r[ee_ops_detail::rt(w)].w[0],false); }
#define EE_MADD(name,index,unsign) PS2_FORCEINLINE void mmi_##name(EEContext* c,u32 w){ const u64 p=unsign ? u64(c->r[ee_ops_detail::rs(w)].w[0])*c->r[ee_ops_detail::rt(w)].w[0] : u64(s64(c->r[ee_ops_detail::rs(w)].sw[0])*c->r[ee_ops_detail::rt(w)].sw[0]); const u64 sum=((u64(c->hi.w[index*2])<<32)|c->lo.w[index*2])+p; ee_ops_detail::hilo(c,index,u32(sum),u32(sum>>32)); ee_ops_detail::low(c,w,c->lo.d[index]); }
EE_MADD(madd,0,false) EE_MADD(maddu,0,true) EE_MADD(madd1,1,false) EE_MADD(maddu1,1,true)
PS2_FORCEINLINE void mmi_mfhi1(EEContext* c,u32 w){ ee_ops_detail::low(c,w,c->hi.d[1]); }
PS2_FORCEINLINE void mmi_mflo1(EEContext* c,u32 w){ ee_ops_detail::low(c,w,c->lo.d[1]); }
PS2_FORCEINLINE void mmi_mthi1(EEContext* c,u32 w){ c->hi.d[1]=c->r[ee_ops_detail::rs(w)].d[0]; }
PS2_FORCEINLINE void mmi_mtlo1(EEContext* c,u32 w){ c->lo.d[1]=c->r[ee_ops_detail::rs(w)].d[0]; }
PS2_FORCEINLINE void mmi_mfsa(EEContext* c,u32 w){ ee_ops_detail::low(c,w,c->sa); }
PS2_FORCEINLINE void mmi_mtsa(EEContext* c,u32 w){ c->sa=c->r[ee_ops_detail::rs(w)].w[0]; }
PS2_FORCEINLINE void mmi_mtsab(EEContext* c,u32 w){ c->sa=(c->r[ee_ops_detail::rs(w)].w[0]^(w&15))&15; }
PS2_FORCEINLINE void mmi_mtsah(EEContext* c,u32 w){ c->sa=((c->r[ee_ops_detail::rs(w)].w[0]^(w&7))&7)*2; }
PS2_FORCEINLINE void mmi_plzcw(EEContext* c,u32 w){ if(!ee_ops_detail::rd(w))return; const qword a=c->r[ee_ops_detail::rs(w)]; c->r[ee_ops_detail::rd(w)].w[0]=std::countl_zero(a.w[0] ^ (a.w[0]&0x80000000 ? ~0u:0u))-1; c->r[ee_ops_detail::rd(w)].w[1]=std::countl_zero(a.w[1] ^ (a.w[1]&0x80000000 ? ~0u:0u))-1; }
#define EE_SHIFT(name,type,expr) PS2_FORCEINLINE void mmi_##name(EEContext* c,u32 w){ const qword b=c->r[ee_ops_detail::rt(w)]; qword v{}; for(int i=0;i<16/int(sizeof(type));++i){ if constexpr(sizeof(type)==2){ v.h[i]=u16(expr); }else{ v.w[i]=u32(expr); } } ee_ops_detail::put(c,w,v); }
EE_SHIFT(psllh,u16,b.h[i]<< (ee_ops_detail::sa(w)&15)) EE_SHIFT(psrlh,u16,b.h[i]>>(ee_ops_detail::sa(w)&15)) EE_SHIFT(psrah,u16,b.sh[i]>>(ee_ops_detail::sa(w)&15))
EE_SHIFT(psllw,u32,b.w[i]<<ee_ops_detail::sa(w)) EE_SHIFT(psrlw,u32,b.w[i]>>ee_ops_detail::sa(w)) EE_SHIFT(psraw,u32,b.sw[i]>>ee_ops_detail::sa(w))
EE_WORD(paddw,0) EE_WORD(psubw,1) EE_WORD(pcgtw,2) EE_WORD(pmaxw,3) EE_WORD(paddsw,4) EE_WORD(psubsw,5)
EE_HALF(paddh,0) EE_HALF(psubh,1) EE_HALF(pcgth,2) EE_HALF(pmaxh,3) EE_HALF(paddsh,4) EE_HALF(psubsh,5)
EE_BYTE(paddb,0) EE_BYTE(psubb,1) EE_BYTE(pcgtb,2) EE_BYTE(paddsb,3) EE_BYTE(psubsb,4)
EE_INTER(pextlw,4,0) EE_INTER(pextlh,2,0) EE_INTER(pextlb,1,0)
EE_PACK(ppacw,4) EE_PACK(ppach,2) EE_PACK(ppacb,1)
PS2_FORCEINLINE void mmi_pext5(EEContext* c,u32 w){ EE_REGS; (void)a; for(int i=0;i<4;++i)v.w[i]=((b.w[i]&31)<<3)|((b.w[i]&0x3e0)<<6)|((b.w[i]&0x7c00)<<9)|((b.w[i]&0x8000)<<16); EE_PUT; }
PS2_FORCEINLINE void mmi_ppac5(EEContext* c,u32 w){ EE_REGS; (void)a; for(int i=0;i<4;++i)v.w[i]=((b.w[i]>>3)&31)|((b.w[i]>>6)&0x3e0)|((b.w[i]>>9)&0x7c00)|((b.w[i]>>16)&0x8000); EE_PUT; }
EE_WORD(pabsw,10) EE_WORD(pceqw,6) EE_WORD(pminw,7) EE_WORD(padduw,8) EE_WORD(psubuw,9)
EE_HALF(pabsh,6) EE_HALF(pceqh,7) EE_HALF(pminh,8) EE_HALF(padduh,9) EE_HALF(psubuh,10) EE_HALF(padsbh,11)
EE_BYTE(pceqb,5) EE_BYTE(paddub,6) EE_BYTE(psubub,7)
EE_INTER(pextuw,4,2) EE_INTER(pextuh,2,4) EE_INTER(pextub,1,8)
PS2_FORCEINLINE void mmi_qfsrv(EEContext* c,u32 w){ EE_REGS; const unsigned n=(c->sa&15)*8; if(!n)v=b; else { v.q=(b.q>>n)|(a.q<<(128-n)); } EE_PUT; }
EE_PRODW(pmultw,0) EE_PRODW(pmaddw,1) EE_PRODW(pmsubw,2) EE_PRODW(pmadduw,3) EE_PRODW(pmultuw,4)
#define EE_DIVPACK(name,sign) PS2_FORCEINLINE void mmi_##name(EEContext* c,u32 w){ const qword a=c->r[ee_ops_detail::rs(w)],b=c->r[ee_ops_detail::rt(w)]; for(unsigned i=0;i<2;++i) ee_ops_detail::divide(c,i,a.w[i*2],b.w[i*2],sign); }
EE_DIVPACK(pdivw,true) EE_DIVPACK(pdivuw,false)
PS2_FORCEINLINE void mmi_pdivbw(EEContext* c,u32 w){ const qword a=c->r[ee_ops_detail::rs(w)],b=c->r[ee_ops_detail::rt(w)]; for(int i=0;i<4;++i){ const s32 d=b.sh[0]; const s32 n=a.sw[i]; u32 quotient,rem; if(!d){ quotient=n<0?1:~0u; rem=u32(n); }else if(n==std::numeric_limits<s32>::min()&&d==-1){quotient=u32(n);rem=0;}else{quotient=u32(n/d);rem=u32(n%d);} c->lo.w[i]=quotient; c->hi.w[i]=rem; } }
PS2_FORCEINLINE void mmi_pmfhi(EEContext* c,u32 w){ ee_ops_detail::put(c,w,c->hi); }
PS2_FORCEINLINE void mmi_pmflo(EEContext* c,u32 w){ ee_ops_detail::put(c,w,c->lo); }
PS2_FORCEINLINE void mmi_pmthi(EEContext* c,u32 w){ c->hi=c->r[ee_ops_detail::rs(w)]; }
PS2_FORCEINLINE void mmi_pmtlo(EEContext* c,u32 w){ c->lo=c->r[ee_ops_detail::rs(w)]; }
PS2_FORCEINLINE void mmi_pinth(EEContext* c,u32 w){ EE_REGS; for(int i=0;i<4;++i){v.h[2*i]=b.h[i];v.h[2*i+1]=a.h[i+4];} EE_PUT; }
PS2_FORCEINLINE void mmi_pinteh(EEContext* c,u32 w){ EE_REGS; for(int i=0;i<4;++i){v.h[2*i]=b.h[2*i];v.h[2*i+1]=a.h[2*i];} EE_PUT; }
PS2_FORCEINLINE void mmi_pcpyld(EEContext* c,u32 w){ EE_REGS;v.d[0]=b.d[0];v.d[1]=a.d[0];EE_PUT; }
PS2_FORCEINLINE void mmi_pcpyud(EEContext* c,u32 w){ EE_REGS;v.d[0]=a.d[1];v.d[1]=b.d[1];EE_PUT; }
#define EE_HALFPROD(name,mode) PS2_FORCEINLINE void mmi_##name(EEContext* c,u32 w){ee_ops_detail::product_half(c,w,mode);}
EE_HALFPROD(pmulth,0) EE_HALFPROD(pmaddh,1) EE_HALFPROD(pmsubh,2) EE_HALFPROD(phmadh,3) EE_HALFPROD(phmsbh,4)
#define EE_LOGIC(name,expr) PS2_FORCEINLINE void mmi_##name(EEContext* c,u32 w){ EE_REGS;v.q=(expr);EE_PUT; }
EE_LOGIC(pand,a.q&b.q) EE_LOGIC(pxor,a.q^b.q) EE_LOGIC(por,a.q|b.q) EE_LOGIC(pnor,~(a.q|b.q))
#define EE_PERM(name,size,...) PS2_FORCEINLINE void mmi_##name(EEContext* c,u32 w){constexpr int order[]={__VA_ARGS__};ee_ops_detail::permute(c,w,order,size);}
EE_PERM(pexeh,2,2,1,0,3,6,5,4,7) EE_PERM(prevh,2,3,2,1,0,7,6,5,4)
EE_PERM(pexew,4,2,1,0,3) EE_PERM(prot3w,4,1,2,0,3) EE_PERM(pexcw,4,0,2,1,3)
EE_PERM(pexch,2,0,2,1,3,4,6,5,7) EE_PERM(pcpyh,2,0,0,0,0,4,4,4,4)
#define EE_VARSHIFT(name,expr) PS2_FORCEINLINE void mmi_##name(EEContext* c,u32 w){ EE_REGS;for(int i=0;i<2;++i)v.d[i]=ee_ops_detail::sx(u32(expr)); EE_PUT; }
EE_VARSHIFT(psllvw,b.w[2*i]<<(a.w[2*i]&31)) EE_VARSHIFT(psrlvw,b.w[2*i]>>(a.w[2*i]&31)) EE_VARSHIFT(psravw,b.sw[2*i]>>(a.w[2*i]&31))
PS2_FORCEINLINE void mmi_pmfhl_lw(EEContext* c,u32 w){qword v{};for(int i=0;i<2;++i){v.w[2*i]=c->lo.w[2*i];v.w[2*i+1]=c->hi.w[2*i];}EE_PUT;}
PS2_FORCEINLINE void mmi_pmfhl_uw(EEContext* c,u32 w){qword v{};for(int i=0;i<2;++i){v.w[2*i]=c->lo.w[2*i+1];v.w[2*i+1]=c->hi.w[2*i+1];}EE_PUT;}
PS2_FORCEINLINE void mmi_pmfhl_slw(EEContext* c,u32 w){qword v{};for(int i=0;i<2;++i){const s64 x=s64((u64(c->hi.w[2*i])<<32)|c->lo.w[2*i]);v.d[i]=ee_ops_detail::sx(u32(ee_ops_detail::clamp<s32>(x)));}EE_PUT;}
PS2_FORCEINLINE void mmi_pmfhl_lh(EEContext* c,u32 w){qword v{};for(int i=0;i<2;++i){for(int j=0;j<2;++j){v.h[i*4+j]=c->lo.h[i*4+j*2];v.h[i*4+j+2]=c->hi.h[i*4+j*2];}}EE_PUT;}
PS2_FORCEINLINE void mmi_pmfhl_sh(EEContext* c,u32 w){qword v{};for(int i=0;i<2;++i){for(int j=0;j<2;++j){v.h[i*4+j]=u16(ee_ops_detail::clamp<s16>(c->lo.sw[i*2+j]));v.h[i*4+j+2]=u16(ee_ops_detail::clamp<s16>(c->hi.sw[i*2+j]));}}EE_PUT;}
PS2_FORCEINLINE void mmi_pmthl_lw(EEContext* c,u32 w){ const qword a=c->r[ee_ops_detail::rs(w)]; for(int i=0;i<2;++i){c->lo.w[2*i]=a.w[2*i];c->hi.w[2*i]=a.w[2*i+1];} }

namespace ee_ops_detail {
constexpr u32 flag_c=0x800000, flag_i=0x20000, flag_d=0x10000,
              flag_o=0x8000, flag_u=0x4000;
constexpr u32 sticky_i=0x40, sticky_d=0x20, sticky_o=0x10, sticky_u=0x08;
PS2_FORCEINLINE float fvalue(u32 bits) {
    if ((bits & 0x7f800000)==0) bits &= 0x80000000;
    else if ((bits & 0x7f800000)==0x7f800000) bits=(bits&0x80000000)|0x7f7fffff;
    return std::bit_cast<float>(bits);
}
PS2_FORCEINLINE u32 result(EEContext* c,float f,u32 flags) {
    u32 bits=std::bit_cast<u32>(f);
    if ((bits&0x7f800000)==0x7f800000) {
        bits=(bits&0x80000000)|0x7f7fffff;
        c->fcr31 |= flags & (flag_o|sticky_o);
    } else if ((bits&0x7f800000)==0 && (bits&0x7fffff)) {
        bits &= 0x80000000;
        c->fcr31 |= flags & (flag_u|sticky_u);
    }
    return bits;
}
PS2_FORCEINLINE unsigned ft(u32 w){return (w>>16)&31;}
PS2_FORCEINLINE unsigned fs(u32 w){return (w>>11)&31;}
PS2_FORCEINLINE unsigned fd(u32 w){return (w>>6)&31;}
PS2_FORCEINLINE float src(EEContext* c,u32 w){return fvalue(c->fpr[fs(w)].u);}
PS2_FORCEINLINE float target(EEContext* c,u32 w){return fvalue(c->fpr[ft(w)].u);}
PS2_FORCEINLINE void store(EEContext* c,u32 w,float f,u32 flags=flag_o|sticky_o|flag_u|sticky_u){
    c->fpr[fd(w)].u=result(c,f,flags);
}
PS2_FORCEINLINE void acc(EEContext* c,float f,u32 flags=flag_o|sticky_o|flag_u|sticky_u){
    c->facc.u=result(c,f,flags);
}
}
#define EE_FPU_BINARY(name,expr) PS2_FORCEINLINE void fpu_##name(EEContext* c,u32 w){const float a=ee_ops_detail::src(c,w),b=ee_ops_detail::target(c,w);ee_ops_detail::store(c,w,expr);}
#define EE_FPU_ACC(name,expr) PS2_FORCEINLINE void fpu_##name(EEContext* c,u32 w){const float a=ee_ops_detail::src(c,w),b=ee_ops_detail::target(c,w);ee_ops_detail::acc(c,expr);}
EE_FPU_BINARY(add_s,a+b) EE_FPU_BINARY(sub_s,a-b) EE_FPU_BINARY(mul_s,a*b)
EE_FPU_ACC(adda_s,a+b) EE_FPU_ACC(suba_s,a-b) EE_FPU_ACC(mula_s,a*b)
PS2_FORCEINLINE void fpu_div_s(EEContext* c,u32 w){
    const u32 fs=c->fpr[ee_ops_detail::fs(w)].u, ft=c->fpr[ee_ops_detail::ft(w)].u;
    if (!(ft&0x7f800000)) {
        c->fcr31 |= (fs&0x7f800000) ? (ee_ops_detail::flag_d|ee_ops_detail::sticky_d) : (ee_ops_detail::flag_i|ee_ops_detail::sticky_i);
        c->fpr[ee_ops_detail::fd(w)].u=((fs^ft)&0x80000000)|0x7f7fffff;
    } else ee_ops_detail::store(c,w,ee_ops_detail::src(c,w)/ee_ops_detail::target(c,w),0);
}
PS2_FORCEINLINE void fpu_sqrt_s(EEContext* c,u32 w){
    const u32 ft=c->fpr[ee_ops_detail::ft(w)].u;
    if (!(ft&0x7f800000)) c->fpr[ee_ops_detail::fd(w)].u=0;
    else {
        if (ft&0x80000000) c->fcr31 |= ee_ops_detail::flag_i|ee_ops_detail::sticky_i;
        ee_ops_detail::store(c,w,std::sqrt(std::fabs(ee_ops_detail::target(c,w))),0);
    }
    c->fcr31 &= ~ee_ops_detail::flag_d;
}
PS2_FORCEINLINE void fpu_rsqrt_s(EEContext* c,u32 w){
    const u32 ft=c->fpr[ee_ops_detail::ft(w)].u,fs=c->fpr[ee_ops_detail::fs(w)].u;
    if (!(ft&0x7f800000)) {
        c->fcr31 |= ee_ops_detail::flag_d|ee_ops_detail::sticky_d;
        c->fpr[ee_ops_detail::fd(w)].u=((fs^ft)&0x80000000)|0x7f7fffff;
    } else {
        if (ft&0x80000000) c->fcr31 |= ee_ops_detail::flag_i|ee_ops_detail::sticky_i;
        const float root=std::sqrt(std::fabs(ee_ops_detail::target(c,w)));
        ee_ops_detail::store(c,w,ee_ops_detail::src(c,w)/root,0);
    }
}
PS2_FORCEINLINE void fpu_abs_s(EEContext* c,u32 w){c->fpr[ee_ops_detail::fd(w)].u=c->fpr[ee_ops_detail::fs(w)].u&0x7fffffff;c->fcr31 &= ~(ee_ops_detail::flag_o|ee_ops_detail::flag_u);}
PS2_FORCEINLINE void fpu_neg_s(EEContext* c,u32 w){c->fpr[ee_ops_detail::fd(w)].u=c->fpr[ee_ops_detail::fs(w)].u^0x80000000;c->fcr31 &= ~(ee_ops_detail::flag_o|ee_ops_detail::flag_u);}
PS2_FORCEINLINE void fpu_mov_s(EEContext* c,u32 w){c->fpr[ee_ops_detail::fd(w)].u=c->fpr[ee_ops_detail::fs(w)].u;}
// Volatile intermediate enforces two single-precision rounding steps, not a host FMA.
PS2_FORCEINLINE void fpu_madd_s(EEContext* c,u32 w){volatile float product=ee_ops_detail::src(c,w)*ee_ops_detail::target(c,w);ee_ops_detail::store(c,w,ee_ops_detail::fvalue(c->facc.u)+ee_ops_detail::fvalue(std::bit_cast<u32>(float(product))));}
PS2_FORCEINLINE void fpu_msub_s(EEContext* c,u32 w){volatile float product=ee_ops_detail::src(c,w)*ee_ops_detail::target(c,w);ee_ops_detail::store(c,w,ee_ops_detail::fvalue(c->facc.u)-ee_ops_detail::fvalue(std::bit_cast<u32>(float(product))));}
PS2_FORCEINLINE void fpu_madda_s(EEContext* c,u32 w){volatile float product=ee_ops_detail::src(c,w)*ee_ops_detail::target(c,w);ee_ops_detail::acc(c,c->facc.f+product);}
PS2_FORCEINLINE void fpu_msuba_s(EEContext* c,u32 w){volatile float product=ee_ops_detail::src(c,w)*ee_ops_detail::target(c,w);ee_ops_detail::acc(c,c->facc.f-product);}
PS2_FORCEINLINE void fpu_cvt_w_s(EEContext* c,u32 w){
    const u32 bits=c->fpr[ee_ops_detail::fs(w)].u;
    if((bits&0x7f800000)>0x4e800000) c->fpr[ee_ops_detail::fd(w)].u=(bits&0x80000000)?0x80000000:0x7fffffff;
    else c->fpr[ee_ops_detail::fd(w)].s=s32(std::trunc(c->fpr[ee_ops_detail::fs(w)].f));
}
PS2_FORCEINLINE void fpu_cvt_s_w(EEContext* c,u32 w){c->fpr[ee_ops_detail::fd(w)].u=std::bit_cast<u32>(float(c->fpr[ee_ops_detail::fs(w)].s));}
PS2_FORCEINLINE void fpu_max_s(EEContext* c,u32 w){const float a=c->fpr[ee_ops_detail::fs(w)].f,b=c->fpr[ee_ops_detail::ft(w)].f;c->fpr[ee_ops_detail::fd(w)].u=std::bit_cast<u32>(a<b?b:a);c->fcr31 &= ~(ee_ops_detail::flag_o|ee_ops_detail::flag_u);}
PS2_FORCEINLINE void fpu_min_s(EEContext* c,u32 w){const float a=c->fpr[ee_ops_detail::fs(w)].f,b=c->fpr[ee_ops_detail::ft(w)].f;c->fpr[ee_ops_detail::fd(w)].u=std::bit_cast<u32>(b<a?b:a);c->fcr31 &= ~(ee_ops_detail::flag_o|ee_ops_detail::flag_u);}
PS2_FORCEINLINE void fpu_c_f_s(EEContext* c,u32){c->fcr31 &= ~ee_ops_detail::flag_c;}
#define EE_COMPARE(name,expr) PS2_FORCEINLINE void fpu_##name(EEContext* c,u32 w){const bool result=c->fpr[ee_ops_detail::fs(w)].f expr c->fpr[ee_ops_detail::ft(w)].f;c->fcr31=(c->fcr31&~ee_ops_detail::flag_c)|(result ? ee_ops_detail::flag_c:0);}
EE_COMPARE(c_eq_s,==) EE_COMPARE(c_lt_s,<) EE_COMPARE(c_le_s,<=)

// Single source of truth for generated-code coverage and compile-time signature checks.
#define PS2_EE_HELPERS(X) \
 X(alu,mult) X(alu,multu) X(alu,div) X(alu,divu) \
 X(mmi,madd) X(mmi,maddu) X(mmi,madd1) X(mmi,maddu1) X(mmi,mult1) X(mmi,multu1) X(mmi,div1) X(mmi,divu1) X(mmi,mfhi1) X(mmi,mflo1) X(mmi,mthi1) X(mmi,mtlo1) \
 X(mmi,plzcw) X(mmi,psllh) X(mmi,psrlh) X(mmi,psrah) X(mmi,psllw) X(mmi,psrlw) X(mmi,psraw) \
 X(mmi,paddw) X(mmi,psubw) X(mmi,pcgtw) X(mmi,pmaxw) X(mmi,paddh) X(mmi,psubh) X(mmi,pcgth) X(mmi,pmaxh) X(mmi,paddb) X(mmi,psubb) X(mmi,pcgtb) X(mmi,paddsw) X(mmi,psubsw) X(mmi,pextlw) X(mmi,ppacw) X(mmi,paddsh) X(mmi,psubsh) X(mmi,pextlh) X(mmi,ppach) X(mmi,paddsb) X(mmi,psubsb) X(mmi,pextlb) X(mmi,ppacb) X(mmi,pext5) X(mmi,ppac5) \
 X(mmi,pabsw) X(mmi,pceqw) X(mmi,pminw) X(mmi,padsbh) X(mmi,pabsh) X(mmi,pceqh) X(mmi,pminh) X(mmi,pceqb) X(mmi,padduw) X(mmi,psubuw) X(mmi,pextuw) X(mmi,padduh) X(mmi,psubuh) X(mmi,pextuh) X(mmi,paddub) X(mmi,psubub) X(mmi,pextub) X(mmi,qfsrv) \
 X(mmi,pmaddw) X(mmi,psllvw) X(mmi,psrlvw) X(mmi,pmsubw) X(mmi,pmfhi) X(mmi,pmflo) X(mmi,pinth) X(mmi,pmultw) X(mmi,pdivw) X(mmi,pcpyld) X(mmi,pmaddh) X(mmi,phmadh) X(mmi,pand) X(mmi,pxor) X(mmi,pmsubh) X(mmi,phmsbh) X(mmi,pexeh) X(mmi,prevh) X(mmi,pmulth) X(mmi,pdivbw) X(mmi,pexew) X(mmi,prot3w) \
 X(mmi,pmadduw) X(mmi,psravw) X(mmi,pmthi) X(mmi,pmtlo) X(mmi,pinteh) X(mmi,pmultuw) X(mmi,pdivuw) X(mmi,pcpyud) X(mmi,por) X(mmi,pnor) X(mmi,pexch) X(mmi,pcpyh) X(mmi,pexcw) \
 X(mmi,pmfhl_lw) X(mmi,pmfhl_uw) X(mmi,pmfhl_slw) X(mmi,pmfhl_lh) X(mmi,pmfhl_sh) X(mmi,pmthl_lw) X(mmi,mfsa) X(mmi,mtsa) X(mmi,mtsab) X(mmi,mtsah) \
 X(fpu,add_s) X(fpu,sub_s) X(fpu,mul_s) X(fpu,div_s) X(fpu,sqrt_s) X(fpu,abs_s) X(fpu,mov_s) X(fpu,neg_s) X(fpu,rsqrt_s) X(fpu,adda_s) X(fpu,suba_s) X(fpu,mula_s) X(fpu,madd_s) X(fpu,msub_s) X(fpu,madda_s) X(fpu,msuba_s) X(fpu,cvt_w_s) X(fpu,max_s) X(fpu,min_s) X(fpu,c_f_s) X(fpu,c_eq_s) X(fpu,c_lt_s) X(fpu,c_le_s) X(fpu,cvt_s_w)
#define EE_NAME(prefix,name) #prefix "_" #name,
inline constexpr const char* ps2_ee_helper_names[] = { PS2_EE_HELPERS(EE_NAME) };
#undef EE_NAME
#define EE_CHECK(prefix,name) static_assert(std::is_same_v<decltype(&prefix##_##name),void(*)(EEContext*,u32)>);
PS2_EE_HELPERS(EE_CHECK)
#undef EE_CHECK
#undef EE_REGS
#undef EE_PUT
#undef EE_WORD
#undef EE_HALF
#undef EE_BYTE
#undef EE_INTER
#undef EE_PACK
#undef EE_PRODW
#undef EE_SCALAR1
#undef EE_MADD
#undef EE_SHIFT
#undef EE_DIVPACK
#undef EE_HALFPROD
#undef EE_LOGIC
#undef EE_PERM
#undef EE_VARSHIFT
#undef EE_FPU_BINARY
#undef EE_FPU_ACC
#undef EE_COMPARE
