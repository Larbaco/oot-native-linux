// s2tex.metal: block-linear <-> linear on the GPU, the same layout as s2tex.c (see s2tex.h, docs/s2tex.md). MIT licence, part of pokopia-native.
// One thread per 16-byte segment of a row: grid (ceil(row_bytes / 16), rows, slices). Offsets are 32-bit: a surface stays below 4 GB.
#include <metal_stdlib>
using namespace metal;

struct s2tex_params {
    uint row_bytes;     // width in blocks x bytes per block
    uint rows;          // height in block rows
    uint slices;        // depth (1 for 2D)
    uint block_height;  // log2 GOBs per block, vertically
    uint block_depth;   // log2 GOBs per block, in depth
    uint gobs_x;        // GOB columns as laid out (>= ceil(row_bytes / 64))
    uint order;         // 0 Tegra X1, 1 Switch 2
    uint row_pitch;     // linear side
    uint slice_pitch;
    uint bl_offset;     // where the surface starts in the block-linear buffer
    uint lin_offset;    // and in the linear one
    uint pad;
};

// Tegra X1: offset bits 8..0 = x5 y2 y1 x4 y0 x3 x2 x1 x0; Switch 2 exchanges bits 5 and 6
static uint s2tex_gob_offset(uint x, uint y, uint order) {
    x &= 63u; y &= 7u;
    uint o = (x & 15u) | (y & 1u) << 4 | (x >> 4 & 1u) << 5 | (y >> 1 & 3u) << 6 | (x >> 5 & 1u) << 8;
    if (order == 1u) o = (o & ~0x60u) | (o & 0x20u) << 1 | (o & 0x40u) >> 1;
    return o;
}

static uint s2tex_address(constant s2tex_params &p, uint x, uint y, uint z) {
    uint block = 512u << (p.block_height + p.block_depth);
    uint row_of_blocks = p.gobs_x * block;
    uint block_rows = (p.rows + (8u << p.block_height) - 1u) >> (3u + p.block_height);
    return (z >> p.block_depth) * block_rows * row_of_blocks
         + (y >> (3u + p.block_height)) * row_of_blocks
         + ((z & ((1u << p.block_depth) - 1u)) << (9u + p.block_height))
         + (((y >> 3) & ((1u << p.block_height) - 1u)) << 9)
         + (x >> 6) * block
         + s2tex_gob_offset(x, y, p.order);
}

kernel void s2tex_deswizzle(device const uchar *bl [[buffer(0)]], device uchar *lin [[buffer(1)]], constant s2tex_params &p [[buffer(2)]],
                            uint3 id [[thread_position_in_grid]]) {
    uint x = id.x << 4;
    if (x >= p.row_bytes || id.y >= p.rows || id.z >= p.slices) return;
    uint src = p.bl_offset + s2tex_address(p, x, id.y, id.z);
    uint dst = p.lin_offset + id.z * p.slice_pitch + id.y * p.row_pitch + x;
    uint n = min(16u, p.row_bytes - x);
    if (n == 16u && ((src | dst) & 15u) == 0u) *(device uint4 *)(lin + dst) = *(device const uint4 *)(bl + src);
    else for (uint i = 0; i < n; i++) lin[dst + i] = bl[src + i];
}

kernel void s2tex_swizzle(device const uchar *lin [[buffer(0)]], device uchar *bl [[buffer(1)]], constant s2tex_params &p [[buffer(2)]],
                          uint3 id [[thread_position_in_grid]]) {
    uint x = id.x << 4;
    if (x >= p.row_bytes || id.y >= p.rows || id.z >= p.slices) return;
    uint dst = p.bl_offset + s2tex_address(p, x, id.y, id.z);
    uint src = p.lin_offset + id.z * p.slice_pitch + id.y * p.row_pitch + x;
    uint n = min(16u, p.row_bytes - x);
    if (n == 16u && ((src | dst) & 15u) == 0u) *(device uint4 *)(bl + dst) = *(device const uint4 *)(lin + src);
    else for (uint i = 0; i < n; i++) bl[dst + i] = lin[src + i];
}
