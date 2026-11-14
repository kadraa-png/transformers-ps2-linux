#pragma once
// A child process in its own process group, stdout and stderr merged into
// one pipe that is read line by line without blocking.
#include <sys/types.h>

#include <filesystem>
#include <string>
#include <vector>

namespace launcher {

class Process {
  public:
    Process() = default;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    ~Process();

    bool start(const std::vector<std::string>& argv, const std::filesystem::path& cwd = {});
    // Appends complete lines read so far; false once the pipe hit EOF and all
    // lines were returned.
    bool read_lines(std::vector<std::string>& out);
    bool running();
    void terminate();  // SIGTERM to the group, SIGKILL after 3 s
    int wait();        // exit code, or 128 + signal
    bool killed_by_sigkill() const { return sigkill_; }
    pid_t pid() const { return pid_; }

  private:
    void reap(int status);

    pid_t pid_ = -1;
    int fd_ = -1;
    bool exited_ = false;
    bool sigkill_ = false;
    int code_ = 0;
    std::string partial_;
};

}  // namespace launcher
