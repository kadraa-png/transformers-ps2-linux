#include "builder.h"

#include "process.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <charconv>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace launcher {

namespace {
constexpr size_t kTailLines = 200;
}

std::optional<NinjaProgress> parse_ninja_line(std::string_view line) {
    if (line.size() < 5 || line[0] != '[') return std::nullopt;
    size_t slash = line.find('/'), close = line.find(']');
    if (slash == std::string_view::npos || close == std::string_view::npos || slash > close) return std::nullopt;
    NinjaProgress p;
    auto a = std::from_chars(line.data() + 1, line.data() + slash, p.done);
    auto b = std::from_chars(line.data() + slash + 1, line.data() + close, p.total);
    if (a.ec != std::errc() || a.ptr != line.data() + slash || b.ec != std::errc() ||
        b.ptr != line.data() + close || p.total <= 0)
        return std::nullopt;
    return p;
}

int eta_seconds(int done, int total, double elapsed_s) {
    if (done <= 0 || total <= 0) return -1;
    return int(elapsed_s * (total - done) / done + 0.5);
}

Builder::~Builder() {
    cancel();
    join();
}

void Builder::start(const BuildConfig& config) {
    join();
    cancel_ = false;
    {
        std::lock_guard lock(mu_);
        tail_.clear();
        progress_ = {};
        log_path_ = config.work_dir / "build.log";
        started_ = std::chrono::steady_clock::now();
    }
    state_ = State::Configuring;
    worker_ = std::thread([this, config] { run(config); });
}

NinjaProgress Builder::progress() const {
    std::lock_guard lock(mu_);
    return progress_;
}

double Builder::elapsed() const {
    std::lock_guard lock(mu_);
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
}

void Builder::cancel() { cancel_ = true; }

void Builder::join() {
    if (worker_.joinable()) worker_.join();
}

std::vector<std::string> Builder::log_tail(size_t n) const {
    std::lock_guard lock(mu_);
    size_t from = tail_.size() > n ? tail_.size() - n : 0;
    return std::vector<std::string>(tail_.begin() + long(from), tail_.end());
}

fs::path Builder::log_path() const {
    std::lock_guard lock(mu_);
    return log_path_;
}

void Builder::log(const std::string& line) {
    std::lock_guard lock(mu_);
    if (auto p = parse_ninja_line(line)) progress_ = *p;
    tail_.push_back(line);
    if (tail_.size() > kTailLines) tail_.pop_front();
    if (log_file_) {
        std::fputs(line.c_str(), log_file_);
        std::fputc('\n', log_file_);
        std::fflush(log_file_);
    }
}

// Runs one tool to completion, logging its output.  Returns its exit code;
// -1 if it could not start or was cancelled.  *sigkilled (initially false) is
// set when the tool or one of its compilers was SIGKILLed.
int Builder::run_step(const std::vector<std::string>& argv, bool* sigkilled) {
    Process p;
    if (!p.start(argv)) {
        log("could not start " + argv[0]);
        return -1;
    }
    std::vector<std::string> lines;
    for (;;) {
        lines.clear();
        bool open = p.read_lines(lines);
        for (const std::string& l : lines) {
            // The OOM killer hits the compiler, not ninja: gcc reports it and
            // ninja exits 1.
            if (sigkilled && l.find("Killed signal terminated program") != std::string::npos) *sigkilled = true;
            log(l);
        }
        if (cancel_) {
            p.terminate();
            return -1;
        }
        if (!open) break;
        if (lines.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    int code = p.wait();
    if (sigkilled && p.killed_by_sigkill()) *sigkilled = true;
    return code;
}

namespace {
// Held for the whole build so a second launcher window can't build into the
// same work dir.
struct BuildLock {
    int fd = -1;
    explicit BuildLock(const fs::path& file) {
        fd = open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) != 0) {
            close(fd);
            fd = -1;
        }
    }
    ~BuildLock() {
        if (fd >= 0) close(fd);
    }
};
}  // namespace

void Builder::run(BuildConfig c) {
    std::error_code ec;
    fs::create_directories(c.work_dir.parent_path(), ec);
    BuildLock held(c.work_dir.parent_path() / "build.lock");
    if (held.fd < 0) {
        log("=== another launcher window is already building the game");
        state_ = State::Failed;
        return;
    }
    fs::create_directories(c.work_dir, ec);
    {
        std::lock_guard lock(mu_);
        log_file_ = std::fopen(log_path_.c_str(), "a");
    }
    std::time_t now = std::time(nullptr);
    char stamp[64];
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    log(std::string("=== build started ") + stamp + ", " + std::to_string(c.jobs) + " jobs");

    auto finish = [&](State s) {
        {
            std::lock_guard lock(mu_);
            if (log_file_) std::fclose(log_file_);
            log_file_ = nullptr;
        }
        state_ = s;
    };
    auto stopped = [&] { return cancel_ ? State::Cancelled : State::Failed; };

    // An AppImage is mounted at a new path on every launch.  Reach it through
    // a link in the work dir so a resumed build sees the paths it was
    // configured with (compilers, Python, the runtime).
    if (!c.appdir.empty()) {
        fs::path link = c.work_dir / "appdir", tmp = c.work_dir / "appdir.new";
        fs::remove(tmp, ec);
        fs::create_directory_symlink(c.appdir, tmp, ec);
        fs::rename(tmp, link, ec);
        if (ec) {
            log("could not link " + link.string() + ": " + ec.message());
            return finish(State::Failed);
        }
        // CMake writes its own resolved path (and its modules') into
        // build.ninja, which a link can't hide, so it runs from a copy.
        fs::path rel = c.tools.cmake.lexically_relative(c.appdir);
        if (!rel.empty() && *rel.begin() != "..") {
            fs::path root = c.tools.cmake.parent_path().parent_path();  // <root>/bin/cmake
            fs::path copy = c.work_dir / "cmake", tmp = c.work_dir / "cmake.new";
            if (!fs::exists(copy / "bin" / "cmake")) {
                fs::remove_all(tmp, ec);
                fs::create_directories(tmp / "bin", ec);
                if (!ec) fs::copy(root / "share", tmp / "share", fs::copy_options::recursive, ec);
                if (!ec) fs::copy_file(c.tools.cmake, tmp / "bin" / "cmake", ec);
                if (!ec) fs::rename(tmp, copy, ec);
                if (ec) {
                    log("could not copy cmake into " + copy.string() + ": " + ec.message());
                    return finish(State::Failed);
                }
            }
            c.tools.cmake = copy / "bin" / "cmake";
        }
        for (fs::path* p : {&c.runtime_dir, &c.tools.cmake, &c.tools.ninja, &c.tools.python, &c.tools.cc, &c.tools.cxx}) {
            fs::path rel = p->lexically_relative(c.appdir);
            if (!p->empty() && !rel.empty() && *rel.begin() != "..") *p = link / rel;
        }
    }

    std::vector<std::string> cfg = {
        c.tools.cmake.string(), "-S", (c.runtime_dir / "src" / "packaging" / "game").string(),
        "-B", c.work_dir.string(), "-G", "Ninja",
        "-DCMAKE_MAKE_PROGRAM=" + c.tools.ninja.string(),
        "-DPython3_EXECUTABLE=" + c.tools.python.string(),
        "-DPS2_ISO=" + c.iso.string(),
        "-DPS2_RUNTIME_DIR=" + c.runtime_dir.string(),
        "-DCMAKE_BUILD_TYPE=Release"};
    if (!c.tools.cc.empty()) cfg.push_back("-DCMAKE_C_COMPILER=" + c.tools.cc.string());
    if (!c.tools.cxx.empty()) cfg.push_back("-DCMAKE_CXX_COMPILER=" + c.tools.cxx.string());
    std::string args;
    for (const std::string& a : cfg) args += a + "\n";

    // Resume only a build configured with exactly these arguments.
    fs::path args_file = c.work_dir / "configure.args";
    std::string old_args;
    if (std::ifstream f{args_file}) old_args.assign(std::istreambuf_iterator<char>(f), {});
    bool configured = fs::exists(c.work_dir / "build.ninja");
    if (configured && old_args != args) {
        log("=== the build settings changed; configuring again");
        fs::remove(c.work_dir / "build.ninja", ec);
        fs::remove(c.work_dir / "CMakeCache.txt", ec);
        fs::remove_all(c.work_dir / "CMakeFiles", ec);
        configured = false;
    }
    if (!configured) {
        if (run_step(cfg, nullptr) != 0) {
            // A half-written build.ninja would make the next start skip configure.
            fs::remove(c.work_dir / "build.ninja", ec);
            return finish(stopped());
        }
        std::ofstream(args_file) << args;
    }

    state_ = State::Compiling;
    bool sigkilled = false;
    unsigned jobs = c.jobs ? c.jobs : 1;
    int code = run_step({c.tools.ninja.string(), "-C", c.work_dir.string(), "-j" + std::to_string(jobs),
                         "transformers"}, &sigkilled);
    if (code != 0 && sigkilled && !cancel_) {
        log("=== the compiler was killed (out of memory?); retrying with one job");
        code = run_step({c.tools.ninja.string(), "-C", c.work_dir.string(), "-j1", "transformers"}, nullptr);
    }
    if (code != 0) return finish(stopped());

    state_ = State::Finishing;
    fs::path staging = c.out_dir;
    staging += ".new";
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    fs::copy_file(c.work_dir / "transformers", staging / "transformers", ec);
    if (ec) {
        log("could not copy the game binary: " + ec.message());
        return finish(State::Failed);
    }
    fs::permissions(staging / "transformers", fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add, ec);
    log("=== build finished");
    {
        std::lock_guard lock(mu_);
        if (log_file_) std::fclose(log_file_);
        log_file_ = nullptr;
    }
    fs::copy_file(c.work_dir / "build.log", staging / "build.log", ec);
    fs::remove_all(c.out_dir, ec);
    fs::rename(staging, c.out_dir, ec);
    if (ec) {
        log("could not install the game: " + ec.message());
        return finish(State::Failed);
    }
    fs::remove_all(c.work_dir, ec);
    {
        std::lock_guard lock(mu_);
        log_path_ = c.out_dir / "build.log";
    }
    state_ = State::Succeeded;
}

}  // namespace launcher
