#pragma once
// Minimal stackful coroutines for guest threads (x86-64 SysV).
#include "ps2/types.h"

struct Fiber {
    void* sp = nullptr;        // saved stack pointer while switched out
    void* stack = nullptr;     // mmap base (incl. guard page), nullptr for the host thread
    size_t stack_size = 0;
};

extern "C" void fiber_switch_asm(void** save_sp, void* new_sp);

// Creates a fiber that will run entry(arg) when first switched to.  entry must
// never return (switch away for good instead).
Fiber* fiber_create(size_t stack_size, void (*entry)(void*), void* arg);
void fiber_destroy(Fiber* f);

PS2_FORCEINLINE void fiber_switch(Fiber* from, Fiber* to) {
    fiber_switch_asm(&from->sp, to->sp);
}
