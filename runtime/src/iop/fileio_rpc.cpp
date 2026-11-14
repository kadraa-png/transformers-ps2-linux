#include "fileio_rpc.h"
#include "ps2/ee_mem.h"
#include "ps2/iop.h"
#include "ps2/iso.h"
#include "ps2/log.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <string>

namespace iop::fileio_rpc {
namespace {
constexpr u32 IOP_MASK = 0x1fffffff;
constexpr s32 NOENT = -2, IO = -5, BADF = -9, NODEV = -19, NOTDIR = -20;
constexpr s32 ISDIR = -21, INVAL = -22, MFILE = -24, ROFS = -30, NOSYS = -88;
constexpr char ROMVER[] = "0200EC20040614\n";
constexpr u32 ROMVER_SIZE = sizeof(ROMVER);
constexpr u32 STAT_SIZE = 64;
struct Handle {
    iso::Entry entry{};
    u32 position = 0;
    bool open = false;
    bool directory = false;
    bool romver = false;
};
std::array<Handle, 64> handles{};
u32 completion_address = 0;

bool iop_range(u32 addr, u32 size) {
    const u32 physical = addr & IOP_MASK;
    return physical <= IOP_RAM_SIZE && size <= IOP_RAM_SIZE - physical;
}
bool ee_range(u32 addr, u32 size) {
    const u32 physical = addr & EE_RAM_MASK;
    return physical <= EE_RAM_SIZE && size <= EE_RAM_SIZE - physical;
}
u8* iop_ptr(u32 addr, u32 size) {
    if (!iop_range(addr, size)) FATAL("FILEIO IOP RAM address outside memory: %08x/%u", addr, size);
    return g_iop_ram + (addr & IOP_MASK);
}
u8* ee_ptr(u32 addr, u32 size) {
    if (!ee_range(addr, size)) FATAL("FILEIO EE RAM address outside memory: %08x/%u", addr, size);
    return ee_host(addr & EE_RAM_MASK);
}
u32 word(u32 addr) { return read32(addr); }
void result(u32 recv, u32 value) { write32(recv, value); }
void need(u32 actual, u32 minimum) {
    if (actual < minimum) FATAL("Short FILEIO RPC packet: got %u need %u", actual, minimum);
}
std::string path(u32 addr) {
    const char* p = reinterpret_cast<const char*>(iop_ptr(addr, 1024));
    const size_t length = strnlen(p, 1024);
    if (length == 1024) FATAL("Unterminated FILEIO path");
    return std::string(p, length);
}
bool disc_path(const std::string& p) {
    return p.compare(0, 7, "cdrom0:") == 0 || p.compare(0, 6, "cdrom:") == 0;
}
s32 lookup(const std::string& p, iso::Entry* entry) {
    if (p == "rom0:ROMVER") {
        *entry = {};
        entry->size = ROMVER_SIZE;
        return 0;
    }
    if (!disc_path(p)) return NODEV;
    if (!iso::sector_count()) return NODEV;
    return iso::find(p, entry) ? 0 : NOENT;
}
Handle* handle(u32 fd, bool directory) {
    const u32 index = fd - 3;
    return index < handles.size() && handles[index].open && handles[index].directory == directory
        ? &handles[index] : nullptr;
}
s32 allocate(const iso::Entry& entry, bool directory, bool romver = false) {
    for (u32 i = 0; i < handles.size(); ++i) {
        if (!handles[i].open) {
            handles[i] = {entry, 0, true, directory, romver};
            return static_cast<s32>(i + 3);
        }
    }
    return MFILE;
}
void stat(u8* out, const iso::Entry& entry) {
    std::memset(out, 0, STAT_SIZE);
    const u32 mode = entry.is_dir ? 0x1000u | 0555u : 0x2000u | 0444u;
    std::memcpy(out, &mode, 4);
    std::memcpy(out + 8, &entry.size, 4);
}
// The Sony 2.8 reply record contains two 64-byte fragments for the EE's
// _fio_read_intr callback; aligned middle bytes go directly into EE RAM.
s32 read_file(u32 send, u8* reply) {
    Handle* h = handle(word(send + 12), false);
    if (!h) return BADF;
    const u32 dest = word(send + 16);
    const s32 requested = static_cast<s32>(word(send + 20));
    if (requested < 0) return INVAL;
    const u32 remaining = h->position < h->entry.size ? h->entry.size - h->position : 0;
    const u32 size = std::min(static_cast<u32>(requested), remaining);
    if (!ee_range(dest, size)) FATAL("FILEIO read destination outside EE RAM: %08x/%u", dest, size);
    const u64 offset = u64(h->entry.lba) * iso::SECTOR_SIZE + h->position;
    auto copy = [&](u64 at, u32 count, u8* out) {
        if (!count) return true;
        if (h->romver) {
            for (u32 n = 0; n < count; ++n) {
                const u32 index = h->position + at + n;
                if (index >= ROMVER_SIZE) FATAL("FILEIO ROMVER read outside 16-byte image");
                out[n] = ROMVER[index];
            }
            return true;
        }
        return iso::read_bytes(offset + at, count, out);
    };
    const u32 first = std::min(size, (64 - (dest & 63)) & 63);
    const u32 middle = (size - first) & ~63u;
    const u32 last = size - first - middle;
    u8* target = ee_ptr(dest, size);
    if (!copy(0, first, reply + 36)) return IO;
    if (!copy(first, middle, target + first)) return IO;
    if (!copy(first + middle, last, reply + 100)) return IO;
    if (first) std::memcpy(target, reply + 36, first);
    if (last) std::memcpy(target + first + middle, reply + 100, last);
    const u32 fields[] = {first, last, dest, dest + first + middle};
    std::memcpy(reply + 20, fields, sizeof(fields));
    h->position += size;
    return static_cast<s32>(size);
}
s32 read_dir(Handle& h, u8* out) {
    // ISO9660 directory records cannot cross a sector boundary. The directory
    // payload itself may end mid-sector; zero-length records skip padding.
    while (h.position < h.entry.size) {
        const u32 absolute = h.position;
        u8 length = 0;
        if (!iso::read_bytes(u64(h.entry.lba) * iso::SECTOR_SIZE + absolute, 1, &length)) return IO;
        if (!length) { h.position = (absolute / iso::SECTOR_SIZE + 1) * iso::SECTOR_SIZE; continue; }
        if (length < 34 || length > h.entry.size - absolute ||
            length > iso::SECTOR_SIZE - (absolute % iso::SECTOR_SIZE)) return IO;
        u8 record[255];
        if (!iso::read_bytes(u64(h.entry.lba) * iso::SECTOR_SIZE + absolute, length, record)) return IO;
        h.position += length;
        const u32 name_len = record[32];
        if (name_len > static_cast<u32>(length - 33)) return IO;
        if (name_len == 1 && record[33] < 2) continue;
        iso::Entry entry{};
        std::memcpy(&entry.lba, record + 2, 4);
        std::memcpy(&entry.size, record + 10, 4);
        entry.is_dir = (record[25] & 2) != 0;
        // Sony 2.8 directory entry: stat[64], name[256].
        std::memset(out, 0, 320);
        stat(out, entry);
        std::memcpy(out + STAT_SIZE, record + 33, name_len);
        return 1;
    }
    return 0;
}
} // namespace

void reset() { handles = {}; completion_address = 0; }

void call(u32 fno, u32 send, u32 send_size, u32 recv, u32 recv_size) {
    iop_ptr(send, send_size);
    iop_ptr(recv, recv_size);
    need(recv_size, 4);
    LOG(LOG_CDVD, "[fileio] RPC fno=%u send=%u\n", fno, send_size);
    if (fno == 255) {
        // ASCII version selects Sony 2.8; second word 2 enables indexed
        // completion records (one 0x440-byte record per request slot).
        need(send_size, 8);
        if (recv_size != 8) FATAL("FILEIO init response size %u", recv_size);
        completion_address = word(send); // Sony may reuse the reply buffer for its request.
        ee_ptr(completion_address, 384);
        const u32 version[2] = {0x30303832, 2};
        std::memcpy(iop_ptr(recv, recv_size), version, sizeof(version));
        return;
    }
    need(send_size, 12); // semaphore ID, EE result address, result byte count.
    std::array<u8, 384> reply{};
    auto set = [&](u32 offset, u32 value) { std::memcpy(reply.data() + offset, &value, 4); };
    set(0, word(send));
    set(4, fno);
    set(8, word(send + 4));
    set(12, word(send + 8));
    u32 reply_size = 20;
    u32 slot = 0;
    s32 status = NOSYS;
    switch (fno) {
    case 0: { // OPEN: mode, aux, path[1024], slot index.
        need(send_size, 1048);
        slot = word(send + 1044);
        const u32 mode = word(send + 12);
        const std::string name = path(send + 20);
        iso::Entry entry;
        status = lookup(name, &entry);
        if (!status) status = (mode & 0xf03) != 1 ? ROFS : entry.is_dir ? ISDIR
            : allocate(entry, false, name == "rom0:ROMVER");
        LOG(LOG_CDVD, "[fileio] open '%s' mode %x -> %d\n", name.c_str(), mode, status);
        break;
    }
    case 1: case 10: { // CLOSE and DCLOSE.
        need(send_size, 20);
        if (fno == 1) slot = word(send + 16);
        Handle* h = handle(word(send + 12), fno == 10);
        if (!h) status = BADF;
        else { *h = {}; status = 0; }
        break;
    }
    case 2:
        need(send_size, 32);
        slot = word(send + 28);
        reply_size = 164;
        status = read_file(send, reply.data());
        break;
    case 3: // WRITE on ISO is read-only.
        need(send_size, 48);
        slot = word(send + 44);
        status = handle(word(send + 12), false) ? ROFS : BADF;
        break;
    case 4: { // LSEEK.
        need(send_size, 28);
        slot = word(send + 24);
        Handle* h = handle(word(send + 12), false);
        if (!h) { status = BADF; break; }
        const u32 whence = word(send + 20);
        if (whence > 2) { status = INVAL; break; }
        const s64 base = whence == 0 ? 0 : whence == 1 ? h->position : h->entry.size;
        const s64 position = base + static_cast<s32>(word(send + 16));
        if (position < 0 || position > 0x7fffffff) { status = INVAL; break; }
        h->position = static_cast<u32>(position);
        status = static_cast<s32>(h->position);
        break;
    }
    case 5: case 26: // IOCTL / IOCTL2.
        need(send_size, 1056);
        slot = word(send + 1052);
        status = handle(word(send + 12), false) ? NOSYS : BADF;
        break;
    case 6: case 8: case 16: case 18: { // REMOVE, RMDIR, DELDRV, CHDIR.
        need(send_size, 13);
        status = disc_path(path(send + 12)) ? (fno == 18 ? 0 : ROFS) : NODEV;
        break;
    }
    case 7: { // MKDIR: mode + path.
        need(send_size, 17);
        status = disc_path(path(send + 16)) ? ROFS : NODEV;
        break;
    }
    case 9: { // DOPEN.
        need(send_size, 13);
        iso::Entry entry;
        status = lookup(path(send + 12), &entry);
        if (!status) status = entry.is_dir ? allocate(entry, true) : NOTDIR;
        break;
    }
    case 11: { // DREAD: EE io_dirent_t pointer.
        need(send_size, 32);
        Handle* h = handle(word(send + 12), true);
        set(20, word(send + 16));
        status = h ? read_dir(*h, reply.data() + 24) : BADF;
        reply_size = 344;
        break;
    }
    case 12: { // GETSTAT: EE io_stat_t pointer followed by path.
        need(send_size, 17);
        iso::Entry entry;
        status = lookup(path(send + 16), &entry);
        set(20, word(send + 12));
        if (!status) stat(reply.data() + 24, entry);
        reply_size = 88;
        break;
    }
    case 13: // CHSTAT: changed-fields mask, stat[64], path.
        need(send_size, 81);
        status = disc_path(path(send + 80)) ? ROFS : NODEV;
        break;
    case 14: // FORMAT: device, block device and options.
        need(send_size, 3088);
        status = disc_path(path(send + 12)) ? ROFS : NODEV;
        break;
    case 15: // ADDDRV cannot alter the virtual IOP device table.
        need(send_size, 16);
        status = NOSYS;
        break;
    case 17: // RENAME: source and destination paths.
        need(send_size, 2060);
        status = disc_path(path(send + 12)) ? ROFS : NODEV;
        break;
    case 19: // Extended path+word command.
        need(send_size, 1044);
        status = disc_path(path(send + 20)) ? NOSYS : NODEV;
        break;
    default: FATAL("Unknown FILEIO RPC function %u", fno);
    }
    set(16, static_cast<u32>(status));
    // Sony's RPC return is a transport acknowledgment, not the operation
    // result. The EE callback consumes the separate completion record.
    result(recv, 1);
    send_fileio_completion(completion_address, reply.data(), reply_size, slot);
}
} // namespace iop::fileio_rpc
