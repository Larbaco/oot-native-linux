// LZ4 block format (lz4_Block_format.md): sequences of a token, literals and a match.
#include "nxload/module.hpp"

namespace nxload {

void lz4_decompress(std::span<const uint8_t> src, std::span<uint8_t> dst) {
    size_t ip = 0, op = 0;
    const size_t in = src.size(), out = dst.size();
    auto length = [&](size_t n) {
        if (n == 15)
            for (uint8_t b = 255; b == 255; n += b) {
                if (ip >= in) fail("lz4: truncated length");
                b = src[ip++];
            }
        return n;
    };
    for (;;) {
        if (ip >= in) fail("lz4: truncated input");
        const uint8_t token = src[ip++];
        const size_t lit = length(token >> 4);
        if (lit > in - ip) fail("lz4: literals past the end of the input");
        if (lit > out - op) fail("lz4: literals past the end of the output");
        std::memcpy(dst.data() + op, src.data() + ip, lit);
        ip += lit, op += lit;
        if (ip == in) break;   // the last sequence has literals only
        if (in - ip < 2) fail("lz4: truncated match offset");
        const size_t off = src[ip] | size_t(src[ip + 1]) << 8;
        ip += 2;
        if (off == 0 || off > op) fail("lz4: match offset " + std::to_string(off) + " out of range");
        const size_t len = length(token & 15) + 4;
        if (len > out - op) fail("lz4: match past the end of the output");
        uint8_t* d = dst.data() + op;
        if (off >= len) std::memcpy(d, d - off, len);
        else for (size_t i = 0; i < len; i++) d[i] = d[i - off];   // overlapping: repeats the last off bytes
        op += len;
    }
    if (op != out) fail("lz4: decompressed " + std::to_string(op) + " bytes, expected " + std::to_string(out));
}

}  // namespace nxload
