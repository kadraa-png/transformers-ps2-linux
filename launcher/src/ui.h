#pragma once
// The launcher window: the wizard screens drawn with Dear ImGui over the art.
#include "builder.h"
#include "wizard.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace launcher {

struct AppContext {
    std::filesystem::path runtime_dir;     // .../share/transformers-port
    std::string runtime_version;
    BuildTools tools;
    std::filesystem::path appdir;          // $APPDIR when run from an AppImage
    std::vector<std::string> game_args;    // passed through to the game
    std::optional<std::filesystem::path> appimage;  // $APPIMAGE when run from one
};

int run_gui(const AppContext& ctx);
// Debug/verification: draw one screen with sample data into a PNG.
int render_shot(const AppContext& ctx, Screen screen, const std::filesystem::path& png);
std::optional<Screen> screen_from_name(const std::string& name);

}  // namespace launcher
