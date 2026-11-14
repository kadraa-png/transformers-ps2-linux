#include "ps2/iop.h"
#include "ps2/ee_mem.h"
#include "ps2/intc.h"
#include "ps2/iso.h"
#include "ps2/platform.h"
#include "ps2/spu2.h"
#include "ps2/log.h"
#include "recompiled.h"
#include "kernel.h"
#include "cdvd_rpc.h"
#include "fileio_rpc.h"
#include "services.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace iop {
namespace {
constexpr u32 CMD_BUFFER = 0x8000;
constexpr u32 RPC_BUFFER = 0xc000;
constexpr u32 RPC_SERVER = 0xc800;
std::array<u8, IOP_RAM_SIZE> ram{};
std::array<u8, 1024> scratch{};
std::array<u32, 32> sysreg{};
std::array<u32, 32> sreg{};
struct Packet {
    u32 size = 0;
    std::array<u8, 112> data{};
    u32 completion_address = 0;
    u32 completion_size = 0;
    std::array<u8, 384> completion{};
};
std::array<Packet, 64> packets{};
u32 packet_head = 0, packet_count = 0;
u32 ee_packet_buffer = 0;
u32 smflag = 0;
u32 msflag = 0;
u32 dma_id = 0;
bool armed = false;
bool running = false;
u32 istat = 0, imask = 0, ictrl = 0;
std::array<u32, 0x3c00> hw{};
std::unordered_map<u32, u32> rpc_servers;
struct RpcCompletion {
    std::array<u8, 64> request{};
    u32 sid = 0, dest = 0, size = 0, mode = 0;
};
std::unordered_map<u32, RpcCompletion> rpc_completions;
u32 next_rpc_token = 1;
std::array<u32, 32> cop0{};
std::array<bool, 4> loaded{};
std::array<bool, 4> hle_loaded{}; // PADMAN, SIO2MAN, MCMAN, MCSERV

u32 get32(const u8* p) { u32 result; std::memcpy(&result, p, 4); return result; }
void put32(u8* p, u32 value) { std::memcpy(p, &value, 4); }
u32 word(u32 addr) { return read32(addr); }
u8* enqueue(const u8* data, u32 size) {
    if (size > 112 || packet_count == packets.size()) FATAL("IOP SIF packet queue full/oversized");
    Packet& p = packets[(packet_head + packet_count) % packets.size()];
    p.size = size;
    p.completion_size = 0;
    std::memcpy(p.data.data(), data, size);
    ++packet_count;
    return p.data.data();
}
// EE-side addresses arriving in SIF packets are physical or uncached/kseg
// mirrors (0x2xxxxxxx, 0x8xxxxxxx...): map them onto EE RAM.
u8* ee_phys(u32 addr, u32 size, const char* what) {
    const u32 a = addr & EE_RAM_MASK;
    if (size > EE_RAM_SIZE - a) FATAL("IOP %s outside EE RAM (%08x, %u bytes)", what, addr, size);
    return ee_host(a);
}
void deliver() {
    if (!armed || !ee_packet_buffer || !packet_count) return;
    Packet& p = packets[packet_head];
    if (p.completion_size)
        std::memcpy(ee_phys(p.completion_address, p.completion_size, "FILEIO completion"),
                    p.completion.data(), p.completion_size);
    std::memcpy(ee_phys(ee_packet_buffer, p.size, "SIF EE packet buffer"), p.data.data(), p.size);
    packet_head = (packet_head + 1) % packets.size();
    --packet_count;
    armed = false;
    dmac_raise(DMAC_SIF0);
}
void reply(const u8* request, bool bind, u32 server, u32 buf = 0, u32 cbuf = 0,
           u32 cid = 0x8000000a) {
    std::array<u8, 64> p{};
    put32(p.data(), 0x40);
    put32(p.data() + 8, 0x80000008);
    for (u32 offset = 16; offset <= 28; offset += 4)
        put32(p.data() + offset, get32(request + offset));
    put32(p.data() + 32, bind ? 0x80000009 : cid);
    put32(p.data() + 36, server);
    put32(p.data() + 40, buf);
    put32(p.data() + 44, cbuf);
    enqueue(p.data(), p.size());
    deliver();
}
void reply(u32 request, bool bind, u32 server, u32 buf = 0, u32 cbuf = 0,
           u32 cid = 0x8000000a) {
    reply(g_iop_ram + (request & 0x1fffff), bind, server, buf, cbuf, cid);
}
void finish_nowait(const u8* request) {
    // sceSifExecRequest uses a direct DMA to pkt_addr when rmode == 0:
    // no SIF_CMD_RPC_END is sent, so the EE never invokes an end callback.
    const u32 packet = get32(request + 20);
    if (!packet) return;
    u8* ee_packet = ee_phys(packet, 64, "RPC no-wait packet");
    put32(ee_packet + 16, 0); // rec_id: release packet
    put32(ee_packet + 24, 0); // rpc_id: no longer pending
}
bool is_builtin(u32 sid) {
    return sid == 0x80000001 || sid == 0x80000003 || sid == 0x80000006 ||
        (sid >= 0x80000592 && sid <= 0x80000597) || sid == 0x8000059a ||
        sid == 0x8000059c ||
        ((sid == 0x80000100 || sid == 0x80000101) && hle_loaded[0]) ||
        (sid == 0x80000400 && hle_loaded[3]);
}
bool valid_ram(u32 addr, u32 size) {
    addr &= 0x1fffffff;
    return addr <= IOP_RAM_SIZE && size <= IOP_RAM_SIZE - addr;
}
const Module* find_module(const std::string& path) {
    size_t count = 0;
    const Module* const* modules = generated_modules(count);
    auto name = path.substr(path.find_last_of("/\\") == std::string::npos ? 0 : path.find_last_of("/\\") + 1);
    auto semicolon = name.find(';');
    if (semicolon != std::string::npos) name.resize(semicolon);
    for (auto& c : name) if (c >= 'a' && c <= 'z') c -= 32;
    for (size_t i = 0; i < count; ++i) {
        std::string candidate = modules[i]->name;
        for (auto& c : candidate) if (c >= 'a' && c <= 'z') c -= 32;
        if (name == candidate || name == candidate + ".IRX") return modules[i];
        if ((name == "TWOFISH.IRX" && modules[i]->base == 0x10000) ||
            (name == "LIBSD.IRX" && modules[i]->base == 0x30000) ||
            (name == "MODMIDI.IRX" && modules[i]->base == 0x40000) ||
            (name == "SIRENS3.IRX" && modules[i]->base == 0x50000))
            return modules[i];
    }
    return nullptr;
}
s32 load_module(const std::string& path, const std::string& args) {
    std::string name = path.substr(path.find_last_of("/\\") == std::string::npos ? 0 : path.find_last_of("/\\") + 1);
    const auto semicolon = name.find(';');
    if (semicolon != std::string::npos) name.resize(semicolon);
    for (auto& c : name) if (c >= 'a' && c <= 'z') c -= 32;
    constexpr const char* standard[] = {"PADMAN.IRX", "SIO2MAN.IRX", "MCMAN.IRX", "MCSERV.IRX"};
    for (u32 i = 0; i < 4; ++i) if (name == standard[i]) {
        hle_loaded[i] = true;
        return static_cast<s32>(12 + i);
    }
    const Module* mod = find_module(path);
    if (!mod) { std::fprintf(stderr, "IOP: missing module %s\n", path.c_str()); return -1; }
    size_t count = 0;
    const Module* const* modules = generated_modules(count);
    for (size_t i = 0; i < count; ++i) if (modules[i] == mod) {
        if (i >= loaded.size()) FATAL("too many IOP modules");
        if (loaded[i]) return static_cast<s32>(i + 17);
        if (!valid_ram(mod->base, mod->size)) FATAL("IOP module %s does not fit RAM", mod->name);
        for (size_t imp_index = 0; imp_index < mod->import_count; ++imp_index) {
            const Import& imp = mod->imports[imp_index];
            bool bound = kernel::supports(imp.library, imp.ordinal);
            for (size_t provider_index = 0; provider_index < count && !bound; ++provider_index) {
                if (!loaded[provider_index]) continue;
                const Module& provider = *modules[provider_index];
                for (size_t export_index = 0; export_index < provider.export_count; ++export_index) {
                    const Export& exp = provider.exports[export_index];
                    if (exp.library && std::strcmp(exp.library, imp.library) == 0 &&
                        exp.ordinal == imp.ordinal && exp.address) {
                        bound = true;
                        break;
                    }
                }
            }
            if (!bound)
                FATAL("IOP unresolved import %s -> %s:%u", mod->name, imp.library, imp.ordinal);
        }
        std::memcpy(g_iop_ram + mod->base, mod->image, mod->size);
        loaded[i] = true;
        Context ctx{};
        ctx.r[28] = mod->gp;
        ctx.r[29] = 0x17f000;
        ctx.r[31] = 0;
        // Startup arguments occupy a separate kernel scratch range.
        const u32 argaddr = 0xd000;
        std::memset(g_iop_ram + argaddr, 0, 1024);
        if (!args.empty()) {
            std::memcpy(g_iop_ram + argaddr + 16, args.c_str(), std::min(args.size() + 1, size_t(1000)));
            write32(argaddr, argaddr + 16);
            ctx.r[4] = 1;
            ctx.r[5] = argaddr;
        }
        // Module startup is invoked on its own IOP fiber by the kernel scheduler.
        kernel::start_module(mod->entry, ctx);
        return static_cast<s32>(i + 17);
    }
    return -1;
}
s32 load_buffer_module(u32 buffer, const std::string& args) {
    if (!valid_ram(buffer, 0xc0)) FATAL("loadfile IRX buffer outside IOP RAM: %08x", buffer);
    const u8* image = g_iop_ram + (buffer & 0x1fffff);
    if (std::memcmp(image, "\x7f" "ELF", 4) != 0 || image[4] != 1 ||
        image[5] != 1 || image[18] != 8)
        FATAL("loadfile buffer %08x is not a 32-bit MIPS IRX", buffer);
    const char* name = reinterpret_cast<const char*>(image + 0x8e);
    const size_t length = strnlen(name, 0xc0 - 0x8e);
    if (length == 0 || length == 0xc0 - 0x8e)
        FATAL("loadfile IRX buffer %08x has no module name", buffer);
    u16 version = 0;
    std::memcpy(&version, image + 0x8c, sizeof(version));
    struct KnownModule { const char* name; u16 version; const char* path; };
    constexpr KnownModule known[] = {
        {"TwoFish", 0x106, "TWOFISH.IRX"},
        {"Sound_Device_Library", 0x303, "LIBSD.IRX"},
        {"Midi_Sequencer_Module", 0x202, "MODMIDI.IRX"},
        {"Sirens3_Server", 0x3aa, "SIRENS3.IRX"},
        {"padman", 0x405, "PADMAN.IRX"},
        {"sio2man", 0x300, "SIO2MAN.IRX"},
        {"mcman", 0x222, "MCMAN.IRX"},
        {"mcserv", 0x210, "MCSERV.IRX"},
    };
    for (const auto& m : known)
        if (version == m.version && std::strcmp(name, m.name) == 0)
            return load_module(m.path, args);
    FATAL("loadfile unknown IRX module '%s' version %04x at %08x", name, version, buffer);
}
void rpc_call(u32 addr) {
    const u32 server_ptr = word(addr + 52);
    const auto server_it = rpc_servers.find(server_ptr);
    if (server_it == rpc_servers.end())
        FATAL("IOP RPC CALL references unbound server");
    const u32 sid = server_it->second;
    const u32 fno = word(addr + 32);
    const u32 size = word(addr + 36);
    const u32 dest = word(addr + 40);
    const u32 rsize = word(addr + 44);
    if (size > 65536 || rsize > 65536) FATAL("oversized IOP RPC request");
    const u32 send = word(addr + 4);
    u32 reply_addr = 0;
    if (sid == 0x80000006) {
        if (fno == 255) {
            write32(RPC_BUFFER, 0x30303832); // Sony loadfile protocol "2800".
            reply_addr = RPC_BUFFER;
        } else if (fno == 0 || fno == 4 || fno == 6) {
            if (!valid_ram(send, 512)) FATAL("loadfile request outside IOP RAM");
            const u32 count = std::min(word(send + (fno == 6 ? 4 : 0)), 252u);
            const char* av = reinterpret_cast<const char*>(g_iop_ram + ((send + 260) & 0x1fffff));
            const std::string args(av, strnlen(av, count));
            s32 id = -1;
            if (fno == 6) {
                // Sony LoadModuleBuffer: +0 IOP ELF image pointer, +4 arg length.
                id = load_buffer_module(word(send), args);
            } else {
                // loadfile-common.h LoadModule: +0 arg length, +8 path, +260 args.
                const char* path = reinterpret_cast<const char*>(g_iop_ram + (send & 0x1fffff) + 8);
                id = load_module(std::string(path, strnlen(path, 252)), args);
            }
            write32(RPC_BUFFER, static_cast<u32>(id));
            write32(RPC_BUFFER + 4, 0);
            reply_addr = RPC_BUFFER;
        } else {
            FATAL("unhandled loadfile RPC function %u", fno);
        }
    } else if (sid == 0x80000001) {
        fileio_rpc::call(fno, send, size, RPC_BUFFER, rsize);
        reply_addr = RPC_BUFFER;
    } else if (sid == 0x80000003) {
        kernel::heap_rpc(fno, send, size, RPC_BUFFER, rsize);
        reply_addr = RPC_BUFFER;
    } else if ((sid >= 0x80000592 && sid <= 0x80000597) ||
               sid == 0x8000059a || sid == 0x8000059c) {
        cdvd_rpc::call(sid, fno, send, size, RPC_BUFFER, rsize);
        reply_addr = RPC_BUFFER;
    } else if (sid == 0x80000100 || sid == 0x80000101 || sid == 0x80000400) {
        services::call(sid, fno, send, size, RPC_BUFFER, rsize);
        reply_addr = RPC_BUFFER;
    } else {
        const u32 token = next_rpc_token++;
        RpcCompletion completion{};
        std::memcpy(completion.request.data(), g_iop_ram + (addr & 0x1fffff), 64);
        completion.sid = sid;
        completion.dest = dest;
        completion.size = rsize;
        completion.mode = word(addr + 48);
        rpc_completions.emplace(token, completion);
        kernel::rpc_request(sid, fno, send, size, token);
        return; // sceSifExecRequest sends the response after the server fiber finishes.
    }
    if (dest && rsize && reply_addr) {
        if (!valid_ram(reply_addr, rsize)) FATAL("IOP RPC response out of bounds");
        std::memcpy(ee_phys(dest, rsize, "RPC response"), g_iop_ram + (reply_addr & 0x1fffff), rsize);
    }
    if (word(addr + 48)) reply(addr, false, sid);
    else finish_nowait(g_iop_ram + (addr & 0x1fffff));
}
// The real IOP's sceSifInitRpc tells the EE that RPC is up by setting EE
// software register SIF_SREG_RPCINIT (0) to 1 with a SIF_CMD_SET_SREG packet;
// the EE's sceSifInitRpc polls that register.
bool rpc_init_sent = false;
void announce_rpc_init() {
    if (rpc_init_sent) return;
    rpc_init_sent = true;
    std::array<u8, 24> p{};
    put32(p.data(), 24);
    put32(p.data() + 8, 0x80000001);
    put32(p.data() + 16, 0);   // SIF_SREG_RPCINIT
    put32(p.data() + 20, 1);
    enqueue(p.data(), p.size());
}
// Reboot completes ~2 ms after the reset command (the real IOP takes longer;
// the EE only needs the flags to appear after its own clears).  The RPC-init
// announcement waits for the EE's CHANGE_SADDR / INIT_CMD handshake, which
// arrives once the EE has re-installed its command handlers.
bool reboot_pending = false;
u64 reboot_started_ns = 0;
void finish_reboot() {
    if (!reboot_pending || platform::now_ns() - reboot_started_ns < 2'000'000) return;
    reboot_pending = false;
    smflag |= 0x70000;   // SIFINIT | CMDINIT | BOOTEND
}
void process_packet(u32 addr, u32 size) {
    if (size < 16) return;
    const u32 cid = word(addr + 8);
    switch (cid) {
    case 0x80000000:
        ee_packet_buffer = word(addr + 16);
        announce_rpc_init();
        deliver();
        return;
    case 0x80000001: if (size >= 24 && word(addr + 16) < 32) sreg[word(addr + 16)] = word(addr + 20); return;
    case 0x80000002:   // INIT_CMD: opt 0 carries the EE buffer, opt 1 is sceSifInitRpc's handshake
        if (word(addr + 12) == 0) ee_packet_buffer = word(addr + 16);
        smflag |= 0x20000;
        announce_rpc_init();
        deliver();
        return;
    case 0x80000003:
        kernel::reset(); services::reset(); fileio_rpc::reset(); spu2::reset();
        ram.fill(0); scratch.fill(0); cop0.fill(0);
        loaded.fill(false); hle_loaded.fill(false); msflag = 0;
        // The rebooting IOP publishes SIFINIT/CMDINIT/BOOTEND only after the
        // EE has cleared them (sceSifResetIop clears them right after sending
        // the reset), so completion is deferred; see finish_reboot().
        smflag = 0;
        reboot_pending = true;
        reboot_started_ns = platform::now_ns();
        // EE-side state (packet buffer, armed SIF0 channel) survives an IOP
        // reboot; the rebooted IOP announces RPC again.
        rpc_init_sent = false; packet_head = packet_count = 0;
        rpc_servers.clear(); rpc_completions.clear();
        return;
    case 0x80000009: {
        const u32 sid = word(addr + 32);
        bool exists = is_builtin(sid) || kernel::has_rpc_server(sid);
        u32 server = 0;
        for (const auto& [addr_, s] : rpc_servers)
            if (s == sid) server = addr_;
        if (exists && !server) {
            server = RPC_SERVER + static_cast<u32>(rpc_servers.size()) * 32;
            rpc_servers[server] = sid;
        }
        if (!exists) server = 0;
        LOG(LOG_SIF, "[sif] bind %08x -> %s\n", sid, exists ? "ok" : "not registered (yet)");
        u32 cbuf = 0;
        const u32 buf = exists && !is_builtin(sid) ? kernel::rpc_buffer(sid, cbuf) : RPC_BUFFER;
        reply(addr, true, server, exists ? buf : 0, cbuf); return;
    }
    case 0x8000000a: rpc_call(addr); return;
    case 0x8000000c: {
        const u32 source = word(addr + 32), target = word(addr + 36), length = word(addr + 40);
        if (valid_ram(source, length))
            std::memcpy(ee_phys(target, length, "RDATA target"), g_iop_ram + (source & 0x1fffff), length);
        reply(addr, false, 0, 0, 0, 0x8000000c); return;
    }
    default: kernel::command(addr, size); return;
    }
}
}
u8* g_iop_ram = ram.data();
void complete_rpc(u32 token, u32 result) {
    auto it = rpc_completions.find(token);
    if (it == rpc_completions.end()) FATAL("IOP RPC response for unknown request %u", token);
    const RpcCompletion completion = it->second;
    rpc_completions.erase(it);
    if (result && completion.dest && completion.size) {
        if (!valid_ram(result, completion.size)) FATAL("IOP RPC response outside RAM");
        std::memcpy(ee_phys(completion.dest, completion.size, "RPC response"),
                    g_iop_ram + (result & 0x1fffff), completion.size);
    }
    if (completion.mode) reply(completion.request.data(), false, completion.sid);
    else finish_nowait(completion.request.data());
}

void spu_dma(u32 channel, u32 ram_address, u32 byte_count, bool to_spu) {
    if ((channel != 4 && channel != 7) || (byte_count & 1) ||
        !valid_ram(ram_address, byte_count))
        FATAL("invalid IOP SPU2 DMA ch%u addr=%08x bytes=%u", channel, ram_address, byte_count);
    const int core = channel == 4 ? 0 : 1;
    auto* words = reinterpret_cast<u16*>(g_iop_ram + (ram_address & 0x1fffff));
    LOG(LOG_SPU, "[iop dma] ch%u %s spu2 %u bytes from %08x\n", channel, to_spu ? "to" : "from", byte_count, ram_address);
    if (to_spu) spu2::dma_write(core, words, byte_count / 2);
    else spu2::dma_read(core, words, byte_count / 2);
}

u8 read8(u32 a) {
    a &= 0x1fffffff;
    if (a < IOP_RAM_SIZE) return g_iop_ram[a];
    if (a >= 0x1f800000 && a < 0x1f800400) return scratch[a & 1023];
    if (a >= 0x1f900000 && a < 0x1f900800)
        return static_cast<u8>(spu2::read16(a) >> ((a & 1) * 8));
    if (a >= 0x1f801000 && a < 0x1f810000) return static_cast<u8>(read32(a & ~3u) >> ((a & 3) * 8));
    FATAL("IOP unmapped byte read %08x", a);
}
// 16-bit accesses must reach SPU2 registers as one access: several have write
// side effects (KON/KOFF, ATTR, the manual-transfer FIFO at 0x1ac reads/writes).
u16 read16(u32 a) {
    a &= 0x1fffffff;
    if (a + 2 <= IOP_RAM_SIZE) { u16 v; std::memcpy(&v, g_iop_ram + a, 2); return v; }
    if (a >= 0x1f900000 && a < 0x1f900800 && !(a & 1)) return spu2::read16(a);
    if (a >= 0x1f801000 && a < 0x1f810000 && !(a & 1)) return static_cast<u16>(read32(a & ~3u) >> ((a & 2) * 8));
    return u16(read8(a)) | (u16(read8(a + 1)) << 8);
}
u32 read32(u32 a) {
    a &= 0x1fffffff;
    if (a + 4 <= IOP_RAM_SIZE) return get32(g_iop_ram + a);
    if (a == 0x1f801070) return istat;
    if (a == 0x1f801074) return imask;
    if (a == 0x1f801078) return ictrl;
    if (a >= 0x1f801000 && a < 0x1f810000) {
        // SPU2 DMA channels (4/7) finish on the SPU2 clock: let polling observe it.
        if ((a >= 0x1f8010c0 && a < 0x1f8010d0) || (a >= 0x1f801500 && a < 0x1f801510)) spu2::sync();
        return hw[(a - 0x1f801000) / 4];
    }
    return u32(read16(a)) | (u32(read16(a + 2)) << 16);
}
void write8(u32 a, u8 v) {
    a &= 0x1fffffff;
    if (a < IOP_RAM_SIZE) { g_iop_ram[a] = v; return; }
    if (a >= 0x1f800000 && a < 0x1f800400) { scratch[a & 1023] = v; return; }
    if (a >= 0x1f900000 && a < 0x1f900800) {
        const auto old = spu2::read16(a);
        spu2::write16(a, (old & (a & 1 ? 0x00ff : 0xff00)) | (u16(v) << ((a & 1) * 8)));
        return;
    }
    if (a >= 0x1f801000 && a < 0x1f810000) { u32 mask = 0xffu << ((a & 3) * 8); write32(a & ~3u, (read32(a & ~3u) & ~mask) | (u32(v) << ((a & 3) * 8))); return; }
    FATAL("IOP unmapped byte write %08x", a);
}
void write16(u32 a, u16 v) {
    a &= 0x1fffffff;
    if (a + 2 <= IOP_RAM_SIZE) { std::memcpy(g_iop_ram + a, &v, 2); return; }
    if (a >= 0x1f900000 && a < 0x1f900800 && !(a & 1)) { spu2::write16(a, v); return; }
    if (a >= 0x1f801000 && a < 0x1f810000 && !(a & 1)) {
        const u32 shift = (a & 2) * 8;
        write32(a & ~3u, (read32(a & ~3u) & ~(0xffffu << shift)) | (u32(v) << shift));
        return;
    }
    write8(a, static_cast<u8>(v)); write8(a + 1, static_cast<u8>(v >> 8));
}
void write32(u32 a, u32 v) {
    a &= 0x1fffffff;
    if (a + 4 <= IOP_RAM_SIZE) { put32(g_iop_ram + a, v); return; }
    if (a == 0x1f801070) { istat &= v; return; }
    if (a == 0x1f801074) { imask = v; return; }
    if (a == 0x1f801078) { ictrl = v; return; }
    if (a >= 0x1f801000 && a < 0x1f810000) {
        hw[(a - 0x1f801000) / 4] = v;
        if ((a == 0x1f8010c8 || a == 0x1f801508) && (v & 0x01000000)) {
            const u32 base = a - 8;
            const u32 madr = hw[(base - 0x1f801000) / 4];
            const u32 bcr = hw[(base - 0x1f801000) / 4 + 1];
            const u32 blocks = (bcr >> 16) ? (bcr >> 16) : 1;
            // CHCR stays busy until the SPU2 reports completion (dma callback).
            spu_dma(a == 0x1f8010c8 ? 4 : 7, madr, (bcr & 0xffff) * blocks * 4,
                    (v & 1) != 0);
        }
        return;
    }
    write16(a, static_cast<u16>(v)); write16(a + 2, static_cast<u16>(v >> 16));
}
void unsupported(Context&, u32 pc, u32 instruction) {
    FATAL("unhandled IOP instruction %08x at %08x", instruction, pc);
}
u32 cop0_read(Context&, unsigned reg) {
    if (reg >= cop0.size()) FATAL("invalid IOP COP0 register %u", reg);
    return cop0[reg];
}
void cop0_write(Context&, unsigned reg, u32 value) {
    if (reg >= cop0.size()) FATAL("invalid IOP COP0 register %u", reg);
    cop0[reg] = value;
}
void cop0_rfe(Context&) {
    cop0[12] = (cop0[12] & ~0x3fu) | ((cop0[12] >> 2) & 0xfu);
}
void iop_syscall(Context&, u32 pc, u32 word) {
    FATAL("IOP syscall %05x at %08x has no HLE handler", (word >> 6) & 0xfffff, pc);
}
void iop_break(Context&, u32 pc, u32 word) {
    FATAL("IOP break %05x at %08x", (word >> 6) & 0xfffff, pc);
}
bool module_loaded(u32 base) {
    size_t count = 0;
    const Module* const* modules = generated_modules(count);
    for (size_t i = 0; i < count && i < loaded.size(); ++i)
        if (modules[i]->base == base) return loaded[i];
    return false;
}
void invoke(u32 address, Context& ctx) {
    size_t count = 0;
    const Module* const* modules = generated_modules(count);
    for (size_t i = 0; i < count; ++i) if (loaded[i]) {
        const Module& m = *modules[i];
        for (size_t j = 0; j < m.import_count; ++j) if (m.imports[j].address == address) {
            const Import& imp = m.imports[j];
            for (size_t k = 0; k < count; ++k) if (loaded[k]) {
                const Module& provider = *modules[k];
                for (size_t n = 0; n < provider.export_count; ++n) {
                    const Export& exp = provider.exports[n];
                    if (exp.library && std::strcmp(imp.library, exp.library) == 0 &&
                        exp.ordinal == imp.ordinal && exp.address) {
                        const u32 gp = ctx.r[28];
                        ctx.r[28] = provider.gp;
                        invoke(exp.address, ctx);
                        ctx.r[28] = gp;
                        return;
                    }
                }
            }
            kernel::dispatch(ctx, imp.library, imp.ordinal);
            return;
        }
        if (address >= m.base && address < m.base + m.size) {
            m.dispatch(ctx, address); return;
        }
    }
    FATAL("IOP unknown call target %08x", address);
}
void checkpoint() { kernel::checkpoint(); }
u32 dma_to_ee(u32 source, u32 destination, u32 bytes) {
    if (!valid_ram(source, bytes)) FATAL("IOP SIF DMA source outside IOP RAM");
    std::memcpy(ee_phys(destination, bytes, "SIF DMA destination"), g_iop_ram + (source & 0x1fffff), bytes);
    return ++dma_id;
}
void send_packet(u32 source, u32 size, u32 extra_src, u32 extra_dest, u32 extra_size) {
    if (!valid_ram(source, size) || size < 16 || size > 112) FATAL("invalid IOP command packet %08x/%u", source, size);
    if (extra_size) dma_to_ee(extra_src, extra_dest, extra_size);
    u8* packet = enqueue(g_iop_ram + (source & 0x1fffff), size);
    LOG(LOG_SIF, "[sif] iop->ee cmd %08x size %u extra %u -> %08x (queued %u, armed %d)\n", get32(packet + 8), size,
        extra_size, extra_dest, packet_count, (int)armed);
    put32(packet, (size & 255) | (extra_size << 8));
    put32(packet + 4, extra_size ? extra_dest : 0);
    deliver();
}
void send_fileio_completion(u32 ee_destination, const void* data, u32 size, u32 slot) {
    if (!ee_destination) FATAL("FILEIO completion buffer not registered");
    if (size > packets[0].completion.size() || slot >= 32)
        FATAL("FILEIO completion oversized/invalid slot: %u/%u", size, slot);
    const u32 header[4] = {16, 0, 0x80000011, slot};
    enqueue(reinterpret_cast<const u8*>(header), sizeof(header));
    Packet& p = packets[(packet_head + packet_count - 1) % packets.size()];
    p.completion_address = ee_destination + slot * 0x440;
    p.completion_size = size;
    std::memcpy(p.completion.data(), data, size);
    deliver();
}
void init() {
    ram.fill(0); scratch.fill(0); hw.fill(0); cop0.fill(0); loaded.fill(false); hle_loaded.fill(false);
    sysreg.fill(0); sreg.fill(0); packet_head = packet_count = 0; dma_id = 0; armed = false;
    rpc_completions.clear(); next_rpc_token = 1;
    ee_packet_buffer = 0; smflag = 0x70000; msflag = 0; rpc_init_sent = false;
    istat = imask = ictrl = 0;
    kernel::reset();
    fileio_rpc::reset();
    spu2::init();
    spu2::set_irq_callback([](int) {
        istat |= 1u << 9;
        kernel::interrupt(9);
    });
    spu2::set_dma_callback([](int core) {
        hw[((core == 0 ? 0x1f8010c8u : 0x1f801508u) - 0x1f801000u) / 4] &= ~0x01000000u;   // CHCR idle
        istat |= 1u << (core == 0 ? 9 : 10);
        kernel::interrupt(core == 0 ? 36 : 40);
    });
    services::reset();
    rpc_servers.clear();
}
void shutdown() {
    kernel::reset(); services::reset(); fileio_rpc::reset(); rpc_completions.clear();
    packet_head = packet_count = 0; loaded.fill(false); hle_loaded.fill(false);
    spu2::shutdown();
}
u32 ee_sif_set_dma(const SifDmaXfer* xfers, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        const auto& x = xfers[i];
        // EE DMA addresses are physical: libsif passes uncached (0x2xxxxxxx) mirrors.
        const u32 src = x.src & EE_RAM_MASK;
        if (x.size > EE_RAM_SIZE - src || !valid_ram(x.dest, x.size))
            FATAL("invalid EE to IOP SIF DMA %08x -> %08x (%u bytes)", x.src, x.dest, x.size);
        const u32 dest = x.dest & 0x1fffff;
        std::memcpy(g_iop_ram + dest, ee_host(src), x.size);
        if (dest == CMD_BUFFER && x.size >= 16) process_packet(dest, x.size);
    }
    return ++dma_id;
}
s32 ee_sif_dma_stat(u32) { return -1; }
void ee_sif_set_dchain() { armed = true; deliver(); }
u32 ee_sif_get_reg(u32 reg) {
    switch (reg) {
    case 1: return ee_packet_buffer;
    case 2: return CMD_BUFFER;
    case 3: return msflag;
    case 4: finish_reboot(); return smflag;
    default: return reg & 0x80000000 ? sysreg[reg & 31] : 0;
    }
}
void ee_sif_set_reg(u32 reg, u32 value) {
    switch (reg) {
    case 1: ee_packet_buffer = value; deliver(); return;
    case 2: return;
    case 3: msflag |= value; return;
    case 4: smflag &= ~value; return;
    default: if (reg & 0x80000000) sysreg[reg & 31] = value;
    }
}
u32 sif_hw_read(u32 offset) {
    switch (offset & 0xff) {
    case 0: return ee_packet_buffer;
    case 0x10: return CMD_BUFFER;
    case 0x20: return msflag;
    case 0x30: finish_reboot(); return smflag;
    case 0x40: return 0x100;
    default: return 0;
    }
}
void sif_hw_write(u32 offset, u32 value) {
    switch (offset & 0xff) {
    case 0: ee_packet_buffer = value; deliver(); break;
    case 0x20: msflag |= value; break;
    case 0x30: smflag &= ~value; break;
    default: break;
    }
}
void run() {
    finish_reboot();
    if (running) return;
    running = true;
    const u64 deadline = platform::now_ns() + 1'000'000;
    for (unsigned i = 0; i < 1024 && platform::now_ns() < deadline && kernel::has_work(); ++i)
        kernel::step();
    running = false;
}
bool has_work() { return kernel::has_work(); }
u64 next_deadline_ns() { return kernel::deadline(); }
void debug_dump() {
    kernel::dump_threads();
    auto reg = [](u32 a) { return hw[(a - 0x1f801000) / 4]; };
    fprintf(stderr, "  dma4 madr %08x bcr %08x chcr %08x | dma7 madr %08x bcr %08x chcr %08x\n",
            reg(0x1f8010c0), reg(0x1f8010c4), reg(0x1f8010c8), reg(0x1f801500), reg(0x1f801504), reg(0x1f801508));
    fprintf(stderr, "  sif0: %u packets queued, EE receive %s (buffer %08x)\n", packet_count,
            armed ? "armed" : "not armed", ee_packet_buffer);
}
void vblank(bool start) {
    kernel::vblank(start);
    services::vblank(start);
}
} // namespace iop
