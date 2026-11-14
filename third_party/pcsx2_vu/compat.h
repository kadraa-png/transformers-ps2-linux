#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

using u8 = std::uint8_t;
using s8 = std::int8_t;
using u16 = std::uint16_t;
using s16 = std::int16_t;
using u32 = std::uint32_t;
using s32 = std::int32_t;
using u64 = std::uint64_t;
using s64 = std::int64_t;
using u128 = unsigned __int128;
using s128 = __int128;
using uint = unsigned int;

#define __aligned16 __attribute__((aligned(16)))
#define __ri
#define __fi
#define __fastcall
#define CPU_LOG(...) ((void)0)
#define pxFailDev(...) std::abort()
#define VUM_LOG(...) ((void)0)
using Fnptr_Void = void (*)();

// The host supplies the current EE instruction for the original VU0 macro ops.
struct VUPortCpuRegs { u32 code; };
extern VUPortCpuRegs cpuRegs;

// Runtime callbacks; none has a silent fallback.
// offset 0: VIF ITOP, offset 1: VIF TOP.
u32 vu_port_vif_reg(unsigned idx, unsigned offset);
// Receives VU1's 16 KiB circular data memory and byte address of the packet.
void vu_port_xgkick(unsigned idx, const u8* mem, unsigned addr);
// Runtime option corresponding to PCSX2's CHECK_VUADDSUBHACK.
extern bool vu_port_addsub_hack;
#define CHECK_VUADDSUBHACK vu_port_addsub_hack
// VU0's register-mapped view of VU1 (VU0 addresses 0x4000-0x43ff): the
// asynchronous VU1 must be idle before VU0 reads or writes its registers.
void vu_port_sync_vu1();
// JR/JALR target (byte address): recorded by PS2_VU_CAPTURE as an entry point.
void vu_port_indirect_branch(unsigned idx, u32 target);
