#include "ps2/ee_mem.h"
#include "ps2/log.h"

#include <sys/mman.h>
#include <unistd.h>

void ee_mem_init() {
    const size_t span = 1ull << 32;
    void* base = mmap(EE_BASE, span, PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (base != EE_BASE) FATAL("cannot reserve the guest address space at %p", (void*)EE_BASE);

    int ram = memfd_create("ee_ram", MFD_CLOEXEC);
    if (ram < 0 || ftruncate(ram, EE_RAM_SIZE) != 0) FATAL("memfd for EE RAM failed");
    const u32 mirrors[] = {0x00000000u, 0x20000000u, 0x30000000u, 0x80000000u, 0xA0000000u};
    for (u32 m : mirrors) {
        void* p = mmap(EE_BASE + m, EE_RAM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, ram, 0);
        if (p != EE_BASE + m) FATAL("mapping EE RAM mirror 0x%08x failed", m);
    }
    close(ram);

    void* spr = mmap(EE_BASE + EE_SPR_ADDR, EE_SPR_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (spr != EE_BASE + EE_SPR_ADDR) FATAL("mapping scratchpad failed");
}
