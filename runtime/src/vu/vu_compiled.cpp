#include "vu_compiled.h"
#include "vu_capture.h"
#include "ps2/vu.h"
#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace {
// Micro memory is pair-addressed; -1 means no exact game-word match.
std::array<std::array<int, 2048>, 2> mapped{};
std::unordered_map<u64, u32> words;
std::once_flag index_once;
std::unique_ptr<std::array<u8, 2>[]> prepared;
struct CachedPair {
    const VUCompiledPair* pair = nullptr;
    u64 generation = 0;
};
std::array<std::array<CachedPair, 2048>, 2> pair_cache{};
std::array<u64, 2> micro_generation{1, 1};

void invalidate_cache(unsigned unit) {
    if (++micro_generation[unit] != 0) return;
    for (auto& entry : pair_cache[unit]) entry.generation = 0;
    micro_generation[unit] = 1;
}

u64 key(const u8* data) {
    u64 value;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

void make_index() {
    for (auto& unit : mapped) unit.fill(-1);
    prepared = std::make_unique<std::array<u8, 2>[]>(g_vu_compiled_count);
    words.reserve(g_vu_compiled_count);
    for (u32 n = 0; n < g_vu_compiled_count; ++n) {
        const auto& p = g_vu_compiled_pairs[n];
        words.try_emplace(u64(p.lower_word) | (u64(p.upper_word) << 32), n);
    }
}

void prepare_pair(u32 n, unsigned unit) {
    if (prepared[n][unit]) return;
    auto& p = g_vu_compiled_pairs[n];
    VURegs& vu = vuRegs[unit];
    const u32 saved = vu.code;
    vu.code = p.upper_word;
    (unit ? VU1regs_UPPER_OPCODE : VU0regs_UPPER_OPCODE)[vu.code & 63](&p.upper_regs[unit]);
    if (!(p.upper_word & 0x80000000u)) {
        vu.code = p.lower_word;
        (unit ? VU1regs_LOWER_OPCODE : VU0regs_LOWER_OPCODE)[vu.code >> 25](&p.lower_regs[unit]);
    }
    vu.code = saved;
    prepared[n][unit] = 1;
}
}

void vu_compiled_prepare() { std::call_once(index_once, make_index); }

void vu_compiled_reset(int unit) {
    if (unit < 0 || unit > 1) return;
    vu_compiled_prepare();
    mapped[unit].fill(-1);
    invalidate_cache(static_cast<unsigned>(unit));
}

void vu_micro_written(int unit, u32 offset, u32 bytes) {
    if (unit < 0 || unit > 1 || !bytes) return;
    vu_capture_written(static_cast<unsigned>(unit));
    invalidate_cache(static_cast<unsigned>(unit));
    vu_compiled_prepare();
    const VUState& state = unit ? g_vu1 : g_vu0;
    const u32 size = state.micro_size;
    offset %= size;
    if (bytes >= size) { offset = 0; bytes = size; }
    // Re-evaluate whole affected pairs: split VIF transfers may end on a
    // half-pair, and the other half may still be the previous upload.
    const u32 first = offset / 8;
    const u32 count = (offset % 8 + bytes + 7) / 8;
    for (u32 i = 0; i < count; ++i) {
        const u32 pair = (first + i) % (size / 8);
        const u64 value = key(state.micro + pair * 8);
        const int previous = mapped[unit][(pair + size / 8 - 1) % (size / 8)];
        int match = -1;
        // Identical NOP pairs occur throughout the ELF. Preserve the
        // surrounding upload's ELF provenance when the next pair agrees;
        // the word itself is still checked byte-for-byte before mapping.
        if (previous >= 0 && static_cast<u32>(previous + 1) < g_vu_compiled_count) {
            const auto& next = g_vu_compiled_pairs[previous + 1];
            if (value == (u64(next.lower_word) | (u64(next.upper_word) << 32)))
                match = previous + 1;
        }
        if (match < 0) {
            const auto found = words.find(value);
            if (found != words.end()) match = static_cast<int>(found->second);
        }
        mapped[unit][pair] = match;
    }
}

const int* vu_compiled_map(unsigned unit) {
    return unit < 2 ? mapped[unit].data() : nullptr;
}

u64 vu_compiled_generation(unsigned unit) { return unit < 2 ? micro_generation[unit] : 0; }

const VUCompiledPair* vu_compiled_at(unsigned unit, u32 pc) {
    if (unit > 1) return nullptr;
    if (!prepared) vu_compiled_prepare();
    const VUState& state = unit ? g_vu1 : g_vu0;
    pc &= state.micro_size - 1;
    if (pc & 7) return nullptr;
    auto& cached = pair_cache[unit][pc / 8];
    if (cached.generation == micro_generation[unit]) return cached.pair;
    const int n = mapped[unit][pc / 8];
    const VUCompiledPair* pair = nullptr;
    if (n >= 0) {
        pair = &g_vu_compiled_pairs[n];
        prepare_pair(static_cast<u32>(n), unit);
    }
    cached.pair = pair;
    cached.generation = micro_generation[unit];
    return pair;
}
