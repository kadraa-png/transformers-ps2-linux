#pragma once
// Where the launcher keeps settings, builds and the built game (XDG dirs).
#include <filesystem>
#include <string>
#include <string_view>

namespace launcher {

std::filesystem::path config_dir();  // $XDG_CONFIG_HOME or ~/.config, + "/transformers-port"
std::filesystem::path data_dir();    // $XDG_DATA_HOME or ~/.local/share, + "/transformers-port"
// First 16 hash characters + "-" + version, with '/' and ' ' replaced by '_'.
std::string cache_key(std::string_view elf_sha256, std::string_view runtime_version);
std::filesystem::path game_dir(std::string_view key);   // data_dir() / "game-<key>"
std::filesystem::path build_dir(std::string_view key);  // data_dir() / "game-<key>.tmp"

}  // namespace launcher
