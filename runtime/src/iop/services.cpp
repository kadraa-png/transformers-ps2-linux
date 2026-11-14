#include "services.h"
#include "ps2/iop.h"
#include "ps2/ee_mem.h"
#include "ps2/log.h"
#include "ps2/platform.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

namespace iop::services {
namespace {
namespace fs = std::filesystem;
constexpr u32 ram_mask = 0x1fffffff;
struct Pad {
    u32 address = 0;
    u32 frame = 0;
    u8 mode = 7;
    u8 lock = 0;
    u32 press_mask = 0;
    std::array<u8, 6> direct{};
    std::array<u8, 6> align{};
};
std::array<Pad, 2> pads;
std::array<int, 32> handles = [] { std::array<int, 32> f{}; f.fill(-1); return f; }();
std::array<bool, 2> card_seen{};
std::vector<fs::path> listing;
size_t listing_pos = 0;

u32 physical(u32 address) { return address & ram_mask; }
void check_iop(u32 address, u32 size) {
    address = physical(address);
    if (address > IOP_RAM_SIZE || size > IOP_RAM_SIZE - address)
        FATAL("IOP service packet outside IOP RAM");
}
u32 ee_address(u32 address, u32 size) {
    // EE cached/uncached aliases all point at the same 32 MiB main RAM.
    u32 phys = address & EE_RAM_MASK;
    if (size > EE_RAM_SIZE - phys)
        FATAL("IOP service DMA outside EE RAM");
    return phys;
}
u32 load(const u8* ptr) { u32 value; std::memcpy(&value, ptr, 4); return value; }
void store(u8* ptr, u32 value) { std::memcpy(ptr, &value, 4); }
u32 field(const u8* ptr, u32 length, u32 offset) {
    if (offset > length || 4 > length - offset) FATAL("short IOP service RPC packet");
    return load(ptr + offset);
}
std::string name_field(const u8* ptr, u32 length) {
    if (length < 21) FATAL("short memory card path packet");
    const char* text = reinterpret_cast<const char*>(ptr + 20);
    size_t n = strnlen(text, std::min<size_t>(1024, length - 20));
    if (n == std::min<size_t>(1024, length - 20)) FATAL("unterminated memory card path");
    std::string path(text, n);
    auto colon = path.find(':');
    if (colon != std::string::npos) path.erase(0, colon + 1);
    return path;
}
fs::path card_root() {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg) return fs::path(xdg) / "transformers-port" / "memcard0";
    const char* home = std::getenv("HOME");
    if (!home || !*home) FATAL("memory card requires XDG_DATA_HOME or HOME");
    return fs::path(home) / ".local/share/transformers-port/memcard0";
}
fs::path card_path(const std::string& path) {
    fs::path result = card_root();
    for (const auto& part : fs::path(path)) {
        if (part == "/" || part == "." || part.empty()) continue;
        if (part == "..") FATAL("memory card path escapes card");
        result /= part;
        std::error_code ec;
        if (fs::is_symlink(fs::symlink_status(result, ec)))
            FATAL("memory card path contains symlink");
    }
    return result;
}
bool match(const char* pattern, const char* name) {
    if (*pattern == '*') return match(pattern + 1, name) || (*name && match(pattern, name + 1));
    if (*pattern == '?') return *name && match(pattern + 1, name + 1);
    return *pattern == *name && (!*pattern || match(pattern + 1, name + 1));
}
void pad_frame(unsigned port) {
    Pad& pad = pads[port];
    if (!pad.address) return;
    const auto state = platform::pad_state(static_cast<int>(port));
    const bool connected = state.connected;
    constexpr u32 stride = 128; // pad_data_new is double-buffered.
    const u32 slot = pad.frame++ & 1;
    u8* data = ee_host(ee_address(pad.address + slot * stride, stride));
    std::memset(data, 0, stride);
    data[0] = 0;
    data[1] = pad.mode == 4 ? 0x41 : pad.press_mask ? 0x79 : 0x73;
    // Sony libpad rdata[2] = SELECT L3 R3 START UP RIGHT DOWN LEFT (bit 0..7),
    // rdata[3] = L2 R2 L1 R1 TRIANGLE CIRCLE CROSS SQUARE; active low.
    data[2] = static_cast<u8>(state.buttons);
    data[3] = static_cast<u8>(state.buttons >> 8);
    data[4] = state.rx; data[5] = state.ry;
    data[6] = state.lx; data[7] = state.ly;
    std::memcpy(data + 8, state.pressure, 12);
    std::memcpy(data + 32, pad.direct.data(), pad.direct.size());
    std::memcpy(data + 40, pad.align.data(), pad.align.size());
    data[48] = 0; data[52] = 1; // Small and large DualShock actuators.
    data[80] = 4; data[82] = 7; // Mode table entries are mode IDs, not packet IDs.
    store(data + 88, pad.frame);
    store(data + 96, connected ? (pad.mode == 4 ? 4 : pad.press_mask ? 20 : 8) : 0);
    data[100] = 2; // Configured DualShock, queried by padInfoMode.
    data[101] = data[1];
    data[102] = 2; // Controller model.
    data[103] = connected ? 1 : 0;
    data[104] = 2; data[105] = pad.mode == 7 ? 1 : 0;
    data[106] = 2; data[107] = 1;
    data[109] = pad.mode; data[110] = pad.lock;
    data[112] = connected ? 6 : 0; // STABLE / DISCONNECTED.
    data[113] = 0; // Request complete; BUSY would make scePadGetState return EXECCMD.
    data[114] = 1; // currentTask, required by the EE InfoMode/InfoAct readers.
    store(data + 121, connected ? 0x3ffff : 0); // EE padGetButtonMask reads this unaligned word.
}
void pad_call(u32 fno, const u8* request, u32 size, u8* response, u32 rsize) {
    const u32 method = size >= 4 ? load(request) : 0;
    auto result = [&](u32 offset, u32 value) {
        if (offset + 4 > rsize) FATAL("short PADMAN RPC response");
        store(response + offset, value);
    };
    if (fno != 1) {
        LOG(LOG_PAD, "PADMAN unknown RPC function %u (method %08x)\n", fno, method);
        if (rsize >= 16) result(12, 1);
        else if (rsize >= 4) result(0, 1);
        return;
    }
    LOG(LOG_PAD, "PADMAN method %08x port=%u slot=%u\n", method,
        size >= 8 ? load(request + 4) : 0, size >= 12 ? load(request + 8) : 0);
    switch (method) {
    case 0x10: result(12, 1); return; // Init.
    case 0x12: result(12, 0x405); return; // Real PADMAN.IRX v4.05; libpad requires major 4.
    case 0x0c: case 0x8000010b: result(12, 2); return;
    case 0x0d: case 0x8000010c:
        result(12, field(request, size, 4) < pads.size() ? 1 : 0); return;
    case 1: case 0x80000100: {
        const u32 port = field(request, size, 4), slot = field(request, size, 8);
        const u32 area = field(request, size, 16);
        if (port < pads.size() && slot == 0 && area) {
            ee_address(area, 256);
            pads[port] = {area, 0, 7};
            pad_frame(port);
            pad_frame(port);
            result(12, 1);
        } else result(12, 0);
        return;
    }
    case 0x06: case 0x80000105: {
        const u32 port = field(request, size, 4), slot = field(request, size, 8);
        const u32 mode = field(request, size, 12);
        const bool open = port < pads.size() && slot == 0 && pads[port].address;
        if (open && mode <= 1) {
            pads[port].mode = mode ? 7 : 4;
            pads[port].lock = static_cast<u8>(field(request, size, 16));
            pad_frame(port);
        }
        result(20, open && mode <= 1 ? 1 : 0);
        return;
    }
    case 0x07: case 0x80000106:
    case 0x08: case 0x80000107: {
        const u32 port = field(request, size, 4), slot = field(request, size, 8);
        if (size < 18) FATAL("short PADMAN actuator packet");
        const bool open = port < pads.size() && slot == 0 && pads[port].address;
        if (open) {
            auto& values = method == 0x07 || method == 0x80000106
                               ? pads[port].direct : pads[port].align;
            std::memcpy(values.data(), request + 12, values.size());
            pad_frame(port);
        }
        result(20, open ? 1 : 0);
        return;
    }
    case 0x09: case 0x80000108: {
        const u32 port = field(request, size, 4), slot = field(request, size, 8);
        result(12, port < pads.size() && !slot && pads[port].address ? 0x3ffff : 0);
        return;
    }
    case 0x0a: case 0x80000109: {
        const u32 port = field(request, size, 4), slot = field(request, size, 8);
        const bool open = port < pads.size() && slot == 0 && pads[port].address;
        if (open) {
            pads[port].press_mask = field(request, size, 12);
            pad_frame(port);
        }
        result(16, open ? 1 : 0);
        return;
    }
    case 0x0b: case 0x8000010a: {
        const u32 port = field(request, size, 4), slot = field(request, size, 8);
        const bool open = port < pads.size() && slot == 0 && pads[port].address;
        if (size < 24) FATAL("short PADMAN VREF packet");
        result(28, open ? 1 : 0);
        return;
    }
    case 0x0e: case 0x8000010d: {
        const u32 port = field(request, size, 4), slot = field(request, size, 8);
        const bool open = port < pads.size() && slot == 0 && pads[port].address;
        if (open) pads[port] = {};
        result(12, open ? 1 : 0);
        return;
    }
    case 0x0f: case 0x8000010e: pads = {}; result(12, 1); return;
    case 0x80000102: case 0x80000103: case 0x80000104: {
        const u32 port = field(request, size, 4), slot = field(request, size, 8);
        u32 value = 0;
        if (port < pads.size() && !slot && pads[port].address) {
            if (method == 0x80000102) {
                const u32 actuator = field(request, size, 12);
                value = actuator == ~0u ? 2 : actuator < 2 ? (actuator ? 1 : 0) : 0;
            } else if (method == 0x80000103) {
                value = 1;
            } else {
                const u32 query = field(request, size, 12);
                const u32 index = field(request, size, 16);
                if (query == 1 || query == 2) value = pads[port].mode;
                else if (query == 3) value = pads[port].mode == 7 ? 1 : 0;
                else if (query == 4)
                    value = index == ~0u ? 2 : index < 2 ? 4 + 3 * index : 0;
            }
        }
        result(20, value);
        return;
    }
    case 20: {
        const u32 port = field(request, size, 4);
        result(8, port < pads.size() ? 1 : 0);
        return;
    }
    default:
        LOG(LOG_PAD, "PADMAN unknown method %08x (RPC function %u)\n", method, fno);
        if (rsize >= 16) result(12, 1);
        else if (rsize >= 4) result(0, 1);
        return;
    }
}

int card_file(u32 fd) { return fd < handles.size() ? handles[fd] : -1; }
int card_slot(const u8* packet, u32 size, bool named = false) {
    return field(packet, size, named ? 0 : 4) == 0 &&
           field(packet, size, named ? 4 : 8) == 0 ? 0 : -12;
}
void card_call(u32 fno, const u8* packet, u32 size, u8* response, u32 rsize) {
    if (rsize < 4) FATAL("short memory card response");
    int result = -5;
    auto arg = [&](u32 offset) { return field(packet, size, offset); };
    auto name = [&] { return card_path(name_field(packet, size)); };
    auto ee = [&](u32 offset, u32 bytes) { return ee_host(ee_address(arg(offset), bytes)); };
    switch (fno) {
    case 0xfe: case 0x70:
        result = 0;
        if (rsize >= 12) { store(response + 4, 0x20a); store(response + 8, 0x20e); }
        break;
    case 0x01: case 0x78: {
        result = card_slot(packet, size);
        if (!result) {
            std::error_code ec;
            fs::create_directories(card_root(), ec);
            if (ec) { result = -5; break; }
            u32 param = arg(28);
            if (param) {
                u32 n = fno == 0x01 ? 192 : 64;
                u8* dest = ee_host(ee_address(param, n));
                std::memset(dest, 0, n);
                store(dest, 2); store(dest + 4, 0x2000);
                if (fno == 0x01) store(dest + 144, 1);
            }
            result = card_seen[0] ? 0 : -1;
            card_seen[0] = true;
        }
        break;
    }
    case 0x02: case 0x71: {
        result = card_slot(packet, size, true);
        if (result) break;
        auto path = name();
        u32 flags = arg(8);
        std::error_code ec;
        if (flags & 0x40) {
            result = fs::create_directory(path, ec) ? 0 : -4;
            break;
        }
        int os_flags = (flags & 3) == 3 ? O_RDWR : (flags & 2) ? O_WRONLY : O_RDONLY;
        if (flags & 0x200) os_flags |= O_CREAT;
        if (flags & 0x400) os_flags |= O_TRUNC;
        int fd = ::open(path.c_str(), os_flags | O_CLOEXEC | O_NOFOLLOW, 0666);
        if (fd < 0) { result = errno == ENOENT ? -4 : -5; break; }
        result = -7;
        for (u32 i = 0; i < handles.size(); ++i) if (handles[i] < 0) {
            handles[i] = fd; result = static_cast<int>(i); break;
        }
        if (result < 0) ::close(fd);
        break;
    }
    case 0x03: case 0x72: {
        u32 fd = arg(0);
        if (card_file(fd) >= 0) {
            result = ::close(handles[fd]) == 0 ? 0 : -5;
            handles[fd] = -1;
        }
        break;
    }
    case 0x04: case 0x75: {
        int fd = card_file(arg(0));
        if (fd >= 0) {
            off_t pos = ::lseek(fd, static_cast<s32>(arg(16)), static_cast<int>(arg(20)));
            result = pos < 0 ? -5 : static_cast<int>(pos);
        }
        break;
    }
    case 0x05: case 0x73: {
        int fd = card_file(arg(0));
        if (fd < 0) break;
        u32 bytes = arg(12);
        u8* dest = ee(24, bytes);
        ssize_t count = ::read(fd, dest, bytes);
        result = count < 0 ? -5 : static_cast<int>(count);
        if (arg(28)) {
            u8* align = ee(28, fno == 0x05 ? 192 : 64);
            std::memset(align, 0, fno == 0x05 ? 192 : 64);
        }
        break;
    }
    case 0x06: case 0x74: {
        int fd = card_file(arg(0));
        if (fd < 0) break;
        u32 prefix = arg(20), bytes = arg(12);
        if (prefix > 16) FATAL("memory card write prefix exceeds packet");
        if (size < 32 + prefix) FATAL("short memory card write packet");
        ssize_t a = prefix ? ::write(fd, packet + 32, prefix) : 0;
        ssize_t b = a >= 0 && bytes ? ::write(fd, ee(24, bytes), bytes) : 0;
        result = a < 0 || b < 0 ? -5 : static_cast<int>(a + b);
        break;
    }
    case 0x0a: case 0x7a: {
        int fd = card_file(arg(0));
        result = fd >= 0 && ::fsync(fd) == 0 ? 0 : -5;
        break;
    }
    case 0x0d: case 0x76: {
        result = card_slot(packet, size, true);
        if (result) break;
        std::error_code ec;
        if (arg(8) == 0) {
            listing.clear();
            listing_pos = 0;
            fs::path path = name();
            std::string pattern = path.filename().string();
            if (pattern.empty()) { pattern = "*"; }
            else path = path.parent_path();
            if (!fs::is_directory(path, ec)) { result = -4; break; }
            for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) {
                auto filename = it->path().filename().string();
                if (!fs::is_symlink(it->symlink_status(ec)) && match(pattern.c_str(), filename.c_str()))
                    listing.push_back(it->path());
            }
            if (ec) { result = -5; break; }
            std::sort(listing.begin(), listing.end());
        }
        u32 max = arg(12);
        if (max > 0x10000 / 64) FATAL("oversized memory card listing");
        u8* table = max ? ee(16, max * 64) : nullptr;
        result = 0;
        while (listing_pos < listing.size() && static_cast<u32>(result) < max) {
            const auto& entry = listing[listing_pos++];
            u8* row = table + result * 64;
            std::memset(row, 0, 64);
            bool dir = fs::is_directory(entry, ec);
            if (ec) break;
            if (!dir) store(row + 16, static_cast<u32>(fs::file_size(entry, ec)));
            if (ec) break;
            u16 attr = static_cast<u16>(0x8000 | 0x400 | 7 | (dir ? 0x20 : 0x10));
            std::memcpy(row + 20, &attr, 2);
            auto filename = entry.filename().string();
            std::memcpy(row + 32, filename.data(), std::min<size_t>(filename.size(), 31));
            ++result;
        }
        if (ec) result = -5;
        break;
    }
    case 0x0f: case 0x79: {
        result = card_slot(packet, size, true);
        if (result) break;
        std::error_code ec;
        result = fs::remove(name(), ec) ? 0 : ec == std::errc::directory_not_empty ? -6 : -4;
        break;
    }
    case 0x10: case 0x77: {
        result = card_slot(packet, size);
        if (result) break;
        const auto root = card_root();
        std::error_code ec;
        if (fs::is_symlink(fs::symlink_status(root, ec))) { result = -5; break; }
        fs::create_directories(root, ec);
        if (!ec) {
            for (auto& fd : handles) { if (fd >= 0) ::close(fd); fd = -1; }
            for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
                fs::remove_all(it->path(), ec);
        }
        listing.clear();
        listing_pos = 0;
        result = ec ? -5 : 0;
        break;
    }
    case 0x14: result = 0; break;
    default: FATAL("unsupported MCSERV RPC function %u", fno);
    }
    store(response, static_cast<u32>(result));
}
} // namespace

void call(u32 sid, u32 fno, u32 send, u32 send_size, u32 recv, u32 recv_size) {
    check_iop(send, send_size);
    check_iop(recv, recv_size);
    const u32 source = physical(send), destination = physical(recv);
    const u8* request = g_iop_ram + source;
    u8* response = g_iop_ram + destination;
    // Sony libpad uses the same 128-byte buffer for both arguments and reply.
    // Preserve the request before clearing the response, including partial overlap.
    std::array<u8, 1152> small; // Also holds Sony memory-card packets with 1 KiB paths.
    std::vector<u8> large;
    if (send_size && recv_size &&
        source < destination + recv_size && destination < source + send_size) {
        if (send_size <= small.size()) {
            std::memcpy(small.data(), request, send_size);
            request = small.data();
        } else {
            large.assign(request, request + send_size);
            request = large.data();
        }
    }
    std::memset(response, 0, recv_size);
    if (sid == 0x80000100 || sid == 0x80000101)
        pad_call(fno, request, send_size, response, recv_size);
    else if (sid == 0x80000400)
        card_call(fno, request, send_size, response, recv_size);
    else FATAL("unsupported IOP service RPC server %u", sid);
}
void vblank(bool start) {
    if (start) for (unsigned port = 0; port < pads.size(); ++port) pad_frame(port);
}
void reset() {
    for (auto& fd : handles) { if (fd >= 0) ::close(fd); fd = -1; }
    pads = {};
    card_seen = {};
    listing.clear();
    listing_pos = 0;
}
} // namespace iop::services
