#pragma once

#include <cstddef>
#include <cstdint>

namespace iop {
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using s32 = std::int32_t;

struct Context {
    u32 r[32]{};
    u32 hi{};
    u32 lo{};
    u32 pc{};
};

struct Import {
    u32 address;
    const char* library;
    u16 ordinal;
};
struct Export {
    const char* library;
    u16 ordinal;
    u32 address;
};
using Dispatch = void (*)(Context&, u32);
struct Module {
    const char* name;
    u32 base;
    u32 size;
    u32 entry;
    u32 gp;
    const u8* image;
    Dispatch dispatch;
    const Import* imports;
    std::size_t import_count;
    const Export* exports;
    std::size_t export_count;
};

// Defined by the IOP runtime. All addresses are guest physical IOP addresses.
u8 read8(u32 addr);
u16 read16(u32 addr);
u32 read32(u32 addr);
void write8(u32 addr, u8 value);
void write16(u32 addr, u16 value);
void write32(u32 addr, u32 value);
void checkpoint();
void complete_rpc(u32 token, u32 result);
void invoke(u32 target, Context& ctx);
[[noreturn]] void unsupported(Context& ctx, u32 pc, u32 word);
u32 cop0_read(Context& ctx, unsigned reg);
void cop0_write(Context& ctx, unsigned reg, u32 value);
void cop0_rfe(Context& ctx);
void iop_syscall(Context& ctx, u32 pc, u32 word);
void iop_break(Context& ctx, u32 pc, u32 word);

// Defined by generated_iop/iop_registry.cpp; array remains static for program lifetime.
const Module* const* generated_modules(std::size_t& count);
}
