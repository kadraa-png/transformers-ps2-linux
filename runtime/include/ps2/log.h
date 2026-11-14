#pragma once
// Minimal logging used by every module.  Categories can be enabled at runtime
// with the PS2_LOG environment variable, e.g. PS2_LOG=sys,dma,gs  (or "all").
#include "ps2/types.h"
#include <cstdio>

enum LogCat : u32 {
    LOG_SYS = 1u << 0,    // kernel / syscalls
    LOG_THREAD = 1u << 1, // thread scheduling
    LOG_INTC = 1u << 2,   // interrupts
    LOG_DMA = 1u << 3,
    LOG_HW = 1u << 4,     // unhandled / interesting hardware register access
    LOG_GS = 1u << 5,
    LOG_VIF = 1u << 6,
    LOG_VU = 1u << 7,
    LOG_SIF = 1u << 8,
    LOG_IOP = 1u << 9,
    LOG_CDVD = 1u << 10,
    LOG_PAD = 1u << 11,
    LOG_MC = 1u << 12,
    LOG_SPU = 1u << 13,
    LOG_IPU = 1u << 14,
    LOG_TTY = 1u << 15,   // guest printf output (enabled by default)
    LOG_CALL = 1u << 16,  // dispatch tracing (very noisy)
};

extern u32 g_log_mask;
void log_init_from_env();
void log_printf(u32 cat, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
PS2_NORETURN void fatal_error(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

#define LOG(cat, ...)                                   \
    do {                                                \
        if (PS2_UNLIKELY(g_log_mask & (cat)))           \
            log_printf((cat), __VA_ARGS__);             \
    } while (0)

#define FATAL(...) fatal_error(__VA_ARGS__)
