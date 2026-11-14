#include "sysinfo.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>
#include <thread>

namespace launcher {

bool cpu_has_avx2() {
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2");
}

uint64_t parse_mem_available(std::string_view meminfo) {
    constexpr std::string_view key = "MemAvailable:";
    size_t pos = meminfo.find(key);
    if (pos == std::string_view::npos) return 0;
    pos = meminfo.find_first_not_of(" \t", pos + key.size());
    if (pos == std::string_view::npos) return 0;
    uint64_t kb = 0;
    auto [ptr, ec] = std::from_chars(meminfo.data() + pos, meminfo.data() + meminfo.size(), kb);
    return ec == std::errc() ? kb * 1024 : 0;
}

uint64_t mem_available_bytes() {
    std::ifstream f("/proc/meminfo");
    std::stringstream ss;
    ss << f.rdbuf();
    return parse_mem_available(ss.str());
}

uint64_t free_disk_bytes(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::path p = dir;
    while (!p.empty() && !std::filesystem::exists(p, ec)) {
        if (p == p.parent_path()) break;
        p = p.parent_path();
    }
    auto info = std::filesystem::space(p.empty() ? "." : p, ec);
    return ec ? 0 : info.available;
}

uint64_t dir_size(const std::filesystem::path& dir) {
    uint64_t total = 0;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code fec;
        if (it->is_regular_file(fec) && !it->is_symlink(fec)) total += it->file_size(fec);
    }
    return total;
}

uint64_t disk_needed(uint64_t work_dir_bytes) {
    constexpr uint64_t floor = 512ull << 20;
    return work_dir_bytes + floor >= kMinFreeDisk ? floor : kMinFreeDisk - work_dir_bytes;
}

unsigned core_count() { return std::max(1u, std::thread::hardware_concurrency()); }

unsigned build_jobs(unsigned cores, uint64_t mem_available) {
    if (cores == 0) return 1;
    uint64_t by_mem = mem_available / kBytesPerJob;
    return unsigned(std::clamp<uint64_t>(std::min<uint64_t>(cores, by_mem), 1, cores));
}

}  // namespace launcher
