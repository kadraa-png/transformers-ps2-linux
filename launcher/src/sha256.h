#pragma once
#include <cstddef>
#include <string>

namespace launcher {

// Lower-case hex SHA-256 of a buffer.
std::string sha256_hex(const void* data, size_t size);

}  // namespace launcher
