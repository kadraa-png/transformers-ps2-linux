#include "paths.h"

#include <cstdlib>

namespace fs = std::filesystem;

namespace launcher {
namespace {

fs::path xdg(const char* var, const char* fallback) {
    const char* v = std::getenv(var);
    if (v && *v) return fs::path(v) / "transformers-port";
    const char* home = std::getenv("HOME");
    return fs::path(home && *home ? home : ".") / fallback / "transformers-port";
}

}  // namespace

fs::path config_dir() { return xdg("XDG_CONFIG_HOME", ".config"); }
fs::path data_dir() { return xdg("XDG_DATA_HOME", ".local/share"); }

std::string cache_key(std::string_view elf_sha256, std::string_view runtime_version) {
    std::string key(elf_sha256.substr(0, 16));
    key += '-';
    for (char c : runtime_version) key += (c == '/' || c == ' ') ? '_' : c;
    return key;
}

fs::path game_dir(std::string_view key) { return data_dir() / ("game-" + std::string(key)); }
fs::path build_dir(std::string_view key) { return data_dir() / ("game-" + std::string(key) + ".tmp"); }

}  // namespace launcher
