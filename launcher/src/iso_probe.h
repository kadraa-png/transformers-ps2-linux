#pragma once
// Identifies a disc image: is it the Transformers (USA) disc this port needs?
#include <filesystem>
#include <string>
#include <string_view>

namespace launcher {

inline constexpr std::string_view kExpectedBoot = "SLUS_206.68";
inline constexpr std::string_view kExpectedSha256 =
    "ab88780324b9492eca59016923d200dd1d95785fd725dd85f233c071805b4acb";

enum class ProbeStatus { Ok, NotIso, NoBootFile, WrongDisc, BadChecksum, ReadError };

struct ProbeResult {
    ProbeStatus status = ProbeStatus::ReadError;
    std::string boot;        // boot file named by SYSTEM.CNF, e.g. "SLUS_206.68" ("" if unknown)
    std::string elf_sha256;  // hash of the boot file ("" if not read)
};

ProbeResult probe_iso(const std::filesystem::path& iso, std::string_view expected_sha256 = kExpectedSha256);
// "SLUS_206.68" -> "SLUS-20668"
std::string display_serial(std::string_view boot);
// One plain sentence for the player.
std::string probe_message(const ProbeResult& result);

}  // namespace launcher
