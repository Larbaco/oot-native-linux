/* Synthetic tests of s2tex: the in-GOB orders against the public formula, round trips, the per-byte address against the copy loops,
 * parameter checks and hand-computed mip chains. MIT licence, part of pokopia-native. */
#include "s2tex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; if (fails < 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(uint32_t n) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return n ? (uint32_t)(rng % n) : 0; }

/* The Tegra X1 formula as public descriptions of block linear write it */
static uint32_t tx1_formula(uint32_t x, uint32_t y) {
    return (x % 64 / 32) * 256 + (y % 8 / 2) * 64 + (x % 32 / 16) * 32 + (y % 2) * 16 + x % 16;
}
static uint32_t swap56(uint32_t o) { return (o & ~0x60u) | ((o >> 5 & 1) << 6) | ((o >> 6 & 1) << 5); }

static void test_gob(void) {
    for (int order = 0; order < 2; order++) {
        unsigned char hit[512] = {0};
        for (uint32_t y = 0; y < 8; y++)
            for (uint32_t x = 0; x < 64; x++) {
                uint32_t o = s2tex_gob_offset(x, y, (s2tex_order)order);
                uint32_t want = order ? swap56(tx1_formula(x, y)) : tx1_formula(x, y);
                CHECK(o == want, "order %d (%u,%u): %u, want %u", order, x, y, o, want);
                CHECK(o < 512 && !hit[o], "order %d (%u,%u): offset %u reused", order, x, y, o);
                if (o < 512) hit[o] = 1;
                CHECK(s2tex_gob_offset(x + 64 * rnd(100), y + 8 * rnd(100), (s2tex_order)order) == o, "order %d: GOB offset not periodic", order);
            }
    }
    /* Hand-computed Switch 2 points: x4 -> bit 6, y1 -> bit 5 */
    CHECK(s2tex_gob_offset(16, 0, S2TEX_SWITCH2) == 64, "S2 (16,0)");
    CHECK(s2tex_gob_offset(0, 2, S2TEX_SWITCH2) == 32, "S2 (0,2)");
    CHECK(s2tex_gob_offset(0, 1, S2TEX_SWITCH2) == 16, "S2 (0,1)");
    CHECK(s2tex_gob_offset(0, 4, S2TEX_SWITCH2) == 128, "S2 (0,4)");
    CHECK(s2tex_gob_offset(32, 0, S2TEX_SWITCH2) == 256, "S2 (32,0)");
    CHECK(s2tex_gob_offset(63, 7, S2TEX_SWITCH2) == 511, "S2 (63,7)");
    CHECK(s2tex_gob_offset(16, 0, S2TEX_TX1) == 32 && s2tex_gob_offset(0, 2, S2TEX_TX1) == 64, "TX1 (16,0) / (0,2)");
}

/* Hand-computed surface addresses (2D and 3D) */
static void test_offsets(void) {
    /* 256x64 RGBA8, block height 2^1: 16 GOBs per row, 1 KB blocks, 4 block rows */
    s2tex_surface s = {256, 64, 1, 4, 1, 0, 0, S2TEX_TX1};
    CHECK(s2tex_size(&s) == 16u * 1024 * 4, "size 256x64");
    CHECK(s2tex_offset(&s, 64, 0, 0) == 1024, "next block column");
    CHECK(s2tex_offset(&s, 0, 8, 0) == 512, "second GOB of a block");
    CHECK(s2tex_offset(&s, 0, 16, 0) == 16 * 1024, "second block row");
    CHECK(s2tex_offset(&s, 100, 19, 0) == 16 * 1024 + 1024 + tx1_formula(36, 3), "inner point");
    CHECK(s2tex_offset(&s, 100, 27, 0) == 16 * 1024 + 1024 + 512 + tx1_formula(36, 3), "inner point, second GOB");
    /* 3D: 16x16x8 RGBA8, block height 2^1, depth 2^2: 1 GOB column, blocks of 1x2x4 GOBs = 4 KB */
    s2tex_surface v = {16, 16, 8, 4, 1, 2, 0, S2TEX_SWITCH2};
    CHECK(s2tex_size(&v) == 2u * 4096, "3D size");
    CHECK(s2tex_offset(&v, 0, 8, 0) == 512, "3D: GOB y");
    CHECK(s2tex_offset(&v, 0, 0, 1) == 1024, "3D: GOB z");
    CHECK(s2tex_offset(&v, 0, 0, 4) == 4096, "3D: next block layer");
    CHECK(s2tex_offset(&v, 16, 9, 5) == 4096 + 1024 + 512 + swap56(tx1_formula(16, 1)), "3D inner point");
    /* Padded rows: gobs_x larger than the width needs */
    s2tex_surface p = {16, 8, 1, 4, 0, 0, 3, S2TEX_TX1};
    CHECK(s2tex_size(&p) == 3 * 512, "padded size");
    CHECK(s2tex_offset(&p, 0, 8, 0) == 3 * 512, "padded: next block row");
}

static void test_params(void) {
    unsigned char a[4096], b[4096];
    s2tex_surface s = {16, 8, 1, 4, 0, 0, 0, S2TEX_TX1};
    CHECK(s2tex_deswizzle(&s, a, sizeof a, b, 64, 0) == S2TEX_OK, "valid");
    CHECK(s2tex_deswizzle(&s, a, 511, b, 64, 0) == S2TEX_ESIZE, "short storage");
    CHECK(s2tex_deswizzle(&s, a, sizeof a, b, 63, 0) == S2TEX_EINVAL, "short pitch");
    s2tex_surface t = s; t.block_height = 6;
    CHECK(s2tex_swizzle(&t, a, 64, 0, b, sizeof b) == S2TEX_EINVAL, "block height 6");
    t = s; t.bytes_per_block = 0;
    CHECK(s2tex_size(&t) == 0, "zero bytes per block");
    t = s; t.gobs_x = 0; t.width = 17;
    CHECK(s2tex_size(&t) == 2 * 512, "68-byte rows take 2 GOBs");
    t.gobs_x = 1;
    CHECK(s2tex_size(&t) == 0, "gobs_x below the width");
    t = s; t.order = (s2tex_order)7;
    CHECK(s2tex_size(&t) == 0, "unknown order");
}

/* Random surfaces: the copy loops agree with s2tex_offset byte for byte, the mapping is one to one inside s2tex_size, and swizzle undoes deswizzle */
static void test_random(int n) {
    for (int i = 0; i < n; i++) {
        s2tex_surface s;
        int three = rnd(4) == 0;
        s.bytes_per_block = 1 + rnd(16);
        s.width = 1 + rnd(three ? 40 : 300);
        s.height = 1 + rnd(three ? 40 : 200);
        s.depth = three ? 1 + rnd(20) : 1;
        s.block_height = rnd(6);
        s.block_depth = three ? rnd(6) : 0;
        uint32_t need = (s.width * s.bytes_per_block + 63) / 64;
        s.gobs_x = rnd(3) == 0 ? need + rnd(4) : 0;
        s.order = (s2tex_order)rnd(2);
        size_t row = (size_t)s.width * s.bytes_per_block, pitch = row + (rnd(2) ? rnd(40) : 0);
        size_t slice = (size_t)s.height * pitch + (rnd(2) ? rnd(100) : 0);
        uint64_t size = s2tex_size(&s);
        size_t lin_size = slice * s.depth;
        unsigned char *bl = malloc(size), *bl2 = malloc(size), *lin = malloc(lin_size), *out = malloc(lin_size), *hit = calloc(size, 1);
        for (uint64_t k = 0; k < size; k++) bl[k] = (unsigned char)rnd(256);
        memset(out, 0xA5, lin_size);
        CHECK(s2tex_deswizzle(&s, bl, size, out, pitch, slice) == S2TEX_OK, "deswizzle");
        int bad = 0;
        for (uint32_t z = 0; z < s.depth && !bad; z++)
            for (uint32_t y = 0; y < s.height && !bad; y++)
                for (uint32_t x = 0; x < row; x++) {
                    uint64_t o = s2tex_offset(&s, x, y, z);
                    if (o >= size || hit[o] || out[z * slice + y * pitch + x] != bl[o]) { bad = 1; break; }
                    hit[o] = 1;
                }
        CHECK(!bad, "surface %u x %u x %u, %u B, bh %u bd %u gobs %u order %d: copy, address or bijection", s.width, s.height, s.depth,
              s.bytes_per_block, s.block_height, s.block_depth, s.gobs_x, s.order);
        int pad_ok = 1;
        for (uint32_t z = 0; z < s.depth; z++)
            for (uint32_t y = 0; y < s.height; y++)
                for (size_t x = row; x < pitch && (z * slice + y * pitch + x) < lin_size; x++)
                    if (out[z * slice + y * pitch + x] != 0xA5) pad_ok = 0;
        CHECK(pad_ok, "row padding written");
        memcpy(bl2, bl, size);
        for (size_t k = 0; k < lin_size; k++) lin[k] = (unsigned char)rnd(256);
        CHECK(s2tex_swizzle(&s, lin, pitch, slice, bl2, size) == S2TEX_OK, "swizzle");
        int untouched = 1;
        for (uint64_t k = 0; k < size; k++) if (!hit[k] && bl2[k] != bl[k]) untouched = 0;
        CHECK(untouched, "swizzle wrote outside the texels");
        memset(out, 0, lin_size);
        s2tex_deswizzle(&s, bl2, size, out, pitch, slice);
        int same = 1;
        for (uint32_t z = 0; z < s.depth; z++)
            for (uint32_t y = 0; y < s.height; y++)
                if (memcmp(out + z * slice + y * pitch, lin + z * slice + y * pitch, row)) same = 0;
        CHECK(same, "round trip");
        free(bl); free(bl2); free(lin); free(out); free(hit);
    }
}

/* Switch 2 storage is Tegra X1 storage with every GOB's bytes moved by the bit 5 / 6 exchange */
static void test_relation(void) {
    s2tex_surface a = {200, 70, 1, 4, 3, 0, 0, S2TEX_TX1}, b = a;
    b.order = S2TEX_SWITCH2;
    uint64_t size = s2tex_size(&a);
    unsigned char *lin = malloc(200 * 4 * 70), *x = calloc(size, 1), *y = calloc(size, 1);
    for (int k = 0; k < 200 * 4 * 70; k++) lin[k] = (unsigned char)rnd(256);
    s2tex_swizzle(&a, lin, 800, 0, x, size);
    s2tex_swizzle(&b, lin, 800, 0, y, size);
    int ok = 1;
    for (uint64_t k = 0; k < size; k++) if (y[(k & ~511ull) | swap56((uint32_t)(k & 511))] != x[k]) ok = 0;
    CHECK(ok, "Switch 2 = Tegra X1 with bits 5 and 6 exchanged");
    free(lin); free(x); free(y);
}

static void test_mips(void) {
    /* 256x256 RGBA8, block height 2^4: 1024-byte rows (16 GOBs) x 2 block rows of 128 rows; 128x128 keeps 2^4 (128 rows > 64);
     * 64x64 drops to 2^3, 32x32 to 2^2, ..., 1x1 to 2^0 */
    s2tex_texture t = {256, 256, 1, 1, 1, 4, 4, 0, 9, S2TEX_SWITCH2};
    s2tex_surface s;
    static const uint64_t off[9] = {0, 262144, 327680, 344064, 348160, 349184, 349696, 350208, 350720};
    static const uint32_t bh[9] = {4, 4, 3, 2, 1, 0, 0, 0, 0};
    for (unsigned l = 0; l < 9; l++) {
        uint64_t o = s2tex_level(&t, l, &s);
        CHECK(o == off[l], "level %u offset %llu, want %llu", l, (unsigned long long)o, (unsigned long long)off[l]);
        CHECK(s.block_height == bh[l], "level %u block height %u, want %u", l, s.block_height, bh[l]);
    }
    CHECK(s2tex_layer_size(&t) == 351232, "layer size %llu", (unsigned long long)s2tex_layer_size(&t));
    CHECK(s2tex_level(&t, 9, &s) == UINT64_MAX, "level past the chain");
    /* BC1 100x60: 25x15 blocks of 8 bytes, 200-byte rows = 4 GOBs, block height 2^1 kept (15 rows > 8) */
    s2tex_texture c = {100, 60, 1, 4, 4, 8, 1, 0, 2, S2TEX_TX1};
    CHECK(s2tex_level(&c, 0, &s) == 0 && s.width == 25 && s.height == 15 && s.block_height == 1, "BC1 level 0");
    CHECK(s2tex_level(&c, 1, &s) == 4 * 1024, "BC1 level 1 offset");
    CHECK(s.width == 13 && s.height == 8 && s.block_height == 0, "BC1 level 1 (8 rows: half a 2-GOB block)");
    /* 3D 32x32x32 RGBA8, block depth 2^4: 2 GOB columns x 4 GOB rows x 2 block layers of 16 slices */
    s2tex_texture v = {32, 32, 32, 1, 1, 4, 0, 4, 3, S2TEX_TX1};
    CHECK(s2tex_level(&v, 0, &s) == 0 && s2tex_size(&s) == 131072, "3D level 0 size");
    CHECK(s2tex_level(&v, 1, &s) == 131072 && s.block_depth == 4, "3D level 1: 16 slices keep 2^4");
    CHECK(s2tex_level(&v, 2, &s) == 131072 + 16384 && s.block_depth == 3, "3D level 2: 8 slices take 2^3");
    /* An array's layers are aligned to level 0's block: 20x20 RGBA8 at 2^1 is 4096 + 1024 + 512 bytes, 1 KB blocks */
    s2tex_texture a = {20, 20, 1, 1, 1, 4, 1, 0, 3, S2TEX_TX1};
    CHECK(s2tex_layer_size(&a) == 5632, "array layer size %llu", (unsigned long long)s2tex_layer_size(&a));
    CHECK(s2tex_layer_stride(&a) == 6144, "array layer stride %llu", (unsigned long long)s2tex_layer_stride(&a));
}

int main(void) {
    test_gob();
    test_offsets();
    test_params();
    test_random(1500);
    test_relation();
    test_mips();
    printf("s2tex synthetic tests: %s (%d failures)\n", fails ? "FAILED" : "passed", fails);
    return fails ? 1 : 0;
}
