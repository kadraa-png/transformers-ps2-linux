// Guest function lookup, dynamic calls and the CPU-level helpers that the
// generated code calls (COP0, traps, cache ops).
#include "ps2/ee_context.h"
#include "ps2/log.h"

#include "hw/hw.h"
#include "kernel/kernel.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace {

constexpr u32 kLookupBase = 0x00100000;
constexpr u32 kLookupEnd = 0x00700000;
std::vector<EEFunc> g_lookup;   // direct-mapped by (addr - base) / 4

void build_lookup() {
    g_lookup.assign((kLookupEnd - kLookupBase) / 4, nullptr);
    for (u32 i = 0; i < g_ee_func_count; ++i) {
        u32 a = g_ee_func_table[i].addr;
        if (a >= kLookupBase && a < kLookupEnd) g_lookup[(a - kLookupBase) >> 2] = g_ee_func_table[i].fn;
    }
}

std::unordered_set<u32> g_warned;
bool warn_once(u32 key) { return g_warned.insert(key).second; }

} // namespace

u32 g_ee_trace[4096];
u32 g_ee_trace_pos = 0;
u32 g_ee_break = 0xFFFFFFFFu;

void ee_dispatch_init() {
    build_lookup();
    if (const char* b = getenv("PS2_BREAK")) g_ee_break = (u32)strtoul(b, nullptr, 16);
}

void ee_dump_trace(int count) {
    fprintf(stderr, "--- last %d guest function entries (oldest first) ---\n", count);
    for (int i = count; i > 0; --i) {
        fprintf(stderr, " %08x", g_ee_trace[(g_ee_trace_pos - (u32)i) & 4095]);
        if ((count - i) % 16 == 15) fputc('\n', stderr);
    }
    fputc('\n', stderr);
}

void ee_break_hit(EEContext* c, u32 addr) {
    fprintf(stderr, "\n*** PS2_BREAK %08x hit: a0=%08x a1=%08x a2=%08x a3=%08x ra=%08x sp=%08x\n", addr,
            c->r[4].w[0], c->r[5].w[0], c->r[6].w[0], c->r[7].w[0], c->r[31].w[0], c->r[29].w[0]);
    for (int r = 4; r < 8; ++r) {  // show printable strings behind pointer arguments
        u32 p = c->r[r].w[0] & 0x1FFFFFFF;
        if (p < 0x1000 || p >= EE_RAM_SIZE) continue;
        const char* s = ee_cstr(p);
        int n = 0;
        while (n < 120 && s[n] >= 0x20 && s[n] < 0x7F) ++n;
        if (n >= 3 && (s[n] == 0 || n == 120)) fprintf(stderr, "  a%d -> \"%.*s\"\n", r - 4, n, s);
    }
    ee_dump_trace(256);
    if (getenv("PS2_BREAK_ABORT")) abort();
}

EEFunc ee_lookup(u32 addr) {
    u32 a = addr & 0x1FFFFFFF;  // kseg0/kseg1 aliases of RAM code
    if (PS2_UNLIKELY(a < kLookupBase || a >= kLookupEnd || (a & 3))) return nullptr;
    return g_lookup[(a - kLookupBase) >> 2];
}

void ee_call(EEContext* c, u32 target) {
    EEFunc f = ee_lookup(target);
    if (PS2_UNLIKELY(!f)) ee_unknown_target(c, target, c->pc);
    LOG(LOG_CALL, "[call] %08x\n", target);
    f(c);
}

void ee_unknown_target(EEContext* c, u32 target, u32 from_pc) {
    FATAL("jump to unknown guest code %08x from %08x (ra=%08x sp=%08x). Add it to tools/extra_entries.txt "
          "and regenerate.",
          target, from_pc, c->r[31].w[0], c->r[29].w[0]);
}

void ee_bad_return(EEContext* c, u32 call_pc, u32 expected) {
    FATAL("call at %08x returned with ra=%08x (expected %08x): non-local control flow (longjmp/EH?)",
          call_pc, c->r[31].w[0], expected);
}

void ee_unhandled_insn(EEContext* c, u32 pc, u32 word) {
    FATAL("unhandled instruction %08x at %08x (ra=%08x)", word, pc, c->r[31].w[0]);
}

void ee_break(EEContext* c, u32 pc, u32 code) {
    if (warn_once(pc)) LOG(LOG_SYS, "[cpu] BREAK %x at %08x (ra=%08x) - continuing\n", code, pc, c->r[31].w[0]);
}

void ee_trap(EEContext* c, u32 pc) {
    if (warn_once(pc)) LOG(LOG_SYS, "[cpu] trap at %08x (ra=%08x) - continuing\n", pc, c->r[31].w[0]);
}

// libkernel's kernel-mode helpers raise ERL/EXL, touch a kseg1 register and
// return with "mtc0 ra, ErrorEPC; eret".  The generated code returns to its
// caller after ee_eret, so the ERET target must be the function's ra.
void ee_eret(EEContext* c, u32 pc) {
    u32 target;
    if (c->cop0_status & 4) {          // ERL
        target = c->cop0_errorepc;
        c->cop0_status &= ~4u;
    } else {
        target = c->cop0_epc;
        c->cop0_status &= ~2u;         // EXL
    }
    if (target != c->r[31].w[0])
        FATAL("ERET at %08x to %08x (ra=%08x): only ERET-as-return helpers are supported", pc, target,
              c->r[31].w[0]);
}

void ee_cache(EEContext*, u32, u32) {}   // caches are coherent here

static u64 g_count_offset = 0;

u32 ee_mfc0(EEContext* c, u32 reg) {
    switch (reg) {
    case 9: kernel::count_poll(); return (u32)(kernel::cycles() + g_count_offset);  // Count
    case 12: return c->cop0_status;
    case 13: return 0;                // Cause
    case 14: return c->cop0_epc;      // EPC
    case 15: return 0x00002E20;       // PRId (R5900)
    case 16: return 0x00000440;       // Config
    case 30: return c->cop0_errorepc;
    default: return 0;                // perf counters, debug, TLB
    }
}

void ee_mtc0(EEContext* c, u32 reg, u32 v) {
    switch (reg) {
    case 9: g_count_offset = (u64)v - kernel::cycles(); break;
    case 12: {
        bool was_on = (c->cop0_status & 0x10001) == 0x10001;
        c->cop0_status = v;
        if (!was_on && (v & 0x10001) == 0x10001) ee_ei(c);
        break;
    }
    case 14: c->cop0_epc = v; break;
    case 30: c->cop0_errorepc = v; break;
    default: break;
    }
}

void kernel_ee_ei(EEContext* c);
void ee_ei(EEContext* c) { kernel_ee_ei(c); }
void ee_di(EEContext* c) { c->cop0_status &= ~0x10000u; }

bool ee_cpcond0() { return hw::cpcond0(); }
