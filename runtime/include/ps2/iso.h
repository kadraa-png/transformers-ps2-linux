#pragma once
// Read-only access to the game disc image (ISO9660, 2048-byte sectors).
// Thread-safe (pread based).
#include "ps2/types.h"
#include <string>

namespace iso {

constexpr u32 SECTOR_SIZE = 2048;

bool open(const char* path);
void close();
u32 sector_count();

// Read `count` 2048-byte sectors starting at `lba` into dst.  Returns false
// on out-of-range reads (the remainder is zero-filled).
bool read_sectors(u32 lba, u32 count, void* dst);
// Byte-granular read.
bool read_bytes(u64 offset, u32 size, void* dst);

struct Entry {
    u32 lba = 0;
    u32 size = 0;
    bool is_dir = false;
};
// Look up "SYSTEM.CNF", "\\IOP\\LIBSD.IRX;1", "cdrom0:\\FMV\\X.PSS;1", "SOUND/MUSIC/A.MS"...
// Case-insensitive, accepts '/' or '\\', an optional "cdrom0:"/"cdrom:" device
// prefix and an optional ";N" version suffix.
bool find(const std::string& path, Entry* out);

} // namespace iso
