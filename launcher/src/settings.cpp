#include "settings.h"

#include <charconv>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;

namespace launcher {
namespace {

std::string fmt(float v) {
    char buf[32];
    auto [end, ec] = std::to_chars(buf, buf + sizeof buf, v);
    return std::string(buf, end);
}

template <class T>
bool parse(const std::string& s, T& out) {
    T v{};
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || end != s.data() + s.size()) return false;
    out = v;
    return true;
}

bool parse_bool(const std::string& s, bool& out) {
    if (s == "1" || s == "true") return out = true, true;
    if (s == "0" || s == "false") return out = false, true;
    return false;
}

// One row per setting: key, read, write.
struct Field {
    const char* key;
    void (*load)(Settings&, const std::string&);
    std::string (*save)(const Settings&);
};

#define STR(f) {#f, [](Settings& s, const std::string& v) { s.f = v; }, [](const Settings& s) { return s.f; }}
#define BOOL(f) {#f, [](Settings& s, const std::string& v) { parse_bool(v, s.f); }, \
                 [](const Settings& s) { return std::string(s.f ? "1" : "0"); }}
#define NUM(f) {#f, [](Settings& s, const std::string& v) { parse(v, s.f); }, \
                [](const Settings& s) { return std::to_string(s.f); }}

const Field kFields[] = {
    STR(iso_path), BOOL(setup_done), BOOL(add_to_menu), BOOL(fullscreen), STR(resolution),
    NUM(upscale), BOOL(widescreen), STR(language), BOOL(software_renderer), NUM(sw_threads),
    BOOL(vsync), NUM(sharpen), BOOL(deflicker),
    {"mouse_sens", [](Settings& s, const std::string& v) { parse(v, s.mouse_sens); },
     [](const Settings& s) { return fmt(s.mouse_sens); }},
    BOOL(invert_mouse), BOOL(mouse_stick),
};

#undef STR
#undef BOOL
#undef NUM

}  // namespace

Settings load_settings(const fs::path& file) {
    Settings s;
    std::ifstream f(file);
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        for (const Field& field : kFields)
            if (key == field.key) field.load(s, value);
    }
    return s;
}

bool save_settings(const Settings& s, const fs::path& file) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path tmp = file;
    tmp += ".tmp" + std::to_string(getpid());
    {
        std::ofstream f(tmp, std::ios::trunc);
        for (const Field& field : kFields) f << field.key << '=' << field.save(s) << '\n';
        if (!f) return false;
    }
    fs::rename(tmp, file, ec);
    return !ec;
}

std::vector<std::string> to_args(const Settings& s) {
    std::vector<std::string> a;
    if (s.software_renderer) {
        a.insert(a.end(), {"--renderer", "sw"});
        if (s.sw_threads >= 0) a.insert(a.end(), {"--sw-threads", std::to_string(s.sw_threads)});
    }
    if (s.upscale > 0) a.insert(a.end(), {"--upscale", std::to_string(s.upscale)});
    if (!s.resolution.empty()) a.insert(a.end(), {"--resolution", s.resolution});
    if (s.fullscreen) a.push_back("--fullscreen");
    if (s.widescreen) a.push_back("--widescreen");
    if (s.vsync) a.push_back("--vsync");
    if (s.sharpen >= 0) a.insert(a.end(), {"--sharpen", std::to_string(s.sharpen)});
    if (s.deflicker) a.push_back("--deflicker");
    if (!s.language.empty() && s.language != "en") a.insert(a.end(), {"--lang", s.language});
    if (s.mouse_sens > 0.f) a.insert(a.end(), {"--mouse-sens", fmt(s.mouse_sens)});
    if (s.invert_mouse) a.push_back("--invert-mouse");
    if (s.mouse_stick) a.push_back("--mouse-stick");
    a.push_back(s.iso_path);
    return a;
}

}  // namespace launcher
