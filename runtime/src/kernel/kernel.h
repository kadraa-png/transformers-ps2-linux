#pragma once
// EE kernel HLE (threads, semaphores, interrupts, alarms, syscalls).
#include "ps2/ee_context.h"

#include <string>
#include <vector>

// g_ee_events bits
enum : u32 {
    EV_TIMER = 1u << 0,   // a host-time deadline passed (vblank, alarm, IOP timer)
    EV_INTR = 1u << 1,    // an INTC/DMAC line became pending
    EV_RESCHED = 1u << 2, // reschedule requested
    EV_IOP = 1u << 3,     // the IOP received work (SIF traffic): run it at the next checkpoint
    EV_QUIT = 1u << 31,
};

namespace kernel {

struct Config {
    std::vector<std::string> argv;  // guest argv (argv[0] = boot path)
    bool pal = true;
    int language = 1;               // OSD language: 0 JP 1 EN 2 FR 3 ES 4 DE 5 IT
    bool widescreen = false;        // OSD screen type 16:9 (the game then renders widescreen)
};

void init(const Config& cfg);
// Runs the boot program from `entry` on the main thread; returns when the
// guest exits or the user quits.
int run(u32 entry);

void post_events(u32 bits);       // thread-safe
void request_quit();

// Interrupt plumbing (emulation thread).
void interrupt_state_changed();   // I_STAT/I_MASK/D_STAT changed: re-evaluate delivery

// Calls a guest function from the runtime on the current fiber with a
// scratch context; returns v0.
u32 call_guest(u32 func, u32 gp, u32 a0, u32 a1, u32 a2, u32 a3);

u64 cycles();                     // EE cycle counter (294.912 MHz) from host time
u32 vblank_count();
void count_poll();                // guest read COP0 Count (idles the host during spin-waits)
void guest_waiting();             // guest provably spinning: run the IOP or sleep briefly
void debug_dump();                // EE thread states to stderr (SIGQUIT diagnostics)

} // namespace kernel
