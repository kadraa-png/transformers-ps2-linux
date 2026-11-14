#pragma once
// Tiny ISO9660 images for tests: one level of subdirectories, 2048-byte sectors.
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

using Bytes = std::vector<uint8_t>;

Bytes bytes(const std::string& s);
// Keys are "NAME" (root) or "DIR/NAME"; files get the ";1" version suffix on disc.
Bytes make_iso(const std::map<std::string, Bytes>& files);
void write_file(const std::filesystem::path& path, const Bytes& data);
