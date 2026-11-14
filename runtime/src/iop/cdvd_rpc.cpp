#include "cdvd_rpc.h"
#include "ps2/ee_mem.h"
#include "ps2/iop.h"
#include "ps2/iso.h"
#include "ps2/log.h"
#include <cstring>
#include <ctime>
#include <string>

namespace iop::cdvd_rpc {
namespace {
constexpr u32 PHYSICAL_MASK = 0x1fffffff;
u32 stream_lba = 0;
u32 stream_buffer_sectors = 0;
bool streaming = false;
u16 nvm[0x10000]{};
u8 config[1020]{};
u32 config_size = 0;
u8 ilink_id[8]{};
u8 console_id[8]{};
char model_name[16]{};

bool in_iop(u32 addr, u32 size) {
    addr &= PHYSICAL_MASK;
    return addr <= IOP_RAM_SIZE && size <= IOP_RAM_SIZE - addr;
}
bool in_ee(u32 addr, u32 size) {
    const u32 physical = addr & EE_RAM_MASK;
    return physical <= EE_RAM_SIZE && size <= EE_RAM_SIZE - physical;
}
u32 word(u32 addr) { return read32(addr); }
void result(u32 recv, u32 size, u32 value) {
    if (size >= 4) write32(recv, value);
}
void read_disc(u32 lba, u32 count, u32 dest, u32 recv, u32 recv_size, bool iop_target = false) {
    const u64 bytes = u64(count) * iso::SECTOR_SIZE;
    if (bytes > (iop_target ? IOP_RAM_SIZE : EE_RAM_SIZE) ||
        !(iop_target ? in_iop(dest, static_cast<u32>(bytes)) : in_ee(dest, static_cast<u32>(bytes))))
        FATAL("CDVD read destination outside %s RAM: %08x size=%llu",
              iop_target ? "IOP" : "EE", dest, static_cast<unsigned long long>(bytes));
    const bool ok = iso::read_sectors(lba, count, iop_target
        ? g_iop_ram + (dest & PHYSICAL_MASK) : ee_host(dest & EE_RAM_MASK));
    result(recv, recv_size, ok ? 1 : 0);
}
void search(u32 send, u32 size, u32 recv, u32 recv_size) {
    // libcdvd-rpc.h: sceCdlFILE (32), optional attributes (4), path (256), EE DMA address (4), optional layer (4).
    if (size != 0x124 && size != 0x128 && size != 0x12c)
        FATAL("Unsupported CDVD SearchFile packet size %u", size);
    const u32 path_offset = size == 0x124 ? 0x20 : 0x24;
    const char* path = reinterpret_cast<const char*>(g_iop_ram + (send & PHYSICAL_MASK) + path_offset);
    const size_t length = strnlen(path, 256);
    if (length == 256) FATAL("Unterminated CDVD SearchFile path");
    if (size == 0x12c && word(send + 0x128) != 0)
        FATAL("Unsupported CDVD SearchFile layer");
    iso::Entry file;
    const bool found = iso::find(std::string(path, length), &file);
    const u32 ee_dest = word(send + size - (size == 0x12c ? 8 : 4));
    const u32 dma_size = size == 0x124 ? 32 : 36;
    if (!in_ee(ee_dest, dma_size)) FATAL("CDVD SearchFile DMA outside EE RAM");
    if (found) {
        // sceCdlFILE contains the ISO entry's basename, not the query path.
        const char* base = path;
        for (const char* p = path; p < path + length; ++p)
            if (*p == '/' || *p == '\\') base = p + 1;
        char name[16]{};
        const size_t base_length = length - (base - path);
        std::memcpy(name, base, base_length < sizeof(name) ? base_length : sizeof(name));
        write32(send, file.lba);
        write32(send + 4, file.size);
        std::memset(g_iop_ram + (send & PHYSICAL_MASK) + 8, 0, 24);
        std::memcpy(g_iop_ram + (send & PHYSICAL_MASK) + 8, name, sizeof(name));
        if (dma_size == 36) write32(send + 32, file.is_dir ? 2 : 0);
    } else {
        std::memset(g_iop_ram + (send & PHYSICAL_MASK), 0, 32);
    }
    std::memcpy(ee_host(ee_dest & EE_RAM_MASK), g_iop_ram + (send & PHYSICAL_MASK), dma_size);
    result(recv, recv_size, found ? 1 : 0);
}
void stream(u32 send, u32 recv, u32 recv_size) {
    const u32 lba = word(send);
    const u32 count = word(send + 4);
    const u32 dest = word(send + 8);
    const u32 command = word(send + 12);
    switch (command) {
    case 1: // StStart
        stream_lba = lba;
        streaming = true;
        result(recv, recv_size, 1);
        break;
    case 2: // StRead returns count in low 16 bits, error in high 16 bits.
        if (!streaming) { result(recv, recv_size, 0); break; }
        if (u64(count) * iso::SECTOR_SIZE > EE_RAM_SIZE ||
            !in_ee(dest, count * iso::SECTOR_SIZE))
            FATAL("CDVD stream destination outside EE RAM");
        if (iso::read_sectors(stream_lba, count, ee_host(dest & EE_RAM_MASK))) {
            stream_lba += count;
            result(recv, recv_size, count);
        } else {
            result(recv, recv_size, 0);
        }
        break;
    case 3: // StStop
        streaming = false;
        result(recv, recv_size, 1);
        break;
    case 4: // StSeek
    case 9: // StSeekF
        stream_lba = lba;
        result(recv, recv_size, 1);
        break;
    case 5: // StInit: first argument is sectors per stream buffer.
        stream_buffer_sectors = lba;
        streaming = false;
        result(recv, recv_size, 1);
        break;
    case 6: // StStat
        result(recv, recv_size, stream_buffer_sectors);
        break;
    case 7: // StPause
        streaming = false;
        result(recv, recv_size, 1);
        break;
    case 8: // StResume
        streaming = true;
        result(recv, recv_size, 1);
        break;
    default:
        FATAL("Unsupported CDVD stream command %u", command);
    }
}
} // namespace

void call(u32 sid, u32 fno, u32 send, u32 send_size, u32 recv, u32 recv_size) {
    LOG(LOG_CDVD, "CDVD RPC sid=%08x fno=%u send=%08x/%u recv=%08x/%u\n",
        sid, fno, send, send_size, recv, recv_size);
    if (!in_iop(send, send_size) || !in_iop(recv, recv_size))
        FATAL("CDVD RPC packet outside IOP RAM");
    if (recv_size) std::memset(g_iop_ram + (recv & PHYSICAL_MASK), 0, recv_size);
    auto input = [&](u32 bytes) {
        if (send_size < bytes) FATAL("Short CDVD RPC request: sid=%08x fno=%u need=%u got=%u",
                                     sid, fno, bytes, send_size);
    };
    switch (sid) {
    case 0x80000592: // sceCdInit: mode word; result + version words + debug word.
        if (fno != 0) break;
        input(4);
        result(recv, recv_size, 1);
        return;
    case 0x80000593: // Sony S-command ordinals (scmd.c).
        switch (fno) {
        case 1: { // ReadClock: result, then eight-byte sceCdCLOCK in BCD.
            const std::time_t now = std::time(nullptr);
            std::tm clock{};
            localtime_r(&now, &clock);
            auto bcd = [](int n) -> u8 { return static_cast<u8>((n / 10 << 4) | (n % 10)); };
            result(recv, recv_size, 1);
            if (recv_size >= 12) {
                u8* out = g_iop_ram + (recv & PHYSICAL_MASK) + 4;
                out[0] = 0; out[1] = bcd(clock.tm_sec); out[2] = bcd(clock.tm_min);
                out[3] = bcd(clock.tm_hour); out[4] = 0;
                out[5] = bcd(clock.tm_mday); out[6] = bcd(clock.tm_mon + 1);
                out[7] = bcd((clock.tm_year + 1900) % 100);
            }
            return;
        }
        case 2: // WriteClock: return the submitted eight-byte clock.
            input(8);
            result(recv, recv_size, 1);
            if (recv_size >= 12)
                std::memcpy(g_iop_ram + (recv & PHYSICAL_MASK) + 4,
                            g_iop_ram + (send & PHYSICAL_MASK), 8);
            return;
        case 3: result(recv, recv_size, iso::sector_count() ? 0x14 : 0); return; // GetDiskType
        case 4: result(recv, recv_size, 0); return; // GetError
        case 5: input(4); result(recv, recv_size, 1); return; // TrayReq; tray changes = 0
        case 6: // ReadILinkID: result, status, 8-byte ID.
        case 18: // ReadConsoleID
        case 20: // ReadMechaconVersion
        case 26: // ReadModelNumber: result, status, 16-byte name.
        case 67: // ReadRegionParams: result, status, 15-byte region data.
            result(recv, recv_size, 1);
            if (recv_size >= 16 && (fno == 6 || fno == 18))
                std::memcpy(g_iop_ram + (recv & PHYSICAL_MASK) + 8,
                            fno == 6 ? ilink_id : console_id, 8);
            if (fno == 26 && recv_size >= 24)
                std::memcpy(g_iop_ram + (recv & PHYSICAL_MASK) + 8, model_name, 16);
            return;
        case 7: // WriteILinkID
        case 19: // WriteConsoleID
            input(8);
            std::memcpy(fno == 7 ? ilink_id : console_id,
                        g_iop_ram + (send & PHYSICAL_MASK), 8);
            result(recv, recv_size, 1);
            return;
        case 8: // ReadNVM: result, address, two-byte data and status.
        case 9: { // WriteNVM: address, two-byte data.
            input(8);
            const u32 address = word(send);
            if (address >= 0x10000) FATAL("CDVD NVM address out of range: %08x", address);
            if (fno == 9) {
                u16 value;
                std::memcpy(&value, g_iop_ram + (send & PHYSICAL_MASK) + 4, 2);
                nvm[address] = value;
            }
            result(recv, recv_size, 1);
            if (recv_size >= 8) write32(recv + 4, address);
            if (recv_size >= 10)
                std::memcpy(g_iop_ram + (recv & PHYSICAL_MASK) + 8, &nvm[address], 2);
            return;
        }
        case 10: // DecSet (three packed bytes).
            input(4); result(recv, recv_size, 1); return;
        case 11: // ApplySCmd: command byte, length, 16-byte payload; raw 16-byte response.
            input(20);
            if (recv_size && g_iop_ram[(send & PHYSICAL_MASK) + 2] > 16)
                FATAL("CDVD ApplySCmd payload too long");
            return;
        case 13: // SetHDMode
            input(4); result(recv, recv_size, 1); return;
        case 14: // OpenConfig: mode, block, block count, padding.
            input(4);
            config_size = u32(g_iop_ram[(send & PHYSICAL_MASK) + 2]) * 15;
            if (config_size > sizeof(config)) FATAL("CDVD config block count out of range");
            result(recv, recv_size, 1);
            return;
        case 15: // CloseConfig
            result(recv, recv_size, 1);
            config_size = 0;
            return;
        case 16: // ReadConfig: result, status, config data.
            result(recv, recv_size, 1);
            if (recv_size >= 8 + config_size)
                std::memcpy(g_iop_ram + (recv & PHYSICAL_MASK) + 8, config, config_size);
            return;
        case 17: // WriteConfig: 1024-byte request, result and status.
            input(1024);
            std::memcpy(config, g_iop_ram + (send & PHYSICAL_MASK), config_size);
            result(recv, recv_size, 1);
            return;
        case 21: // CtrlADout
        case 25: // AutoAdjustCtrl
        case 29: // SpinCtrl
            input(4);
            result(recv, recv_size, 1);
            return;
        case 24: // ForbidDVDP
        case 28: // ForbidRead
            result(recv, recv_size, 1);
            return;
        case 27: // WriteModelName
            input(16);
            std::memcpy(model_name, g_iop_ram + (send & PHYSICAL_MASK), 16);
            result(recv, recv_size, 1);
            return;
        case 30: // BootCertify
            input(4);
            result(recv, recv_size, 1);
            return;
        case 12: result(recv, recv_size, iso::sector_count() ? 0x0a : 0); return; // Status
        case 22: // Break
        case 23: // ReadSUBQ: result, status, subchannel bytes.
        case 31: // CancelPowerOff
        case 33: // PowerOff
            result(recv, recv_size, 1);
            return;
        case 32: // BlueLedCtrl
        case 34: // MMode
        case 35: // SetThreadPri
            input(4);
            result(recv, recv_size, 1);
            return;
        }
        break;
    case 0x80000595: // Sony N-command ordinals (ncmd.c).
        switch (fno) {
        case 1: // Read: 2048-byte data sectors.
        case 13: // ReadIOPm: data sectors addressed to IOP RAM.
            input(24); // lba, count, destination, mode, alignment DMA, progress DMA.
            if ((word(send + 12) >> 16 & 0xff) != 0)
                FATAL("Unsupported CDVD non-2048 read mode");
            read_disc(word(send), word(send + 4), word(send + 8), recv, recv_size,
                      fno == 13);
            return;
        case 3: { // CdDvdRead: DVD video sectors are 2064 bytes, with a 12-byte header.
            input(24);
            const u32 count = word(send + 4);
            const u32 dest = word(send + 8);
            const u64 bytes = u64(count) * 2064;
            if (bytes > EE_RAM_SIZE || !in_ee(dest, static_cast<u32>(bytes)))
                FATAL("CDVD DVD-video destination outside EE RAM");
            u8* out = ee_host(dest & EE_RAM_MASK);
            bool ok = true;
            for (u32 i = 0; i < count; ++i) {
                std::memset(out, 0, 12);
                ok = iso::read_sectors(word(send) + i, 1, out + 12) && ok;
                std::memset(out + 2060, 0, 4);
                out += 2064;
            }
            result(recv, recv_size, ok ? 1 : 0);
            return;
        }
        case 4: { // GetToc: first word is EE DMA target, 2064 bytes.
            input(12);
            const u32 dest = word(send);
            if (!in_ee(dest, 2064)) FATAL("CDVD GetToc DMA outside EE RAM");
            std::memset(ee_host(dest & EE_RAM_MASK), 0, 2064);
            result(recv, recv_size, iso::sector_count() ? 1 : 0);
            if (recv_size >= 8) write32(recv + 4, iso::sector_count() ? 1 : 0); // DVD TOC
            return;
        }
        case 5: input(4); result(recv, recv_size, 1); return; // Seek
        case 6: case 7: case 8: result(recv, recv_size, 1); return; // Standby/Stop/Pause
        case 9: input(20); stream(send, recv, recv_size); return; // StStart/Read/Stop/Seek/Init/Stat/Pause/Resume
        case 14: result(recv, recv_size, iso::sector_count() ? 2 : 6); return; // DiskReady
        }
        break;
    case 0x80000596: // power-off callback registration (no payload response).
        if (fno == 1) return;
        break;
    case 0x80000597:
        if (fno == 0) { search(send, send_size, recv, recv_size); return; }
        break;
    case 0x8000059a: // Legacy sceCdDiskReady server.
        if (fno != 0) break;
        [[fallthrough]];
    case 0x8000059c: // New sceCdDiskReady server; Sony callback ignores fno.
        input(4);
        result(recv, recv_size, iso::sector_count() ? 2 : 6);
        return;
    }
    FATAL("Unsupported CDVD RPC sid=%08x fno=%u", sid, fno);
}
} // namespace iop::cdvd_rpc
