#include "ui.h"

#include "desktop.h"
#include "iso_probe.h"
#include "iso_scan.h"
#include "paths.h"
#include "process.h"
#include "settings.h"
#include "sysinfo.h"

#include <SDL3/SDL.h>
#include <png.h>

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include "imgui.h"

#include <cstdlib>
#include <deque>
#include <future>
#include <mutex>

namespace fs = std::filesystem;

namespace launcher {
namespace {

const char* const kControlsMd =
#include "controls_md.inc"
    ;

constexpr int kWidth = 960, kHeight = 600;
const ImVec4 kAccent(0.184f, 0.420f, 1.0f, 1.0f);  // #2f6bff
const ImVec4 kStep(0.561f, 0.706f, 1.0f, 1.0f);    // #8fb4ff
const ImVec4 kMuted(0.725f, 0.776f, 0.863f, 1.0f); // #b9c6dc
const ImVec4 kGood(0.608f, 0.890f, 0.659f, 1.0f);  // #9be3a8
const ImVec4 kBad(1.0f, 0.55f, 0.50f, 1.0f);

// ---- images --------------------------------------------------------------------------

struct Image {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;
};

Image load_png(SDL_Renderer* r, const fs::path& file) {
    png_image img{};
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&img, file.c_str())) return {};
    img.format = PNG_FORMAT_RGBA;
    std::vector<unsigned char> px(PNG_IMAGE_SIZE(img));
    if (!png_image_finish_read(&img, nullptr, px.data(), 0, nullptr)) return {};
    Image out{SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, int(img.width), int(img.height)),
              int(img.width), int(img.height)};
    if (!out.tex) return {};
    SDL_UpdateTexture(out.tex, nullptr, px.data(), int(img.width * 4));
    SDL_SetTextureBlendMode(out.tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(out.tex, SDL_SCALEMODE_LINEAR);
    return out;
}

bool save_png(SDL_Renderer* r, const fs::path& file) {
    SDL_Surface* shot = SDL_RenderReadPixels(r, nullptr);
    if (!shot) return false;
    SDL_Surface* rgba = SDL_ConvertSurface(shot, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(shot);
    if (!rgba) return false;
    png_image img{};
    img.version = PNG_IMAGE_VERSION;
    img.width = png_uint_32(rgba->w);
    img.height = png_uint_32(rgba->h);
    img.format = PNG_FORMAT_RGBA;
    bool ok = png_image_write_to_file(&img, file.c_str(), 0, rgba->pixels, rgba->pitch, nullptr);
    SDL_DestroySurface(rgba);
    return ok;
}

ImTextureID tex_id(const Image& i) { return ImTextureID(reinterpret_cast<intptr_t>(i.tex)); }

std::string gib(uint64_t bytes) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f GB", double(bytes) / double(1ull << 30));
    return buf;
}

// ---- the app --------------------------------------------------------------------------

class App {
  public:
    App(const AppContext& ctx, SDL_Window* window, SDL_Renderer* renderer)
        : ctx_(ctx), window_(window), renderer_(renderer) {
        settings_path_ = config_dir() / "settings.ini";
        settings_ = load_settings(settings_path_);
        fs::path art = ctx_.runtime_dir / "art";
        bg_ = load_png(renderer_, art / "launcher_bg.png");
        logo_ = load_png(renderer_, art / "logo_alpha.png");
        refresh_disc();
        Start s = initial_screen(launch_state());
        screen_ = s.screen;
        resume_ = s.resume;
        if (screen_ == Screen::Disc) start_scan();
        if (ctx_.appimage && settings_.add_to_menu && settings_.setup_done)
            install_desktop_entry(*ctx_.appimage, art / "icon.png", data_home());
    }

    bool overflow_ = false;  // set by end_body() when a screen's content was clipped

    // Sample state for --shot.
    void demo(Screen s) {
        screen_ = s;
        demo_ = true;
        probe_ = ProbeResult{ProbeStatus::Ok, std::string(kExpectedBoot), std::string(kExpectedSha256)};
        settings_.iso_path = "/home/player/Games/Transformers (USA).iso";
        scan_done_ = true;
        hits_ = {{settings_.iso_path, probe_}};
        error_title_ = "The one-time build failed.";
        error_dir_ = "/tmp";
        error_lines_ = {"[212/346] Building CXX object CMakeFiles/ee_generated.dir/generated/ee_120.cpp.o",
                        "FAILED: CMakeFiles/ee_generated.dir/generated/ee_120.cpp.o",
                        "c++: fatal error: Killed signal terminated program cc1plus",
                        "ninja: build stopped: subcommand failed."};
    }

    void handle_event(const SDL_Event& e) {
        if (e.type == SDL_EVENT_DROP_FILE && e.drop.data && screen_ == Screen::Disc) choose_iso(e.drop.data);
    }

    // Returns false when the window should close.
    bool frame() {
        poll_background();
        if (playing_) return true;
        draw_background();
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGui::Begin("##main", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        switch (screen_) {
        case Screen::Disc: draw_disc(); break;
        case Screen::Check: draw_check(); break;
        case Screen::Building: draw_building(); break;
        case Screen::QuickSetup: draw_quick_setup(); break;
        case Screen::Home: draw_home(); break;
        case Screen::Settings: draw_settings(); break;
        case Screen::Controls: draw_controls(); break;
        case Screen::Error: draw_error(); break;
        }
        ImGui::End();
        return !quit_;
    }

    bool playing() const { return playing_; }

  private:
    // ---- state ----

    fs::path data_home() const {
        const char* x = std::getenv("XDG_DATA_HOME");
        if (x && *x) return x;
        const char* h = std::getenv("HOME");
        return fs::path(h ? h : ".") / ".local" / "share";
    }

    void refresh_disc() {
        std::error_code ec;
        if (!settings_.iso_path.empty() && fs::exists(settings_.iso_path, ec)) {
            probe_ = probe_iso(settings_.iso_path);
            key_ = cache_key(probe_.elf_sha256, ctx_.runtime_version);
        }
    }

    LaunchState launch_state() const {
        std::error_code ec;
        LaunchState s;
        s.iso_set = !settings_.iso_path.empty();
        s.iso_exists = s.iso_set && fs::exists(settings_.iso_path, ec) && probe_.status == ProbeStatus::Ok;
        s.game_built = !key_.empty() && fs::exists(game_dir(key_) / "transformers", ec);
        s.build_in_progress = !key_.empty() && fs::exists(build_dir(key_), ec);
        s.setup_done = settings_.setup_done;
        return s;
    }

    void go(Event e) {
        Screen next = next_screen(screen_, e, launch_state());
        if (next == Screen::Disc && screen_ != Screen::Disc) start_scan();
        if (next == Screen::Check) {
            resume_ = launch_state().build_in_progress;
            sys_checked_ = false;
        }
        screen_ = next;
    }

    void save() { save_settings(settings_, settings_path_); }

    void start_scan() {
        if (scan_.valid()) return;
        scan_done_ = false;
        const char* home = std::getenv("HOME");
        fs::path h = home ? home : ".";
        scan_ = std::async(std::launch::async, [h] { return scan_for_isos(default_scan_roots(h)); });
    }

    void choose_iso(const std::string& path) {
        ProbeResult r = probe_iso(path);
        if (r.status != ProbeStatus::Ok) {
            disc_message_ = fs::path(path).filename().string() + ": " + probe_message(r);
            go(Event::IsoRejected);
            return;
        }
        disc_message_.clear();
        settings_.iso_path = path;
        save();
        probe_ = r;
        key_ = cache_key(probe_.elf_sha256, ctx_.runtime_version);
        go(Event::IsoAccepted);
    }

    static void SDLCALL on_dialog(void* self, const char* const* files, int) {
        if (!files || !files[0]) return;
        App* app = static_cast<App*>(self);
        std::lock_guard lock(app->dialog_mu_);
        app->dialog_pick_ = files[0];
    }

    void poll_background() {
        if (scan_.valid() && scan_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto found = scan_.get();
            if (!demo_) hits_ = std::move(found);
            scan_done_ = true;
        }
        std::string pick;
        {
            std::lock_guard lock(dialog_mu_);
            pick.swap(dialog_pick_);
        }
        if (!pick.empty()) choose_iso(pick);

        if (screen_ == Screen::Building && !demo_) {
            Builder::State s = builder_.state();
            if (s == Builder::State::Succeeded) {
                builder_.join();
                go(Event::BuildSucceeded);
            } else if (s == Builder::State::Cancelled) {
                builder_.join();
                go(Event::BuildCancelled);
            } else if (s == Builder::State::Failed) {
                builder_.join();
                error_title_ = "The one-time build failed.";
                error_lines_ = builder_.log_tail(30);
                error_dir_ = builder_.log_path().parent_path();
                go(Event::BuildFailed);
            }
        }

        if (playing_) {
            std::vector<std::string> lines;
            bool open = game_.read_lines(lines);
            for (auto& l : lines) {
                game_tail_.push_back(l);
                if (game_tail_.size() > 40) game_tail_.pop_front();
            }
            if (!open && !game_.running()) {
                int code = game_.wait();
                playing_ = false;
                SDL_ShowWindow(window_);
                SDL_RaiseWindow(window_);
                if (code != 0) {
                    error_title_ = "The game stopped with an error (exit code " + std::to_string(code) + ").";
                    error_lines_.assign(game_tail_.begin(), game_tail_.end());
                    error_dir_ = game_dir(key_);
                    go(Event::GameCrashed);
                }
            }
        }
    }

    // ---- drawing helpers ----

    void draw_background() {
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        ImVec2 size = ImGui::GetIO().DisplaySize;
        dl->AddRectFilled({0, 0}, size, IM_COL32(10, 20, 48, 255));
        if (bg_.tex) {
            // Cover: crop the image to the window's aspect, anchored 20% from the top.
            float wa = size.x / size.y, ia = float(bg_.w) / float(bg_.h);
            ImVec2 uv0(0, 0), uv1(1, 1);
            if (ia > wa) {
                float f = wa / ia;
                uv0.x = (1 - f) / 2;
                uv1.x = uv0.x + f;
            } else {
                float f = ia / wa;
                uv0.y = (1 - f) * 0.2f;
                uv1.y = uv0.y + f;
            }
            dl->AddImage(tex_id(bg_), {0, 0}, size, uv0, uv1);
        }
        float mid = size.y * 0.50f;
        dl->AddRectFilledMultiColor({0, 0}, {size.x, mid}, IM_COL32(4, 8, 20, 40), IM_COL32(4, 8, 20, 40),
                                    IM_COL32(4, 8, 20, 237), IM_COL32(4, 8, 20, 237));
        dl->AddRectFilled({0, mid}, size, IM_COL32(4, 8, 20, 237));
        if (logo_.tex) {
            float w = size.x * 0.42f, h = w * float(logo_.h) / float(logo_.w);
            dl->AddImage(tex_id(logo_), {size.x * 0.06f, size.y * 0.06f}, {size.x * 0.06f + w, size.y * 0.06f + h});
        }
    }

    // The content block: the lower part of the window, inside the side margins.
    void body(float height) {
        ImVec2 size = ImGui::GetIO().DisplaySize;
        float top = std::max(size.y * 0.30f, size.y - height - size.y * 0.06f);
        ImGui::SetCursorPos({size.x * 0.06f, top});
        ImGui::BeginChild("body", {size.x * 0.88f, size.y - top - size.y * 0.04f}, ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
        ImGui::PushTextWrapPos(0.0f);
    }
    void end_body() {
        // Content taller than the body is clipped (a button can vanish); --shot reports it.
        if (ImGui::GetCursorPosY() > ImGui::GetWindowHeight()) overflow_ = true;
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
    }

    void step(const char* text) { ImGui::TextColored(kStep, "%s", text); }

    void title(const char* text) {
        ImGui::PushFont(big_font_);
        ImGui::TextUnformatted(text);
        ImGui::PopFont();
        ImGui::Spacing();
    }

    bool primary(const char* label, bool enabled = true) {
        ImGui::BeginDisabled(!enabled);
        ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.52f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.12f, 0.33f, 0.85f, 1.0f));
        bool pressed = ImGui::Button(label);
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();
        return pressed;
    }

    bool secondary(const char* label) { return ImGui::Button(label); }

    void check_line(bool ok, const std::string& text) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float r = ImGui::GetTextLineHeight() * 0.3f;
        ImVec2 c(p.x + r, p.y + ImGui::GetTextLineHeight() * 0.5f);
        dl->AddCircleFilled(c, r, ImGui::GetColorU32(ok ? kGood : kBad));
        ImGui::Dummy({r * 2 + 6, 0});
        ImGui::SameLine();
        ImGui::TextColored(ok ? kGood : kBad, "%s", text.c_str());
    }

    void open_folder(const fs::path& dir) { SDL_OpenURL(("file://" + dir.string()).c_str()); }

    // ---- screens ----

    void draw_disc() {
        body(230);
        step("Step 1 of 4  ·  Game disc");
        title("Choose your Transformers ISO");
        ImGui::TextColored(kMuted, "Needs the US release of Transformers (2004) for PlayStation 2, serial %s.",
                           display_serial(kExpectedBoot).c_str());
        ImGui::Spacing();
        bool any = false;
        if (!scan_done_) {
            ImGui::TextColored(kMuted, "Looking in your home folder...");
        } else {
            for (const ScanHit& h : hits_) {
                if (h.result.status != ProbeStatus::Ok) continue;
                any = true;
                ImGui::PushID(h.path.c_str());
                if (primary(("Use " + h.path.string()).c_str())) choose_iso(h.path.string());
                ImGui::PopID();
            }
            if (!any) ImGui::TextColored(kMuted, "No matching disc found in your home folder.");
        }
        ImGui::Spacing();
        if (any ? secondary("Browse...") : primary("Browse...")) {
            static const SDL_DialogFileFilter filter = {"PS2 disc image (*.iso)", "iso;ISO"};
            SDL_ShowOpenFileDialog(&App::on_dialog, this, window_, &filter, 1, nullptr, false);
        }
        ImGui::SameLine();
        ImGui::TextColored(kMuted, "or drop the .iso file on this window");
        if (!disc_message_.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(kBad, "%s", disc_message_.c_str());
        }
        end_body();
    }

    void draw_check() {
        if (!sys_checked_) {
            avx2_ = cpu_has_avx2();
            free_ = free_disk_bytes(data_dir());
            needed_ = disk_needed(key_.empty() ? 0 : dir_size(build_dir(key_)));
            mem_ = mem_available_bytes();
            sys_checked_ = true;
        }
        body(250);
        step("Step 2 of 4  ·  Check");
        bool disc_ok = probe_.status == ProbeStatus::Ok;
        bool disk_ok = free_ >= needed_;
        title(disc_ok && avx2_ && disk_ok ? "Disc looks good" : "Something needs attention");
        check_line(disc_ok, disc_ok ? "US release, " + display_serial(kExpectedBoot) : probe_message(probe_));
        check_line(disc_ok, disc_ok ? "Game files intact (checksum matches)" : "Checksum not verified");
        check_line(avx2_, avx2_ ? "CPU supports AVX2" : "This CPU lacks AVX2, which the port requires.");
        check_line(disk_ok, disk_ok ? gib(free_) + " free disk space"
                                    : "Only " + gib(free_) + " free; the one-time build needs " + gib(needed_) + ".");
        unsigned jobs = build_jobs(core_count(), mem_);
        ImGui::TextColored(kMuted, "The build takes about 10-20 minutes once, using %u of %u CPU cores.", jobs,
                           core_count());
        ImGui::Spacing();
        if (primary(resume_ ? "Resume the build" : "Build the game", disc_ok && avx2_ && disk_ok)) start_build();
        ImGui::SameLine();
        if (secondary("Back")) go(Event::Back);
        end_body();
    }

    void start_build() {
        BuildConfig c;
        c.iso = settings_.iso_path;
        c.runtime_dir = ctx_.runtime_dir;
        c.work_dir = build_dir(key_);
        c.out_dir = game_dir(key_);
        c.tools = ctx_.tools;
        c.appdir = ctx_.appdir;
        c.jobs = build_jobs(core_count(), mem_available_bytes());
        jobs_ = c.jobs;
        builder_.start(c);
        go(Event::StartBuild);
    }

    void draw_building() {
        body(200);
        step("Step 3 of 4  ·  One-time build");
        title("Building the game for your PC");
        NinjaProgress p = demo_ ? NinjaProgress{214, 346} : builder_.progress();
        double elapsed = demo_ ? 600 : builder_.elapsed();
        unsigned jobs = demo_ ? 3 : jobs_;
        if (p.total > 0) {
            int eta = eta_seconds(p.done, p.total, elapsed);
            std::string left = eta < 0 ? "" : eta < 90 ? "about a minute left" : "about " + std::to_string((eta + 30) / 60) + " min left";
            ImGui::TextColored(kMuted, "Compiling %d / %d  ·  %s  ·  using %u cores", p.done, p.total, left.c_str(), jobs);
            ImGui::ProgressBar(float(p.done) / float(p.total), {-1, 8}, "");
        } else {
            ImGui::TextColored(kMuted, "Recompiling the game from your disc...");
            ImGui::ProgressBar(-float(ImGui::GetTime()) * 0.5f, {-1, 8}, "");
        }
        ImGui::TextColored(kMuted, "You can close this window; the build continues where it left off next time.");
        ImGui::Spacing();
        if (secondary("Cancel")) builder_.cancel();
        ImGui::SameLine();
        if (secondary("Show log")) open_folder(builder_.log_path().parent_path());
        end_body();
    }

    bool combo(const char* label, int* index, const std::vector<const char*>& items) {
        ImGui::SetNextItemWidth(220);
        return ImGui::Combo(label, index, items.data(), int(items.size()));
    }

    void display_controls() {
        int display = settings_.fullscreen ? 1 : 0;
        if (combo("Display", &display, {"Window", "Fullscreen"})) {
            settings_.fullscreen = display == 1;
            save();
        }
        int quality = settings_.upscale;
        if (combo("Graphics quality", &quality, {"Auto", "1x (native)", "2x", "3x", "4x"})) {
            settings_.upscale = quality;
            save();
        }
        if (ImGui::Checkbox("Widescreen", &settings_.widescreen)) save();
        static const std::vector<const char*> langs = {"en", "fr", "de", "es", "it"};
        static const std::vector<const char*> lang_names = {"English", "French", "German", "Spanish", "Italian"};
        int lang = 0;
        for (size_t i = 0; i < langs.size(); ++i)
            if (settings_.language == langs[i]) lang = int(i);
        if (combo("Language", &lang, lang_names)) {
            settings_.language = langs[size_t(lang)];
            save();
        }
    }

    void draw_quick_setup() {
        body(330);
        step("Step 4 of 4  ·  Quick setup");
        title("A few settings");
        display_controls();
        ImGui::Checkbox("Add to app menu", &settings_.add_to_menu);
        ImGui::Spacing();
        if (primary("Finish")) {
            settings_.setup_done = true;
            save();
            if (ctx_.appimage && settings_.add_to_menu)
                install_desktop_entry(*ctx_.appimage, ctx_.runtime_dir / "art" / "icon.png", data_home());
            go(Event::Finish);
        }
        end_body();
    }

    void draw_home() {
        body(150);
        title("Ready");
        int n = 0;
        SDL_JoystickID* pads = SDL_GetGamepads(&n);
        const char* pad = n > 0 ? SDL_GetGamepadNameForID(pads[0]) : nullptr;
        std::string saves = (data_dir() / "memcard0").string();
        const char* home = std::getenv("HOME");
        if (home && *home && saves.rfind(home, 0) == 0) saves = "~" + saves.substr(std::string(home).size());
        ImGui::TextColored(kMuted, "%s  ·  Saves: %s", pad ? pad : "No controller (keyboard and mouse work)",
                           saves.c_str());
        SDL_free(pads);
        ImGui::Spacing();
        ImGui::PushFont(big_font_);
        if (primary("  Play  ")) play();
        ImGui::PopFont();
        ImGui::SameLine();
        if (secondary("Settings")) go(Event::OpenSettings);
        ImGui::SameLine();
        if (secondary("Controls")) go(Event::OpenControls);
        ImGui::SameLine();
        if (secondary("Change disc")) go(Event::ChangeDisc);
        end_body();
    }

    void play() {
        if (demo_) return;
        std::vector<std::string> argv = {(game_dir(key_) / "transformers").string()};
        std::vector<std::string> args = to_args(settings_);
        argv.insert(argv.end(), args.begin(), args.end() - 1);
        argv.insert(argv.end(), ctx_.game_args.begin(), ctx_.game_args.end());
        argv.push_back(args.back());
        game_tail_.clear();
        if (!game_.start(argv)) {
            error_title_ = "The game could not be started.";
            error_lines_ = {argv[0]};
            go(Event::GameCrashed);
            return;
        }
        playing_ = true;
        SDL_HideWindow(window_);
    }

    void draw_settings() {
        ImVec2 size = ImGui::GetIO().DisplaySize;
        ImGui::SetCursorPos({size.x * 0.06f, size.y * 0.24f});
        ImGui::BeginChild("settings", {size.x * 0.88f, size.y * 0.66f});
        title("Settings");
        display_controls();
        static const std::vector<const char*> res = {"Default", "1280x720", "1600x900", "1920x1080", "2560x1440",
                                                     "3840x2160"};
        int r = 0;
        for (size_t i = 1; i < res.size(); ++i)
            if (settings_.resolution == res[i]) r = int(i);
        if (combo("Resolution", &r, res)) {
            settings_.resolution = r == 0 ? "" : res[size_t(r)];
            save();
        }
        int renderer = settings_.software_renderer ? 1 : 0;
        if (combo("Renderer", &renderer, {"Hardware (OpenGL)", "Software (exact, slower)"})) {
            settings_.software_renderer = renderer == 1;
            save();
        }
        if (settings_.software_renderer) {
            int t = settings_.sw_threads + 1;
            if (combo("Software threads", &t, {"Auto", "0", "1", "2", "3", "4", "5", "6", "7", "8"})) {
                settings_.sw_threads = t - 1;
                save();
            }
        }
        if (ImGui::Checkbox("VSync", &settings_.vsync)) save();
        bool sharpen_on = settings_.sharpen >= 0;
        if (ImGui::Checkbox("Custom sharpening", &sharpen_on)) {
            settings_.sharpen = sharpen_on ? 40 : -1;
            save();
        }
        if (sharpen_on) {
            ImGui::SetNextItemWidth(220);
            if (ImGui::SliderInt("Sharpening", &settings_.sharpen, 0, 100)) save();
        }
        if (ImGui::Checkbox("Deflicker filter", &settings_.deflicker)) save();
        ImGui::SeparatorText("Mouse");
        bool sens_on = settings_.mouse_sens > 0;
        if (ImGui::Checkbox("Custom mouse sensitivity", &sens_on)) {
            settings_.mouse_sens = sens_on ? 1.0f : 0.0f;
            save();
        }
        if (sens_on) {
            ImGui::SetNextItemWidth(220);
            if (ImGui::SliderFloat("Sensitivity", &settings_.mouse_sens, 0.1f, 5.0f, "%.2f")) save();
        }
        if (ImGui::Checkbox("Invert mouse Y", &settings_.invert_mouse)) save();
        if (ImGui::Checkbox("Mouse moves the stick (vehicle-style)", &settings_.mouse_stick)) save();
        ImGui::SeparatorText("App");
        if (ImGui::Checkbox("Show in app menu", &settings_.add_to_menu)) {
            save();
            if (ctx_.appimage && settings_.add_to_menu)
                install_desktop_entry(*ctx_.appimage, ctx_.runtime_dir / "art" / "icon.png", data_home());
        }
        ImGui::EndChild();
        ImGui::SetCursorPos({size.x * 0.06f, size.y * 0.92f});
        if (primary("Done")) go(Event::Close);
    }

    void draw_markdown(const char* md) {
        std::string text(md);
        size_t pos = 0;
        std::vector<std::vector<std::string>> table;
        auto flush_table = [&] {
            if (table.empty()) return;
            int cols = int(table[0].size());
            if (ImGui::BeginTable("t", cols, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
                for (size_t r = 0; r < table.size(); ++r) {
                    ImGui::TableNextRow();
                    for (int c = 0; c < cols && c < int(table[r].size()); ++c) {
                        ImGui::TableSetColumnIndex(c);
                        if (r == 0) ImGui::TextColored(kStep, "%s", table[r][size_t(c)].c_str());
                        else ImGui::TextWrapped("%s", table[r][size_t(c)].c_str());
                    }
                }
                ImGui::EndTable();
            }
            table.clear();
        };
        while (pos < text.size()) {
            size_t eol = text.find('\n', pos);
            if (eol == std::string::npos) eol = text.size();
            std::string line = text.substr(pos, eol - pos);
            pos = eol + 1;
            std::string clean;
            for (char ch : line)
                if (ch != '`' && ch != '*') clean += ch;
            if (!clean.empty() && clean[0] == '|') {
                if (clean.find("---") != std::string::npos) continue;
                std::vector<std::string> cells;
                size_t s = 1;
                for (size_t bar; (bar = clean.find('|', s)) != std::string::npos; s = bar + 1) {
                    std::string cell = clean.substr(s, bar - s);
                    cell.erase(0, cell.find_first_not_of(' '));
                    cell.erase(cell.find_last_not_of(' ') + 1);
                    cells.push_back(cell);
                }
                table.push_back(cells);
                continue;
            }
            flush_table();
            if (clean.rfind("# ", 0) == 0) continue;  // the page has its own title
            if (clean.rfind("## ", 0) == 0) {
                ImGui::Spacing();
                ImGui::SeparatorText(clean.substr(3).c_str());
            } else if (!clean.empty()) {
                ImGui::TextWrapped("%s", clean.c_str());
                ImGui::Spacing();
            }
        }
        flush_table();
    }

    void draw_controls() {
        ImVec2 size = ImGui::GetIO().DisplaySize;
        ImGui::SetCursorPos({size.x * 0.06f, size.y * 0.24f});
        ImGui::BeginChild("controls", {size.x * 0.88f, size.y * 0.66f});
        title("Controls");
        draw_markdown(kControlsMd);
        ImGui::EndChild();
        ImGui::SetCursorPos({size.x * 0.06f, size.y * 0.92f});
        if (primary("Done")) go(Event::Close);
    }

    void draw_error() {
        body(300);
        title(error_title_.c_str());
        ImGui::BeginChild("details", {0, 150}, ImGuiChildFlags_Borders);
        for (const std::string& l : error_lines_) ImGui::TextUnformatted(l.c_str());
        ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
        ImGui::Spacing();
        if (primary("Retry")) go(Event::Retry);
        ImGui::SameLine();
        if (secondary("Choose another disc")) go(Event::Back);
        if (!error_dir_.empty()) {
            ImGui::SameLine();
            if (secondary("Open log folder")) open_folder(error_dir_);
        }
        end_body();
    }

  public:
    ImFont* big_font_ = nullptr;

  private:
    const AppContext& ctx_;
    SDL_Window* window_;
    SDL_Renderer* renderer_;
    Image bg_, logo_;
    fs::path settings_path_;
    Settings settings_;
    ProbeResult probe_;
    std::string key_;
    Screen screen_ = Screen::Disc;
    bool resume_ = false;
    bool demo_ = false;
    bool quit_ = false;

    std::future<std::vector<ScanHit>> scan_;
    std::vector<ScanHit> hits_;
    bool scan_done_ = false;
    std::string disc_message_;
    std::mutex dialog_mu_;
    std::string dialog_pick_;

    bool sys_checked_ = false, avx2_ = false;
    uint64_t free_ = 0, mem_ = 0, needed_ = kMinFreeDisk;

    Builder builder_;
    unsigned jobs_ = 1;

    Process game_;
    bool playing_ = false;
    std::deque<std::string> game_tail_;

    std::string error_title_;
    std::vector<std::string> error_lines_;
    fs::path error_dir_;
};

// ---- window setup ---------------------------------------------------------------------

const char* const kFontPaths[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
};

struct Gui {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    ImFont* big = nullptr;

    bool init(bool hidden, bool software) {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
            std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            return false;
        }
        SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
        if (hidden) flags |= SDL_WINDOW_HIDDEN;
        window = SDL_CreateWindow("Transformers", kWidth, kHeight, flags);
        if (!window) return false;
        renderer = SDL_CreateRenderer(window, software ? "software" : nullptr);
        if (!renderer) return false;
        SDL_SetRenderVSync(renderer, 1);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        ImFont* body = nullptr;
        for (const char* f : kFontPaths) {
            std::error_code ec;
            if (!fs::exists(f, ec)) continue;
            body = io.Fonts->AddFontFromFileTTF(f, 17.0f);
            big = io.Fonts->AddFontFromFileTTF(f, 26.0f);
            break;
        }
        if (!body) {
            ImFontConfig cfg;
            cfg.SizePixels = 16;
            io.Fonts->AddFontDefault(&cfg);
            cfg.SizePixels = 24;
            big = io.Fonts->AddFontDefault(&cfg);
        }
        ImGuiStyle& st = ImGui::GetStyle();
        ImGui::StyleColorsDark(&st);
        st.FrameRounding = 4;
        st.FramePadding = {14, 7};
        st.ItemSpacing = {10, 9};
        st.Colors[ImGuiCol_Text] = ImVec4(0.91f, 0.93f, 0.97f, 1);
        st.Colors[ImGuiCol_Button] = ImVec4(1, 1, 1, 0.08f);
        st.Colors[ImGuiCol_ButtonHovered] = ImVec4(1, 1, 1, 0.16f);
        st.Colors[ImGuiCol_ButtonActive] = ImVec4(1, 1, 1, 0.24f);
        st.Colors[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, 0.08f);
        st.Colors[ImGuiCol_FrameBgHovered] = ImVec4(1, 1, 1, 0.14f);
        st.Colors[ImGuiCol_CheckMark] = kStep;
        st.Colors[ImGuiCol_SliderGrab] = kAccent;
        st.Colors[ImGuiCol_PlotHistogram] = kAccent;
        st.Colors[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0.25f);
        st.Colors[ImGuiCol_PopupBg] = ImVec4(0.06f, 0.09f, 0.16f, 0.98f);
        st.Colors[ImGuiCol_Header] = ImVec4(0.18f, 0.42f, 1.0f, 0.4f);
        st.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.18f, 0.42f, 1.0f, 0.6f);
        ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
        ImGui_ImplSDLRenderer3_Init(renderer);
        return true;
    }

    void begin_frame() {
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
    }

    void end_frame() {
        ImGui::Render();
        SDL_SetRenderScale(renderer, ImGui::GetIO().DisplayFramebufferScale.x,
                           ImGui::GetIO().DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(renderer, 4, 8, 20, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    }

    ~Gui() {
        if (ImGui::GetCurrentContext()) {
            ImGui_ImplSDLRenderer3_Shutdown();
            ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext();
        }
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
    }
};

}  // namespace

std::optional<Screen> screen_from_name(const std::string& n) {
    static const std::pair<const char*, Screen> names[] = {
        {"disc", Screen::Disc}, {"check", Screen::Check}, {"building", Screen::Building},
        {"quicksetup", Screen::QuickSetup}, {"home", Screen::Home}, {"settings", Screen::Settings},
        {"controls", Screen::Controls}, {"error", Screen::Error}};
    for (auto& [name, s] : names)
        if (n == name) return s;
    return std::nullopt;
}

int run_gui(const AppContext& ctx) {
    Gui gui;
    if (!gui.init(false, false)) return 1;
    App app(ctx, gui.window, gui.renderer);
    app.big_font_ = gui.big;
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT && !app.playing()) running = false;
            app.handle_event(e);
        }
        if (app.playing()) {
            app.frame();
            SDL_Delay(50);
            continue;
        }
        gui.begin_frame();
        running = app.frame() && running;
        gui.end_frame();
        SDL_RenderPresent(gui.renderer);
    }
    return 0;
}

int render_shot(const AppContext& ctx, Screen screen, const fs::path& png) {
    Gui gui;
    if (!gui.init(true, true)) return 1;
    App app(ctx, gui.window, gui.renderer);
    app.big_font_ = gui.big;
    app.demo(screen);
    for (int i = 0; i < 3; ++i) {  // ImGui settles layout over a couple of frames
        SDL_Event e;
        while (SDL_PollEvent(&e)) ImGui_ImplSDL3_ProcessEvent(&e);
        gui.begin_frame();
        app.frame();
        gui.end_frame();
        if (i == 2) {
            if (!save_png(gui.renderer, png)) return 1;
            if (app.overflow_) {
                std::fprintf(stderr, "content does not fit the window\n");
                return 3;
            }
        }
        SDL_RenderPresent(gui.renderer);
    }
    return 0;
}

}  // namespace launcher
