/* Native emulation-thread adapter for PCSX2 1.6 SPU2-X.
 * Upstream DSP/register/DMA code and its LGPL notice live in third_party/spu2x/.
 */
#include "ps2/spu2.h"
#include "ps2/platform.h"
#include "Global.h"
#include <array>
#include <algorithm>
#include <memory>
#include <new>
#include <vector>

void SPU2_FastWrite(u32 rmem, u16 value);
void TimeUpdate(u32 cClocks);
extern bool has_to_call_irq;

// Plugin callbacks are deliberately invoked on the emulation thread only.
void (*dma4callback)() = nullptr;
void (*dma7callback)() = nullptr;
void (*spu2_irq_for_core)(int) = nullptr;
unsigned spu2_pending_irq = 0;
u32 lClocks = 0;
float VolumeAdjustFL = 1.0f;
float VolumeAdjustFR = 1.0f;
StereoOut32 StereoOut32::Empty{};

namespace {
std::array<s16, 0x100000> sound_ram{};
std::array<s16, 0x1000> register_ram{};
std::unique_ptr<PcmCacheEntry[]> cache;
std::array<std::vector<u16>, 2> adma_source;
std::array<s16, 1024 * 2> output{};
u32 output_size = 0;
void (*irq_handler)(int) = nullptr;
void (*dma_handler)(int) = nullptr;

void flush_audio() {
    if (output_size) {
        platform::audio_push(output.data(), output_size);
        output_size = 0;
    }
}
void irq_dispatch(int core) { if (irq_handler) irq_handler(core); }
// PCSX2's SPU2interruptDMA4/7 plugin glue: the core reports DMA ready
// (STATX bit 7) before the IOP DMA interrupt handler runs; libsd's handler
// waits for that bit.
void dma4_dispatch() { Cores[0].Regs.STATX |= 0x80; if (dma_handler) dma_handler(0); }
void dma7_dispatch() { Cores[1].Regs.STATX |= 0x80; if (dma_handler) dma_handler(1); }

// SPU2 time follows the host clock: frames_due() = 48 kHz frames elapsed since
// the first query minus the frames already produced.  IOP register and DMA
// accesses catch up first, so the IOP sees the SPU2 progress between its
// accesses (DMA completion, key-on latency, STATX/ENDX polling) even though
// recompiled IOP code runs far faster than the real IOP.
constexpr u64 RATE = 48000;
constexpr u64 MAX_BACKLOG = 4800;   // a stall longer than 100 ms is skipped, not replayed
u64 clock_origin_ns = 0;
u64 frames_done = 0;
bool advancing = false;             // TimeUpdate callbacks may re-enter through the IOP
void catch_up();
} // namespace

namespace SndBuffer {
void Write(const StereoOut32& sample) {
    // SPU2-X's output remains 12-bit fixed-point until SndOut's DownSample.
    output[output_size * 2] = static_cast<s16>(sample.Left >> SndOutVolumeShift);
    output[output_size * 2 + 1] = static_cast<s16>(sample.Right >> SndOutVolumeShift);
    if (++output_size == output.size() / 2) flush_audio();
}
} // namespace SndBuffer

namespace spu2 {
void init() {
    if (!cache) cache = std::make_unique<PcmCacheEntry[]>(pcm_BlockCount + 1);
    _spu2mem = sound_ram.data();
    spu2regs = register_ram.data();
    pcm_cache_data = cache.get();
    dma4callback = dma4_dispatch;
    dma7callback = dma7_dispatch;
    spu2_irq_for_core = irq_dispatch;
    InitADSR();
    reset();
}

void reset() {
    flush_audio();
    sound_ram.fill(0);
    register_ram.fill(0);
    if (!cache) cache = std::make_unique<PcmCacheEntry[]>(pcm_BlockCount + 1);
    std::fill_n(cache.get(), pcm_BlockCount + 1, PcmCacheEntry{});
    _spu2mem = sound_ram.data();
    spu2regs = register_ram.data();
    pcm_cache_data = cache.get();
    for (auto& source : adma_source) source.clear();
    for (int i = 0; i < 2; ++i) {
        Cores[i].~V_Core();
        std::memset(static_cast<void*>(&Cores[i]), 0, sizeof(V_Core));
        new (&Cores[i]) V_Core();
        Cores[i].Init(i);
    }
    for (u32 i = 0; i < 0x400; ++i)
        regtable[i] = const_cast<u16*>(regtable_original[i] ? regtable_original[i] :
                                        reinterpret_cast<u16*>(&register_ram[i]));
    regtable[0x400] = nullptr;
    Spdif = {};
    Cycles = lClocks = 0;
    OutPos = InputPos = 0;
    PlayMode = 0;
    spu2_pending_irq = 0;
    has_to_call_irq = false;
}

void shutdown() {
    flush_audio();
    irq_handler = dma_handler = nullptr;
    dma4callback = dma7callback = nullptr;
    spu2_irq_for_core = nullptr;
    cache.reset();
    pcm_cache_data = nullptr;
    _spu2mem = spu2regs = nullptr;
}
void set_irq_callback(void (*cb)(int)) { irq_handler = cb; }
void set_dma_callback(void (*cb)(int)) { dma_handler = cb; }
void debug_dump() {
    for (int c = 0; c < 2; ++c) {
        int active = 0;
        for (int v = 0; v < 24; ++v) active += Cores[c].Voices[v].ADSR.Value != 0;
        fprintf(stderr, "--- SPU2 core %d: %d voices with envelope, ATTR %04x STATX %04x ADMA %x DMAI %d"
                " adma_busy %d left %d pos %d TSA %05x\n", c, active, Cores[c].Regs.ATTR, Cores[c].Regs.STATX,
                Cores[c].AutoDMACtrl, Cores[c].DMAICounter, (int)Cores[c].AdmaInProgress, Cores[c].InputDataLeft,
                Cores[c].InputPosRead, Cores[c].TSA);
        for (int v = 0; v < 24; ++v) {
            const V_Voice& voice = Cores[c].Voices[v];
            if (!voice.ADSR.Value) continue;
            fprintf(stderr, "  v%-2d env %08x phase %u rel %d pitch %04x start %05x loop %05x next %05x adsr %04x/%04x\n",
                    v, voice.ADSR.Value, voice.ADSR.Phase, voice.ADSR.Releasing, voice.Pitch, voice.StartA,
                    voice.LoopStartA, voice.NextA, voice.ADSR.regADSR1, voice.ADSR.regADSR2);
        }
    }
    fprintf(stderr, "--- SPU2 clock: %llu frames done, %u due\n", (unsigned long long)frames_done, frames_due());
}

u16 read16(u32 addr) {
    if (addr < 0x1f900000 || addr >= 0x1f900800 || (addr & 1)) return 0;
    catch_up();
    const u32 offset = addr & 0x7ff;
    if ((offset & 0x3ff) == REG__1AC && offset < 0x800)
        return Cores[offset >> 10].DmaRead();
    return *regtable[offset >> 1];
}
void write16(u32 addr, u16 value) {
    if (addr < 0x1f900000 || addr >= 0x1f900800 || (addr & 1)) return;
    catch_up();
    SPU2_FastWrite(addr, value);
}
void dma_write(int core, const u16* data, u32 halfwords) {
    if (core < 0 || core > 1 || !data || !halfwords) return;
    catch_up();
    // ADMA can consume blocks many ticks after dma_write returns: retain the IOP
    // input until SPU2-X has finished reading that block's source words.
    if (Cores[core].AutoDMACtrl & (core + 1)) {
        auto& source = adma_source[core];
        source.assign(data, data + halfwords);
        Cores[core].DoDMAwrite(source.data(), halfwords);
    } else {
        Cores[core].DoDMAwrite(const_cast<u16*>(data), halfwords);
    }
}
void dma_read(int core, u16* data, u32 halfwords) {
    if (core < 0 || core > 1 || !data || !halfwords) return;
    catch_up();
    Cores[core].DoDMAread(data, halfwords);
}
void advance(u32 frames) {
    const bool nested = advancing;
    advancing = true;
    frames_done += frames;
    // Upstream TimeUpdate caps huge clock jumps at 4800 frames. Feed bounded
    // updates so a single host queue fill never loses an emulated sample.
    while (frames) {
        const u32 batch = std::min(frames, 4800u);
        TimeUpdate(lClocks + batch * 768);
        frames -= batch;
    }
    flush_audio();
    advancing = nested;
}
u32 frames_due() {
    const u64 now = platform::now_ns();
    if (!clock_origin_ns) clock_origin_ns = now;
    const u64 target = now > clock_origin_ns ? (now - clock_origin_ns) * RATE / 1'000'000'000ull : 0;
    if (target <= frames_done) return 0;
    if (target - frames_done > MAX_BACKLOG) frames_done = target - MAX_BACKLOG;
    return static_cast<u32>(target - frames_done);
}
void nudge(s32 frames) {
    if (!clock_origin_ns) return;
    const s64 ns = static_cast<s64>(frames) * 1'000'000'000ll / static_cast<s64>(RATE);
    clock_origin_ns = static_cast<u64>(static_cast<s64>(clock_origin_ns) - ns);
}
} // namespace spu2

namespace {
void catch_up() {
    if (advancing) return;
    if (const u32 due = spu2::frames_due()) spu2::advance(due);
}
} // namespace

void spu2::sync() { catch_up(); }
