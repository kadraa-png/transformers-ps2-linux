#pragma once
// Glue for the PCSX2 1.6 MPEG-2 decoder (Mpeg.cpp, Vlc.h, Idct.cpp).
// PCSX2's copyright and licensing notices remain in the vendored files.
#include "ps2/types.h"
#include <cassert>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <emmintrin.h>
using uint = unsigned int;
using uptr = uintptr_t;
#define __aligned16 __attribute__((aligned(16)))
#define __fi
#define __ri __attribute__((noinline))
#define jNO_DEFAULT default: break
#define pxAssume(x) assert(x)
#define pxAssert(x) assert(x)
#define pxAssertMsg(x, message) assert(x)
#define memzero_sse_a(x) std::memset(&(x), 0, sizeof(x))

struct InputFifo {
    qword data[8]{};
    unsigned readpos = 0, writepos = 0, size = 0;
    unsigned write(const u32* words, unsigned count);
    unsigned read(void* dest);
    void clear() { readpos = writepos = size = 0; }
};
struct OutputFifo {
    qword data[8]{};
    unsigned readpos = 0, writepos = 0, size = 0;
    unsigned write(const u32* words, unsigned count);
    unsigned read(void* dest, unsigned count);
    void clear() { readpos = writepos = size = 0; }
};
struct IPUFifo {
    InputFifo in;
    OutputFifo out;
    void clear() { in.clear(); out.clear(); }
};
extern IPUFifo ipu_fifo;

struct BitPointer {
    qword internal_qwc[2]{};
    u32 BP = 0, IFC = 0, FP = 0;
    void Advance(unsigned bits);
    void Align() { Advance((8 - (BP & 7)) & 7); }
    bool FillBuffer(unsigned bits);
};
using tIPU_BP = BitPointer;
extern BitPointer g_BP;

union Control {
    struct {
        u32 IFC:4, OFC:4, CBP:6, ECD:1, SCD:1, IDP:2, reserved0:2,
            AS:1, IVF:1, QST:1, MP1:1, PCT:3, reserved1:3, RST:1, BUSY:1;
    };
    u32 raw;
    Control() : raw(0) {}
};
struct IpuRegisters {
    Control ctrl;
    struct { u32 DATA = 0, BUSY = 0; } cmd;
    u32 top = 0, topbusy = 0;
};
extern IpuRegisters ipuRegs;
extern int coded_block_pattern;
struct CommandState {
    u32 current = ~0u;
    int index = 0, pos[6]{};
    void clear() { current = ~0u; index = 0; std::memset(pos, 0, sizeof(pos)); }
};
extern CommandState ipu_cmd;

#include "Mpeg.h"
#include "yuv2rgb.h"

// IPU.cpp compatibility functions used by the unmodified MPEG decoder.
u8 getBits8(u8* address, bool advance);
u8 getBits32(u8* address, bool advance);
u32 UBITS(uint bits);
s32 SBITS(uint bits);
void ipu_csc(macroblock_8&, macroblock_rgb32&, int sgn);
void ipu_dither(const macroblock_rgb32&, macroblock_rgb16&, int dte);
