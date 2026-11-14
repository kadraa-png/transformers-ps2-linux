#pragma once
// "Add to app menu": a .desktop entry and icon under the XDG data home.
#include <filesystem>
#include <string>

namespace launcher {

// Writes data_home/applications/transformers-port.desktop (Exec = the
// AppImage) and data_home/icons/hicolor/256x256/apps/transformers-port.png.
// Overwrites an older entry, so it follows the AppImage when it moves.
bool install_desktop_entry(const std::filesystem::path& appimage, const std::filesystem::path& icon_png,
                           const std::filesystem::path& data_home);
// Quotes a path for an Exec= line (desktop entry spec).
std::string desktop_exec_quote(const std::string& path);

}  // namespace launcher
