#include "iso_scan.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace fs = std::filesystem;

namespace launcher {

std::vector<fs::path> default_scan_roots(const fs::path& home) {
    std::vector<fs::path> roots;
    std::error_code ec;
    for (const char* sub : {"", "Downloads", "Games", "Documents"}) {
        fs::path p = *sub ? home / sub : home;
        if (fs::is_directory(p, ec)) roots.push_back(p);
    }
    return roots;
}

namespace {

bool is_iso_name(const fs::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext == ".iso";
}

}  // namespace

std::vector<ScanHit> scan_for_isos(const std::vector<fs::path>& roots, int max_depth) {
    std::vector<ScanHit> hits;
    std::set<fs::path> seen;
    for (const fs::path& root : roots) {
        std::error_code ec;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            const fs::directory_entry& e = *it;
            std::error_code fec;
            if (e.is_symlink(fec)) {
                if (e.is_directory(fec)) it.disable_recursion_pending();
                continue;
            }
            if (e.is_directory(fec)) {
                if (it.depth() >= max_depth) it.disable_recursion_pending();
                continue;
            }
            if (!e.is_regular_file(fec) || !is_iso_name(e.path())) continue;
            fs::path canon = fs::weakly_canonical(e.path(), fec);
            if (!seen.insert(fec ? e.path() : canon).second) continue;
            hits.push_back({e.path(), probe_iso(e.path())});
        }
    }
    return hits;
}

}  // namespace launcher
