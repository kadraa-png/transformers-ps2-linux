#include "ps2/iso.h"
#include "ps2/log.h"

#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace iso {
namespace {

int g_fd = -1;
u64 g_size = 0;
u32 g_root_lba = 0, g_root_size = 0;

struct DirCache {
    std::mutex lock;
    // key: upper-case directory path ("" = root) -> entries (upper-case name -> Entry)
    std::unordered_map<std::string, std::unordered_map<std::string, Entry>> dirs;
} g_cache;

std::string upper(std::string s) {
    for (auto& ch : s) ch = (char)std::toupper((unsigned char)ch);
    return s;
}

const std::unordered_map<std::string, Entry>* load_dir(const std::string& key, u32 lba, u32 size) {
    auto it = g_cache.dirs.find(key);
    if (it != g_cache.dirs.end()) return &it->second;
    std::vector<u8> buf((size + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1));
    if (!read_sectors(lba, (u32)(buf.size() / SECTOR_SIZE), buf.data())) return nullptr;
    std::unordered_map<std::string, Entry> entries;
    u32 pos = 0;
    while (pos < size) {
        u8 len = buf[pos];
        if (len == 0) {  // records never straddle sectors: skip to next sector
            pos = (pos / SECTOR_SIZE + 1) * SECTOR_SIZE;
            continue;
        }
        const u8* r = &buf[pos];
        Entry e;
        memcpy(&e.lba, r + 2, 4);
        memcpy(&e.size, r + 10, 4);
        e.is_dir = (r[25] & 2) != 0;
        u8 nlen = r[32];
        std::string name((const char*)r + 33, nlen);
        pos += len;
        if (nlen == 1 && (name[0] == 0 || name[0] == 1)) continue;  // "." / ".."
        auto semi = name.find(';');
        if (semi != std::string::npos) name.resize(semi);
        if (!name.empty() && name.back() == '.') name.pop_back();
        entries[upper(name)] = e;
    }
    return &(g_cache.dirs[key] = std::move(entries));
}

} // namespace

bool open(const char* path) {
    g_fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (g_fd < 0) return false;
    struct stat st;
    fstat(g_fd, &st);
    g_size = (u64)st.st_size;
    u8 pvd[SECTOR_SIZE];
    if (!read_sectors(16, 1, pvd) || pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) {
        LOG(LOG_CDVD, "iso: %s has no ISO9660 primary volume descriptor\n", path);
        ::close(g_fd);
        g_fd = -1;
        return false;
    }
    memcpy(&g_root_lba, pvd + 156 + 2, 4);
    memcpy(&g_root_size, pvd + 156 + 10, 4);
    return true;
}

void close() {
    if (g_fd >= 0) ::close(g_fd);
    g_fd = -1;
}

u32 sector_count() { return (u32)(g_size / SECTOR_SIZE); }

bool read_bytes(u64 offset, u32 size, void* dst) {
    u8* d = (u8*)dst;
    bool ok = true;
    if (offset >= g_size) {
        memset(dst, 0, size);
        return false;
    }
    if (offset + size > g_size) {
        u32 valid = (u32)(g_size - offset);
        memset(d + valid, 0, size - valid);
        size = valid;
        ok = false;
    }
    while (size) {
        ssize_t n = pread(g_fd, d, size, (off_t)offset);
        if (n <= 0) {
            memset(d, 0, size);
            return false;
        }
        d += n;
        offset += (u64)n;
        size -= (u32)n;
    }
    return ok;
}

bool read_sectors(u32 lba, u32 count, void* dst) {
    return read_bytes((u64)lba * SECTOR_SIZE, count * SECTOR_SIZE, dst);
}

bool find(const std::string& in_path, Entry* out) {
    std::string p = in_path;
    auto colon = p.find(':');
    if (colon != std::string::npos) p = p.substr(colon + 1);
    for (auto& ch : p) if (ch == '\\') ch = '/';
    auto semi = p.find(';');
    if (semi != std::string::npos) p.resize(semi);
    p = upper(p);
    std::lock_guard<std::mutex> lk(g_cache.lock);
    std::string dir_key;
    u32 lba = g_root_lba, size = g_root_size;
    size_t pos = 0;
    Entry cur{lba, size, true};
    while (pos <= p.size()) {
        size_t slash = p.find('/', pos);
        std::string comp = p.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        pos = slash == std::string::npos ? p.size() + 1 : slash + 1;
        if (comp.empty()) continue;
        if (!comp.empty() && comp.back() == '.') comp.pop_back();
        if (!cur.is_dir) return false;
        auto* d = load_dir(dir_key, cur.lba, cur.size);
        if (!d) return false;
        auto it = d->find(comp);
        if (it == d->end()) return false;
        cur = it->second;
        dir_key += "/" + comp;
    }
    *out = cur;
    return true;
}

} // namespace iso
