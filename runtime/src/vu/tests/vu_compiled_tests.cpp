#include "ps2/vu.h"
#include "ps2/gs.h"
#include "ps2/intc.h"
#include "VU.h"
#include "vu_compiled.h"
#include "vu_program_entries.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

namespace {
std::vector<u8> kicks;
u32 random_state;
u32 random_word() {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}
struct Result {
    VURegs regs;
    VURegs vu0;
    std::array<u8, 16384> data;
    std::vector<u8> kick;
    u32 pairs;
    u64 elapsed_ns;
};
void upload(const u8* bytes, u32 count, u32 micro_offset, int unit = 1) {
    // Real VIF MPG commands, each limited to the command's 8-bit pair count.
    for (u32 base = 0; base < count; base += 2048) {
        const u32 chunk = std::min<u32>(2048, count - base);
        assert(chunk % 8 == 0);
        std::vector<u32> payload;
        payload.reserve(chunk / 4 + 1);
        payload.push_back(0x4a000000u | (((chunk / 8u) & 255u) << 16) |
                          (((micro_offset + base) / 8u) & 2047u));
        for (u32 n = 0; n < chunk; n += 4) {
            u32 word;
            std::memcpy(&word, bytes + base + n, 4);
            payload.push_back(word);
        }
        vif_dma_words(unit, payload.data(), static_cast<u32>(payload.size()));
    }
}
Result run(const u8* bytes, u32 count, u32 entry, bool compiled, u32 limit,
           bool through_api = false) {
    vu_init(); vif_init();
    upload(bytes, count, entry);
    auto& r = VU1;
    random_state = 0x691ad53bu;
    for (auto& vf : r.VF) for (auto& lane : vf.UL) lane = (random_word() & 0x807fffffu) | 0x3e800000u;
    r.VF[0].UL[0] = r.VF[0].UL[1] = r.VF[0].UL[2] = 0;
    r.VF[0].UL[3] = 0x3f800000u;
    for (u32 i = 1; i < 16; ++i) r.VI[i].UL = random_word() & 0xff;
    for (auto& lane : r.ACC.UL) lane = (random_word() & 0x807fffffu) | 0x3e800000u;
    for (u32 i = 0; i < 16384; i += 4) {
        const u32 value = (random_word() & 0x807fffffu) | 0x3e800000u;
        std::memcpy(g_vu1.data + i, &value, 4);
    }
    kicks.clear();
    VU0.VI[REG_VPU_STAT].UL |= 0x100;
    const auto begin = std::chrono::steady_clock::now();
    u32 pairs;
    if (through_api) {
        setenv("PS2_VU_INTERP", compiled ? "0" : "1", 1);
        vu_exec(1, entry);
        pairs = r.cycle;
        unsetenv("PS2_VU_INTERP");
    } else {
        pairs = compiled ? vu_port_run_compiled(1, entry, limit)
                         : vu_port_run_micro(1, entry, limit);
    }
    const auto end = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end-begin).count();
    Result result{r, VU0, {}, kicks, pairs, static_cast<u64>(elapsed)};
    std::memcpy(result.data.data(), g_vu1.data, result.data.size());
    return result;
}
}
namespace gs {
void gif_path1(const u8* data, u32 address) {
    kicks.push_back(static_cast<u8>(address));
    kicks.push_back(static_cast<u8>(address >> 8));
    kicks.insert(kicks.end(), data, data + 16384);
}
void gif_path2(const u8*, u32) {}
void read_fifo(u8* data, u32 qwc) { std::memset(data, 0, qwc * 16); }
bool set_path1_async(bool) { return false; }   // no GS thread: VU1 stays synchronous
void path1_program() {}
void path1_program_end() {}
void flush() {}
}
void intc_raise(u32) {}

int main() {
    std::ifstream in(VU_GAME_ELF, std::ios::binary);
    assert(in && "game ELF is required for differential test");
    constexpr u32 offset = 0x5dcb80 - 0x100000 + 0x100;
    constexpr u32 size = 0x607f20 - 0x5dcb80;
    std::vector<u8> elf(size);
    in.seekg(offset);
    in.read(reinterpret_cast<char*>(elf.data()), size);
    assert(in.gcount() == size);
    // VIF overwrites must replace the mapped identity, while partial EE
    // aperture writes must invalidate even if only one byte changes.
    vu_init(); vif_init();
    upload(elf.data(), 512, 0x100);
    const auto* original = vu_compiled_at(1, 0x100);
    assert(original);
    upload(elf.data() + vu_program_entries[1] * 8, 512, 0x100);
    const auto* overwritten = vu_compiled_at(1, 0x100);
    assert(overwritten && overwritten != original);
    g_vu1.micro[0x101] ^= 0x80;
    vu_micro_written(1, 0x101, 1);
    assert(vu_compiled_at(1, 0x100) == nullptr);
    upload(elf.data(), 512, 0x100);
    assert(vu_compiled_at(1, 0x100) == original);
    // A single MPG that crosses the 16 KiB VU1 boundary must map both the
    // high-address pair and the wrapped low-address pairs.
    upload(elf.data(), 512, 0x3fc0);
    assert(vu_compiled_at(1, 0x3fc0) == original);
    assert(vu_compiled_at(1, 0) != nullptr);

    // Exercise public dispatch and PS2_VU_INTERP=1 on a real E-bit pair
    // (and its delayed successor), so an unbounded vu_exec call terminates.
    u32 short_program = 0;
    for (u32 index = 0; index + 2 < size / 8; ++index) {
        u32 lower, upper;
        std::memcpy(&lower, elf.data() + index * 8, 4);
        std::memcpy(&upper, elf.data() + index * 8 + 4, 4);
        const u32 primary = lower >> 25;
        if ((upper & 0x40000000u) && !(upper & 0x80000000u) &&
            !(primary >= 0x20 && primary <= 0x2f)) {
            short_program = index * 8;
            break;
        }
    }
    assert(short_program);
    auto api_interp = run(elf.data() + short_program, 512, 0x200, false, 128, true);
    auto api_compiled = run(elf.data() + short_program, 512, 0x200, true, 128, true);
    assert(api_interp.pairs == api_compiled.pairs &&
           api_interp.regs.VI[REG_TPC].UL == 0x210 &&
           api_compiled.regs.VI[REG_TPC].UL == 0x210);
    assert(std::memcmp(&api_interp.regs, &api_compiled.regs, sizeof(VURegs)) == 0 &&
           api_interp.data == api_compiled.data && api_interp.kick == api_compiled.kick);
    // Exercise the distinct VU0 pipe/flag loop on the same actual E-bit pair.
    struct VU0Result { VURegs state; std::array<u8, 4096> data; u32 pairs; };
    auto vu0_run = [&](bool compiled) {
        vu_init(); vif_init();
        upload(elf.data() + short_program, 512, 0x100, 0);
        VU0.VI[REG_VPU_STAT].UL |= 1;
        const u32 pairs = compiled ? vu_port_run_compiled(0, 0x100, 128) :
                                      vu_port_run_micro(0, 0x100, 128);
        VU0Result result{VU0, {}, pairs};
        std::memcpy(result.data.data(), g_vu0.data, result.data.size());
        return result;
    };
    auto vu0_reference = vu0_run(false), vu0_candidate = vu0_run(true);
    assert(vu0_reference.pairs == vu0_candidate.pairs &&
           std::memcmp(&vu0_reference.state, &vu0_candidate.state, sizeof(VURegs)) == 0 &&
           vu0_reference.data == vu0_candidate.data);
    unsigned checked = 0;
    u64 compiled_ns = 0, interpreted_ns = 0;
    u64 compiled_pairs = 0, interpreted_pairs = 0;
    u64 observed_kicks = 0;
    // Cover every statically rooted registration plus 500 other pairs spread
    // across authentic registered programs, not the large zero-filled gap
    // between program clusters.
    std::vector<u32> entries;
    for (auto entry : vu_program_entries) entries.push_back(entry * 8);
    std::vector<u32> sample_entries;
    for (size_t i = 0; i < vu_program_entries.size(); ++i) {
        for (u32 offset = 0; offset + 512 <= vu_program_bytes[i]; offset += 8)
            sample_entries.push_back(vu_program_entries[i] * 8 + offset);
    }
    for (u32 sample = 0; sample < 500; ++sample)
        entries.push_back(sample_entries[u64(sample) * sample_entries.size() / 500]);
    for (u32 entry : entries) {
        const u32 bytes = checked < vu_program_bytes.size() ?
                          vu_program_bytes[checked] : 512;
        if (entry + bytes > size) continue;
        const u8* program = elf.data() + entry;
        const u32 micro_address = (checked % 4) * 0x300;
        auto reference = run(program, bytes, micro_address, false, 128);
        auto candidate = run(program, bytes, micro_address, true, 128);
        interpreted_ns += reference.elapsed_ns;
        compiled_ns += candidate.elapsed_ns;
        compiled_pairs += candidate.pairs;
        interpreted_pairs += reference.pairs;
        observed_kicks += reference.kick.size() / (16384 + 2);
        // Pointers and pad bytes are identical after vu_init; all pipeline
        // descriptors, flag histories and branch-delay slots are included.
        if (reference.pairs != candidate.pairs ||
            std::memcmp(&reference.regs, &candidate.regs, sizeof(VURegs)) ||
            std::memcmp(&reference.vu0, &candidate.vu0, sizeof(VURegs)) ||
            reference.data != candidate.data || reference.kick != candidate.kick) {
            std::fprintf(stderr, "VU mismatch: ELF pair %u (after %u/%u pairs)\n",
                         entry / 8, reference.pairs, candidate.pairs);
            return 1;
        }
        ++checked;
    }
    // Warm dispatch on three registered game programs from different ELF
    // regions, excluding VIF upload and one-off analysis from timed execution.
    constexpr u32 repeats = 5000;
    constexpr std::array<unsigned, 3> programs = {0, 23, 35};
    std::array<double, 2> rates{};
    std::array<std::array<double, 3>, 2> program_rates{};
    for (unsigned mode = 0; mode < 2; ++mode) {
        u64 total_pairs = 0;
        double elapsed = 0;
        for (unsigned slot = 0; slot < programs.size(); ++slot) {
            const unsigned index = programs[slot];
            const u32 program = vu_program_entries[index] * 8;
            vu_init(); vif_init();
            upload(elf.data() + program, vu_program_bytes[index], 0);
            VU0.VI[REG_VPU_STAT].UL |= 0x100;
            (void)vu_port_run_compiled(1, 0, 128);
            u64 program_pairs = 0;
            const auto start = std::chrono::steady_clock::now();
            for (u32 n = 0; n < repeats; ++n) {
                VU0.VI[REG_VPU_STAT].UL |= 0x100;
                program_pairs += mode ? vu_port_run_micro(1, 0, 128) :
                                        vu_port_run_compiled(1, 0, 128);
            }
            const double seconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start).count();
            elapsed += seconds;
            total_pairs += program_pairs;
            program_rates[mode][slot] = program_pairs / seconds / 1e6;
        }
        rates[mode] = total_pairs / elapsed / 1e6;
    }
    std::printf("Warmed registered programs: %.2f/%.2f Mpairs/s, %.2fx speedup\n",
                rates[0], rates[1], rates[0] / rates[1]);
    for (unsigned slot = 0; slot < programs.size(); ++slot)
        std::printf("program %u: %.2f/%.2f Mpairs/s\n",
                    programs[slot], program_rates[0][slot], program_rates[1][slot]);
    std::printf("VU differential: %u entries (%zu registered), %llu/%llu pairs, "
                "%llu XGKICK packets; execution throughput %.2f/%.2f Mpairs/s "
                "(compiled/interpreted), speedup %.2fx\n",
                checked, vu_program_entries.size(),
                static_cast<unsigned long long>(compiled_pairs),
                static_cast<unsigned long long>(interpreted_pairs),
                static_cast<unsigned long long>(observed_kicks),
                compiled_pairs * 1000.0 / compiled_ns,
                interpreted_pairs * 1000.0 / interpreted_ns,
                double(compiled_pairs) * interpreted_ns /
                    (double(interpreted_pairs) * compiled_ns));
    assert(checked >= 500 && vu_program_entries.size() >= 40 && observed_kicks > 0);
}
