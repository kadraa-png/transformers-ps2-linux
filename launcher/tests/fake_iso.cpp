#include "fake_iso.h"

#include <algorithm>
#include <fstream>

namespace {

constexpr size_t SECTOR = 2048;

void put32(Bytes& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b[at + i] = uint8_t(v >> (8 * i));       // little endian
        b[at + 7 - i] = uint8_t(v >> (8 * i));   // big endian copy
    }
}

Bytes record(const std::string& name, uint32_t lba, uint32_t size, bool dir) {
    size_t n = 33 + name.size();
    n += n & 1;
    Bytes r(n, 0);
    r[0] = uint8_t(n);
    put32(r, 2, lba);
    put32(r, 10, size);
    r[25] = dir ? 2 : 0;
    r[32] = uint8_t(name.size());
    for (size_t i = 0; i < name.size(); ++i) r[33 + i] = uint8_t(name[i]);
    return r;
}

size_t sectors(size_t bytes) { return (bytes + SECTOR - 1) / SECTOR; }

}  // namespace

Bytes bytes(const std::string& s) { return Bytes(s.begin(), s.end()); }

Bytes make_iso(const std::map<std::string, Bytes>& files) {
    // Layout: 0-15 system area, 16 PVD, 17 terminator, 18 root dir,
    // then one sector per subdirectory, then file data.
    std::map<std::string, std::map<std::string, const Bytes*>> dirs;  // "" = root
    for (auto& [path, data] : files) {
        auto slash = path.find('/');
        if (slash == std::string::npos) dirs[""][path] = &data;
        else dirs[path.substr(0, slash)][path.substr(slash + 1)] = &data;
    }
    dirs[""];
    std::map<std::string, uint32_t> dir_lba;
    uint32_t next = 19;
    for (auto& [d, _] : dirs)
        if (!d.empty()) dir_lba[d] = next++;
    dir_lba[""] = 18;
    std::map<std::string, uint32_t> file_lba;
    for (auto& [path, data] : files) {
        file_lba[path] = next;
        next += uint32_t(std::max<size_t>(1, sectors(data.size())));
    }

    Bytes img(size_t(next) * SECTOR, 0);
    Bytes pvd(SECTOR, 0);
    pvd[0] = 1;
    std::copy_n("CD001", 5, pvd.begin() + 1);
    Bytes root_rec = record(std::string(1, '\0'), 18, SECTOR, true);
    std::copy(root_rec.begin(), root_rec.end(), pvd.begin() + 156);
    std::copy(pvd.begin(), pvd.end(), img.begin() + 16 * SECTOR);
    img[17 * SECTOR] = 255;
    std::copy_n("CD001", 5, img.begin() + 17 * SECTOR + 1);

    for (auto& [d, entries] : dirs) {
        Bytes dir;
        auto append = [&](const Bytes& r) { dir.insert(dir.end(), r.begin(), r.end()); };
        append(record(std::string(1, '\0'), dir_lba[d], SECTOR, true));
        append(record(std::string(1, '\1'), 18, SECTOR, true));
        if (d.empty())
            for (auto& [sub, lba] : dir_lba)
                if (!sub.empty()) append(record(sub, lba, SECTOR, true));
        for (auto& [name, data] : entries) {
            std::string full = d.empty() ? name : d + "/" + name;
            append(record(name + ";1", file_lba[full], uint32_t(data->size()), false));
        }
        std::copy(dir.begin(), dir.end(), img.begin() + dir_lba[d] * SECTOR);
    }
    for (auto& [path, data] : files)
        std::copy(data.begin(), data.end(), img.begin() + file_lba[path] * SECTOR);
    return img;
}

void write_file(const std::filesystem::path& path, const Bytes& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
}
