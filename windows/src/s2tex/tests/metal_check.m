// metal_check: the Metal kernels of s2tex.metal against the C version (Mac only). Random surfaces in both orders and both directions must
// match s2tex.c byte for byte, padding included; with a corpus folder, every real pair must also come out of the GPU bit for bit (Switch 2 order).
// Usage: s2tex_metal_check <s2tex.metal> [corpus folder]. MIT licence, part of pokopia-native.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "s2tex.h"
#include "corpus.h"

typedef struct {   // s2tex_params in s2tex.metal
    uint32_t row_bytes, rows, slices, block_height, block_depth, gobs_x, order, row_pitch, slice_pitch, bl_offset, lin_offset, pad;
} params;
_Static_assert(sizeof(params) == 48, "params");

static id<MTLDevice> dev;
static id<MTLCommandQueue> queue;
static id<MTLComputePipelineState> ps_de, ps_sw;
static uint64_t rng = 0xD1B54A32D192ED03ull;
static uint32_t rnd(uint32_t n) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return n ? (uint32_t)(rng % n) : 0; }

static params params_of(const s2tex_surface *s, size_t pitch, size_t slice) {
    params p = {s->width * s->bytes_per_block, s->height, s->depth ? s->depth : 1, s->block_height, s->block_depth,
                s->gobs_x ? s->gobs_x : (s->width * s->bytes_per_block + 63) / 64, (uint32_t)s->order, (uint32_t)pitch,
                (uint32_t)(slice ? slice : s->height * pitch), 0, 0, 0};
    return p;
}

// Runs one kernel over buffers holding src and dst; dst is read back into out
static void gpu(id<MTLComputePipelineState> ps, const params *p, const void *src, size_t src_n, void *dst, size_t dst_n) {
    id<MTLBuffer> a = [dev newBufferWithBytes:src length:src_n options:MTLResourceStorageModeShared];
    id<MTLBuffer> b = [dev newBufferWithBytes:dst length:dst_n options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:ps];
    [e setBuffer:a offset:0 atIndex:0];
    [e setBuffer:b offset:0 atIndex:1];
    [e setBytes:p length:sizeof *p atIndex:2];
    MTLSize grid = MTLSizeMake((p->row_bytes + 15) / 16, p->rows, p->slices), tg = MTLSizeMake(16, 4, 1);
    [e dispatchThreads:grid threadsPerThreadgroup:tg];
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    memcpy(dst, b.contents, dst_n);
}

static int synthetic(int n) {
    int bad = 0;
    for (int i = 0; i < n; i++) {
        s2tex_surface s;
        int three = rnd(4) == 0;
        s.bytes_per_block = 1 + rnd(16);
        s.width = 1 + rnd(three ? 60 : 700);
        s.height = 1 + rnd(three ? 60 : 400);
        s.depth = three ? 1 + rnd(24) : 1;
        s.block_height = rnd(6);
        s.block_depth = three ? rnd(6) : 0;
        uint32_t need = (s.width * s.bytes_per_block + 63) / 64;
        s.gobs_x = rnd(3) == 0 ? need + rnd(3) : 0;
        s.order = (s2tex_order)rnd(2);
        size_t row = (size_t)s.width * s.bytes_per_block, pitch = row + (rnd(2) ? 16 * rnd(4) : rnd(20)), slice = pitch * s.height;
        size_t size = (size_t)s2tex_size(&s), lin_n = slice * s.depth;
        unsigned char *bl = malloc(size), *c_lin = malloc(lin_n), *g_lin = malloc(lin_n), *lin = malloc(lin_n), *c_bl = malloc(size), *g_bl = malloc(size);
        for (size_t k = 0; k < size; k++) bl[k] = (unsigned char)rnd(256);
        for (size_t k = 0; k < lin_n; k++) lin[k] = (unsigned char)rnd(256);
        memset(c_lin, 0x3C, lin_n); memset(g_lin, 0x3C, lin_n);
        params p = params_of(&s, pitch, slice);
        s2tex_deswizzle(&s, bl, size, c_lin, pitch, slice);
        gpu(ps_de, &p, bl, size, g_lin, lin_n);
        memcpy(c_bl, bl, size); memcpy(g_bl, bl, size);
        s2tex_swizzle(&s, lin, pitch, slice, c_bl, size);
        gpu(ps_sw, &p, lin, lin_n, g_bl, size);
        if (memcmp(c_lin, g_lin, lin_n) || memcmp(c_bl, g_bl, size)) {
            if (bad++ < 10) printf("FAIL %ux%ux%u, %u B, blocks 2^%u/2^%u, gobs %u, order %d, pitch %zu\n", s.width, s.height, s.depth, s.bytes_per_block,
                                   s.block_height, s.block_depth, s.gobs_x, s.order, pitch);
        }
        free(bl); free(c_lin); free(g_lin); free(lin); free(c_bl); free(g_bl);
    }
    printf("metal: %d random surfaces, both orders and directions: %s (%d differ from s2tex.c)\n", n, bad ? "FAILED" : "identical", bad);
    return bad;
}

static int corpus(const char *dir) {
    corpus_entry *e = NULL;
    int n = corpus_load(dir, &e);
    if (n < 0) { printf("metal: no corpus in %s, skipped\n", dir); return 0; }
    int pass = 0, fail = 0, skip = 0;
    for (int i = 0; i < n; i++) {
        corpus_pair p;
        if (corpus_open(dir, &e[i], &p)) { skip++; corpus_close(&p); continue; }
        size_t lin_n = p.slice_pitch * p.surf.depth;
        unsigned char *lin = calloc(lin_n, 1);
        params q = params_of(&p.surf, p.row_pitch, p.slice_pitch);
        gpu(ps_de, &q, p.bl, p.bl_size, lin, lin_n);
        unsigned long long bad = corpus_diff(&p, lin);
        if (bad) { fail++; printf("FAIL %s: %llu bytes differ\n", e[i].id, bad); } else pass++;
        free(lin);
        corpus_close(&p);
    }
    free(e);
    printf("metal: corpus %d pairs reproduced bit for bit (Switch 2 order), %d differ, %d skipped\n", pass, fail, skip);
    return fail;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 2) { fprintf(stderr, "usage: s2tex_metal_check <s2tex.metal> [corpus folder]\n"); return 2; }
        dev = MTLCreateSystemDefaultDevice();
        if (!dev) { printf("metal: no device, skipped\n"); return 77; }
        NSError *err = nil;
        NSString *src = [NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:&err];
        if (!src) { printf("metal: cannot read %s\n", argv[1]); return 1; }
        id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
        if (!lib) { printf("metal: compile failed: %s\n", err.localizedDescription.UTF8String); return 1; }
        ps_de = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"s2tex_deswizzle"] error:&err];
        ps_sw = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"s2tex_swizzle"] error:&err];
        if (!ps_de || !ps_sw) { printf("metal: pipeline failed\n"); return 1; }
        queue = [dev newCommandQueue];
        int bad = synthetic(400);
        if (argc > 2) bad += corpus(argv[2]);
        return bad ? 1 : 0;
    }
}
