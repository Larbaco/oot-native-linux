// Standalone GPL-2.0 utility: convert ZBIC-compressed NSOs to uncompressed NSOs.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" size_t nxload_zbic_decompress(void*, size_t, const void*, size_t);
extern "C" unsigned long long nxload_zbic_content_size(const void*, size_t);
extern "C" int nxload_zbic_is_error(size_t);
extern "C" const char* nxload_zbic_error_name(size_t);

static uint32_t get32(const std::vector<uint8_t>& b, size_t p) {
    if (p + 4 > b.size()) throw std::runtime_error("truncated NSO header");
    return uint32_t(b[p]) | uint32_t(b[p + 1]) << 8 | uint32_t(b[p + 2]) << 16 | uint32_t(b[p + 3]) << 24;
}
static void put32(std::vector<uint8_t>& b, size_t p, uint32_t v) {
    if (p + 4 > b.size()) throw std::runtime_error("truncated NSO header");
    for (unsigned i = 0; i < 4; ++i) b[p + i] = uint8_t(v >> (8 * i));
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--stream") {
        try {
            std::vector<uint8_t> input((std::istreambuf_iterator<char>(std::cin)), {});
            if (input.size() < 4 || std::memcmp(input.data(), "ZBIC", 4)) throw std::runtime_error("input is not a ZBIC frame");
            const auto size = nxload_zbic_content_size(input.data(), input.size());
            if (size == std::numeric_limits<unsigned long long>::max() || size == std::numeric_limits<unsigned long long>::max() - 1 || size > std::numeric_limits<size_t>::max())
                throw std::runtime_error("frame has no bounded content size");
            std::vector<uint8_t> output(static_cast<size_t>(size));
            const size_t n = nxload_zbic_decompress(output.data(), output.size(), input.data(), input.size());
            if (nxload_zbic_is_error(n)) throw std::runtime_error(std::string("ZBIC decompression failed: ") + nxload_zbic_error_name(n));
            if (n != output.size()) throw std::runtime_error("ZBIC decompressed size mismatch");
            std::cout.write(reinterpret_cast<const char*>(output.data()), static_cast<std::streamsize>(output.size()));
            if (!std::cout) throw std::runtime_error("cannot write decompressed stream");
            return 0;
        } catch (const std::exception& e) { std::cerr << "zbic-unpack: " << e.what() << '\n'; return 1; }
    }
    if (argc != 2) { std::cerr << "usage: zbic-unpack <build-copy.nso> | --stream < input.zbic > output\n"; return 2; }
    const std::filesystem::path path = argv[1];
    try {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("cannot open input");
        std::vector<uint8_t> input((std::istreambuf_iterator<char>(in)), {});
        if (input.size() < 0x100 || std::memcmp(input.data(), "NSO0", 4)) throw std::runtime_error("not an NSO0 file");
        const uint32_t flags = get32(input, 0x0c);
        if (!(flags & 0x80)) return 0;
        std::vector<uint8_t> output(input.begin(), input.begin() + 0x100);
        uint32_t new_flags = flags & ~0x87u;
        for (unsigned i = 0; i < 3; ++i) {
            const size_t desc = 0x10 + 0x10 * i;
            const uint32_t source_off = get32(input, desc);
            const uint32_t size = get32(input, desc + 8);
            const uint32_t stored_size = get32(input, 0x60 + 4 * i);
            const bool compressed = (flags >> i) & 1;
            const uint64_t end = uint64_t(source_off) + (compressed ? stored_size : size);
            if (end > input.size()) throw std::runtime_error("NSO segment extends past end of file");
            if (output.size() > UINT32_MAX) throw std::runtime_error("normalized NSO is too large");
            put32(output, desc, uint32_t(output.size()));
            if (compressed) {
                const size_t old_size = output.size();
                output.resize(old_size + size);
                const size_t n = nxload_zbic_decompress(output.data() + old_size, size, input.data() + source_off, stored_size);
                if (nxload_zbic_is_error(n)) {
                    const auto frame_size = nxload_zbic_content_size(input.data() + source_off, stored_size);
                    std::string detail = "ZBIC segment " + std::to_string(i) + " failed: " + nxload_zbic_error_name(n);
                    if (frame_size == std::numeric_limits<unsigned long long>::max()) detail += "; frame content size unknown";
                    else if (frame_size == std::numeric_limits<unsigned long long>::max() - 1) detail += "; frame header invalid";
                    else detail += "; frame declares " + std::to_string(frame_size) + " bytes, NSO header expects " + std::to_string(size);
                    throw std::runtime_error(detail);
                }
                if (n != size) throw std::runtime_error("ZBIC segment " + std::to_string(i) + " produced " + std::to_string(n) + " bytes; NSO header expects " + std::to_string(size));
            } else {
                output.insert(output.end(), input.begin() + source_off, input.begin() + source_off + size);
            }
            put32(output, 0x60 + 4 * i, size);
        }
        put32(output, 0x0c, new_flags);
        const auto temp = path.string() + ".zbic-part";
        { std::ofstream out(temp, std::ios::binary | std::ios::trunc); out.write(reinterpret_cast<const char*>(output.data()), output.size());
          if (!out) throw std::runtime_error("cannot write normalized NSO"); }
        std::filesystem::rename(temp, path);
    } catch (const std::exception& e) { std::cerr << "zbic-unpack: " << e.what() << '\n'; return 1; }
    return 0;
}
