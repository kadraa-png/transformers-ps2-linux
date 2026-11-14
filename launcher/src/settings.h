#pragma once
// Player settings (settings.ini) and the game flags they turn into.
#include <filesystem>
#include <string>
#include <vector>

namespace launcher {

struct Settings {
    std::string iso_path;
    bool setup_done = false;
    bool add_to_menu = true;
    bool fullscreen = false;
    std::string resolution;          // "" = game default, else "WxH"
    int upscale = 0;                 // 0 = auto, 1..4
    bool widescreen = false;
    std::string language = "en";     // en fr de es it
    bool software_renderer = false;
    int sw_threads = -1;             // -1 = auto
    bool vsync = false;
    int sharpen = -1;                // -1 = game default, 0..100
    bool deflicker = false;
    float mouse_sens = 0.f;          // 0 = game default
    bool invert_mouse = false;
    bool mouse_stick = false;

    bool operator==(const Settings&) const = default;
};

// Missing file or unreadable keys give defaults.
Settings load_settings(const std::filesystem::path& file);
// Writes a temp file and renames it over `file`; creates the directory.
bool save_settings(const Settings& settings, const std::filesystem::path& file);
// Game flags for non-default settings, then the ISO path.
std::vector<std::string> to_args(const Settings& settings);

}  // namespace launcher
