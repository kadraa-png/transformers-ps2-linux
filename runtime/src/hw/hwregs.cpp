// EE hardware registers: INTC, DMAC, timers, GIF, VIF/IPU windows, SIF,
// GS privileged registers, VU memory.  Everything is synchronous: a DMA kick
// runs the whole transfer before the register write returns.
#include "hw.h"
#include "ipu.h"

#include "kernel/kernel.h"
#include "ps2/ee_mem.h"
#include "ps2/gs.h"
#include "ps2/intc.h"
#include "ps2/iop.h"
#include "ps2/log.h"
#include "ps2/platform.h"
#include "ps2/vu.h"

#include <cstring>
#include <atomic>
#include <unordered_set>
#include <unordered_map>

namespace hw {

u32 g_istat = 0;
u32 g_imask = 0;
u32 g_dstat = 0;

namespace {

// ---- DMAC ----------------------------------------------------------------------
DmaChan g_ch[10];
bool g_gif_deferred = false;   // GIF DMA kicked while VIF1 masks PATH3
const u32 kChanBase[10] = {0x8000, 0x9000, 0xA000, 0xB000, 0xB400, 0xC000, 0xC400, 0xC800, 0xD000, 0xD400};
// The BIOS leaves the DMAC enabled (D_CTRL.DMAE) before the game starts.
u32 g_dctrl = 1, g_dpcr = 0, g_dsqwc = 0, g_drbsr = 0, g_drbor = 0, g_dstadr = 0;
u32 g_denable = 0x1201;   // D_ENABLER reset value
u32 g_kputchar_len = 0;
char g_kputchar_buf[256];

int chan_index(u32 off) {
    for (int i = 0; i < 10; ++i)
        if ((off & 0xFF00) == (kChanBase[i] & 0xFF00) && (off >> 12) == (kChanBase[i] >> 12)) return i;
    return -1;
}

inline u8* spr_ptr(u32 a) { return EE_BASE + EE_SPR_ADDR + (a & (EE_SPR_SIZE - 1) & ~15u); }

void dma_payload_inner(int ch, u32 madr, u32 qwc);
// PS2_LOG=dma: per-channel host time spent queueing DMA payloads (a full
// VU1/GS ring makes the EE wait here).
void dma_payload(int ch, u32 madr, u32 qwc) {
    if (PS2_LIKELY(!(g_log_mask & LOG_DMA))) return dma_payload_inner(ch, madr, qwc);
    static u64 ns[10], calls[10], window = 0;
    const u64 t0 = platform::now_ns();
    dma_payload_inner(ch, madr, qwc);
    const u64 t1 = platform::now_ns();
    ns[ch] += t1 - t0; ++calls[ch];
    if (t1 - window > 1000000000ull) {
        window = t1;
        LOG(LOG_DMA, "[dma] payload ms/s: vif0 %.1f vif1 %.1f gif %.1f (calls %llu/%llu/%llu)\n",
            ns[0] / 1e6, ns[1] / 1e6, ns[2] / 1e6, (unsigned long long)calls[0],
            (unsigned long long)calls[1], (unsigned long long)calls[2]);
        for (auto& v : ns) v = 0;
        for (auto& v : calls) v = 0;
    }
}
void dma_payload_inner(int ch, u32 madr, u32 qwc) {
    if (!qwc) return;
    DmaChan& c = g_ch[ch];
    switch (ch) {
    case DMAC_VIF0:
        vif_dma(0, ee_dma_ptr(madr), qwc);
        break;
    case DMAC_VIF1:
        if (c.chcr & 1) {
            vif_dma(1, ee_dma_ptr(madr), qwc);
        } else {  // VIF1 -> memory (GS download)
            qword* dst = (qword*)ee_dma_ptr(madr);
            vif_fifo_read_n(1, dst, qwc);
        }
        break;
    case DMAC_GIF:
        gs::gif_path3(ee_dma_ptr(madr), qwc);
        break;
    case DMAC_TO_IPU:
    case DMAC_FROM_IPU:
        FATAL("IPU DMA payload routed through the generic DMAC path");
    case DMAC_FROM_SPR: {
        u8* dst = ee_dma_ptr(madr);
        for (u32 i = 0; i < qwc; ++i) {
            memcpy(dst + i * 16, spr_ptr(c.sadr), 16);
            c.sadr = (c.sadr + 16) & (EE_SPR_SIZE - 1);
        }
        break;
    }
    case DMAC_TO_SPR: {
        const u8* src = ee_dma_ptr(madr);
        for (u32 i = 0; i < qwc; ++i) {
            memcpy(spr_ptr(c.sadr), src + i * 16, 16);
            c.sadr = (c.sadr + 16) & (EE_SPR_SIZE - 1);
        }
        break;
    }
    default:
        LOG(LOG_DMA, "[dma] ch%d payload %u qw from %08x ignored\n", ch, qwc, madr);
        break;
    }
}

void dma_finish(int ch) { dma_done((u32)ch); }

// Source chain (VIF0/VIF1/GIF/toIPU/toSPR/SIF1).
void dma_source_chain(int ch) {
    DmaChan& c = g_ch[ch];
    const bool tte = (c.chcr & 0x40) && (ch == DMAC_VIF0 || ch == DMAC_VIF1);
    const bool tie = (c.chcr & 0x80) != 0;
    // Data left over from an interrupted/refe transfer.
    if (c.qwc) {
        dma_payload(ch, c.madr, c.qwc);
        c.madr += c.qwc * 16;
        c.qwc = 0;
    }
    for (int guard = 0; guard < 1 << 20; ++guard) {
        const u32* tag = (const u32*)ee_dma_ptr(c.tadr);
        u32 t0 = tag[0], t1 = tag[1];
        u32 qwc = t0 & 0xFFFF;
        u32 id = (t0 >> 28) & 7;
        bool irq = (t0 >> 31) != 0;
        u32 addr = t1 & ~15u;   // bit 31 = SPR
        c.chcr = (c.chcr & 0xFFFF) | (t0 & 0xFFFF0000u);
        if (tte) vif_dma_words(ch == DMAC_VIF0 ? 0 : 1, tag + 2, 2);
        bool end = false;
        u32 data = 0;
        switch (id) {
        case 0:  // refe
            data = addr;
            c.tadr += 16;
            end = true;
            break;
        case 1:  // cnt
            data = c.tadr + 16;
            c.tadr = data + qwc * 16;
            break;
        case 2:  // next
            data = c.tadr + 16;
            c.tadr = addr;
            break;
        case 3:  // ref
        case 4:  // refs
            data = addr;
            c.tadr += 16;
            break;
        case 5: {  // call
            data = c.tadr + 16;
            u32 ret = data + qwc * 16;
            u32 asp = (c.chcr >> 4) & 3;
            if (asp == 0) c.asr0 = ret;
            else c.asr1 = ret;
            c.chcr = (c.chcr & ~0x30u) | (((asp + 1) & 3) << 4);
            c.tadr = addr;
            break;
        }
        case 6: {  // ret
            data = c.tadr + 16;
            u32 asp = (c.chcr >> 4) & 3;
            if (asp == 0) {
                end = true;
                c.tadr = data + qwc * 16;
            } else {
                c.tadr = (asp == 2) ? c.asr1 : c.asr0;
                c.chcr = (c.chcr & ~0x30u) | (((asp - 1) & 3) << 4);
            }
            break;
        }
        case 7:  // end
            data = c.tadr + 16;
            end = true;
            break;
        }
        c.madr = data;
        dma_payload(ch, data, qwc);
        c.madr = data + qwc * 16;
        c.qwc = 0;
        if (end || (irq && tie)) break;
    }
}

// Destination chain for fromSPR: tags are embedded in the scratchpad stream.
void dma_dest_chain_spr(int ch) {
    DmaChan& c = g_ch[ch];
    for (int guard = 0; guard < 1 << 16; ++guard) {
        const u32* tag = (const u32*)spr_ptr(c.sadr);
        u32 t0 = tag[0], t1 = tag[1];
        c.sadr = (c.sadr + 16) & (EE_SPR_SIZE - 1);
        u32 qwc = t0 & 0xFFFF;
        u32 id = (t0 >> 28) & 7;
        bool irq = (t0 >> 31) != 0;
        c.chcr = (c.chcr & 0xFFFF) | (t0 & 0xFFFF0000u);
        c.madr = t1 & ~15u;
        dma_payload(ch, c.madr, qwc);
        c.madr += qwc * 16;
        if (id == 7 || id == 0 || (irq && (c.chcr & 0x80))) break;
    }
}

u32 g_deferred_mask = 0;   // channels kicked while the DMAC was disabled

void dma_start(int ch) {
    DmaChan& c = g_ch[ch];
    if (!(g_dctrl & 1) || (g_denable & 0x10000)) {
        LOG(LOG_DMA, "[dma] ch%d start while DMAC disabled (deferred)\n", ch);
        g_deferred_mask |= 1u << ch;
        return;
    }
    u32 mode = (c.chcr >> 2) & 3;
    LOG(LOG_DMA, "[dma] ch%d start chcr=%08x madr=%08x qwc=%u tadr=%08x sadr=%08x\n", ch, c.chcr, c.madr,
        c.qwc, c.tadr, c.sadr);
    if (ch == DMAC_FROM_IPU || ch == DMAC_TO_IPU) {
        ipu::dma_start((u32)ch);   // the IPU calls dma_done() when finished
        return;
    }
    if (ch == DMAC_SIF0 || ch == DMAC_SIF1 || ch == DMAC_SIF2) {
        // SIF channels are driven by the kernel's sceSif* syscalls; a direct
        // kick only (re)arms the channel.
        if (ch == DMAC_SIF0) iop::ee_sif_set_dchain();
        return;
    }
    if (ch == DMAC_GIF && vif_path3_masked()) {
        g_gif_deferred = true;   // released when VIF1 clears MSKPATH3
        return;
    }
    if (mode == 0) {
        dma_payload(ch, c.madr, c.qwc);
        c.madr += c.qwc * 16;
        c.qwc = 0;
    } else if (mode == 1) {
        if (ch == DMAC_FROM_SPR) dma_dest_chain_spr(ch);
        else dma_source_chain(ch);
    } else {  // interleave (SPR only)
        u32 tqwc = (g_dsqwc >> 16) & 0xFF, sqwc = g_dsqwc & 0xFF;
        while (c.qwc) {
            u32 n = tqwc ? std::min(tqwc, c.qwc) : c.qwc;
            dma_payload(ch, c.madr, n);
            c.madr += (n + sqwc) * 16;
            c.qwc -= n;
        }
    }
    dma_finish(ch);
    if (ch == DMAC_VIF1 && g_gif_deferred && !vif_path3_masked()) {
        g_gif_deferred = false;
        dma_start(DMAC_GIF);
    }
}

void retry_deferred_dma() {
    if (!(g_dctrl & 1) || (g_denable & 0x10000)) return;
    u32 m = g_deferred_mask;
    g_deferred_mask = 0;
    for (int ch = 0; ch < 10; ++ch)
        if ((m & (1u << ch)) && (g_ch[ch].chcr & 0x100)) dma_start(ch);
}

u32 dma_chan_read(int ch, u32 reg) {
    DmaChan& c = g_ch[ch];
    switch (reg) {
    case 0x00: return c.chcr;
    case 0x10: return c.madr;
    case 0x20: return c.qwc;
    case 0x30: return c.tadr;
    case 0x40: return c.asr0;
    case 0x50: return c.asr1;
    case 0x80: return c.sadr;
    }
    return 0;
}

void dma_chan_write(int ch, u32 reg, u32 v) {
    DmaChan& c = g_ch[ch];
    switch (reg) {
    case 0x00: {
        bool start = (v & 0x100) && !(c.chcr & 0x100);
        c.chcr = v;
        if (start) dma_start(ch);
        break;
    }
    case 0x10: c.madr = v & ~15u; break;
    case 0x20: c.qwc = v & 0xFFFF; break;
    case 0x30: c.tadr = v & ~15u; break;
    case 0x40: c.asr0 = v & ~15u; break;
    case 0x50: c.asr1 = v & ~15u; break;
    case 0x80: c.sadr = v & 0x3FF0; break;
    }
}

// ---- timers -------------------------------------------------------------------------
struct Timer {
    u32 mode = 0, comp = 0, hold = 0;
    u32 base_count = 0;   // count at base_ns
    u64 base_ns = 0;
    u64 next_event_ns = ~0ull;
};
Timer g_tim[4];

double timer_rate(const Timer& t) {  // counts per ns
    switch (t.mode & 3) {
    case 0: return 147.456e6 / 1e9;
    case 1: return 147.456e6 / 16 / 1e9;
    case 2: return 147.456e6 / 256 / 1e9;
    default: return 15625.0 / 1e9;  // PAL HBLANK
    }
}

u32 timer_count(Timer& t, u64 now) {
    if (!(t.mode & 0x80)) return t.base_count & 0xFFFF;  // CUE=0: stopped
    u64 elapsed = (u64)((double)(now - t.base_ns) * timer_rate(t));
    u64 v = t.base_count + elapsed;
    if ((t.mode & 0x40) && t.comp) v %= (u64)t.comp + 1;   // ZRET
    return (u32)(v & 0xFFFF);
}

void timer_rebase(Timer& t, u64 now) {
    t.base_count = timer_count(t, now);
    t.base_ns = now;
}

void timer_schedule(Timer& t, u64 now) {
    t.next_event_ns = ~0ull;
    if (!(t.mode & 0x80) || !(t.mode & 0x300)) return;
    u32 cnt = timer_count(t, now);
    u64 to_cmp = ~0ull, to_ovf = ~0ull;
    if (t.mode & 0x100) to_cmp = (t.comp > cnt) ? t.comp - cnt : 0x10000 - cnt + t.comp;
    if (t.mode & 0x200) to_ovf = 0x10000 - cnt;
    u64 counts = std::min(to_cmp, to_ovf);
    t.next_event_ns = now + (u64)((double)counts / timer_rate(t)) + 1;
}

// ---- GS runtime-owned bits ----------------------------------------------------------
u64 g_gs_imr = 0xFF00;
u64 g_gs_csr_bits = 0;   // VSINT (bit 3) / HSINT (bit 2)

// May run on the GS thread (async GS): only record the bits and wake the
// emulation thread, which raises the interrupt in poll_async().
std::atomic<u32> g_gs_async_bits{0};
void gs_event_cb(u32 bits) {
    g_gs_async_bits.fetch_or(bits | 0x80000000u, std::memory_order_release);
    kernel::post_events(EV_INTR);
}

std::unordered_set<u32> g_logged_addrs;
void log_unknown(const char* what, u32 a, u64 v) {
    if (g_logged_addrs.insert(a).second) LOG(LOG_HW, "[hw] unhandled %s %08x (%llx)\n", what, a, (unsigned long long)v);
}

// EE access to VU memory.  An asynchronous VU1 owns its memories while it has
// queued work, so micro-memory access and stores order behind it.  EE *loads*
// of VU1 data memory do not wait (PS2_VU1_READ_SYNC=1 restores the drain): on
// the console the EE reads VU1 memory while VU1 runs, and the game polls such
// values once per frame (e.g. 0x1100c0b0), which would otherwise serialize the
// whole frame behind VU1.
bool vu1_read_sync() {
    static const bool sync = [] {
        const char* e = std::getenv("PS2_VU1_READ_SYNC");
        return e && *e == '1';
    }();
    return sync;
}
u8* vu_mem_ptr(u32 a, bool load = false) {
    u32 off = a & 0xFFFF;
    if (off < 0x4000) return g_vu0.micro + (off & 0xFFF);
    if (off < 0x8000) return g_vu0.data + (off & 0xFFF);
    if (!(load && off >= 0xC000) || vu1_read_sync()) vu1_sync();
    if (off < 0xC000) return g_vu1.micro + (off & 0x3FFF);
    return g_vu1.data + (off & 0x3FFF);
}

// Direct EE stores into VU micro memory must invalidate the compiled-microcode
// map (call after the store).
void vu_mem_written(u32 a, u32 bytes) {
    u32 off = a & 0xFFFF;
    if (off < 0x4000) vu_micro_written(0, off & 0xFFF, bytes);
    else if (off >= 0x8000 && off < 0xC000) vu_micro_written(1, off & 0x3FFF, bytes);
}
// EE stores into VU memory.  An asynchronous VU1 applies stores to its
// memories in order behind its queued work (PCSX2 1.6 MTVU WriteDataMem /
// WriteMicroMem) instead of draining it; loads go through vu_mem_ptr.
void vu_mem_store(u32 a, const void* src, u32 bytes) {
    const u32 off = a & 0xFFFF;
    if (off >= 0x8000 && vu1_queue_store(off & 0x7FFF, src, bytes)) return;
    std::memcpy(vu_mem_ptr(a), src, bytes);
    vu_mem_written(a, bytes);
}

u32 read32_inner(u32 a);
void write32_inner(u32 a, u32 v);

} // namespace

void init() {
    gs::set_event_callback(gs_event_cb);
    vu1_set_irq_wake([] { kernel::post_events(EV_INTR); });
    ipu::init();
}

void poll_async() {
    gs::flush();   // let the GS thread work on everything queued so far
    if (vu1_take_irq()) intc_raise(INTC_VU1);
    u32 bits = g_gs_async_bits.exchange(0, std::memory_order_acq_rel);
    if (!bits) return;
    if (PS2_UNLIKELY(g_log_mask & LOG_GS)) {   // which GS events the game waits on
        static u32 sig = 0, fin = 0;
        static u64 window = 0;
        sig += bits & 1; fin += (bits >> 1) & 1;
        const u64 now = platform::now_ns();
        if (now - window > 1000000000ull) {
            LOG(LOG_GS, "[gs] async events/s: SIGNAL %u FINISH %u (IMR %x)\n", sig, fin, g_gs_imr);
            sig = fin = 0; window = now;
        }
    }
    // SIGNAL (bit0) / FINISH (bit1) / LABEL; raise INTC_GS unless masked by IMR.
    bool raise = false;
    if ((bits & 1) && !(g_gs_imr & 0x100)) raise = true;
    if ((bits & 2) && !(g_gs_imr & 0x200)) raise = true;
    if (raise) intc_raise(INTC_GS);
}

DmaChan& dma_chan(u32 ch) { return g_ch[ch]; }

void dma_done(u32 ch) {
    g_ch[ch].chcr &= ~0x100u;
    g_dstat |= 1u << ch;
    kernel::interrupt_state_changed();
}

void debug_dump() {
    std::fprintf(stderr, "--- EE hw: D_CTRL %08x D_ENABLE %08x deferred %03x gif_deferred %d MSKPATH3 %d\n",
                 g_dctrl, g_denable, g_deferred_mask, (int)g_gif_deferred, (int)vif_path3_masked());
    for (int ch = 0; ch < 10; ++ch)
        std::fprintf(stderr, "  dma%d chcr %08x madr %08x qwc %04x tadr %08x\n", ch, g_ch[ch].chcr, g_ch[ch].madr,
                     g_ch[ch].qwc, g_ch[ch].tadr);
    std::fprintf(stderr, "  VIF0_STAT %08x VIF1_STAT %08x VPU_STAT %08x\n", vif_read_reg(0, 0), vif_read_reg(1, 0),
                 vu0_cfc2(29));
}

void gs_vblank_start() {
    g_gs_csr_bits |= 8;   // VSINT
    if (!(g_gs_imr & 0x800)) intc_raise(INTC_GS);
}
u64 gs_read_csr() { return gs::read_priv64(0x1000) | g_gs_csr_bits; }
void gs_write_imr(u64 v) {
    g_gs_imr = v;
    gs::write_priv64(0x1010, v);
}
u64 gs_read_imr() { return g_gs_imr; }

void timers_service(u64 now) {
    for (int i = 0; i < 4; ++i) {
        Timer& t = g_tim[i];
        if (t.next_event_ns > now) continue;
        if (t.mode & 0x100) t.mode |= 0x400;  // EQUF
        if (t.mode & 0x200) t.mode |= 0x800;  // OVFF
        intc_raise(INTC_TIMER0 + (u32)i);
        timer_schedule(t, now);
    }
}
u64 timers_next_deadline_ns() {
    u64 d = ~0ull;
    for (auto& t : g_tim) d = std::min(d, t.next_event_ns);
    return d;
}

bool cpcond0() {
    // CPCOND0 = all channels selected in D_PCR.CPC have completed (CIS set).
    u32 cpc = g_dpcr & 0x3FF;
    return (g_dstat & cpc) == cpc;
}

namespace {

u32 read32_inner(u32 a) {
    u32 p = a & 0x1FFFFFFF;
    if (p < 0x10002000) {  // timers
        int i = (int)((p >> 11) & 3);
        Timer& t = g_tim[i];
        switch (p & 0x7F0) {
        case 0x00: return timer_count(t, platform::now_ns());
        case 0x10: return t.mode;
        case 0x20: return t.comp;
        case 0x30: return t.hold;
        }
        return 0;
    }
    if (p < 0x10003000) return ipu::read_reg(p & 0xFF);
    if (p < 0x10003800) {  // GIF
        if ((p & 0xFF) == 0x20) return 0;  // GIF_STAT: idle
        return 0;
    }
    if (p < 0x10003C00) return vif_read_reg(0, p & 0x3FF);
    if (p < 0x10004000) return vif_read_reg(1, p & 0x3FF);
    if (p >= 0x10008000 && p < 0x1000E000) {
        int ch = chan_index(p & 0xFFFF);
        if (ch >= 0) return dma_chan_read(ch, p & 0xF0 ? (p & 0xF0) : 0);
        return 0;
    }
    switch (p) {
    case 0x1000E000: return g_dctrl;
    case 0x1000E010: return g_dstat;
    case 0x1000E020: return g_dpcr;
    case 0x1000E030: return g_dsqwc;
    case 0x1000E040: return g_drbsr;
    case 0x1000E050: return g_drbor;
    case 0x1000E060: return g_dstadr;
    case 0x1000F000: return g_istat;
    case 0x1000F010: return g_imask;
    case 0x1000F520: return g_denable;
    case 0x1000F130: return 0;
    case 0x1000F400: case 0x1000F410: case 0x1000F420: return 0;
    case 0x1000F430: return 0;          // MCH_RICM
    case 0x1000F440: return 0;          // MCH_DRD
    }
    if (p >= 0x1000F200 && p < 0x1000F300) return iop::sif_hw_read(p & 0xFF);
    if (p >= 0x11000000 && p < 0x11010000) return *(u32a*)vu_mem_ptr(p, true);
    if (p >= 0x12000000 && p < 0x12002000) {
        u32 off = p & 0x1FF8;
        u64 v = (off == 0x1000) ? gs_read_csr() : (off == 0x1010 ? g_gs_imr : gs::read_priv64(off));
        return (p & 4) ? (u32)(v >> 32) : (u32)v;
    }
    if (p >= 0x1C000000 && p < 0x1C200000) return *(u32a*)(iop::g_iop_ram + (p & 0x1FFFFF));
    if (p >= 0x1FC00000) return 0;  // BIOS ROM
    log_unknown("read32", a, 0);
    return 0;
}

void write32_inner(u32 a, u32 v) {
    u32 p = a & 0x1FFFFFFF;
    if (p < 0x10002000) {
        int i = (int)((p >> 11) & 3);
        Timer& t = g_tim[i];
        u64 now = platform::now_ns();
        switch (p & 0x7F0) {
        case 0x00:
            t.base_count = v & 0xFFFF;
            t.base_ns = now;
            break;
        case 0x10: {
            timer_rebase(t, now);
            u32 clear = v & 0xC00;           // writing 1 clears EQUF/OVFF
            t.mode = (v & 0x3FF) | (t.mode & 0xC00 & ~clear);
            break;
        }
        case 0x20: timer_rebase(t, now); t.comp = v & 0xFFFF; break;
        case 0x30: t.hold = v & 0xFFFF; break;
        }
        timer_schedule(t, now);
        return;
    }
    if (p < 0x10003000) { ipu::write_reg(p & 0xFF, v); return; }
    if (p < 0x10003800) {
        if ((p & 0xFF) == 0x00 && (v & 1)) LOG(LOG_GS, "[gif] reset\n");
        return;
    }
    if (p < 0x10003C00) { vif_write_reg(0, p & 0x3FF, v); return; }
    if (p < 0x10004000) { vif_write_reg(1, p & 0x3FF, v); return; }
    if (p >= 0x10008000 && p < 0x1000E000) {
        int ch = chan_index(p & 0xFFFF);
        if (ch >= 0) dma_chan_write(ch, p & 0xF0, v);
        return;
    }
    switch (p) {
    case 0x1000E000: g_dctrl = v; retry_deferred_dma(); return;
    case 0x1000E010:
        g_dstat &= ~(v & 0xFFFF);              // clear status bits
        g_dstat ^= (v & 0xFFFF0000u);           // toggle mask bits
        kernel::interrupt_state_changed();
        return;
    case 0x1000E020: g_dpcr = v; return;
    case 0x1000E030: g_dsqwc = v; return;
    case 0x1000E040: g_drbsr = v; return;
    case 0x1000E050: g_drbor = v; return;
    case 0x1000E060: g_dstadr = v; return;
    case 0x1000F000: g_istat &= ~v; return;
    case 0x1000F010: g_imask ^= v; kernel::interrupt_state_changed(); return;
    case 0x1000F180: {   // KPUTCHAR
        char ch = (char)(v & 0xFF);
        if (ch == '\n' || g_kputchar_len >= sizeof(g_kputchar_buf) - 1) {
            g_kputchar_buf[g_kputchar_len] = 0;
            LOG(LOG_TTY, "%s\n", g_kputchar_buf);
            g_kputchar_len = 0;
        } else if (ch) {
            g_kputchar_buf[g_kputchar_len++] = ch;
        }
        return;
    }
    case 0x1000F590: g_denable = v; retry_deferred_dma(); return;
    case 0x1000F400: case 0x1000F410: case 0x1000F420: case 0x1000F430: case 0x1000F440: return;
    }
    if (p >= 0x1000F200 && p < 0x1000F300) { iop::sif_hw_write(p & 0xFF, v); return; }
    if (p >= 0x11000000 && p < 0x11010000) { vu_mem_store(p, &v, 4); return; }
    if (p >= 0x12000000 && p < 0x12002000) {
        u32 off = p & 0x1FF8;
        u64 cur = (off == 0x1010) ? g_gs_imr : gs::read_priv64(off);
        u64 nv = (p & 4) ? ((cur & 0xFFFFFFFFull) | ((u64)v << 32)) : ((cur & ~0xFFFFFFFFull) | v);
        if (off == 0x1000) {
            g_gs_csr_bits &= ~(nv & 0xC);        // VSINT/HSINT write-1-to-clear
            gs::write_priv64(off, nv);
        } else if (off == 0x1010) {
            gs_write_imr(nv);
        } else {
            gs::write_priv64(off, nv);
        }
        return;
    }
    if (p >= 0x1C000000 && p < 0x1C200000) { *(u32a*)(iop::g_iop_ram + (p & 0x1FFFFF)) = v; return; }
    log_unknown("write32", a, v);
}

} // namespace
} // namespace hw

// ---- public slow-path accessors ------------------------------------------------------
using namespace hw;

u32 hw_read32(u32 a) {
    u32 v = read32_inner(a);
    // Polling loops on hardware registers must still see interrupts.
    if (PS2_UNLIKELY(g_ee_events)) kernel::post_events(0);
    return v;
}
void hw_write32(u32 a, u32 v) { write32_inner(a, v); }

u8 hw_read8(u32 a) {
    u32 p = a & 0x1FFFFFFF;
    if (p >= 0x11000000 && p < 0x11010000) return *vu_mem_ptr(p, true);
    return (u8)(read32_inner(a & ~3u) >> ((a & 3) * 8));
}
u16 hw_read16(u32 a) {
    u32 p = a & 0x1FFFFFFF;
    if (p >= 0x11000000 && p < 0x11010000) return *(u16a*)vu_mem_ptr(p, true);
    return (u16)(read32_inner(a & ~3u) >> ((a & 2) * 8));
}
u64 hw_read64(u32 a) {
    u32 p = a & 0x1FFFFFFF;
    if (p >= 0x11000000 && p < 0x11010000) return *(u64a*)vu_mem_ptr(p, true);
    if (p >= 0x10002000 && p < 0x10003000) return ipu::read_reg64(p & 0xFF);
    if (p >= 0x12000000 && p < 0x12002000) {
        u32 off = p & 0x1FF8;
        if (off == 0x1000) return gs_read_csr();
        if (off == 0x1010) return g_gs_imr;
        return gs::read_priv64(off);
    }
    return (u64)read32_inner(a) | ((u64)read32_inner(a + 4) << 32);
}
void hw_read128(u32 a, qword* out) {
    u32 p = a & 0x1FFFFFFF;
    if (p >= 0x11000000 && p < 0x11010000) { memcpy(out, vu_mem_ptr(p, true), 16); return; }
    if (p == 0x10005000) { vif_fifo_read(1, out); return; }
    if (p == 0x10007000) { ipu::read_fifo(out); return; }
    out->d[0] = hw_read64(a);
    out->d[1] = hw_read64(a + 8);
}

void hw_write8(u32 a, u8 v) {
    u32 p = a & 0x1FFFFFFF;
    if (p >= 0x11000000 && p < 0x11010000) { vu_mem_store(p, &v, 1); return; }
    if (p == 0x1000F180) { write32_inner(p, v); return; }
    u32 cur = read32_inner(a & ~3u);
    u32 sh = (a & 3) * 8;
    write32_inner(a & ~3u, (cur & ~(0xFFu << sh)) | ((u32)v << sh));
}
void hw_write16(u32 a, u16 v) {
    u32 p = a & 0x1FFFFFFF;
    if (p >= 0x11000000 && p < 0x11010000) { vu_mem_store(p, &v, 2); return; }
    u32 cur = read32_inner(a & ~3u);
    u32 sh = (a & 2) * 8;
    write32_inner(a & ~3u, (cur & ~(0xFFFFu << sh)) | ((u32)v << sh));
}
void hw_write64(u32 a, u64 v) {
    u32 p = a & 0x1FFFFFFF;
    if (p >= 0x11000000 && p < 0x11010000) { vu_mem_store(p, &v, 8); return; }
    if (p >= 0x10002000 && p < 0x10003000) { ipu::write_reg64(p & 0xFF, v); return; }
    if (p >= 0x12000000 && p < 0x12002000) {
        u32 off = p & 0x1FF8;
        if (off == 0x1000) {
            g_gs_csr_bits &= ~(v & 0xC);
            gs::write_priv64(off, v);
        } else if (off == 0x1010) {
            gs_write_imr(v);
        } else {
            gs::write_priv64(off, v);
        }
        return;
    }
    write32_inner(a, (u32)v);
    write32_inner(a + 4, (u32)(v >> 32));
}
void hw_write128(u32 a, const qword& v) {
    u32 p = a & 0x1FFFFFFF;
    if (p >= 0x11000000 && p < 0x11010000) { vu_mem_store(p, &v, 16); return; }
    if (p == 0x10004000) { vif_fifo_write(0, v); return; }
    if (p == 0x10005000) { vif_fifo_write(1, v); return; }
    if (p == 0x10006000) { gs::gif_path3((const u8*)&v, 1); return; }
    if (p == 0x10007010) { ipu::write_fifo(v); return; }
    hw_write64(a, v.d[0]);
    hw_write64(a + 8, v.d[1]);
}

void intc_raise(u32 line) {
    g_istat |= 1u << line;
    kernel::interrupt_state_changed();
}
void dmac_raise(u32 channel) {
    g_dstat |= 1u << channel;
    kernel::interrupt_state_changed();
}
