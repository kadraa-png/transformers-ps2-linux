#pragma once
// What the machine offers for the one-time build.
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace launcher {

inline constexpr uint64_t kBytesPerJob = 2300ull << 20;  // peak RAM of the heaviest compile job
inline constexpr uint64_t kMinFreeDisk = 3ull << 30;

bool cpu_has_avx2();
uint64_t parse_mem_available(std::string_view meminfo);  // bytes; 0 if absent
uint64_t mem_available_bytes();                          // from /proc/meminfo
uint64_t free_disk_bytes(const std::filesystem::path& dir);  // at dir's nearest existing ancestor
unsigned core_count();
// Total size of the regular files under dir (0 if it doesn't exist).
uint64_t dir_size(const std::filesystem::path& dir);
// Free space the build still needs when work_dir_bytes are already used by an
// interrupted build: kMinFreeDisk minus that, but at least 512 MB.
uint64_t disk_needed(uint64_t work_dir_bytes);
// clamp(min(cores, mem_available / kBytesPerJob), 1, cores)
unsigned build_jobs(unsigned cores, uint64_t mem_available);

}  // namespace launcher
