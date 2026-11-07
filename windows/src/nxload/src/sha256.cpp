#include "nxload/module.hpp"

#include <algorithm>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#elif defined(__linux__)
#include <cstring>
#else
#include <CommonCrypto/CommonDigest.h>   // the Mac build (the swap-in harness, docs/m4-swapin.md)
#endif

namespace nxload {

#ifdef _WIN32
std::array<uint8_t, 32> sha256(std::span<const uint8_t> data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    std::array<uint8_t, 32> out{};
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
              BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0));
    for (size_t p = 0; ok && p < data.size();) {
        const ULONG n = ULONG(std::min<size_t>(data.size() - p, 1u << 30));
        ok = BCRYPT_SUCCESS(BCryptHashData(h, const_cast<PUCHAR>(data.data() + p), n, 0));
        p += n;
    }
    ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(h, out.data(), ULONG(out.size()), 0));
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) fail("SHA-256: BCrypt failed");
    return out;
}
#elif defined(__linux__)
// FIPS 180-4
namespace {
constexpr uint32_t k_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
inline uint32_t rr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
struct Sha {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t b[64]; size_t n = 0; uint64_t total = 0;
    void block(const uint8_t* p) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
        for (int i = 16; i < 64; i++) w[i] = w[i - 16] + (rr(w[i - 15], 7) ^ rr(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] + (rr(w[i - 2], 17) ^ rr(w[i - 2], 19) ^ (w[i - 2] >> 10));
        uint32_t s[8]; std::memcpy(s, h, sizeof s);
        for (int i = 0; i < 64; i++) {
            const uint32_t t1 = s[7] + (rr(s[4], 6) ^ rr(s[4], 11) ^ rr(s[4], 25)) + ((s[4] & s[5]) ^ (~s[4] & s[6])) + k_k[i] + w[i];
            const uint32_t t2 = (rr(s[0], 2) ^ rr(s[0], 13) ^ rr(s[0], 22)) + ((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
            s[7] = s[6]; s[6] = s[5]; s[5] = s[4]; s[4] = s[3] + t1; s[3] = s[2]; s[2] = s[1]; s[1] = s[0]; s[0] = t1 + t2;
        }
        for (int i = 0; i < 8; i++) h[i] += s[i];
    }
    void add(const uint8_t* p, size_t len) {
        total += len;
        while (len) {
            if (!n && len >= 64) { block(p); p += 64; len -= 64; continue; }
            const size_t k = std::min(len, 64 - n); std::memcpy(b + n, p, k); n += k; p += k; len -= k;
            if (n == 64) { block(b); n = 0; }
        }
    }
    void end(uint8_t* out) {
        const uint64_t bits = total * 8; const uint8_t pad = 0x80, z = 0;
        add(&pad, 1); while (n != 56) add(&z, 1);
        uint8_t l[8]; for (int i = 0; i < 8; i++) l[i] = uint8_t(bits >> (56 - 8 * i));
        add(l, 8);
        for (int i = 0; i < 8; i++) for (int k = 0; k < 4; k++) out[i * 4 + k] = uint8_t(h[i] >> (24 - 8 * k));
    }
};
}  // namespace
std::array<uint8_t, 32> sha256(std::span<const uint8_t> data) {
    std::array<uint8_t, 32> out{};
    Sha c; c.add(data.data(), data.size()); c.end(out.data());
    return out;
}
extern "C" void nxload_sha256_2(const void* a, size_t na, const void* b, size_t nb, uint8_t* out) {
    Sha c; c.add(static_cast<const uint8_t*>(a), na); if (nb) c.add(static_cast<const uint8_t*>(b), nb); c.end(out);
}
#else
std::array<uint8_t, 32> sha256(std::span<const uint8_t> data) {
    std::array<uint8_t, 32> out{};
    CC_SHA256_CTX c;
    CC_SHA256_Init(&c);
    for (size_t p = 0; p < data.size();) {
        const CC_LONG n = CC_LONG(std::min<size_t>(data.size() - p, 1u << 30));
        CC_SHA256_Update(&c, data.data() + p, n);
        p += n;
    }
    CC_SHA256_Final(out.data(), &c);
    return out;
}
#endif

}  // namespace nxload
