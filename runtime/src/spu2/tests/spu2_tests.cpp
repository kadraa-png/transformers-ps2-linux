#include "ps2/spu2.h"
#include "ps2/platform.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
std::vector<s16> captured;
int irqs[2]{}, dmas[2]{};
constexpr u32 base = 0x1f900000;
void wr(u32 reg, u16 value) { spu2::write16(base + reg, value); }
u16 rd(u32 reg) { return spu2::read16(base + reg); }
void setup_output(int core) {
    const u32 offset = core * 0x400;
    wr(0x760 + core * 0x28, 0x3fff);
    wr(0x762 + core * 0x28, 0x3fff);
    wr(0x198 + offset, 0x0c00);
    wr(0x188 + offset, 1);
    wr(0x190 + offset, 1);
    if (core == 0) {
        wr(0x788, 0x3fff); wr(0x78a, 0x3fff);
        wr(0x598, 0x0c0c); // core 1 receives core 0 as external source
    }
}
std::array<u16, 56> make_sine(bool loop) {
    std::array<u16, 56> blocks{};
    for (int block = 0; block < 7; ++block) {
        blocks[block * 8] = static_cast<u16>(((block == 0 ? 4 : block == 6 ? (loop ? 3 : 1) : 0) << 8) | 0x06);
        for (int n = 0; n < 28; ++n) {
            const auto q = static_cast<int>(std::lround(7 * std::sin(2 * 3.14159265358979323846 * n / 28.0)));
            blocks[block * 8 + 1 + n / 4] |= static_cast<u16>((q & 15) << ((n & 3) * 4));
        }
    }
    return blocks;
}
void start(bool loop, int core = 0) {
    const u32 offset = core * 0x400;
    const auto sample = make_sine(loop);
    wr(offset + 0x1a8, 0);
    wr(offset + 0x1aa, 0x4000);
    spu2::dma_write(core, sample.data(), sample.size());
    wr(offset + 0x1c0, 0); wr(offset + 0x1c2, 0x4000);
    wr(offset + 0x000, 0x3fff); wr(offset + 0x002, 0x3fff);
    wr(offset + 0x004, 0x1000);
    wr(offset + 0x006, 0x000f);
    wr(offset + 0x008, 0x1fcc);
    wr(offset + 0x1a0, 1);
}
void test_loop_and_release() {
    spu2::reset(); captured.clear(); setup_output(0); start(true);
    spu2::advance(4800);
    assert(captured.size() == 9600);
    const int peak = *std::max_element(captured.begin(), captured.end());
    assert(peak > 50);
    int crossings = 0;
    for (int i = 1001; i < 4799; ++i)
        if (captured[i * 2 - 2] <= 0 && captured[i * 2] > 0) ++crossings;
    assert(crossings > 120 && crossings < 150);
    wr(0x1a4, 1);
    captured.clear(); spu2::advance(10000);
    const auto last = captured.end() - 1024;
    assert(std::all_of(last, captured.end(), [](s16 sample) { return sample == 0; }));
}
void test_endx() {
    spu2::reset(); captured.clear(); setup_output(0); start(false);
    spu2::advance(300);
    assert(rd(0x340) & 1);
    wr(0x340, 0xffff);
    assert(!(rd(0x340) & 1));
}
void test_irq_and_transfer() {
    spu2::reset(); irqs[0] = irqs[1] = dmas[0] = 0;
    wr(0x19c, 0); wr(0x19e, 0x4008); wr(0x19a, 0x40);
    start(true);
    spu2::advance(1); // SPU2-X delivers the latched DMA IRQ and DMA completion on a tick.
    assert(irqs[0] == 1);
    assert(dmas[0] == 1);
    std::array<u16, 1> got{};
    wr(0x1a8, 0); wr(0x1aa, 0x4000);
    spu2::dma_read(0, got.data(), 1);
    assert(got[0] == make_sine(true)[0]);
    assert(rd(0x1aa) == 0x4021);
    spu2::advance(1);
    assert(dmas[0] == 2);
    wr(0x19a, 0); wr(0x19a, 0x40); // Clear SPU2-X's latched IRQINFO.
    const int before = irqs[0];
    spu2::advance(64); // Voice crosses the IRQA at the next ADPCM packet.
    assert(irqs[0] > before);
    wr(0x1a8, 0); wr(0x1aa, 0x4000);
    assert(rd(0x1ac) == make_sine(true)[0]);
    assert(rd(0x1aa) == 0x4001);
}
void test_adma() {
    spu2::reset(); captured.clear(); dmas[0] = 0;
    setup_output(0);
    wr(0x198, 0x00c0);
    wr(0x76c, 0x7fff); wr(0x76e, 0x7fff);
    wr(0x1b0, 1);
    std::array<u16, 512> pcm{};
    for (int i = 0; i < 256; ++i) {
        pcm[i] = static_cast<u16>(i < 128 ? 12000 : -12000);
        pcm[256 + i] = static_cast<u16>(i < 128 ? -9000 : 9000);
    }
    spu2::dma_write(0, pcm.data(), pcm.size());
    spu2::advance(256);
    assert(dmas[0] == 1);
    assert(captured[20] > 1000 && captured[21] < -1000);
    pcm.fill(5000);
    spu2::dma_write(0, pcm.data(), pcm.size());
    captured.clear(); spu2::advance(256);
    assert(dmas[0] == 2);
    assert(captured[20] > 100);
}
void test_core1_and_bulk_adma() {
    spu2::reset(); captured.clear();
    setup_output(1); start(true, 1);
    spu2::advance(500);
    assert(std::any_of(captured.begin(), captured.end(), [](s16 sample) { return sample > 100; }));
    spu2::reset(); captured.clear(); dmas[1] = 0;
    setup_output(1);
    wr(0x598, 0x00c0);
    wr(0x794, 0x7fff); wr(0x796, 0x7fff);
    wr(0x5b0, 2);
    std::array<u16, 1024> pcm{};
    std::fill(pcm.begin(), pcm.begin() + 512, 9000);
    std::fill(pcm.begin() + 512, pcm.end(), 4000);
    spu2::dma_write(1, pcm.data(), pcm.size());
    spu2::advance(512);
    assert(dmas[1] == 2);
    assert(captured[20] > captured[600]);
    assert(captured[600] > 100);
}
void test_reverb() {
    spu2::reset(); captured.clear(); setup_output(0); start(true);
    wr(0x2e0, 0); wr(0x2e2, 0x8000); wr(0x33c, 0);
    wr(0x764, 0x4000); wr(0x766, 0x4000);
    wr(0x774, 0x2000); wr(0x776, 0x2000);
    wr(0x77e, 0x1000); wr(0x784, 0x2000); wr(0x786, 0x2000);
    wr(0x18c, 1); wr(0x194, 1);
    wr(0x19a, 0x80);
    spu2::advance(2048);
    assert(std::any_of(captured.begin(), captured.end(), [](s16 value) { return value != 0; }));
    assert(std::none_of(captured.begin(), captured.end(), [](s16 value) {
        return value == -32768 || value == 32767;
    }));
}
} // namespace
namespace platform { void audio_push(const s16* samples, u32 n) { captured.insert(captured.end(), samples, samples + n * 2); } }
int main() {
    spu2::init();
    spu2::set_irq_callback([](int core) { ++irqs[core]; });
    spu2::set_dma_callback([](int core) { ++dmas[core]; });
    test_loop_and_release(); test_endx(); test_irq_and_transfer(); test_adma(); test_core1_and_bulk_adma(); test_reverb();
    spu2::shutdown();
    std::puts("spu2_tests: passed");
}
