// Native PS2 IPU. MPEG VLC/IDCT/CSC cores are ported from PCSX2 1.6 in ipu/.
// All state is emulation-thread-local; DMA and the command worker are pumped
// together so that neither a full input FIFO nor a full output FIFO deadlocks.
#include "ipu.h"
#include "ipu/compat.h"
#include "hw.h"
#include "ps2/ee_mem.h"
#include "ps2/intc.h"
#include "ps2/log.h"
#include <algorithm>
#include <cstring>

IPUFifo ipu_fifo;
BitPointer g_BP;
IpuRegisters ipuRegs;
CommandState ipu_cmd;
alignas(16) decoder_t decoder;
int coded_block_pattern = 0;

unsigned InputFifo::write(const u32* words, unsigned count) {
    const unsigned n = std::min(count, 8u - size);
    for (unsigned i = 0; i < n; ++i) {
        std::memcpy(&data[writepos], words + i * 4, 16);
        writepos = (writepos + 1) & 7;
    }
    size += n;
    g_BP.IFC = size;
    return n;
}
unsigned InputFifo::read(void* dest) {
    if (!size) return 0;
    std::memcpy(dest, &data[readpos], 16);
    readpos = (readpos + 1) & 7;
    --size;
    g_BP.IFC = size;
    return 1;
}
unsigned OutputFifo::write(const u32* words, unsigned count) {
    const unsigned n = std::min(count, 8u - size);
    for (unsigned i = 0; i < n; ++i) {
        std::memcpy(&data[writepos], words + i * 4, 16);
        writepos = (writepos + 1) & 7;
    }
    size += n;
    ipuRegs.ctrl.OFC = size;
    return n;
}
unsigned OutputFifo::read(void* dest, unsigned count) {
    const unsigned n = std::min(count, size);
    for (unsigned i = 0; i < n; ++i) {
        std::memcpy((u8*)dest + i * 16, &data[readpos], 16);
        readpos = (readpos + 1) & 7;
    }
    size -= n;
    ipuRegs.ctrl.OFC = size;
    return n;
}
void BitPointer::Advance(unsigned bits) {
    BP += bits;
    if (BP >= 128) {
        BP -= 128;
        if (FP == 2) {
            internal_qwc[0] = internal_qwc[1];
            FP = 1;
        } else {
            if (!FP) ipu_fifo.in.read(&internal_qwc[0]);
            FP = 0;
        }
    }
}
bool BitPointer::FillBuffer(unsigned bits) {
    while (FP < 2) {
        if (!ipu_fifo.in.read(&internal_qwc[FP]))
            return FP != 0 && BP + bits <= 128;
        ++FP;
    }
    return true;
}

u32 UBITS(uint bits) {
    u8 bytes[4]{};
    const auto* src = reinterpret_cast<const u8*>(g_BP.internal_qwc);
    const unsigned byte = g_BP.BP / 8, shift = g_BP.BP & 7;
    for (unsigned i = 0; i < 4; ++i)
        bytes[i] = (src[byte + i] << shift) | (shift ? src[byte + i + 1] >> (8 - shift) : 0);
    const u32 n = (u32(bytes[0]) << 24) | (u32(bytes[1]) << 16) | (u32(bytes[2]) << 8) | bytes[3];
    return bits ? n >> (32 - bits) : 0;
}
s32 SBITS(uint bits) {
    if (!bits) return 0;
    const u32 value = UBITS(bits);
    return s32((value ^ (1u << (bits - 1))) - (1u << (bits - 1)));
}
static bool read_bytes(u8* out, unsigned count, bool advance) {
    if (!g_BP.FillBuffer(count * 8)) return false;
    const u8* src = reinterpret_cast<const u8*>(g_BP.internal_qwc) + g_BP.BP / 8;
    const unsigned shift = g_BP.BP & 7;
    for (unsigned i = 0; i < count; ++i)
        out[i] = shift ? u8((src[i] << shift) | (src[i + 1] >> (8 - shift))) : src[i];
    if (advance) g_BP.Advance(count * 8);
    return true;
}
u8 getBits8(u8* address, bool advance) { return read_bytes(address, 1, advance); }
u8 getBits32(u8* address, bool advance) { return read_bytes(address, 4, advance); }

void ipu_csc(macroblock_8&, macroblock_rgb32& rgb, int sgn);
namespace {
u8 threshold[2]{};
u16 palette[16]{};
alignas(16) u8 indices[128]{};
bool pumping = false;
bool chain_end = false;

bool refill_input() {
    auto& ch = hw::dma_chan(DMAC_TO_IPU);
    if (!(ch.chcr & 0x100) || ipu_fifo.in.size == 8) return false;
    if (ch.qwc == 0) {
        if (((ch.chcr >> 2) & 3) == 0) { hw::dma_done(DMAC_TO_IPU); return true; }
        if (chain_end) { chain_end = false; hw::dma_done(DMAC_TO_IPU); return true; }
        const u32* tag = reinterpret_cast<const u32*>(ee_dma_ptr(ch.tadr));
        const u32 t0 = tag[0], addr = tag[1] & ~15u;
        const u32 id = (t0 >> 28) & 7, count = t0 & 0xffff;
        ch.chcr = (ch.chcr & 0xffff) | (t0 & 0xffff0000u);
        ch.qwc = count;
        const u32 data = ch.tadr + 16;
        switch (id) {
        case 0: ch.madr = addr; ch.tadr += 16; chain_end = true; break;
        case 1: ch.madr = data; ch.tadr = data + count * 16; break;
        case 2: ch.madr = data; ch.tadr = addr; break;
        case 3: case 4: ch.madr = addr; ch.tadr += 16; break;
        case 5: {
            ch.madr = data;
            const u32 asp = (ch.chcr >> 4) & 3;
            if (asp == 0) ch.asr0 = data + count * 16;
            else ch.asr1 = data + count * 16;
            ch.chcr = (ch.chcr & ~0x30u) | (((asp + 1) & 3) << 4);
            ch.tadr = addr;
            break;
        }
        case 6: {
            ch.madr = data;
            const u32 asp = (ch.chcr >> 4) & 3;
            if (asp == 0) { chain_end = true; ch.tadr = data + count * 16; }
            else { ch.tadr = asp == 2 ? ch.asr1 : ch.asr0;
                   ch.chcr = (ch.chcr & ~0x30u) | ((asp - 1) << 4); }
            break;
        }
        case 7: ch.madr = data; ch.tadr = data + count * 16; chain_end = true; break;
        }
        if ((t0 & 0x80000000u) && (ch.chcr & 0x80)) chain_end = true;
        return true;
    }
    const unsigned n = ipu_fifo.in.write(reinterpret_cast<const u32*>(ee_dma_ptr(ch.madr)), ch.qwc);
    ch.madr += n * 16;
    ch.qwc -= n;
    return n != 0;
}

bool drain_output() {
    auto& ch = hw::dma_chan(DMAC_FROM_IPU);
    if (!(ch.chcr & 0x100)) return false;
    if (!ch.qwc) { hw::dma_done(DMAC_FROM_IPU); return true; }
    if (!ipu_fifo.out.size) return false;
    const unsigned n = ipu_fifo.out.read(ee_dma_ptr(ch.madr), ch.qwc);
    ch.madr += n * 16;
    ch.qwc -= n;
    return n != 0;
}

bool vdec() {
    if (ipu_cmd.pos[0] == 0) {
        if (!g_BP.FillBuffer(32)) return false;
        const u32 op = (ipu_cmd.current >> 26) & 3;
        switch (op) {
        case 0: decoder.mpeg1 = ipuRegs.ctrl.MP1;
                ipuRegs.cmd.DATA = get_macroblock_address_increment(); break;
        case 1: decoder.frame_pred_frame_dct = 1; decoder.coding_type = ipuRegs.ctrl.PCT;
                ipuRegs.cmd.DATA = get_macroblock_modes(); break;
        case 2: ipuRegs.cmd.DATA = get_motion_delta(0); break;
        case 3: ipuRegs.cmd.DATA = get_dmv(); break;
        }
        ipuRegs.ctrl.ECD = ipuRegs.cmd.DATA == 0;
        ipu_cmd.pos[0] = 1;
    }
    if (!g_BP.FillBuffer(32)) return false;
    ipuRegs.top = UBITS(32);
    return true;
}

void pack_vq() {
    for (unsigned pixel = 0; pixel < 256; ++pixel) {
        const auto& p = decoder.rgb16.c[pixel / 16][pixel % 16];
        unsigned best = 0, distance = ~0u;
        for (unsigned i = 0; i < 16; ++i) {
            const u16 c = palette[i];
            const int dr = int(p.r) - int(c & 31);
            const int dg = int(p.g) - int((c >> 5) & 31);
            const int db = int(p.b) - int((c >> 10) & 31);
            const unsigned error = unsigned(dr * dr + dg * dg + db * db);
            if (error < distance) { best = i; distance = error; }
        }
        if (!(pixel & 1)) indices[pixel / 2] = u8(best);
        else indices[pixel / 2] |= u8(best << 4);
    }
}

bool execute() {
    const unsigned op = ipu_cmd.current >> 28;
    switch (op) {
    case 1: return mpeg2sliceIDEC();
    case 2: return mpeg2_slice();
    case 3: return vdec();
    case 4: if (!g_BP.FillBuffer(32)) return false;
            ipuRegs.top = ipuRegs.cmd.DATA = UBITS(32); return true;
    case 5: {
        u8* matrix = (ipu_cmd.current & (1u << 27)) ? decoder.niq : decoder.iq;
        while (ipu_cmd.pos[0] < 64) {
            if (!read_bytes(matrix + ipu_cmd.pos[0], 8, true)) return false;
            ipu_cmd.pos[0] += 8;
        }
        return true;
    }
    case 6: {
        auto* colors = reinterpret_cast<u8*>(palette);
        while (ipu_cmd.pos[0] < 32) {
            if (!read_bytes(colors + ipu_cmd.pos[0], 8, true)) return false;
            ipu_cmd.pos[0] += 8;
        }
        return true;
    }
    case 7: case 8: {
        const unsigned count = ipu_cmd.current & 0x7ff;
        while (ipu_cmd.index < int(count)) {
            const unsigned input = op == 7 ? 384 : 1024;
            auto* dest = op == 7 ? reinterpret_cast<u8*>(&decoder.mb8) : reinterpret_cast<u8*>(&decoder.rgb32);
            while (ipu_cmd.pos[0] < int(input)) {
                if (!read_bytes(dest + ipu_cmd.pos[0], 8, true)) return false;
                ipu_cmd.pos[0] += 8;
            }
            const bool ofm = ipu_cmd.current & (1u << 27);
            if (ipu_cmd.pos[1] == 0) {
                if (op == 7) ipu_csc(decoder.mb8, decoder.rgb32, 0);
                if (ofm || op == 8)
                    ipu_dither(decoder.rgb32, decoder.rgb16, (ipu_cmd.current >> 26) & 1);
                if (op == 8 && !ofm) pack_vq();
            }
            const u32* source;
            unsigned total;
            if (op == 8 && !ofm) {
                source = reinterpret_cast<const u32*>(indices); total = 8;
            } else if (ofm) {
                source = reinterpret_cast<const u32*>(&decoder.rgb16); total = 32;
            } else {
                source = reinterpret_cast<const u32*>(&decoder.rgb32); total = 64;
            }
            ipu_cmd.pos[1] += ipu_fifo.out.write(source + ipu_cmd.pos[1] * 4,
                                                   total - ipu_cmd.pos[1]);
            if (ipu_cmd.pos[1] != int(total)) return false;
            ipu_cmd.pos[0] = ipu_cmd.pos[1] = 0;
            ++ipu_cmd.index;
        }
        return true;
    }
    default: return true;
    }
}

void pump() {
    if (pumping) return;
    pumping = true;
    // Every iteration must transfer at least one qword/bit or complete a command.
    for (unsigned guard = 0; guard < (1u << 22); ++guard) {
        bool changed = refill_input();
        changed |= drain_output();
        if (ipuRegs.ctrl.BUSY) {
            const u32 bp = g_BP.BP, fp = g_BP.FP;
            const unsigned in = ipu_fifo.in.size, out = ipu_fifo.out.size;
            if (execute()) {
                ipuRegs.ctrl.BUSY = 0;
                ipuRegs.cmd.BUSY = ipuRegs.topbusy = 0;
                ipu_cmd.current = ~0u;
                intc_raise(INTC_IPU);
                changed = true;
            } else if (g_BP.BP != bp || g_BP.FP != fp || in != ipu_fifo.in.size ||
                       out != ipu_fifo.out.size) changed = true;
        }
        changed |= drain_output();
        if (!changed) break;
    }
    pumping = false;
}
} // namespace

void ipu_csc(macroblock_8&, macroblock_rgb32& rgb, int sgn) {
    yuv2rgb();
    for (auto& row : rgb.c) for (auto& pixel : row) {
        if (pixel.r < threshold[0] && pixel.g < threshold[0] && pixel.b < threshold[0]) {
            pixel = {0, 0, 0, 0};
        } else if (pixel.r < threshold[1] && pixel.g < threshold[1] && pixel.b < threshold[1]) {
            pixel.a = 0x40;
        }
        if (sgn) { pixel.r ^= 0x80; pixel.g ^= 0x80; pixel.b ^= 0x80; }
    }
}
void ipu_dither(const macroblock_rgb32& rgb, macroblock_rgb16& out, int dte) {
    static constexpr int matrix[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};
    for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x) {
        const auto& p = rgb.c[y][x];
        const int add = dte ? matrix[y & 3][x & 3] : 0;
        out.c[y][x].r = std::clamp(int(p.r) + add, 0, 255) >> 3;
        out.c[y][x].g = std::clamp(int(p.g) + add, 0, 255) >> 3;
        out.c[y][x].b = std::clamp(int(p.b) + add, 0, 255) >> 3;
        out.c[y][x].a = p.a == 0x40;
    }
}

namespace ipu {
void init() {
    ipu_fifo.clear(); g_BP = {}; ipuRegs = {}; ipu_cmd.clear(); decoder = {};
    decoder.picture_structure = FRAME_PICTURE;
    coded_block_pattern = 0;
    chain_end = pumping = false;
    threshold[0] = threshold[1] = 0;
    std::memset(palette, 0, sizeof palette);
}
u32 read_reg(u32 offset) {
    pump();
    switch (offset & 0xf0) {
    case 0: return (offset & 4) ? ipuRegs.cmd.BUSY : ipuRegs.cmd.DATA;
    case 0x10: return (ipuRegs.ctrl.raw & ~0xffu) | ipu_fifo.in.size |
                      (ipu_fifo.out.size << 4) | (coded_block_pattern << 8);
    case 0x20: return (g_BP.BP & 127) | (ipu_fifo.in.size << 8) | (g_BP.FP << 16);
    case 0x30: return (offset & 4) ? ipuRegs.topbusy : ipuRegs.top;
    default: return 0;
    }
}
u64 read_reg64(u32 offset) {
    return u64(read_reg(offset)) | (u64(read_reg(offset + 4)) << 32);
}
void write_reg(u32 offset, u32 value) {
    switch (offset & 0xf0) {
    case 0:
        ipu_cmd.clear(); ipu_cmd.current = value;
        ipuRegs.ctrl.ECD = ipuRegs.ctrl.SCD = 0;
        if ((value >> 28) == 0) {
            ipu_fifo.in.clear(); g_BP = {}; g_BP.BP = value & 127;
            ipuRegs.ctrl.BUSY = 0; ipuRegs.cmd.BUSY = 0;
            intc_raise(INTC_IPU);
        } else if ((value >> 28) == 9) {
            threshold[0] = value & 255;
            threshold[1] = (value >> 16) & 255;
            ipuRegs.ctrl.BUSY = 0; ipuRegs.cmd.BUSY = 0;
            intc_raise(INTC_IPU);
        } else {
            if ((value >> 28) >= 1 && (value >> 28) <= 5) g_BP.Advance(value & 63);
            if ((value >> 28) == 1) {
                ipuRegs.ctrl.PCT = 1;
                decoder.coding_type = 1;
                decoder.mpeg1 = ipuRegs.ctrl.MP1;
                decoder.q_scale_type = ipuRegs.ctrl.QST;
                decoder.intra_vlc_format = ipuRegs.ctrl.IVF;
                decoder.scantype = ipuRegs.ctrl.AS;
                decoder.intra_dc_precision = ipuRegs.ctrl.IDP;
                decoder.quantizer_scale = (value >> 16) & 31;
                decoder.frame_pred_frame_dct = !(value & (1u << 24));
                decoder.sgn = (value >> 25) & 1;
                decoder.dte = (value >> 26) & 1;
                decoder.ofm = (value >> 27) & 1;
                decoder.dcr = 1;
            } else if ((value >> 28) == 2) {
                decoder.coding_type = 1;
                decoder.mpeg1 = ipuRegs.ctrl.MP1;
                decoder.q_scale_type = ipuRegs.ctrl.QST;
                decoder.intra_vlc_format = ipuRegs.ctrl.IVF;
                decoder.scantype = ipuRegs.ctrl.AS;
                decoder.intra_dc_precision = ipuRegs.ctrl.IDP;
                const u32 scale = (value >> 16) & 31;
                decoder.quantizer_scale = decoder.q_scale_type ? non_linear_quantizer_scale[scale] : scale * 2;
                decoder.macroblock_modes = (value & (1u << 25) ? DCT_TYPE_INTERLACED : 0) |
                                           (value & (1u << 27) ? MACROBLOCK_INTRA : MACROBLOCK_PATTERN);
                decoder.dcr = (value >> 26) & 1;
            }
            ipuRegs.ctrl.BUSY = 1;
            if ((value >> 28) >= 1 && (value >> 28) <= 4) {
                ipuRegs.topbusy = 0x80000000u;
                if ((value >> 28) >= 3) ipuRegs.cmd.BUSY = 0x80000000u;
            }
        }
        break;
    case 0x10:
        ipuRegs.ctrl.raw = (ipuRegs.ctrl.raw & 0x8000ffffu) | (value & 0x47f30000u);
        if (ipuRegs.ctrl.IDP == 3) ipuRegs.ctrl.IDP = 1;
        if (value & (1u << 30)) {
            ipu_fifo.clear(); g_BP = {}; ipuRegs = {}; ipu_cmd.clear();
            coded_block_pattern = 0;
            chain_end = false;
        }
        break;
    default: break;
    }
    pump();
}
void write_reg64(u32 offset, u64 value) { write_reg(offset, u32(value)); }
void write_fifo(const qword& value) {
    pump();
    if (!ipu_fifo.in.write(value.w, 1))
        LOG(LOG_IPU, "[ipu] EE write while input FIFO full\n");
    pump();
}
void read_fifo(qword* result) {
    pump();
    *result = {};
    ipu_fifo.out.read(result, 1);
    pump();
}
void dma_start(u32 ch) {
    if (ch == DMAC_TO_IPU) chain_end = false;
    if (ch == DMAC_TO_IPU || ch == DMAC_FROM_IPU) pump();
}
} // namespace ipu
