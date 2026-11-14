// transformers-launcher: setup wizard and launcher for the Transformers port.
//
//   transformers-launcher [--runtime <dir>] [game args...]
//   transformers-launcher --prepare <iso> [--runtime <dir>]   one-time build, no window
//   transformers-launcher --shot <screen> <png>               draw one screen (testing)
#include "builder.h"
#include "iso_probe.h"
#include "paths.h"
#include "settings.h"
#include "sysinfo.h"
#include "ui.h"

#include <signal.h>
#include <unistd.h>

#include <atomic>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;
using namespace launcher;

namespace {

std::atomic<bool> g_stop{false};
void on_stop_signal(int) { g_stop = true; }

fs::path exe_dir() {
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec).parent_path();
}

std::string read_version(const fs::path& runtime_dir) {
    std::ifstream f(runtime_dir / "runtime" / "version");
    std::string v;
    std::getline(f, v);
    return v.empty() ? "unknown" : v;
}

BuildTools find_tools() {
    BuildTools t;
    const char* appdir = std::getenv("APPDIR");
    if (!appdir || !*appdir) return t;  // cmake, ninja, python3 from PATH
    fs::path tc = fs::path(appdir) / "usr" / "toolchain";
    t.cmake = tc / "cmake" / "bin" / "cmake";
    t.ninja = tc / "ninja" / "ninja";
    t.python = tc / "python" / "bin" / "python3";
    t.cc = tc / "gcc" / "bin" / "gcc";
    t.cxx = tc / "gcc" / "bin" / "g++";
    return t;
}

int prepare(const AppContext& ctx, const std::string& iso) {
    ProbeResult r = probe_iso(iso);
    if (r.status != ProbeStatus::Ok) {
        std::fprintf(stderr, "%s\n", probe_message(r).c_str());
        return 1;
    }
    if (!cpu_has_avx2()) {
        std::fprintf(stderr, "This CPU lacks AVX2, which the port requires.\n");
        return 1;
    }
    std::string key = cache_key(r.elf_sha256, ctx.runtime_version);
    uint64_t needed = disk_needed(dir_size(build_dir(key)));
    if (free_disk_bytes(data_dir()) < needed) {
        std::fprintf(stderr, "Not enough free disk space in %s; the build needs %.1f GB more.\n",
                     data_dir().c_str(), double(needed) / double(1ull << 30));
        return 1;
    }
    BuildConfig c;
    c.iso = fs::absolute(iso);
    c.runtime_dir = ctx.runtime_dir;
    c.work_dir = build_dir(key);
    c.out_dir = game_dir(key);
    c.tools = ctx.tools;
    c.appdir = ctx.appdir;
    c.jobs = build_jobs(core_count(), mem_available_bytes());
    std::printf("building %s with %u jobs\n", c.out_dir.c_str(), c.jobs);
    std::fflush(stdout);

    // Ctrl-C or kill stops the build (and its compiler processes) cleanly; the
    // work dir stays, so the next run resumes.
    struct sigaction sa{};
    sa.sa_handler = on_stop_signal;
    for (int sig : {SIGINT, SIGTERM, SIGHUP}) sigaction(sig, &sa, nullptr);

    Builder b;
    b.start(c);
    int last = -1;
    for (;;) {
        if (g_stop) b.cancel();
        Builder::State s = b.state();
        if (s == Builder::State::Succeeded || s == Builder::State::Failed || s == Builder::State::Cancelled) break;
        NinjaProgress p = b.progress();
        if (p.done != last && p.total > 0) {
            std::printf("%d/%d\n", p.done, p.total);
            std::fflush(stdout);
            last = p.done;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    b.join();
    if (b.state() == Builder::State::Cancelled) {
        std::fprintf(stderr, "build stopped; run again to continue where it left off\n");
        return 1;
    }
    if (b.state() != Builder::State::Succeeded) {
        for (const std::string& l : b.log_tail(30)) std::fprintf(stderr, "%s\n", l.c_str());
        std::fprintf(stderr, "build failed; full log: %s\n", b.log_path().c_str());
        return 1;
    }
    Settings s = load_settings(config_dir() / "settings.ini");
    s.iso_path = c.iso.string();
    save_settings(s, config_dir() / "settings.ini");
    std::printf("ready: %s\n", (c.out_dir / "transformers").c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    AppContext ctx;
    std::string prepare_iso, shot_screen, shot_png;
    fs::path runtime;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--prepare" && i + 1 < argc) prepare_iso = argv[++i];
        else if (a == "--runtime" && i + 1 < argc) runtime = argv[++i];
        else if (a == "--shot" && i + 2 < argc) {
            shot_screen = argv[++i];
            shot_png = argv[++i];
        } else if (a == "--") {
            for (++i; i < argc; ++i) ctx.game_args.push_back(argv[i]);
        } else ctx.game_args.push_back(a);
    }
    if (runtime.empty()) {
        const char* appdir = std::getenv("APPDIR");
        runtime = appdir && *appdir ? fs::path(appdir) / "usr" / "share" / "transformers-port"
                                    : exe_dir().parent_path() / "share" / "transformers-port";
    }
    ctx.runtime_dir = fs::absolute(runtime);
    ctx.runtime_version = read_version(ctx.runtime_dir);
    ctx.tools = find_tools();
    if (const char* ad = std::getenv("APPDIR"); ad && *ad) ctx.appdir = fs::path(ad);
    if (const char* ai = std::getenv("APPIMAGE"); ai && *ai) ctx.appimage = fs::path(ai);

    if (!prepare_iso.empty()) return prepare(ctx, prepare_iso);
    if (!shot_screen.empty()) {
        auto s = screen_from_name(shot_screen);
        if (!s) {
            std::fprintf(stderr, "unknown screen %s\n", shot_screen.c_str());
            return 2;
        }
        return render_shot(ctx, *s, shot_png);
    }
    return run_gui(ctx);
}
