#pragma once
#include "VUops.h"

// The words are adjacent so the existing PCSX2 pipeline loop can read them as
// an instruction pair without copying the uploaded micro memory.
struct VUCompiledPair {
    u32 lower_word;
    u32 upper_word;
    Fnptr_Void lower[2]{};
    Fnptr_Void upper[2]{};
    _VURegsNum lower_regs[2]{};
    _VURegsNum upper_regs[2]{};
};

extern VUCompiledPair g_vu_compiled_pairs[];
extern const u32 g_vu_compiled_count;
const VUCompiledPair* vu_compiled_at(unsigned unit, u32 pc);
const int* vu_compiled_map(unsigned unit);
// Bumped by every write into that unit's micro memory (vu_micro_written).
u64 vu_compiled_generation(unsigned unit);
void vu_compiled_prepare();
u32 vu_port_run_compiled(unsigned unit, u32 start_pc_bytes, u32 max_pairs);
u32 vu_port_run_micro(unsigned unit, u32 start_pc_bytes, u32 max_pairs);
struct VUFastPipes {
    u32 fmac_mask = 0;
    bool valid = false;
    const int* mapped = nullptr;
};
u32 vu_port_run_fast_pair(u32 elf_pair_index, u32 max_pairs,
                          VUFastPipes& pipes);
#ifdef PS2_VU_FAST
// Whole captured micro image graphs (vu_image.inc); 0 = not entered.
u32 vu_port_run_image(u32 max_pairs);
u32 vu_port_run_image0(u32 max_pairs);   // VU0 images (VU0 TPC)
u32 vu_port_run_graph_pair(u32 elf_pair_index, u32 max_pairs,
                           VUFastPipes& pipes);
#endif
