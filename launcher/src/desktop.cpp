#include "desktop.h"

#include <fstream>

namespace fs = std::filesystem;

namespace launcher {

std::string desktop_exec_quote(const std::string& path) {
    // Inside an Exec= quoted argument, " ` $ and \ take a backslash; the
    // entry's string escaping then doubles every backslash.
    std::string q = "\"";
    for (char c : path) {
        if (c == '"' || c == '`' || c == '$' || c == '\\') q += "\\\\";
        q += c == '\\' ? std::string("\\\\") : std::string(1, c);
    }
    return q + "\"";
}

bool install_desktop_entry(const fs::path& appimage, const fs::path& icon_png, const fs::path& data_home) {
    std::error_code ec;
    fs::path apps = data_home / "applications";
    fs::path icons = data_home / "icons" / "hicolor" / "256x256" / "apps";
    fs::create_directories(apps, ec);
    fs::create_directories(icons, ec);
    fs::copy_file(icon_png, icons / "transformers-port.png", fs::copy_options::overwrite_existing, ec);
    if (ec) return false;

    fs::path entry = apps / "transformers-port.desktop";
    fs::path tmp = entry;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        f << "[Desktop Entry]\n"
             "Type=Application\n"
             "Name=Transformers\n"
             "Comment=Transformers (PS2, 2004), native Linux port\n"
             "Exec=" << desktop_exec_quote(appimage.string()) << "\n"
             "Icon=transformers-port\n"
             "Terminal=false\n"
             "Categories=Game;ActionGame;\n";
        if (!f) return false;
    }
    fs::rename(tmp, entry, ec);
    return !ec;
}

}  // namespace launcher
