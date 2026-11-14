// Fatal-signal reporter: prints the faulting host/guest address and a host
// backtrace (generated functions are named f_<guest address>).
#include "ps2/ee_mem.h"
#include "ps2/iop.h"
#include "ps2/spu2.h"
#include "ps2/gs.h"
#include "kernel/kernel.h"
#include "hw/hw.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <algorithm>
#include <atomic>
#include <initializer_list>
#include <map>
#include <string>
#include <sys/mman.h>
#include <ucontext.h>
#include <pthread.h>
#include <unistd.h>

namespace {

// Debug write watchpoint (PS2_CONTROL "watch <hex guest addr>"): the 4 KiB
// page of EE_BASE + addr becomes read-only; a write to it faults, the store
// pc is recorded if it hits the watched word, and the faulting instruction is
// single-stepped (TF) with the page writable before it is protected again.
// Only the kuseg mapping is protected; stores through RAM mirrors are missed.
std::atomic<uintptr_t> g_watch_page{0};
uintptr_t g_watch_lo = 0;
constexpr unsigned kWatchHits = 1 << 16;
void* g_watch_hits[kWatchHits];
std::atomic<unsigned> g_watch_count{0};
thread_local bool t_watch_step = false;

bool watch_fault(siginfo_t* si, void* ucv) {
    const uintptr_t page = g_watch_page.load(std::memory_order_relaxed);
    const auto a = reinterpret_cast<uintptr_t>(si->si_addr);
    if (!page || (a & ~uintptr_t(4095)) != page) return false;
    auto* uc = static_cast<ucontext_t*>(ucv);
    if (a >= g_watch_lo && a < g_watch_lo + 4) {
        const unsigned i = g_watch_count.fetch_add(1, std::memory_order_relaxed);
        if (i < kWatchHits) g_watch_hits[i] = reinterpret_cast<void*>(uc->uc_mcontext.gregs[REG_RIP]);
    }
    mprotect(reinterpret_cast<void*>(page), 4096, PROT_READ | PROT_WRITE);
    uc->uc_mcontext.gregs[REG_EFL] |= 0x100;   // trap after this one store
    t_watch_step = true;
    return true;
}

void on_trap(int, siginfo_t*, void* ucv) {
    if (!t_watch_step) return;
    t_watch_step = false;
    auto* uc = static_cast<ucontext_t*>(ucv);
    uc->uc_mcontext.gregs[REG_EFL] &= ~0x100ll;
    if (const uintptr_t page = g_watch_page.load(std::memory_order_relaxed))
        mprotect(reinterpret_cast<void*>(page), 4096, PROT_READ);
}

void on_fatal(int sig, siginfo_t* si, void* ucv) {
    if (sig == SIGSEGV && watch_fault(si, ucv)) return;
    char buf[256];
    uintptr_t a = (uintptr_t)si->si_addr;
    uintptr_t base = (uintptr_t)EE_BASE;
    int n;
    if (a >= base && a < base + (1ull << 32))
        n = snprintf(buf, sizeof(buf), "\n*** signal %d (%s): guest address 0x%08lx\n", sig, strsignal(sig),
                     (unsigned long)(a - base));
    else
        n = snprintf(buf, sizeof(buf), "\n*** signal %d (%s): host address %p\n", sig, strsignal(sig), si->si_addr);
    if (n > 0) (void)!write(2, buf, (size_t)n);
    void* frames[64];
    int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, 2);
    signal(sig, SIG_DFL);
    raise(sig);
}
pthread_t g_emu_thread;

// SIGQUIT (Ctrl-\): print where the emulation thread is, then keep running.
void on_quit(int) {
    if (!pthread_equal(pthread_self(), g_emu_thread)) {
        pthread_kill(g_emu_thread, SIGQUIT);
        return;
    }
    static const char msg[] = "\n*** SIGQUIT: emulation thread backtrace\n";
    (void)!write(2, msg, sizeof(msg) - 1);
    void* frames[64];
    int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, 2);
    kernel::debug_dump();
    iop::debug_dump();   // not async-signal-safe; diagnostics only
    spu2::debug_dump();
    gs::debug_dump();
    hw::debug_dump();
}

} // namespace

void crash_handler_install() {
    g_emu_thread = pthread_self();
    struct sigaction sq{};
    sq.sa_handler = on_quit;
    sq.sa_flags = SA_ONSTACK | SA_RESTART;
    sigemptyset(&sq.sa_mask);
    sigaction(SIGQUIT, &sq, nullptr);
    static char altstack[1 << 16];
    stack_t ss{};
    ss.ss_sp = altstack;
    ss.ss_size = sizeof(altstack);
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = on_fatal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int s : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(s, &sa, nullptr);
    struct sigaction st{};
    st.sa_sigaction = on_trap;
    st.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&st.sa_mask);
    sigaction(SIGTRAP, &st, nullptr);
}

std::string profiler_symbolize(void* pc);

// Emulation thread only.  addr 0 stops watching.
void debug_watch(u32 addr) {
    if (const uintptr_t old = g_watch_page.exchange(0))
        mprotect(reinterpret_cast<void*>(old), 4096, PROT_READ | PROT_WRITE);
    g_watch_count = 0;
    if (!addr) return;
    g_watch_lo = reinterpret_cast<uintptr_t>(EE_BASE + addr);
    const uintptr_t page = g_watch_lo & ~uintptr_t(4095);
    g_watch_page = page;
    mprotect(reinterpret_cast<void*>(page), 4096, PROT_READ);
    fprintf(stderr, "[watch] watching guest 0x%08x\n", addr);
}

void debug_watch_report() {
    const unsigned n = std::min(g_watch_count.load(), kWatchHits);
    std::map<std::string, unsigned> by_fn;
    for (unsigned i = 0; i < n; ++i) {
        char pc[32];
        snprintf(pc, sizeof pc, " (pc %p)", g_watch_hits[i]);
        ++by_fn[profiler_symbolize(g_watch_hits[i]) + pc];
    }
    fprintf(stderr, "[watch] %u stores to the watched word\n", g_watch_count.load());
    for (auto& [name, count] : by_fn) fprintf(stderr, "[watch]   %6u  %s\n", count, name.c_str());
}
