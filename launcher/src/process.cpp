#include "process.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <thread>

extern char** environ;

namespace launcher {

Process::~Process() {
    if (pid_ > 0 && !exited_) terminate();
    if (fd_ >= 0) close(fd_);
}

bool Process::start(const std::vector<std::string>& argv, const std::filesystem::path& cwd) {
    if (argv.empty()) return false;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return false;

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (!cwd.empty()) posix_spawn_file_actions_addchdir_np(&fa, cwd.c_str());
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);  // own group, so terminate() reaches grandchildren

    std::vector<char*> args;
    for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    int rc = posix_spawnp(&pid_, args[0], &fa, &attr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        pid_ = -1;
        return false;
    }
    fd_ = fds[0];
    fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) | O_NONBLOCK);
    exited_ = sigkill_ = false;
    code_ = 0;
    partial_.clear();
    return true;
}

bool Process::read_lines(std::vector<std::string>& out) {
    if (fd_ < 0) return false;
    char buf[4096];
    for (;;) {
        ssize_t n = read(fd_, buf, sizeof buf);
        if (n > 0) {
            partial_.append(buf, size_t(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        bool eof = n == 0;
        size_t start = 0;
        for (size_t nl; (nl = partial_.find('\n', start)) != std::string::npos; start = nl + 1)
            out.push_back(partial_.substr(start, nl - start));
        partial_.erase(0, start);
        if (eof) {
            if (!partial_.empty()) out.push_back(partial_);
            partial_.clear();
            close(fd_);
            fd_ = -1;
            return false;
        }
        return true;  // EAGAIN: nothing more for now
    }
}

void Process::reap(int status) {
    exited_ = true;
    if (WIFEXITED(status)) code_ = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) {
        code_ = 128 + WTERMSIG(status);
        sigkill_ = WTERMSIG(status) == SIGKILL;
    }
}

bool Process::running() {
    if (pid_ <= 0 || exited_) return false;
    int status;
    if (waitpid(pid_, &status, WNOHANG) == pid_) reap(status);
    return !exited_;
}

void Process::terminate() {
    if (!running()) return;
    kill(-pid_, SIGTERM);
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (running() && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (running()) {
        kill(-pid_, SIGKILL);
        wait();
    }
}

int Process::wait() {
    if (pid_ <= 0) return -1;
    if (!exited_) {
        int status;
        while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
        reap(status);
    }
    return code_;
}

}  // namespace launcher
