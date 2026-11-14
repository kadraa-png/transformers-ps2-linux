#pragma once
// Looks for disc images in the usual places of a home directory.
#include "iso_probe.h"

#include <filesystem>
#include <vector>

namespace launcher {

struct ScanHit {
    std::filesystem::path path;
    ProbeResult result;
};

// home, home/Downloads, home/Games, home/Documents: the ones that exist.
std::vector<std::filesystem::path> default_scan_roots(const std::filesystem::path& home);
// *.iso (any case) up to max_depth directories below each root; symlinks are
// not followed, unreadable directories are skipped, each image is listed once.
std::vector<ScanHit> scan_for_isos(const std::vector<std::filesystem::path>& roots, int max_depth = 2);

}  // namespace launcher
