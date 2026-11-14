// Built-in sampling profiler (no perf on the target machine).
// PS2_PROFILE=1 samples the program counter of whichever thread is running
// (ITIMER_PROF, 1 kHz of consumed CPU time) and prints the hottest functions
// per thread at exit.  Generated guest functions show up as f_<guest addr>.
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cxxabi.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <map>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>
#include <vector>
#include <chrono>
#include <mutex>
#include <thread>

namespace {

struct Sample {
    void* pc;
    int tid;
};
constexpr size_t kMaxSamples = 1u << 22;
Sample* g_samples = nullptr;
std::atomic<size_t> g_count{0};
timespec g_record_after{};   // CLOCK_MONOTONIC; PS2_PROFILE=<N> skips the first N seconds

void on_prof(int, siginfo_t*, void* uc) {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);   // async-signal-safe
    if (now.tv_sec < g_record_after.tv_sec ||
        (now.tv_sec == g_record_after.tv_sec && now.tv_nsec < g_record_after.tv_nsec))
        return;
    size_t i = g_count.fetch_add(1, std::memory_order_relaxed);
    if (i >= kMaxSamples) return;
    auto* ctx = static_cast<ucontext_t*>(uc);
    g_samples[i].pc = reinterpret_cast<void*>(ctx->uc_mcontext.gregs[REG_RIP]);
    g_samples[i].tid = static_cast<int>(syscall(SYS_gettid));
}

// The executable is linked without -rdynamic, so dladdr() cannot name its
// functions: read the static .symtab of /proc/self/exe instead.
struct ExeSym {
    uintptr_t lo, hi;
    const char* name;
};
std::vector<ExeSym> g_exe_syms;
uintptr_t g_exe_lo = 0, g_exe_hi = 0;

void load_exe_symbols() {
    int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    struct stat st{};
    if (fstat(fd, &st) != 0) { close(fd); return; }
    void* map = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return;   // stays mapped: names point into it
    const auto* base = static_cast<const unsigned char*>(map);
    const auto* eh = reinterpret_cast<const Elf64_Ehdr*>(base);
    const auto* sh = reinterpret_cast<const Elf64_Shdr*>(base + eh->e_shoff);
    uintptr_t bias = 0;
    if (eh->e_type == ET_DYN) {
        dl_iterate_phdr([](dl_phdr_info* info, size_t, void* out) {
            *static_cast<uintptr_t*>(out) = info->dlpi_addr;
            return 1;   // first entry = the executable
        }, &bias);
    }
    for (int i = 0; i < eh->e_shnum; ++i) {
        if (sh[i].sh_type != SHT_SYMTAB) continue;
        const auto* syms = reinterpret_cast<const Elf64_Sym*>(base + sh[i].sh_offset);
        const char* strtab = reinterpret_cast<const char*>(base + sh[sh[i].sh_link].sh_offset);
        const size_t count = sh[i].sh_size / sizeof(Elf64_Sym);
        for (size_t k = 0; k < count; ++k) {
            if (ELF64_ST_TYPE(syms[k].st_info) != STT_FUNC || !syms[k].st_value) continue;
            const uintptr_t lo = bias + syms[k].st_value;
            g_exe_syms.push_back({lo, lo + std::max<uintptr_t>(syms[k].st_size, 1), strtab + syms[k].st_name});
        }
    }
    std::sort(g_exe_syms.begin(), g_exe_syms.end(),
              [](const ExeSym& a, const ExeSym& b) { return a.lo < b.lo; });
    if (!g_exe_syms.empty()) { g_exe_lo = g_exe_syms.front().lo; g_exe_hi = g_exe_syms.back().hi; }
}

std::string demangle(const char* name) {
    int status = 0;
    char* dem = abi::__cxa_demangle(name, nullptr, nullptr, &status);
    std::string s = (status == 0 && dem) ? dem : name;
    free(dem);
    auto paren = s.find('(');
    if (paren != std::string::npos) s.resize(paren);
    return s;
}

std::string symbol(void* pc) {
    const auto a = reinterpret_cast<uintptr_t>(pc);
    if (a >= g_exe_lo && a < g_exe_hi) {
        auto it = std::upper_bound(g_exe_syms.begin(), g_exe_syms.end(), a,
                                   [](uintptr_t v, const ExeSym& s) { return v < s.lo; });
        if (it != g_exe_syms.begin() && a < (--it)->hi) return demangle(it->name);
    }
    Dl_info info{};
    if (!dladdr(pc, &info)) {
        // Not in any loaded object (JIT code, driver-generated code): name the
        // /proc/self/maps region so these samples are not one opaque bucket.
        static std::map<uintptr_t, std::string> by_page;
        auto cached = by_page.find(a >> 12);
        if (cached != by_page.end()) return cached->second;
        if (FILE* maps = fopen("/proc/self/maps", "r")) {
            char line[512];
            std::string region = "??";
            while (fgets(line, sizeof line, maps)) {
                unsigned long lo = 0, hi = 0;
                char perms[8] = {};
                int name_at = 0;
                if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %n", &lo, &hi, perms, &name_at) < 3) continue;
                if (a < lo || a >= hi) continue;
                std::string name = line + name_at;
                while (!name.empty() && (name.back() == '\n' || name.back() == ' ')) name.pop_back();
                region = "[anon " + std::string(perms) + (name.empty() ? "" : " " + name) + "]";
                break;
            }
            fclose(maps);
            by_page[a >> 12] = region;
            return region;
        }
        return "??";
    }
    if (!info.dli_sname) {
        const char* file = info.dli_fname ? info.dli_fname : "??";
        const char* slash = strrchr(file, '/');
        return std::string(slash ? slash + 1 : file) + "+?";
    }
    return demangle(info.dli_sname);
}

void report() {
    // Runs once: main calls it right after the game loop, the quit watchdog
    // before it force-exits, atexit as a fallback.  A second caller waits for
    // the first to finish printing.
    static std::mutex once;
    static bool done = false;
    std::lock_guard lock(once);
    if (done) return;
    done = true;
    itimerval stop{};
    setitimer(ITIMER_PROF, &stop, nullptr);
    size_t n = std::min(g_count.load(), kMaxSamples);
    if (!n) return;
    load_exe_symbols();
    std::map<int, std::map<std::string, size_t>> per_thread;
    std::map<int, size_t> totals;
    for (size_t i = 0; i < n; ++i) {
        per_thread[g_samples[i].tid][symbol(g_samples[i].pc)]++;
        totals[g_samples[i].tid]++;
    }
    // PS2_PROFILE_RAW=<file>: "tid offset" per sample (offset relative to the
    // executable's load address) for line-level attribution with addr2line.
    if (const char* raw = getenv("PS2_PROFILE_RAW")) {
        uintptr_t bias = 0;
        dl_iterate_phdr([](dl_phdr_info* info, size_t, void* out) {
            *static_cast<uintptr_t*>(out) = info->dlpi_addr;
            return 1;
        }, &bias);
        if (FILE* f = fopen(raw, "w")) {
            for (size_t i = 0; i < n; ++i) {
                const auto a = reinterpret_cast<uintptr_t>(g_samples[i].pc);
                if (a >= g_exe_lo && a < g_exe_hi) fprintf(f, "%d %lx\n", g_samples[i].tid, (unsigned long)(a - bias));
                else fprintf(f, "%d A%lx\n", g_samples[i].tid, (unsigned long)a);   // see <raw>.maps
            }
            fclose(f);
        }
    }
    // PS2_PROFILE_OUT=<file> also writes the report there (survives a
    // terminal pipe such as `| tee` dying on Ctrl-C).
    FILE* outs[2] = {stderr, nullptr};
    if (const char* path = getenv("PS2_PROFILE_OUT")) outs[1] = fopen(path, "w");
    for (FILE* out : outs) {
    if (!out) continue;
    fprintf(out, "\n=== PS2_PROFILE: %zu samples ===\n", n);
    for (auto& [tid, syms] : per_thread) {
        std::vector<std::pair<size_t, std::string>> v;
        for (auto& [name, c] : syms) v.emplace_back(c, name);
        std::sort(v.rbegin(), v.rend());
        char comm[64] = "?";
        char comm_path[64];
        snprintf(comm_path, sizeof comm_path, "/proc/self/task/%d/comm", tid);
        if (FILE* c = fopen(comm_path, "r")) {
            if (fgets(comm, sizeof comm, c)) comm[strcspn(comm, "\n")] = 0;
            fclose(c);
        }
        fprintf(out, "--- thread %d (%s): %zu samples (%.1f%%)\n", tid, comm, totals[tid], 100.0 * totals[tid] / n);
        for (size_t k = 0; k < v.size() && k < 25; ++k)
            fprintf(out, "  %6.2f%%  %s\n", 100.0 * v[k].first / totals[tid], v[k].second.c_str());
    }
    if (out != stderr) fclose(out);
    }
}

} // namespace

// Host pc -> function name (generated guest functions are f_<addr>).
std::string profiler_symbolize(void* pc) {
    static std::once_flag loaded;
    std::call_once(loaded, load_exe_symbols);
    return symbol(pc);
}

void profiler_start() {
    // PS2_PROFILE=1 samples from launch; PS2_PROFILE=<N> (N > 1) starts after N seconds.
    const char* e = getenv("PS2_PROFILE");
    if (!e || !*e || *e == '0') return;
    const long delay = strtol(e, nullptr, 10);
    clock_gettime(CLOCK_MONOTONIC, &g_record_after);
    if (delay > 1) g_record_after.tv_sec += delay;
    g_samples = static_cast<Sample*>(calloc(kMaxSamples, sizeof(Sample)));
    // A dead `| tee` (Ctrl-C) must not kill the game before the report.
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa{};
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, nullptr);
    itimerval tv{};
    tv.it_interval.tv_usec = 1000;
    tv.it_value.tv_usec = 1000;
    setitimer(ITIMER_PROF, &tv, nullptr);
    atexit(report);
    // Shared libraries (the GL driver) may be unloaded before the atexit
    // report: keep a copy of the address map from while the game runs so
    // absolute "A<addr>" samples in the raw dump can be attributed offline.
    if (const char* raw = getenv("PS2_PROFILE_RAW")) {
        std::string out = std::string(raw) + ".maps";
        const long wait_s = (delay > 1 ? delay : 0) + 10;
        std::thread([out, wait_s] {
            std::this_thread::sleep_for(std::chrono::seconds(wait_s));
            FILE* in = fopen("/proc/self/maps", "r");
            FILE* o = fopen(out.c_str(), "w");
            char buf[4096];
            size_t k;
            while (in && o && (k = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, k, o);
            if (in) fclose(in);
            if (o) fclose(o);
        }).detach();
    }
}

void profiler_stop_and_report() {
    if (!g_samples) return;
    report();
}
