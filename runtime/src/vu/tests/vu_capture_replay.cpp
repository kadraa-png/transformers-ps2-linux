#include "ps2/vu.h"
#include "ps2/gs.h"
#include "ps2/intc.h"
#include "VU.h"
#include "vu_compiled.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <memory>
#include <unistd.h>
#include <cstdlib>

namespace {
struct StateHeader {
    char magic[4];
    u32 unit, start, micro_bytes, vu0_data_bytes, vu1_data_bytes;
    u64 hash;
    u32 vif_top[2], vif_itop[2];
};
static_assert(sizeof(StateHeader) == 48);
// Every XGKICK is folded into a running 64-bit hash of its position and the
// whole VU1 data memory it reads, so launches with ~10^5 packets compare
// without writing gigabytes of captures.
bool hashing_kicks = false;
u64 emitted_kicks = 0;
u64 kick_hash = 0;
void hash_word(u64 word) {
    kick_hash = (kick_hash ^ word) * 0x9e3779b97f4a7c15ull;
    kick_hash ^= kick_hash >> 29;
}
struct Snapshot {
    StateHeader header{};
    VURegs vu0{}, vu1{};
    std::array<u8, 4096> data0{};
    std::array<u8, 16384> data1{}, micro{};
};
struct Result {
    VURegs vu0, vu1;
    std::array<u8, 4096> data0;
    std::array<u8, 16384> data1;
    u64 kicks;
    u64 kick_count;
    u32 pairs;
    u64 elapsed_ns;
};
// Disabled FMAC slots keep stale flag payloads that are never read again
// (a slot is only published while enabled).  Graphs may leave different
// stale values there when liveness proved an operation's flags unobserved.
bool same_vu(const VURegs& first, const VURegs& second) {
    // PS2_REPLAY_IGNORE_MS=1: whole-image graphs skip MAC/status flags in
    // images without a flag reader; compare everything else exactly.
    static const bool ignore_ms = std::getenv("PS2_REPLAY_IGNORE_MS") != nullptr;
    VURegs a = first, b = second;
    for (VURegs* vu : {&a, &b}) {
        for (auto& pipe : vu->fmac)
            if (!pipe.enable) std::memset(&pipe, 0, sizeof(pipe));
            else if (ignore_ms) pipe.macflag = pipe.statusflag = 0;
        if (!vu->fdiv.enable) std::memset(&vu->fdiv, 0, sizeof(vu->fdiv));
        if (!vu->efu.enable) std::memset(&vu->efu, 0, sizeof(vu->efu));
        if (ignore_ms) {
            vu->VI[REG_MAC_FLAG].UL = vu->VI[REG_STATUS_FLAG].UL = 0;
            vu->macflag = vu->statusflag = 0;
            vu->fdiv.statusflag = 0;
        }
    }
    return std::memcmp(&a, &b, sizeof(VURegs)) == 0;
}
void report_difference(const char* what, const Result& a, const Result& b, unsigned unit) {
    const VURegs& x = unit ? a.vu1 : a.vu0;
    const VURegs& y = unit ? b.vu1 : b.vu0;
    std::fprintf(stderr, "  %s: pairs %u/%u kicks %llu/%llu hash %s\n", what, a.pairs, b.pairs,
                 (unsigned long long)a.kick_count, (unsigned long long)b.kick_count,
                 a.kicks == b.kicks ? "same" : "DIFFERENT");
    for (int i = 0; i < 32; ++i)
        if (x.VI[i].UL != y.VI[i].UL)
            std::fprintf(stderr, "  VI[%d] %08x / %08x\n", i, x.VI[i].UL, y.VI[i].UL);
    for (int i = 0; i < 32; ++i)
        if (std::memcmp(&x.VF[i], &y.VF[i], 16)) std::fprintf(stderr, "  VF[%d] differs\n", i);
    if (x.macflag != y.macflag || x.statusflag != y.statusflag || x.clipflag != y.clipflag)
        std::fprintf(stderr, "  working mac %x/%x status %x/%x clip %x/%x\n", x.macflag, y.macflag,
                     x.statusflag, y.statusflag, x.clipflag, y.clipflag);
    for (int i = 0; i < 8; ++i)
        if (std::memcmp(&x.fmac[i], &y.fmac[i], sizeof(x.fmac[i])))
            std::fprintf(stderr, "  fmac[%d] en %d/%d mac %x/%x st %x/%x\n", i, x.fmac[i].enable,
                         y.fmac[i].enable, x.fmac[i].macflag, y.fmac[i].macflag,
                         x.fmac[i].statusflag, y.fmac[i].statusflag);
    if (std::memcmp(&x.fdiv, &y.fdiv, sizeof(x.fdiv))) std::fprintf(stderr, "  fdiv differs\n");
    if (x.cycle != y.cycle) std::fprintf(stderr, "  cycle %u/%u\n", x.cycle, y.cycle);
    if (a.data0 != b.data0) std::fprintf(stderr, "  VU0 data memory differs\n");
    if (a.data1 != b.data1) std::fprintf(stderr, "  VU1 data memory differs\n");
}
Snapshot load(const std::filesystem::path& state_file) {
    Snapshot snapshot;
    std::ifstream in(state_file, std::ios::binary);
    in.read(reinterpret_cast<char*>(&snapshot.header), sizeof(snapshot.header));
    if (std::memcmp(snapshot.header.magic, "VUC2", 4) || snapshot.header.unit > 1 ||
        snapshot.header.micro_bytes != (snapshot.header.unit ? 16384u : 4096u) ||
        snapshot.header.vu0_data_bytes != snapshot.data0.size() ||
        snapshot.header.vu1_data_bytes != snapshot.data1.size())
        throw std::runtime_error("invalid VUC2 state: " + state_file.string());
    in.read(reinterpret_cast<char*>(&snapshot.vu0), sizeof(snapshot.vu0));
    in.read(reinterpret_cast<char*>(&snapshot.vu1), sizeof(snapshot.vu1));
    in.read(reinterpret_cast<char*>(snapshot.data0.data()), snapshot.data0.size());
    in.read(reinterpret_cast<char*>(snapshot.data1.data()), snapshot.data1.size());
    if (!in) throw std::runtime_error("truncated VUC2 state: " + state_file.string());
    char image_name[64];
    std::snprintf(image_name, sizeof(image_name), "micro_vu%u_%016llx.bin", snapshot.header.unit,
                  static_cast<unsigned long long>(snapshot.header.hash));
    std::ifstream image(state_file.parent_path() / image_name, std::ios::binary);
    image.read(reinterpret_cast<char*>(snapshot.micro.data()), snapshot.header.micro_bytes);
    if (!image) throw std::runtime_error("missing micro image for " + state_file.string());
    return snapshot;
}
Result execute(const Snapshot& source, bool compiled, u32 repetitions = 1,
               bool record_kicks = true) {
    vu_init();
    vif_init();
    const unsigned unit = source.header.unit;
    const u32 running = unit ? 0x100u : 0x1u;
    std::memcpy(unit ? g_vu1.micro : g_vu0.micro, source.micro.data(), source.header.micro_bytes);
    vu_micro_written(static_cast<int>(unit), 0, source.header.micro_bytes);
    VU0 = source.vu0;
    VU1 = source.vu1;
    VU0.Mem = g_vu0.data;
    VU0.Micro = g_vu0.micro;
    VU1.Mem = g_vu1.data;
    VU1.Micro = g_vu1.micro;
    std::memcpy(g_vu0.data, source.data0.data(), source.data0.size());
    std::memcpy(g_vu1.data, source.data1.data(), source.data1.size());
    vu_set_vif_position(0, source.header.vif_top[0], source.header.vif_itop[0]);
    vu_set_vif_position(1, source.header.vif_top[1], source.header.vif_itop[1]);
    hashing_kicks = record_kicks;
    emitted_kicks = 0;
    kick_hash = 0xcbf29ce484222325ull;
    u32 pairs = 0;
    const auto begin = std::chrono::steady_clock::now();
    for (u32 n = 0; n < repetitions; ++n) {
        VU0.VI[REG_VPU_STAT].UL |= running;
        pairs += compiled ? vu_port_run_compiled(unit, source.header.start, 10'000'000)
                          : vu_port_run_micro(unit, source.header.start, 10'000'000);
        if (VU0.VI[REG_VPU_STAT].UL & running)
            throw std::runtime_error("VU did not stop at its E-bit");
    }
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    hashing_kicks = false;
    Result result{VU0, VU1, {}, {}, kick_hash, emitted_kicks, pairs,
                  static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count())};
    std::memcpy(result.data0.data(), g_vu0.data, result.data0.size());
    std::memcpy(result.data1.data(), g_vu1.data, result.data1.size());
    return result;
}
} // namespace
namespace gs {
void gif_path1(const u8* data, u32 address) {
    ++emitted_kicks;
    if (!hashing_kicks) return;
    hash_word(address & 0xffff);
    for (u32 offset = 0; offset < 16384; offset += 8) {
        u64 word;
        std::memcpy(&word, data + offset, sizeof(word));
        hash_word(word);
    }
}
void gif_path2(const u8*, u32) {}
void read_fifo(u8* data, u32 qwc) { std::memset(data, 0, qwc * 16); }
bool set_path1_async(bool) { return false; }   // no GS thread: VU1 stays synchronous
void path1_program() {}
void path1_program_end() {}
void flush() {}
void request_dump(const char*, int) {}   // F-key GS dump hook in platform_sdl
} // namespace gs
void intc_raise(u32) {}

void profiler_start();
int main(int argc, char** argv) {
    profiler_start();   // PS2_PROFILE=1 (see core/profile.cpp)
    if (argc != 2) {
        std::fprintf(stderr, "usage: vu_capture_replay CAPTURE_DIRECTORY\n");
        return 2;
    }
    try {
        unsigned checked = 0;
        u64 total_pairs = 0, compiled_ns = 0, interpreted_ns = 0;
        for (const auto& file : std::filesystem::directory_iterator(argv[1])) {
            const auto name = file.path().filename().string();
            if (!(name.starts_with("state_vu1_") || name.starts_with("state_vu0_")) ||
                file.path().extension() != ".bin") continue;
            const Snapshot source = load(file.path());
            // Benchmark knobs: PS2_REPLAY_REPS=N fixes the clean-launch count;
            // PS2_REPLAY_NOCHECK=1 times deliberately inexact builds.
            static const char* const reps_env = std::getenv("PS2_REPLAY_REPS");
            static const bool nocheck = std::getenv("PS2_REPLAY_NOCHECK") != nullptr;
            const Result reference = execute(source, false);
            const Result candidate = execute(source, true);
            if (!nocheck && (reference.pairs != candidate.pairs ||
                !same_vu(reference.vu0, candidate.vu0) ||
                !same_vu(reference.vu1, candidate.vu1) ||
                reference.data0 != candidate.data0 || reference.data1 != candidate.data1 ||
                reference.kick_count != candidate.kick_count ||
                reference.kicks != candidate.kicks)) {
                std::fprintf(stderr, "VU replay mismatch: %s (%u/%u pairs)\n",
                             name.c_str(), candidate.pairs, reference.pairs);
                report_difference("interpreter/compiled", reference, candidate, source.header.unit);
                return 1;
            }
            if (reference.pairs == 10'000'000 ||
                (reference.vu0.VI[REG_VPU_STAT].UL & (source.header.unit ? 0x100u : 0x1u))) {
                std::fprintf(stderr, "VU replay did not reach E-bit: %s\n", name.c_str());
                return 1;
            }
            const u32 repetitions = reps_env ? static_cast<u32>(std::atoi(reps_env)) :
                std::clamp<u32>(10'000'000 / reference.pairs, reference.kick_count ? 2u : 100u,
                                reference.kick_count ? 8u : 100'000u);
            // PS2_REPLAY_COMPILED_ONLY=1 (profiling): time only the compiled run.
            static const bool compiled_only = std::getenv("PS2_REPLAY_COMPILED_ONLY") != nullptr;
            const Result steady_compiled = execute(source, true, repetitions, false);
            const Result steady_interp = compiled_only ? steady_compiled
                                                       : execute(source, false, repetitions, false);
            if (!nocheck && (steady_interp.pairs != steady_compiled.pairs ||
                !same_vu(steady_interp.vu0, steady_compiled.vu0) ||
                !same_vu(steady_interp.vu1, steady_compiled.vu1) ||
                steady_interp.data0 != steady_compiled.data0 ||
                steady_interp.data1 != steady_compiled.data1 ||
                steady_interp.kick_count != steady_compiled.kick_count ||
                steady_interp.kicks != steady_compiled.kicks)) {
                std::fprintf(stderr, "VU repeated-launch mismatch: %s\n", name.c_str());
                return 1;
            }
            ++checked;
            total_pairs += steady_interp.pairs;
            compiled_ns += steady_compiled.elapsed_ns;
            interpreted_ns += steady_interp.elapsed_ns;
            std::printf("%s: %u pairs, %llu GIF packets, %.2fx single (including GIF capture); "
                        "%.2f/%.2f Mpairs/s compiled/interpreted, %.2fx across %u clean launches\n",
                        name.c_str(), reference.pairs,
                        static_cast<unsigned long long>(reference.kick_count),
                        double(reference.elapsed_ns) / candidate.elapsed_ns,
                        steady_compiled.pairs * 1000.0 / steady_compiled.elapsed_ns,
                        steady_interp.pairs * 1000.0 / steady_interp.elapsed_ns,
                        double(steady_interp.elapsed_ns) / steady_compiled.elapsed_ns,
                        repetitions);
        }
        if (!checked) {
            std::fprintf(stderr, "No VU captured launches in %s\n", argv[1]);
            return 2;
        }
        std::printf("VU capture replay: %u full E-bit launches, %llu pairs, %.2fx speedup\n",
                    checked, static_cast<unsigned long long>(total_pairs),
                    double(interpreted_ns) / compiled_ns);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "VU replay: %s\n", error.what());
        return 2;
    }
}
