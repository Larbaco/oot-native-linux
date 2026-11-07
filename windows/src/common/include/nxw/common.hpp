// Shared helpers: errors, whole-file reads, little-endian loads.
#pragma once
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace nxw {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(const std::string& what);

std::vector<uint8_t> read_file(const std::filesystem::path& p);
void write_file(const std::filesystem::path& p, std::span<const uint8_t> data);

template <class T> T load_le(std::span<const uint8_t> b, size_t off) {
    if (off > b.size() || b.size() - off < sizeof(T)) fail("read past the end at offset " + std::to_string(off));
    T v;
    std::memcpy(&v, b.data() + off, sizeof v);   // x86-64 and arm64 hosts are little-endian
    return v;
}

}  // namespace nxw
