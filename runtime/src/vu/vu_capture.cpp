#include "vu_capture.h"
#include "ps2/vu.h"
#include <cstdint>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <mutex>
#include <unordered_set>
#include <set>
#include <tuple>
u32 vu_port_vif_reg(unsigned unit, unsigned which);

namespace {
struct ImageKey {
    unsigned unit;
    std::uint64_t digest;
    bool operator==(const ImageKey&) const = default;
};
struct ImageHash {
    std::size_t operator()(ImageKey key) const {
        return std::hash<std::uint64_t>{}(key.digest ^ (std::uint64_t(key.unit) << 56));
    }
};
struct LaunchKey {
    ImageKey image;
    unsigned start;
    bool operator==(const LaunchKey&) const = default;
};
struct LaunchHash {
    std::size_t operator()(LaunchKey key) const {
        return ImageHash{}(key.image) ^ (std::size_t(key.start) << 1);
    }
};
struct Capture {
    std::filesystem::path dir;
    std::array<std::uint64_t, 2> generation{};
    std::array<std::uint64_t, 2> last_generation{~0ull, ~0ull};
    std::array<std::uint64_t, 2> digest{};
    std::unordered_set<ImageKey, ImageHash> images;
    std::unordered_set<LaunchKey, LaunchHash> launches;
    std::set<std::tuple<unsigned, std::uint64_t, std::uint64_t>> indirect_edges;   // unit, image, target<<32|source
    std::ofstream log, indirect;
    bool active = false;

    Capture() {
        const char* name = std::getenv("PS2_VU_CAPTURE");
        if (!name || !*name) return;
        dir = name;
        std::error_code error;
        std::filesystem::create_directories(dir, error);
        if (error) {
            std::fprintf(stderr, "VU capture: cannot create %s: %s\n", name,
                         error.message().c_str());
            return;
        }
        log.open(dir / "launches.tsv", std::ios::app);
        indirect.open(dir / "indirect.tsv", std::ios::app);
        active = log && indirect;
        if (!active) std::fprintf(stderr, "VU capture: cannot open launches.tsv/indirect.tsv in %s\n", name);
    }
};
Capture& capture() {
    static Capture state;
    return state;
}
std::uint64_t hash_image(const u8* data, std::size_t length) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (std::size_t index = 0; index < length; ++index) {
        hash ^= data[index];
        hash *= 0x100000001b3ull;
    }
    return hash;
}
std::string filename(const char* kind, unsigned unit, std::uint64_t hash,
                     unsigned start = ~0u) {
    char buffer[100];
    if (start == ~0u)
        std::snprintf(buffer, sizeof(buffer), "%s_vu%u_%016llx.bin", kind, unit,
                      static_cast<unsigned long long>(hash));
    else
        std::snprintf(buffer, sizeof(buffer), "%s_vu%u_%016llx_%04x.bin", kind,
                      unit, static_cast<unsigned long long>(hash), start);
    return buffer;
}
struct StateHeader {
    char magic[4]{'V', 'U', 'C', '2'};
    std::uint32_t unit;
    std::uint32_t start;
    std::uint32_t micro_bytes;
    std::uint32_t vu0_data_bytes;
    std::uint32_t vu1_data_bytes;
    std::uint64_t hash;
    std::uint32_t vif_top[2];
    std::uint32_t vif_itop[2];
};
} // namespace

// VU0 launches happen on the EE thread, VU1 launches maybe on the VU1 thread.
// (A VU1 capture then snapshots VU0 state the EE may be changing.)
static std::mutex capture_mutex;

void vu_capture_written(unsigned unit) {
    if (unit > 1) return;
    Capture& state = capture();
    if (!state.active) return;
    std::lock_guard lock(capture_mutex);
    ++state.generation[unit];
}

void vu_capture_launch(unsigned unit, unsigned start_pc) {
    if (unit > 1) return;
    Capture& state = capture();
    if (!state.active) return;
    std::lock_guard lock(capture_mutex);
    const VUState& vu = unit ? g_vu1 : g_vu0;
    if (state.last_generation[unit] != state.generation[unit]) {
        state.digest[unit] = hash_image(vu.micro, vu.micro_size);
        state.last_generation[unit] = state.generation[unit];
    }
    const ImageKey image{unit, state.digest[unit]};
    if (state.images.insert(image).second) {
        std::ofstream out(state.dir / filename("micro", unit, image.digest),
                          std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(vu.micro), vu.micro_size);
        if (!out) std::fprintf(stderr, "VU capture: failed to write micro image\n");
    }
    const LaunchKey launch{image, start_pc & (vu.micro_size - 1)};
    if (state.launches.insert(launch).second) {
        std::ofstream out(state.dir / filename("state", unit, image.digest,
                                               launch.start),
                          std::ios::binary | std::ios::trunc);
        StateHeader header{{'V', 'U', 'C', '2'}, unit, launch.start, vu.micro_size,
                           g_vu0.data_size, g_vu1.data_size, image.digest,
                           {vu_port_vif_reg(0, 1), vu_port_vif_reg(1, 1)},
                           {vu_port_vif_reg(0, 0), vu_port_vif_reg(1, 0)}};
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        out.write(reinterpret_cast<const char*>(&VU0), sizeof(VU0));
        out.write(reinterpret_cast<const char*>(&VU1), sizeof(VU1));
        out.write(reinterpret_cast<const char*>(g_vu0.data), g_vu0.data_size);
        out.write(reinterpret_cast<const char*>(g_vu1.data), g_vu1.data_size);
        if (!out) std::fprintf(stderr, "VU capture: failed to write launch state\n");
    }
    state.log << unit << '\t' << std::hex << image.digest << '\t' << launch.start
              << std::dec << '\n';
    state.log.flush();
}

void vu_capture_indirect(unsigned unit, unsigned target_pc, unsigned source_pc) {
    if (unit > 1) return;
    Capture& state = capture();
    if (!state.active) return;
    std::lock_guard lock(capture_mutex);
    const VUState& vu = unit ? g_vu1 : g_vu0;
    if (state.last_generation[unit] != state.generation[unit]) {
        state.digest[unit] = hash_image(vu.micro, vu.micro_size);
        state.last_generation[unit] = state.generation[unit];
    }
    const LaunchKey target{{unit, state.digest[unit]}, target_pc & (vu.micro_size - 1)};
    // Column 4: the JR/JALR pair, so each jump site gets only its own targets.
    const std::uint64_t edge = (std::uint64_t(target.start) << 32) | (source_pc & (vu.micro_size - 1));
    if (!state.indirect_edges.insert({unit, target.image.digest, edge}).second) return;
    state.indirect << unit << '\t' << std::hex << target.image.digest << '\t' << target.start
                   << '\t' << (source_pc & (vu.micro_size - 1)) << std::dec << '\n';
    state.indirect.flush();
}
