#include "fiber.h"
#include "ps2/log.h"

#include <sys/mman.h>
#include <unistd.h>

// Callee-saved register switch.  MXCSR / x87 CW are identical for every fiber
// of the emulation thread, so they are not saved.
asm(R"(
    .text
    .globl fiber_switch_asm
    .type fiber_switch_asm,@function
fiber_switch_asm:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rdi)
    movq %rsi, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
    .size fiber_switch_asm, .-fiber_switch_asm

    .globl fiber_trampoline
    .type fiber_trampoline,@function
fiber_trampoline:
    movq %r12, %rdi
    callq *%r13
    ud2
    .size fiber_trampoline, .-fiber_trampoline
)");

extern "C" void fiber_trampoline();

Fiber* fiber_create(size_t stack_size, void (*entry)(void*), void* arg) {
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    stack_size = (stack_size + page - 1) & ~(page - 1);
    u8* mem = (u8*)mmap(nullptr, stack_size + page, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
    if (mem == MAP_FAILED) FATAL("fiber stack allocation failed");
    mprotect(mem, page, PROT_NONE);  // guard page at the bottom
    Fiber* f = new Fiber;
    f->stack = mem;
    f->stack_size = stack_size + page;
    u64* top = (u64*)(mem + stack_size + page);  // 16-byte aligned
    u64* sp = top - 1;                            // return address slot at top-8
    *sp = (u64)&fiber_trampoline;
    *--sp = 0;                 // rbp
    *--sp = 0;                 // rbx
    *--sp = (u64)arg;          // r12
    *--sp = (u64)entry;        // r13
    *--sp = 0;                 // r14
    *--sp = 0;                 // r15
    f->sp = sp;
    return f;
}

void fiber_destroy(Fiber* f) {
    if (!f) return;
    if (f->stack) munmap(f->stack, f->stack_size);
    delete f;
}
