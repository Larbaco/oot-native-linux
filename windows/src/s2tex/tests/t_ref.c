/* s2tex in Tegra X1 order against upstream nxnative's unpatched implementation (commit c1df4dd, dm/gfx/tex/nvn_deswizzle.c and
 * dm/gfx/nvn/nvn_tex.c, copied unchanged into tests/ref): level offsets, per-level block sizes, layer strides, single addresses, and both copy
 * directions byte for byte on random 2D, 2D array and 3D textures. MIT licence, part of pokopia-native. */
#include "s2tex.h"
#include "tex/nvn_deswizzle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; if (fails < 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint64_t rng = 0x2545F4914F6CDD1Dull;
static uint32_t rnd(uint32_t n) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return n ? (uint32_t)(rng % n) : 0; }

/* Formats covering every block size: R8, RG8, RGB8, RGBA8, RGB16F, RGBA16F, RGB32F, RGBA32F, BC1, BC7, BC4, ASTC 5x4, ASTC 10x8, ASTC 12x12 */
static const int formats[] = {0x01, 0x0D, 0x19, 0x25, 0x1D, 0x29, 0x22, 0x2E, 0x42, 0x4D, 0x49, 0x7A, 0x83, 0x86};

static unsigned max_levels(uint32_t w, uint32_t h, uint32_t d) {
    uint32_t m = w > h ? w : h; if (d > m) m = d;
    unsigned n = 1; while (m > 1) { m >>= 1; n++; } return n;
}

static int cases, levels_checked;

static void one(int target) {
    nvn_texdesc d;
    memset(&d, 0, sizeof d);
    d.target = target;
    d.format = formats[rnd(sizeof formats / sizeof *formats)];
    int three = target == 2;
    d.width = 1 + rnd(rnd(4) ? 300 : 1200);
    d.height = 1 + rnd(three ? 70 : rnd(4) ? 300 : 900);
    d.depth = three ? 1 + rnd(40) : target == 4 ? 1 + rnd(5) : 1;
    d.levels = 1 + rnd(max_levels(d.width, d.height, three ? d.depth : 1));
    if (rnd(3) == 0) d.pkg_block_h = (int)((rnd(6) | (three ? rnd(6) << 4 : 0)) + 1);   /* a packaged layout: any block height / depth */
    nvn_texlayout L;
    nvn_tex_layout(&d, &L);
    int bw, bh;
    int bpb = nvn_fmt_bpb(d.format, &bw, &bh);
    s2tex_texture t = {d.width, d.height, three ? d.depth : 1, (uint32_t)bw, (uint32_t)bh, (uint32_t)bpb, L.tileH, L.tileD, d.levels, S2TEX_TX1};
    cases++;
    if (target == 4) CHECK(s2tex_layer_stride(&t) == L.layer_size, "array %ux%u fmt %#x levels %u bh %u: layer stride %llu, upstream %llu", d.width, d.height,
                           d.format, d.levels, L.tileH, (unsigned long long)s2tex_layer_stride(&t), (unsigned long long)L.layer_size);
    else CHECK(s2tex_layer_size(&t) == L.layer_size, "%ux%ux%u fmt %#x levels %u bh %u bd %u: layer size %llu, upstream %llu", d.width, d.height, d.depth,
               d.format, d.levels, L.tileH, L.tileD, (unsigned long long)s2tex_layer_size(&t), (unsigned long long)L.layer_size);
    size_t total = (size_t)L.size;
    unsigned char *storage = malloc(total), *a = malloc(total), *b = malloc(total);
    for (size_t k = 0; k < total; k++) storage[k] = (unsigned char)rnd(256);
    unsigned layers = L.layers;
    for (unsigned level = 0; level < d.levels; level++) {
        s2tex_surface s;
        uint64_t off = s2tex_level(&t, level, &s);
        unsigned th, td;
        nvn_tex_level_tiles(&d, level, &th, &td);
        CHECK(off == nvn_tex_level_offset(&d, level), "level %u offset %llu, upstream %llu", level, (unsigned long long)off,
              (unsigned long long)nvn_tex_level_offset(&d, level));
        CHECK(s.block_height == th && s.block_depth == td, "level %u blocks 2^%u/2^%u, upstream 2^%u/2^%u", level, s.block_height, s.block_depth, th, td);
        CHECK((s.width * s.bytes_per_block + 63) / 64 == nvn_tex_level_gobs(&d, level), "level %u GOB columns", level);
        for (int k = 0; k < 8; k++) {
            uint32_t x = rnd(s.width * s.bytes_per_block), y = rnd(s.height), z = rnd(s.depth);
            CHECK(s2tex_offset(&s, x, y, z) == nvn_bl_offset(x, y, z, s.width * s.bytes_per_block, s.height, th, td), "level %u point (%u,%u,%u)", level, x, y, z);
        }
        for (unsigned layer = 0; layer < layers; layer += layers > 2 ? layers - 1 : 1) {
            size_t pitch = (size_t)s.width * s.bytes_per_block + rnd(3) * 16 + rnd(2);
            size_t lin_size = pitch * s.height * s.depth;
            unsigned char *u = malloc(lin_size), *v = malloc(lin_size);
            memset(u, 0x5A, lin_size); memset(v, 0x5A, lin_size);
            uint64_t base = (uint64_t)layer * L.layer_size + off;
            CHECK(nvn_deswizzle(&d, level, layer, storage, u, pitch) == 0, "upstream deswizzle refused");
            CHECK(s2tex_deswizzle(&s, storage + base, total - base, v, pitch, 0) == 0, "s2tex deswizzle refused level %u layer %u", level, layer);
            CHECK(!memcmp(u, v, lin_size), "%ux%ux%u fmt %#x level %u layer %u: deswizzle differs", d.width, d.height, d.depth, d.format, level, layer);
            for (size_t k = 0; k < lin_size; k++) u[k] = (unsigned char)rnd(256);
            memcpy(a, storage, total); memcpy(b, storage, total);
            CHECK(nvn_swizzle(&d, level, layer, u, pitch, a) == 0, "upstream swizzle refused");
            CHECK(s2tex_swizzle(&s, u, pitch, 0, b + base, total - base) == 0, "s2tex swizzle refused");
            CHECK(!memcmp(a, b, total), "%ux%ux%u fmt %#x level %u layer %u: swizzle differs", d.width, d.height, d.depth, d.format, level, layer);
            free(u); free(v);
            levels_checked++;
        }
    }
    free(storage); free(a); free(b);
}

int main(void) {
    for (int i = 0; i < 300; i++) one(1);
    for (int i = 0; i < 120; i++) one(2);
    for (int i = 0; i < 60; i++) one(4);
    printf("s2tex vs nxnative c1df4dd (Tegra X1 order): %d textures, %d level/layer copies in both directions: %s (%d failures)\n", cases, levels_checked,
           fails ? "FAILED" : "identical", fails);
    return fails ? 1 : 0;
}
