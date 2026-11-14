#pragma once
// EE CPU register context and the runtime entry points used by recompiled code.
//
// Every guest thread (and every interrupt-handler invocation) owns one
// EEContext.  VU0 registers are *not* here: they are global hardware state
// (g_vu0 in ps2/vu.h), exactly like on the real machine.
#include "ps2/types.h"
#include "ps2/ee_mem.h"

// Sign-extend a 32-bit value to the 64-bit register width.
#define SE32(x) ((u64)(s64)(s32)(u32)(x))

union fpr_t {
    float f;
    u32 u;
    s32 s;
};

struct alignas(64) EEContext {
    qword r[32];     // GPRs; r[0] is kept zero by the code generator
    qword hi, lo;    // hi.d[0]=HI hi.d[1]=HI1, lo.d[0]=LO lo.d[1]=LO1
    u64 sa;          // shift-amount register (raw value as written by MTSA)
    fpr_t fpr[32];
    fpr_t facc;      // FPU accumulator (MADD.S & co)
    u32 fcr31;       // FPU control/status; bit 23 = condition flag C
    u32 cop0_status; // only IE/EXL/ERL/EIE-relevant bits are modelled
    u32 cop0_epc;    // EPC / ErrorEPC for the kernel-mode ERET helpers in libkernel
    u32 cop0_errorepc;
    u32 pc;          // last pc published to the runtime (syscall/dispatch/checkpoint)
    u32 intr_depth;  // >0 while running inside an interrupt handler
    void* thread;    // owning EE thread object (runtime private)
};

typedef void (*EEFunc)(EEContext*);

// ---- provided by the generated code -------------------------------------------
struct EEFuncEntry {
    u32 addr;
    EEFunc fn;
};
extern const EEFuncEntry g_ee_func_table[];
extern const u32 g_ee_func_count;

// ---- provided by the runtime ------------------------------------------------
EEFunc ee_lookup(u32 addr);                          // nullptr if unknown
void ee_call(EEContext* c, u32 target);              // jalr / runtime call into guest
PS2_NORETURN void ee_unknown_target(EEContext* c, u32 target, u32 from_pc);
void ee_syscall(EEContext* c, u32 pc, u32 code);     // SYSCALL (number in v1)
void ee_break(EEContext* c, u32 pc, u32 code);       // BREAK
void ee_trap(EEContext* c, u32 pc);                  // conditional trap taken
u32 ee_mfc0(EEContext* c, u32 reg);
void ee_mtc0(EEContext* c, u32 reg, u32 v);
void ee_ei(EEContext* c);
void ee_di(EEContext* c);
void ee_eret(EEContext* c, u32 pc);
void ee_cache(EEContext* c, u32 op, u32 addr);
void ee_unhandled_insn(EEContext* c, u32 pc, u32 word);

// Pending asynchronous work (interrupts, thread switch requests).  Host threads
// set bits; recompiled code polls at loop back-edges and the runtime polls at
// syscalls / hardware accesses.
extern volatile u32 g_ee_events;
void ee_service_events(EEContext* c, u32 pc);
#define EE_CHECKPOINT(c, pcv)                         \
    do {                                              \
        if (PS2_UNLIKELY(g_ee_events)) {              \
            ee_service_events((c), (pcv));            \
        }                                             \
    } while (0)
// Back-edge of a side-effect-free polling loop (see tools/recomp_ee.py): after
// a burst of iterations with nothing pending the runtime idles the host until
// the next event or deadline.
extern u32 g_ee_spin;
void ee_spin_wait(EEContext* c);
#define EE_SPIN_CHECKPOINT(c, pcv)                    \
    do {                                              \
        if (PS2_UNLIKELY(g_ee_events)) {              \
            ee_service_events((c), (pcv));            \
        } else if (PS2_UNLIKELY(++g_ee_spin >= 64)) { \
            ee_spin_wait(c);                          \
        }                                             \
    } while (0)
