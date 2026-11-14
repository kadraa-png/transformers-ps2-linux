#pragma once
// Runs the one-time game build (packaging/game) in the background:
// configure, compile with progress, then move the binary into place.
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace launcher {

struct BuildTools {
    std::filesystem::path cmake = "cmake", ninja = "ninja", python = "python3";
    std::filesystem::path cc, cxx;  // empty: CMake picks the compiler
};

struct BuildConfig {
    std::filesystem::path iso;
    std::filesystem::path runtime_dir;  // .../share/transformers-port
    std::filesystem::path work_dir;     // build_dir(key)
    std::filesystem::path out_dir;      // game_dir(key)
    std::filesystem::path appdir;       // AppImage mount ($APPDIR), empty otherwise
    BuildTools tools;
    unsigned jobs = 1;
};

struct NinjaProgress {
    int done = 0, total = 0;
};

std::optional<NinjaProgress> parse_ninja_line(std::string_view line);  // "[214/346] ..."
int eta_seconds(int done, int total, double elapsed_s);                 // -1 while done == 0

class Builder {
  public:
    enum class State { Idle, Configuring, Compiling, Finishing, Succeeded, Failed, Cancelled };

    ~Builder();
    // Starts the worker thread.  An existing work_dir is reused (resume).
    void start(const BuildConfig& config);
    State state() const { return state_; }
    NinjaProgress progress() const;
    double elapsed() const;
    void cancel();  // keeps work_dir so the next start resumes
    void join();
    std::vector<std::string> log_tail(size_t n) const;
    std::filesystem::path log_path() const;  // work_dir/build.log; out_dir/build.log after success

  private:
    void run(BuildConfig config);
    int run_step(const std::vector<std::string>& argv, bool* sigkilled);
    void log(const std::string& line);

    std::thread worker_;
    std::atomic<State> state_{State::Idle};
    std::atomic<bool> cancel_{false};
    mutable std::mutex mu_;
    std::deque<std::string> tail_;
    NinjaProgress progress_;
    std::filesystem::path log_path_;
    std::chrono::steady_clock::time_point started_;
    FILE* log_file_ = nullptr;
};

}  // namespace launcher
