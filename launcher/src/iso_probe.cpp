#include "iso_probe.h"

#include "sha256.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <optional>
#include <vector>

namespace launcher {
namespace {

constexpr uint32_t SECTOR = 2048;
// Directory and boot-file sizes beyond these are not a PS2 disc's.
constexpr uint32_t kMaxDirBytes = 1u << 20;
constexpr uint32_t kMaxBootBytes = 64u << 20;

struct Entry {
    uint32_t lba = 0, size = 0;
    bool is_dir = false;
};

uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }

std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// "NAME.EXT;1" -> "NAME.EXT", upper case, trailing '.' dropped (as ISO9660 does).
std::string normalize(std::string s) {
    s = upper(s.substr(0, s.find(';')));
    while (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

class Disc {
  public:
    explicit Disc(const std::filesystem::path& path) : f_(path, std::ios::binary) {}
    bool ok() const { return bool(f_); }

    bool read(uint64_t offset, uint32_t size, std::vector<uint8_t>& out) {
        out.assign(size, 0);
        f_.clear();
        f_.seekg(std::streamoff(offset));
        f_.read(reinterpret_cast<char*>(out.data()), size);
        return f_.gcount() == std::streamsize(size);
    }

    // Root directory from the primary volume descriptor; nullopt if not ISO9660.
    std::optional<Entry> root() {
        std::vector<uint8_t> pvd;
        if (!read(16ull * SECTOR, SECTOR, pvd)) return std::nullopt;
        if (pvd[0] != 1 || std::string(pvd.begin() + 1, pvd.begin() + 6) != "CD001") return std::nullopt;
        return Entry{le32(&pvd[158]), le32(&pvd[166]), true};
    }

    // Path with '/' or '\' separators, relative to the root.
    std::optional<Entry> find(Entry dir, const std::string& path) {
        size_t start = 0;
        while (start <= path.size()) {
            size_t end = path.find_first_of("/\\", start);
            if (end == std::string::npos) end = path.size();
            std::string comp = normalize(path.substr(start, end - start));
            start = end + 1;
            if (comp.empty()) continue;
            if (!dir.is_dir || dir.size > kMaxDirBytes) return std::nullopt;
            std::vector<uint8_t> data;
            if (!read(uint64_t(dir.lba) * SECTOR, dir.size, data)) return std::nullopt;
            std::optional<Entry> found;
            for (size_t pos = 0; pos < data.size();) {
                uint8_t n = data[pos];
                if (n == 0) {  // records never cross a sector boundary
                    pos = (pos / SECTOR + 1) * SECTOR;
                    continue;
                }
                if (n < 34 || pos + n > data.size()) break;
                uint8_t name_len = data[pos + 32];
                if (33u + name_len > n) break;
                std::string name(data.begin() + long(pos) + 33, data.begin() + long(pos) + 33 + name_len);
                if (normalize(name) == comp) {
                    found = Entry{le32(&data[pos + 2]), le32(&data[pos + 10]), (data[pos + 25] & 2) != 0};
                    break;
                }
                pos += n;
            }
            if (!found) return std::nullopt;
            dir = *found;
        }
        return dir;
    }

  private:
    std::ifstream f_;
};

// "cdrom0:\SLUS_206.68;1" -> "\SLUS_206.68;1"
std::optional<std::string> boot_path(const std::string& cnf) {
    size_t pos = 0;
    while (pos < cnf.size()) {
        size_t eol = cnf.find('\n', pos);
        std::string line = cnf.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = eol == std::string::npos ? cnf.size() : eol + 1;
        size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || upper(line.substr(first, 5)) != "BOOT2") continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string value = line.substr(eq + 1);
        size_t colon = value.find(':');
        if (colon != std::string::npos) value = value.substr(colon + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
        if (!value.empty()) return value;
    }
    return std::nullopt;
}

}  // namespace

ProbeResult probe_iso(const std::filesystem::path& iso, std::string_view expected_sha256) {
    ProbeResult r;
    Disc disc(iso);
    std::error_code ec;
    if (!disc.ok() || !std::filesystem::is_regular_file(iso, ec)) return r;  // ReadError
    auto root = disc.root();
    if (!root) {
        r.status = ProbeStatus::NotIso;
        return r;
    }
    r.status = ProbeStatus::NoBootFile;
    auto cnf_entry = disc.find(*root, "SYSTEM.CNF");
    if (!cnf_entry || cnf_entry->is_dir || cnf_entry->size > kMaxDirBytes) return r;
    std::vector<uint8_t> cnf;
    if (!disc.read(uint64_t(cnf_entry->lba) * SECTOR, cnf_entry->size, cnf)) return r;
    auto path = boot_path(std::string(cnf.begin(), cnf.end()));
    if (!path) return r;
    std::string file = *path;
    size_t slash = file.find_last_of("/\\");
    if (slash != std::string::npos) file = file.substr(slash + 1);
    r.boot = normalize(file);

    auto elf_entry = disc.find(*root, *path);
    if (!elf_entry || elf_entry->is_dir || elf_entry->size > kMaxBootBytes) return r;
    std::vector<uint8_t> elf;
    if (!disc.read(uint64_t(elf_entry->lba) * SECTOR, elf_entry->size, elf)) {
        r.status = ProbeStatus::ReadError;
        return r;
    }
    r.elf_sha256 = sha256_hex(elf.data(), elf.size());
    if (r.boot != kExpectedBoot) r.status = ProbeStatus::WrongDisc;
    else if (r.elf_sha256 != expected_sha256) r.status = ProbeStatus::BadChecksum;
    else r.status = ProbeStatus::Ok;
    return r;
}

std::string display_serial(std::string_view boot) {
    std::string s;
    for (char c : boot) {
        if (c == '_') s += '-';
        else if (c != '.') s += c;
    }
    return s;
}

std::string probe_message(const ProbeResult& r) {
    const std::string needed = display_serial(kExpectedBoot);
    switch (r.status) {
    case ProbeStatus::Ok: return "This is the right disc (" + needed + ").";
    case ProbeStatus::NotIso: return "This file is not a PS2 disc image (.iso).";
    case ProbeStatus::NoBootFile: return "This disc image has no PS2 boot file.";
    case ProbeStatus::WrongDisc:
        return "This disc is " + display_serial(r.boot) + ". This port needs the US release, " + needed + ".";
    case ProbeStatus::BadChecksum:
        return "This looks like " + needed +
               " but its files don't match the original disc (modified or damaged image).";
    case ProbeStatus::ReadError: break;
    }
    return "Could not read this file.";
}

}  // namespace launcher
