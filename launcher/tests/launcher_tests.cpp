// Launcher unit tests.  Run: ./build/launcher/launcher_tests
// PS2_TEST_ISO=<Transformers.iso> also checks the real disc.
#include "builder.h"
#include "desktop.h"
#include "fake_iso.h"
#include "iso_probe.h"
#include "iso_scan.h"
#include "paths.h"
#include "settings.h"
#include "sha256.h"
#include "sysinfo.h"
#include "wizard.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace launcher;

namespace {

void check(bool passed, const char* description) {
    if (!passed) throw std::runtime_error(description);
    std::printf("PASS %s\n", description);
}

// A fresh empty directory under the system temp dir, removed by the destructor.
struct TempDir {
    fs::path path;
    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "launcher-test-XXXXXX").string();
        if (!mkdtemp(tmpl.data())) throw std::runtime_error("mkdtemp failed");
        path = tmpl;
    }
    ~TempDir() {
        std::error_code ec;
        fs::permissions(path, fs::perms::owner_all, fs::perm_options::add, ec);
        for (auto it = fs::recursive_directory_iterator(path, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec))
            if (it->is_directory(ec) && !it->is_symlink(ec))
                fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add, ec);
        fs::remove_all(path, ec);
    }
};

const std::string kGoodCnf = "BOOT2 = cdrom0:\\SLUS_206.68;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n";

Bytes good_disc(const std::string& elf) {
    return make_iso({{"SYSTEM.CNF", bytes(kGoodCnf)}, {"SLUS_206.68", bytes(elf)}});
}

void test_sha256() {
    check(sha256_hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "sha256 of \"abc\"");
    check(sha256_hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "sha256 of empty input");
    std::string million(1000000, 'a');
    check(sha256_hex(million.data(), million.size()) ==
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
          "sha256 of one million 'a' (multi-block)");
}

void test_probe() {
    TempDir t;
    fs::path iso = t.path / "good.iso";
    write_file(iso, good_disc("ELF!"));
    ProbeResult r = probe_iso(iso, sha256_hex("ELF!", 4));
    check(r.status == ProbeStatus::Ok, "probe accepts a disc whose boot ELF matches");
    check(r.boot == "SLUS_206.68", "probe reports the boot file name");
    check(r.elf_sha256 == sha256_hex("ELF!", 4), "probe reports the ELF hash");

    r = probe_iso(iso);
    check(r.status == ProbeStatus::BadChecksum, "probe rejects a modified SLUS-20668 ELF");
    check(probe_message(r).find("don't match the original disc") != std::string::npos,
          "bad-checksum message explains the mismatch");

    fs::path pal = t.path / "pal.iso";
    write_file(pal, make_iso({{"SYSTEM.CNF", bytes("BOOT2 = cdrom0:\\SLES_523.88;1\r\n")},
                              {"SLES_523.88", bytes("ELF!")}}));
    r = probe_iso(pal);
    check(r.status == ProbeStatus::WrongDisc, "probe rejects another serial");
    check(display_serial(r.boot) == "SLES-52388", "display_serial formats SLES_523.88");
    std::string msg = probe_message(r);
    check(msg.find("SLES-52388") != std::string::npos && msg.find("SLUS-20668") != std::string::npos,
          "wrong-disc message names the found and the needed serial");

    fs::path zeros = t.path / "zeros.iso";
    write_file(zeros, Bytes(4096, 0));
    check(probe_iso(zeros).status == ProbeStatus::NotIso, "probe rejects a non-ISO file");
    check(probe_iso(t.path / "missing.iso").status == ProbeStatus::ReadError, "probe reports a missing file");

    fs::path noboot = t.path / "noboot.iso";
    write_file(noboot, make_iso({{"SYSTEM.CNF", bytes("VER = 1.00\r\n")}}));
    check(probe_iso(noboot).status == ProbeStatus::NoBootFile, "probe rejects SYSTEM.CNF without BOOT2");

    fs::path nocnf = t.path / "nocnf.iso";
    write_file(nocnf, make_iso({{"README.TXT", bytes("hi")}}));
    check(probe_iso(nocnf).status == ProbeStatus::NoBootFile, "probe rejects an ISO without SYSTEM.CNF");

    fs::path sub = t.path / "sub.iso";
    write_file(sub, make_iso({{"SYSTEM.CNF", bytes("BOOT2 = cdrom0:\\GAME\\SLUS_206.68;1\r\n")},
                              {"GAME/SLUS_206.68", bytes("ELF!")}}));
    check(probe_iso(sub, sha256_hex("ELF!", 4)).status == ProbeStatus::Ok,
          "probe follows a boot path in a subdirectory");

    if (const char* real = std::getenv("PS2_TEST_ISO"))
        check(probe_iso(real).status == ProbeStatus::Ok, "probe accepts the real Transformers (USA) disc");
}

void test_sysinfo() {
    check(build_jobs(16, 8ull << 30) == 3, "build_jobs: 16 cores, 8 GiB -> 3");
    check(build_jobs(4, 1ull << 30) == 1, "build_jobs: 4 cores, 1 GiB -> 1");
    check(build_jobs(2, 64ull << 30) == 2, "build_jobs: 2 cores, 64 GiB -> 2");
    check(build_jobs(8, 4ull << 30) == 1, "build_jobs: 8 cores, 4 GiB -> 1 (low-RAM machine)");
    check(build_jobs(0, 8ull << 30) == 1, "build_jobs: unknown core count -> 1");
    check(parse_mem_available("MemTotal: 100 kB\nMemAvailable:    2048 kB\n") == 2048ull * 1024,
          "parse_mem_available reads MemAvailable");
    check(parse_mem_available("MemTotal: 100 kB\n") == 0, "parse_mem_available without the line -> 0");
    check(core_count() >= 1, "core_count is at least 1");
    check(mem_available_bytes() > 0, "mem_available_bytes reads /proc/meminfo");
    check(disk_needed(0) == kMinFreeDisk, "disk_needed: fresh build needs the full 3 GB");
    check(disk_needed(1ull << 30) == 2ull << 30, "disk_needed: a resumed build counts what it already wrote");
    check(disk_needed(5ull << 30) == 512ull << 20, "disk_needed never drops below 512 MB");
    TempDir t;
    fs::create_directories(t.path / "w" / "sub");
    write_file(t.path / "w" / "a", Bytes(1000, 1));
    write_file(t.path / "w" / "sub" / "b", Bytes(24, 1));
    check(dir_size(t.path / "w") == 1024, "dir_size sums files recursively");
    check(dir_size(t.path / "missing") == 0, "dir_size of a missing dir is 0");
    check(free_disk_bytes(t.path / "not" / "yet" / "created") > 0,
          "free_disk_bytes uses the nearest existing ancestor");
}

void test_scan() {
    TempDir t;
    fs::path root = t.path / "home";
    fs::create_directories(root / "a" / "b" / "c");
    fs::create_directories(root / "x");
    fs::create_directories(root / "locked");
    write_file(root / "a" / "b" / "c" / "deep.iso", good_disc("ELF!"));
    write_file(root / "x" / "good.ISO", good_disc("ELF!"));
    write_file(root / "x" / "notes.txt", bytes("not a disc"));
    write_file(root / "locked" / "y.iso", good_disc("ELF!"));
    write_file(root / "\xff.iso", Bytes(4096, 7));
    fs::create_directory_symlink(root, root / "loop");
    fs::permissions(root / "locked", fs::perms::none);

    std::vector<ScanHit> hits = scan_for_isos({root});
    auto find = [&](const std::string& name) {
        return std::find_if(hits.begin(), hits.end(),
                            [&](const ScanHit& h) { return h.path.filename() == name; });
    };
    check(find("good.ISO") != hits.end(), "scan finds .ISO files case-insensitively");
    check(find("good.ISO")->result.status == ProbeStatus::BadChecksum, "scan probes each hit");
    check(find("\xff.iso") != hits.end() && find("\xff.iso")->result.status == ProbeStatus::NotIso,
          "scan copes with a non-UTF-8 file name");
    check(find("deep.iso") == hits.end(), "scan stops at depth 2");
    check(find("notes.txt") == hits.end(), "scan ignores non-.iso files");
    // root (e.g. in the AppImage build container) can read the locked dir.
    size_t expected = geteuid() == 0 ? 3 : 2;
    check(hits.size() == expected, "scan does not follow the symlink loop or enter the locked dir");

    fs::create_directories(t.path / "h2" / "Games");
    std::vector<fs::path> roots = default_scan_roots(t.path / "h2");
    check(roots.size() == 2 && roots[0] == t.path / "h2" && roots[1] == t.path / "h2" / "Games",
          "default_scan_roots lists home plus the existing folders");
}


using Args = std::vector<std::string>;

void test_settings() {
    const std::string iso = "/g/Transformers (USA) (En,Fr,De).iso";
    Settings d;
    d.iso_path = iso;
    check(to_args(d) == Args{iso}, "default settings pass only the ISO path");

    auto one = [&](auto edit, Args flags, const char* what) {
        Settings s;
        s.iso_path = iso;
        edit(s);
        flags.push_back(iso);
        check(to_args(s) == flags, what);
    };
    one([](Settings& s) { s.software_renderer = true; }, {"--renderer", "sw"}, "software renderer -> --renderer sw");
    one([](Settings& s) { s.software_renderer = true; s.sw_threads = 4; }, {"--renderer", "sw", "--sw-threads", "4"},
        "software threads -> --sw-threads N");
    one([](Settings& s) { s.sw_threads = 4; }, {}, "software threads without software renderer -> no flag");
    one([](Settings& s) { s.upscale = 3; }, {"--upscale", "3"}, "upscale 3 -> --upscale 3");
    one([](Settings& s) { s.resolution = "1920x1080"; }, {"--resolution", "1920x1080"}, "resolution -> --resolution");
    one([](Settings& s) { s.fullscreen = true; }, {"--fullscreen"}, "fullscreen -> --fullscreen");
    one([](Settings& s) { s.widescreen = true; }, {"--widescreen"}, "widescreen -> --widescreen");
    one([](Settings& s) { s.vsync = true; }, {"--vsync"}, "vsync -> --vsync");
    one([](Settings& s) { s.sharpen = 70; }, {"--sharpen", "70"}, "sharpen 70 -> --sharpen 70");
    one([](Settings& s) { s.deflicker = true; }, {"--deflicker"}, "deflicker -> --deflicker");
    one([](Settings& s) { s.language = "de"; }, {"--lang", "de"}, "language de -> --lang de");
    one([](Settings& s) { s.mouse_sens = 1.5f; }, {"--mouse-sens", "1.5"}, "mouse sensitivity -> --mouse-sens");
    one([](Settings& s) { s.invert_mouse = true; }, {"--invert-mouse"}, "invert mouse -> --invert-mouse");
    one([](Settings& s) { s.mouse_stick = true; }, {"--mouse-stick"}, "mouse as stick -> --mouse-stick");
    check(to_args(d).back() == iso, "an ISO path with spaces, commas and parentheses stays one argument");

    TempDir t;
    Settings full;
    full.iso_path = iso;
    full.setup_done = true;
    full.add_to_menu = false;
    full.fullscreen = true;
    full.resolution = "2560x1440";
    full.upscale = 4;
    full.widescreen = true;
    full.language = "fr";
    full.software_renderer = true;
    full.sw_threads = 6;
    full.vsync = true;
    full.sharpen = 10;
    full.deflicker = true;
    full.mouse_sens = 0.25f;
    full.invert_mouse = true;
    full.mouse_stick = true;
    fs::path file = t.path / "cfg" / "settings.ini";
    check(save_settings(full, file), "save_settings creates the directory and writes");
    check(load_settings(file) == full, "settings round-trip through the ini file");

    write_file(file, bytes("upscale=banana\nmystery=1\nwidescreen=1\n"));
    Settings g = load_settings(file);
    check(g.upscale == 0 && g.widescreen, "garbled values load as defaults, good ones still load");
    check(load_settings(t.path / "missing.ini") == Settings{}, "a missing file loads defaults");
}

void test_paths() {
    check(cache_key("ab88780324b9492eca59016923d200dd1d95785fd725dd85f233c071805b4acb", "v1.0 /x") ==
              "ab88780324b9492e-v1.0__x",
          "cache_key = 16 hash chars + sanitized version");
    setenv("XDG_DATA_HOME", "/tmp/q", 1);
    setenv("XDG_CONFIG_HOME", "/tmp/c", 1);
    check(data_dir() == "/tmp/q/transformers-port", "data_dir follows XDG_DATA_HOME");
    check(config_dir() == "/tmp/c/transformers-port", "config_dir follows XDG_CONFIG_HOME");
    check(game_dir("k") == "/tmp/q/transformers-port/game-k", "game_dir");
    check(build_dir("k") == "/tmp/q/transformers-port/game-k.tmp", "build_dir");
    unsetenv("XDG_DATA_HOME");
    setenv("HOME", "/home/someone", 1);
    check(data_dir() == "/home/someone/.local/share/transformers-port", "data_dir falls back to ~/.local/share");
}


bool log_has(const Builder& b, const std::string& needle) {
    for (const std::string& line : b.log_tail(200))
        if (line.find(needle) != std::string::npos) return true;
    return false;
}

// Waits for a terminal state (at most `secs`).
Builder::State finish(Builder& b, double secs = 20) {
    auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(secs);
    while (std::chrono::steady_clock::now() < until) {
        Builder::State s = b.state();
        if (s == Builder::State::Succeeded || s == Builder::State::Failed || s == Builder::State::Cancelled) {
            b.join();
            return s;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    throw std::runtime_error("build did not finish in time");
}

BuildConfig fake_config(const fs::path& root) {
    BuildConfig c;
    c.iso = root / "My Games" / "Transformers (USA) (En,Fr,De).iso";
    c.runtime_dir = root / "rt";
    c.work_dir = root / "data" / "game-k.tmp";
    c.out_dir = root / "data" / "game-k";
    c.tools.cmake = fs::path(LAUNCHER_FAKE_TOOLS) / "cmake";
    c.tools.ninja = fs::path(LAUNCHER_FAKE_TOOLS) / "ninja";
    c.jobs = 3;
    return c;
}

int count_lines(const fs::path& file) {
    std::ifstream f(file);
    int n = 0;
    std::string line;
    while (std::getline(f, line)) ++n;
    return n;
}

std::string last_line(const fs::path& file) {
    std::ifstream f(file);
    std::string line, last;
    while (std::getline(f, line)) last = line;
    return last;
}

void test_builder() {
    check(parse_ninja_line("[214/346] Building CXX object x").has_value() &&
              parse_ninja_line("[214/346] Building CXX object x")->done == 214 &&
              parse_ninja_line("[214/346] Building CXX object x")->total == 346,
          "parse_ninja_line reads [N/M]");
    check(!parse_ninja_line("ninja: build stopped: subcommand failed."), "parse_ninja_line ignores other lines");
    check(!parse_ninja_line("[x/3] nope"), "parse_ninja_line rejects garbage counts");
    check(eta_seconds(50, 100, 60.0) == 60, "eta_seconds: half done after 60 s -> 60 s left");
    check(eta_seconds(0, 100, 5.0) == -1, "eta_seconds unknown before the first step");

    {
        TempDir t;
        BuildConfig c = fake_config(t.path);
        setenv("FAKE_NINJA_MODE", "ok", 1);
        Builder b;
        b.start(c);
        check(finish(b) == Builder::State::Succeeded, "fake build succeeds");
        check(fs::exists(c.out_dir / "transformers"), "binary moved into the game dir");
        check(fs::exists(c.out_dir / "build.log"), "build.log kept next to the binary");
        check(!fs::exists(c.work_dir), "work dir removed after success");
        check(b.progress().done == 3 && b.progress().total == 3, "progress reaches 3/3");
        check(log_has(b, "-DPS2_ISO=" + c.iso.string()), "ISO path with spaces/commas/parens reaches cmake intact");
        check(log_has(b, "-DPS2_RUNTIME_DIR=" + c.runtime_dir.string()), "runtime dir passed to cmake");
        check(!log_has(b, "-DCMAKE_C_COMPILER"), "no compiler override unless set");
    }
    {
        TempDir t;
        BuildConfig c = fake_config(t.path);
        c.tools.cc = "/opt/tc/bin/gcc";
        c.tools.cxx = "/opt/tc/bin/g++";
        setenv("FAKE_NINJA_MODE", "ok", 1);
        Builder b;
        b.start(c);
        finish(b);
        check(log_has(b, "-DCMAKE_C_COMPILER=/opt/tc/bin/gcc") && log_has(b, "-DCMAKE_CXX_COMPILER=/opt/tc/bin/g++"),
              "bundled compilers are passed to cmake when set");
    }
    {
        TempDir t;
        BuildConfig c = fake_config(t.path);
        setenv("FAKE_NINJA_MODE", "fail", 1);
        Builder b;
        b.start(c);
        check(finish(b) == Builder::State::Failed, "a failing compile ends Failed");
        check(!fs::exists(c.out_dir), "a failed build leaves no game dir");
        check(log_has(b, "No space left on device"), "log tail shows the compiler's error");
        check(fs::exists(b.log_path()), "build.log exists after failure");
    }
    {
        TempDir t;
        BuildConfig c = fake_config(t.path);
        setenv("FAKE_NINJA_MODE", "oomonce", 1);
        Builder b;
        b.start(c);
        check(finish(b) == Builder::State::Succeeded, "an OOM-killed compile is retried and succeeds");
        check(fs::exists(c.out_dir / "transformers"), "retried build produced the binary");
    }
    {
        TempDir t;
        BuildConfig c = fake_config(t.path);
        fs::path jobs = t.path / "jobs.txt";
        setenv("FAKE_NINJA_MODE", "oomonce", 1);
        setenv("FAKE_JOBS_FILE", jobs.c_str(), 1);
        Builder b;
        b.start(c);
        finish(b);
        unsetenv("FAKE_JOBS_FILE");
        check(last_line(jobs) == "-j1", "the retry runs with -j1");
        check(count_lines(jobs) == 2, "exactly one retry");
    }
    {
        TempDir t;
        BuildConfig c = fake_config(t.path);
        setenv("FAKE_NINJA_MODE", "slow", 1);
        Builder b;
        b.start(c);
        while (b.state() != Builder::State::Compiling && b.state() != Builder::State::Failed)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto t0 = std::chrono::steady_clock::now();
        b.cancel();
        check(finish(b, 5) == Builder::State::Cancelled, "cancel stops the build");
        check(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5), "cancel takes under 5 s");
        check(fs::exists(c.work_dir / "build.ninja"), "cancel keeps the work dir");

        setenv("FAKE_NINJA_MODE", "ok", 1);
        Builder again;
        again.start(c);
        check(finish(again) == Builder::State::Succeeded, "a second start resumes and succeeds");
        check(!log_has(again, "-DPS2_ISO="), "resume skips configure");
    }
    {
        // An AppImage mounts at a new /tmp/.mount_* path on every launch.
        TempDir t;
        fs::path mount_a = t.path / "mount_a", mount_b = t.path / "mount_b";
        fs::create_directories(mount_a / "cmake" / "bin");
        fs::create_directories(mount_a / "cmake" / "share" / "cmake-3.30");
        fs::create_directories(mount_a / "tools");
        fs::copy_file(fs::path(LAUNCHER_FAKE_TOOLS) / "cmake", mount_a / "cmake" / "bin" / "cmake");
        fs::copy_file(fs::path(LAUNCHER_FAKE_TOOLS) / "ninja", mount_a / "tools" / "ninja");
        auto mounted = [&](const fs::path& m) {
            BuildConfig c = fake_config(t.path);
            c.appdir = m;
            c.runtime_dir = m / "rt";
            c.tools.cmake = m / "cmake" / "bin" / "cmake";
            c.tools.ninja = m / "tools" / "ninja";
            return c;
        };
        setenv("FAKE_NINJA_MODE", "slow", 1);
        Builder b;
        b.start(mounted(mount_a));
        while (b.state() != Builder::State::Compiling && b.state() != Builder::State::Failed)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        b.cancel();
        finish(b, 5);
        fs::rename(mount_a, mount_b);

        setenv("FAKE_NINJA_MODE", "ok", 1);
        Builder again;
        again.start(mounted(mount_b));
        check(finish(again) == Builder::State::Succeeded, "resume works after the AppImage remounts elsewhere");
        check(!log_has(again, "-DPS2_ISO="), "a remounted resume keeps the configured build");
    }
    {
        TempDir t;
        BuildConfig c = fake_config(t.path);
        setenv("FAKE_NINJA_MODE", "slow", 1);
        Builder b;
        b.start(c);
        while (b.state() != Builder::State::Compiling && b.state() != Builder::State::Failed)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        b.cancel();
        finish(b, 5);

        c.runtime_dir = t.path / "rt2";
        setenv("FAKE_NINJA_MODE", "ok", 1);
        Builder again;
        again.start(c);
        check(finish(again) == Builder::State::Succeeded && log_has(again, "-DPS2_RUNTIME_DIR=" + c.runtime_dir.string()),
              "a changed configuration reconfigures instead of resuming");
    }
    {
        TempDir t;
        BuildConfig c = fake_config(t.path);
        fs::create_directories(c.work_dir.parent_path());
        int fd = open((c.work_dir.parent_path() / "build.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        flock(fd, LOCK_EX);
        setenv("FAKE_NINJA_MODE", "ok", 1);
        Builder b;
        b.start(c);
        check(finish(b) == Builder::State::Failed && log_has(b, "already building"),
              "a second launcher does not build while another one is");
        close(fd);
        Builder again;
        again.start(c);
        check(finish(again) == Builder::State::Succeeded, "the build runs once the other launcher is done");
    }
    unsetenv("FAKE_NINJA_MODE");
}


void test_wizard() {
    using S = Screen;
    using E = Event;
    LaunchState fresh{};
    LaunchState ready{true, true, true, false, true};
    LaunchState built_no_setup{true, true, true, false, false};
    LaunchState iso_only{true, true, false, false, false};
    LaunchState resumable{true, true, false, true, false};
    LaunchState iso_gone{true, false, true, false, true};

    check(initial_screen(fresh).screen == S::Disc, "first launch opens on Disc");
    check(initial_screen(iso_only).screen == S::Check && !initial_screen(iso_only).resume,
          "ISO chosen but not built -> Check");
    check(initial_screen(resumable).screen == S::Check && initial_screen(resumable).resume,
          "an interrupted build -> Check with resume");
    check(initial_screen(built_no_setup).screen == S::QuickSetup, "built but setup unfinished -> QuickSetup");
    check(initial_screen(ready).screen == S::Home, "everything ready -> Home");
    check(initial_screen(iso_gone).screen == S::Disc, "stored ISO missing -> Disc");

    check(next_screen(S::Disc, E::IsoAccepted, fresh) == S::Check, "Disc + accepted -> Check");
    check(next_screen(S::Disc, E::IsoRejected, fresh) == S::Disc, "Disc + rejected stays on Disc");
    check(next_screen(S::Check, E::StartBuild, iso_only) == S::Building, "Check + build -> Building");
    check(next_screen(S::Check, E::Back, iso_only) == S::Disc, "Check + back -> Disc");
    check(next_screen(S::Building, E::BuildSucceeded, iso_only) == S::QuickSetup,
          "first successful build -> QuickSetup");
    check(next_screen(S::Building, E::BuildSucceeded, ready) == S::Home, "rebuild after setup -> Home");
    check(next_screen(S::Building, E::BuildFailed, iso_only) == S::Error, "failed build -> Error");
    check(next_screen(S::Building, E::BuildCancelled, iso_only) == S::Check, "cancelled build -> Check");
    check(next_screen(S::QuickSetup, E::Finish, ready) == S::Home, "QuickSetup + finish -> Home");
    check(next_screen(S::Home, E::OpenSettings, ready) == S::Settings, "Home -> Settings");
    check(next_screen(S::Home, E::OpenControls, ready) == S::Controls, "Home -> Controls");
    check(next_screen(S::Home, E::ChangeDisc, ready) == S::Disc, "Home + change disc -> Disc");
    check(next_screen(S::Settings, E::Close, ready) == S::Home, "Settings + close -> Home");
    check(next_screen(S::Controls, E::Close, ready) == S::Home, "Controls + close -> Home");
    check(next_screen(S::Home, E::GameCrashed, ready) == S::Error, "game crash -> Error");
    check(next_screen(S::Error, E::Retry, iso_only) == S::Check, "Error + retry -> Check");
    check(next_screen(S::Error, E::Retry, ready) == S::Home, "Error + retry with the game built -> Home, no rebuild");
    check(next_screen(S::Disc, E::IsoAccepted, ready) == S::Home, "re-picking an already built disc -> Home");
    check(next_screen(S::Disc, E::IsoAccepted, built_no_setup) == S::QuickSetup,
          "re-picking a built disc before setup -> QuickSetup");
    check(next_screen(S::Error, E::Back, iso_only) == S::Disc, "Error + back -> Disc");
    check(next_screen(S::Home, E::BuildFailed, ready) == S::Home, "unrelated events change nothing");
}


std::string slurp(const fs::path& file) {
    std::ifstream f(file);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

void test_desktop() {
    check(desktop_exec_quote("/a b/T (E).AppImage") == "\"/a b/T (E).AppImage\"", "Exec path is quoted");
    check(desktop_exec_quote("/x/$a\"b`c\\d") == "\"/x/\\\\$a\\\\\"b\\\\`c\\\\\\\\d\"",
          "Exec quoting escapes $ \" ` and backslash");

    TempDir t;
    fs::path icon = t.path / "icon.png";
    write_file(icon, bytes("PNG"));
    fs::path entry = t.path / "share" / "applications" / "transformers-port.desktop";
    fs::create_directories(entry.parent_path());
    write_file(entry, bytes("[Desktop Entry]\nExec=/old/path\n"));
    check(install_desktop_entry("/new/T.AppImage", icon, t.path / "share"), "install_desktop_entry succeeds");
    std::string text = slurp(entry);
    check(text.find("Exec=\"/new/T.AppImage\"") != std::string::npos, "entry points at the AppImage");
    check(text.find("/old/path") == std::string::npos, "a stale entry is rewritten");
    check(text.find("Icon=transformers-port") != std::string::npos && text.find("Type=Application") != std::string::npos,
          "entry has icon and type");
    check(fs::exists(t.path / "share" / "icons" / "hicolor" / "256x256" / "apps" / "transformers-port.png"),
          "icon installed");
}

}  // namespace

int main() {
    try {
        test_sha256();
        test_probe();
        test_sysinfo();
        test_scan();
        test_settings();
        test_paths();
        test_builder();
        test_wizard();
        test_desktop();
    } catch (const std::exception& e) {
        std::printf("FAIL %s\n", e.what());
        return 1;
    }
    std::printf("launcher tests passed\n");
    return 0;
}
