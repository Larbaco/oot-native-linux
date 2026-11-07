/* The texture path (docs/m5-vulkan.md §4, §6): storage -> s2tex (Switch 2 order) -> staging -> VkImage, read back from the GPU and compared with the CPU
   decode (gfx/nvn_texio.c) for shapes and formats (mip chains, arrays, cubes, 3D, non-power-of-two, 3-component expansion); the icon rule (a BC7 texture
   flagged IMAGE in a CPU-writable pool is read from storage and uploaded again when the CPU rewrites it, also through a new texture object over the same
   storage); sharing by storage, shape, format and usage; every NVN format created and uploaded on this device; fallbacks on every view; and, when
   _local/s2tex/corpus is present, the 591 real game textures read back and compared with the CPU decode and with the Mac host's decode. */
#include "vk_test.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "../../../s2tex/tests/corpus.h"
#pragma clang diagnostic pop

NXRT_EARLY_RESERVE()

static uint32_t mipu(uint32_t s, unsigned l) { s >>= l; return s ? s : 1; }

// Reads level / layer of im back and compares it with the CPU decode of t's storage (expanded like the upload for 3-component formats); returns bytes that differ
static size_t compare_level(vkt_image *im, const tex_t *t, unsigned level, unsigned layer) {
    const vkf_info *fi = im->fi; int is3d = t->d.target == NVN_TEXTURE_TARGET_3D, is1d = t->d.target == NVN_TEXTURE_TARGET_1D || t->d.target == NVN_TEXTURE_TARGET_1D_ARRAY;
    uint32_t lw = mipu(im->w, level), lh = is1d ? 1 : mipu(im->h, level), ld = is3d ? mipu(im->depth, level) : 1;
    uint32_t bx = (lw + fi->bw - 1) / fi->bw, by = (lh + fi->bh - 1) / fi->bh;
    size_t cpu_size = (size_t)bx * by * ld * fi->bpb, gpu_size = (size_t)bx * by * ld * fi->vbpb;
    uint8_t *cpu = calloc(1, cpu_size), *gpu = calloc(1, gpu_size), *exp = cpu;
    NVNcopyRegion r = {0, 0, is3d ? 0 : 0, (int)lw, (int)lh, (int)ld};
    if (nvn_texio_read(t, level, layer, &r, cpu, 0, 0)) { free(cpu); free(gpu); return (size_t)-1; }
    if (vkt_readback(im, level, layer, gpu, gpu_size)) { free(cpu); free(gpu); return (size_t)-2; }
    if (fi->conv == VKF_RGB_TO_RGBA) {   // what the upload makes of it: each texel, then the component type's "one" as alpha
        exp = calloc(1, gpu_size); size_t n = (size_t)bx * by * ld; uint32_t c = fi->bpb / 3; uint8_t one[4] = {0};
        if (c == 1) one[0] = (fi->flags & VKF_INT) ? 1 : fi->vk == VK_FORMAT_R8G8B8A8_SNORM ? 0x7f : 0xff;
        else if (c == 2) { uint16_t o = (fi->flags & VKF_INT) ? 1 : fi->vk == VK_FORMAT_R16G16B16A16_SFLOAT ? 0x3c00 : fi->vk == VK_FORMAT_R16G16B16A16_SNORM ? 0x7fff : 0xffff; memcpy(one, &o, 2); }
        else { uint32_t o = (fi->flags & VKF_INT) ? 1 : 0x3f800000u; memcpy(one, &o, 4); }
        for (size_t i = 0; i < n; i++) { memcpy(exp + i * fi->vbpb, cpu + i * fi->bpb, fi->bpb); memcpy(exp + i * fi->vbpb + fi->bpb, one, c); }
    }
    size_t bad = 0; for (size_t i = 0; i < gpu_size; i++) bad += exp[i] != gpu[i];
    if (exp != cpu) free(exp);
    free(cpu); free(gpu);
    return bad;
}

static int roundtrip(const char *what, int target, int format, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, int flags, int cpu) {
    tex_t probe = vt_tex(target, format, w, h, d, levels, flags, 0, NULL);
    uint8_t *mem = _aligned_malloc((size_t)probe.L.size + 4096, 4096);
    for (uint64_t i = 0; i < probe.L.size; i++) mem[i] = (uint8_t)vt_rand();
    tex_t t = vt_tex(target, format, w, h, d, levels, flags, 0, mem);
    vkt_tex_init(&t); if (cpu) vkt_tex_cpu_written(&t);
    vkt_image *im = vkt_resolve(&t);
    int ok = im && im->content && im->cls == VKT_STORAGE;
    size_t bad = 0, n = 0;
    if (ok) for (uint32_t l = 0; l < im->levels; l++) for (uint32_t z = 0; z < im->layers; z++) { size_t b = compare_level(im, &t, l, z); bad += b; n++; if (b) printf("  %s level %u layer %u: %lld bytes differ\n", what, l, z, (long long)b); }
    printf("%-34s %ux%ux%u levels %u: %s, %zu subresources, %zu bytes differ\n", what, w, h, d, levels, ok ? (im->fi->conv ? "converted" : "exact") : "NOT CREATED", n, bad);
    CHECK(ok && !bad, "%s round trip", what);
    vkt_tex_release(&t); vkt_drop_pool(t.pool);
    _aligned_free(mem);
    return ok && !bad;
}

int main(void) {
    crash_install();
    int rc = vt_up(0, 1);
    if (rc) return rc;
    if (vka_init(NULL)) return 1;
    // ---- Shapes and formats through s2tex, read back from the GPU ----
    roundtrip("RGBA8 2D mip chain", NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 64, 48, 1, 4, 0, 1);
    roundtrip("RGBA8 2D array", NVN_TEXTURE_TARGET_2D_ARRAY, NVN_FORMAT_RGBA8, 32, 32, 3, 2, 0, 1);
    roundtrip("R32F non-power-of-two", NVN_TEXTURE_TARGET_2D, NVN_FORMAT_R32F, 100, 37, 1, 3, 0, 1);
    roundtrip("RGBA16F 3D", NVN_TEXTURE_TARGET_3D, NVN_FORMAT_RGBA16F, 16, 16, 8, 2, 0, 1);
    roundtrip("RGBA8 cube", NVN_TEXTURE_TARGET_CUBEMAP, NVN_FORMAT_RGBA8, 32, 32, 1, 3, 0, 1);
    roundtrip("BGRA8_SRGB", NVN_TEXTURE_TARGET_2D, NVN_FORMAT_BGRA8_SRGB, 80, 60, 1, 1, 0, 1);
    roundtrip("RGB8 (expanded to RGBA8)", NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGB8, 33, 17, 1, 1, 0, 1);
    roundtrip("RGB32F (expanded)", NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGB32F, 20, 10, 1, 1, 0, 1);
    roundtrip("BC1 mip chain (CPU pool: from storage)", NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA_DXT1, 128, 64, 1, 3, 0, 0);
    roundtrip("BC5 2D array", NVN_TEXTURE_TARGET_2D_ARRAY, NVN_FORMAT_RGTC2_UNORM, 64, 64, 2, 2, 0, 0);
    roundtrip("BC7 flagged IMAGE (the icon rule)", NVN_TEXTURE_TARGET_2D, NVN_FORMAT_BPTC_UNORM, 256, 256, 1, 1, NVN_TEXTURE_FLAGS_IMAGE, 0);

    // ---- The icon rule over time, and sharing ----
    {
        tex_t probe = vt_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_BPTC_UNORM_SRGB, 128, 128, 1, 1, NVN_TEXTURE_FLAGS_IMAGE, 0, NULL);
        size_t sz = (size_t)probe.L.size;
        uint8_t *mem = VirtualAlloc(NULL, sz + 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        for (size_t i = 0; i < sz; i++) mem[i] = (uint8_t)vt_rand();
        nvn_dirty_add_range(mem, sz);   // the recording layer registers the pool's range
        tex_t t = vt_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_BPTC_UNORM_SRGB, 128, 128, 1, 1, NVN_TEXTURE_FLAGS_IMAGE, 0, mem);
        vkt_tex_init(&t);
        vkt_image *im = vkt_resolve(&t); vkt_stats_t a, b; vkt_stats(&a);
        CHECK(im && im->cls == VKT_STORAGE, "a BC7 texture flagged IMAGE in a CPU-writable pool is read from storage");
        CHECK(compare_level(im, &t, 0, 0) == 0, "its first upload");
        vkt_resolve(&t); vkt_stats(&b);
        CHECK(b.reuploads == a.reuploads, "unchanged storage is not uploaded again");
        for (size_t i = 0; i < 4096; i++) mem[1000 + i] = (uint8_t)vt_rand();   // the game draws a new icon into the storage (write faults stamp the pages)
        vkt_resolve(&t); vkt_stats(&a);
        CHECK(a.reuploads == b.reuploads + 1, "rewritten storage is uploaded again (%llu)", (unsigned long long)(a.reuploads - b.reuploads));
        CHECK(compare_level(im, &t, 0, 0) == 0, "and the GPU holds the new icon");
        tex_t t2 = t; t2.serial = ++vt_serial;   // a new texture object over the same storage with the same description
        for (size_t i = 0; i < 2048; i++) mem[i] = (uint8_t)vt_rand();
        vkt_image *im2 = vkt_resolve(&t2); vkt_stats(&b);
        CHECK(im2 == im && b.shared_hits > a.shared_hits, "the same storage and description share one image");
        CHECK(compare_level(im2, &t2, 0, 0) == 0, "a new object over rewritten storage shows the new content (not the previous item's)");
        // GPU-owned textures
        tex_t gt = t; gt.serial = ++vt_serial; gt.d.format = NVN_FORMAT_RGBA8; gt.d.flags = NVN_TEXTURE_FLAGS_IMAGE | NVN_TEXTURE_FLAGS_COMPRESSIBLE; nvn_tex_layout(&gt.d, &gt.L);
        vkt_tex_init(&gt);
        vkt_image *gi = vkt_resolve(&gt);
        CHECK(gi && gi->cls == VKT_GPU && (gi->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT), "an uncompressed texture is a GPU one (render target)");
        CHECK(gi && (gi->usage & VK_IMAGE_USAGE_STORAGE_BIT), "IMAGE on an uncompressed format adds storage use");
        CHECK(vkt_tex_gpu_owned(gt.serial), "and is reported GPU-owned");
        CHECK(gi != im, "a different format over the same storage is another image");
        vkt_tex_release(&t); vkt_tex_release(&t2); vkt_tex_release(&gt);
        vkt_stats(&a); vkt_drop_pool(t.pool); vkt_stats(&b);
        CHECK(b.evicted >= a.evicted + 2, "finalizing the pool evicts its images (%llu)", (unsigned long long)(b.evicted - a.evicted));
        nvn_dirty_remove_range(mem);
        vks_finish(); VirtualFree(mem, 0, MEM_RELEASE);
    }

    // ---- Every NVN format on this device: created, uploaded, viewed through its fallback ----
    {
        int made = 0, total = 0, conv = 0; char failed[2048] = "";
        for (int f = 1; f <= 0x9C; f++) {
            const vkf_info *fi = vkf_get(f);
            if (!fi) continue;
            total++;
            uint32_t w = fi->bw > 1 ? fi->bw * 4 : 16, h = fi->bh > 1 ? fi->bh * 4 : 16;
            tex_t probe = vt_tex(NVN_TEXTURE_TARGET_2D, f, w, h, 1, 1, 0, 0, NULL);
            uint8_t *mem = _aligned_malloc((size_t)probe.L.size + 4096, 4096);
            for (uint64_t i = 0; i < probe.L.size; i++) mem[i] = (uint8_t)vt_rand();
            tex_t t = vt_tex(NVN_TEXTURE_TARGET_2D, f, w, h, 1, 1, 0, 0, mem);
            vkt_tex_init(&t); vkt_tex_cpu_written(&t);
            vkt_image *im = vkt_resolve(&t);
            view_t v; memset(&v, 0, sizeof v); v.set_format = 1; v.format = (uint16_t)f;
            VkImageView vw = im ? vkt_view(im, &v, 0, 0) : VK_NULL_HANDLE;
            if (im && im->content && vw) { made++; conv += fi->conv != VKF_COPY; } else snprintf(failed + strlen(failed), sizeof failed - strlen(failed), " %#x(%s)", f, fi->name);
            vkt_tex_release(&t); vkt_drop_pool(t.pool); vks_finish();
            _aligned_free(mem);
        }
        printf("formats: %d of %d NVN formats with a Vulkan mapping created, uploaded and viewed (%d through a conversion)%s%s\n", made, total, conv, *failed ? "; failed:" : "", failed);
        CHECK(made == total, "every mapped format works on this device");
        VkFormat f1 = vkf_view_format(NVN_FORMAT_RGBA_ASTC_4x4, NVN_FORMAT_RGBA_ASTC_4x4_SRGB), f2 = vkf_view_format(NVN_FORMAT_RGBA8, NVN_FORMAT_RGBA8_SRGB),
                 f3 = vkf_view_format(NVN_FORMAT_DEPTH24_STENCIL8, NVN_FORMAT_DEPTH24_STENCIL8), f4 = vkf_view_format(NVN_FORMAT_RGB8, NVN_FORMAT_RGBA8);
        CHECK(f1 == vkf_get(NVN_FORMAT_RGBA_ASTC_4x4_SRGB)->vk, "an sRGB view of an ASTC image takes the sRGB twin of its fallback (%d)", f1);
        CHECK(f2 == VK_FORMAT_R8G8B8A8_SRGB, "an sRGB view of RGBA8");
        CHECK(f3 == vkf_get(NVN_FORMAT_DEPTH24_STENCIL8)->vk, "a D24S8 view gets the image's own (fallback) format (%d)", f3);
        CHECK(f4 == vkf_get(NVN_FORMAT_RGB8)->vk, "a view cannot reinterpret an expanded image (%d)", f4);
        printf("fallbacks on this device: DEPTH24_STENCIL8 -> %d, DEPTH24 -> %d, STENCIL8 -> %d, ASTC 4x4 -> %d, RGBA4 -> %d\n", vkf_get(NVN_FORMAT_DEPTH24_STENCIL8)->vk,
               vkf_get(NVN_FORMAT_DEPTH24)->vk, vkf_get(NVN_FORMAT_STENCIL8)->vk, vkf_get(NVN_FORMAT_RGBA_ASTC_4x4)->vk, vkf_get(NVN_FORMAT_RGBA4)->vk);
    }

    // ---- The real corpus (game data, _local only) ----
    const char *dir = getenv("S2TEX_CORPUS");
    corpus_entry *e = NULL; int n = dir ? corpus_load(dir, &e) : -1;
    if (n < 0) printf("corpus: none at %s, skipped\n", dir ? dir : "(S2TEX_CORPUS unset)");
    else {
        int pass_cpu = 0, pass_mac = 0, tried = 0, skipped = 0; uint64_t bytes = 0, t0 = now_ns(), up_ns = 0;
        for (int i = 0; i < n; i++) {
            corpus_pair p; const char *why = corpus_open(dir, &e[i], &p);
            if (why) { skipped++; corpus_close(&p); continue; }
            tried++;
            uint8_t *mem = _aligned_malloc(p.bl_size + 4096, 4096); memcpy(mem, p.bl, p.bl_size);
            pool_t pool = vt_pool;   // a pool object per texture, finalized after it
            int pkg = (int)((e[i].bd << 4 | e[i].bh) + 1);
            tex_t t = vt_tex((int)e[i].target, (int)e[i].format, e[i].width, e[i].height, e[i].target == 2 ? e[i].depth : 1, e[i].levels, (int)e[i].flags, pkg, mem);
            t.pool = &pool;
            vkt_tex_init(&t);
            uint64_t u0 = now_ns(); vkt_image *im = vkt_resolve(&t); up_ns += now_ns() - u0;
            size_t bad_cpu = im ? compare_level(im, &t, 0, 0) : (size_t)-3;
            // the Mac host's level 0 (Metal holds these formats as they are stored): row by row at its pitch
            size_t bad_mac = (size_t)-1;
            if (im && !im->fi->conv) {
                const vkf_info *fi = im->fi; uint32_t bx = (im->w + fi->bw - 1) / fi->bw, by = (im->h + fi->bh - 1) / fi->bh, bd = im->vtype == VK_IMAGE_VIEW_TYPE_3D ? im->depth : 1;
                size_t rb = (size_t)bx * fi->vbpb, sz = rb * by * bd; uint8_t *g = malloc(sz);
                if (!vkt_readback(im, 0, 0, g, sz)) { bad_mac = 0; for (uint32_t z = 0; z < bd; z++) for (uint32_t y = 0; y < by; y++) bad_mac += memcmp(g + (z * by + y) * rb, p.raw + z * p.slice_pitch + y * p.row_pitch, rb) != 0; }
                free(g);
            }
            if (!bad_cpu) pass_cpu++;
            if (!bad_mac) pass_mac++;
            if (bad_cpu || bad_mac) printf("  corpus %s: %ux%u format %#x: GPU vs CPU decode %lld, GPU vs Mac host %lld\n", e[i].id, e[i].width, e[i].height, e[i].format, (long long)bad_cpu, (long long)bad_mac);
            bytes += p.bl_size;
            vkt_tex_release(&t); vkt_drop_pool(&pool); vks_finish();
            _aligned_free(mem); corpus_close(&p);
        }
        printf("corpus: %d textures (%d skipped): %d read back equal to the CPU decode, %d equal to the Mac host's decode; %.1f MB of storage, upload %.1f ms (%.0f MB/s), %.1f s in all\n",
               tried, skipped, pass_cpu, pass_mac, bytes / 1048576.0, up_ns / 1e6, bytes / 1048576.0 / (up_ns / 1e9), (now_ns() - t0) / 1e9);
        CHECK(tried > 0 && pass_cpu == tried, "corpus: every texture through the GPU equals the CPU decode");
        CHECK(pass_mac == tried, "corpus: every texture equals the Mac host's level 0");
        free(e);
    }
    vka_shutdown();
    uint32_t verr = atomic_load(&g_vk.val_errors);
    vt_down(0);
    CHECK(verr == 0, "no validation errors (%u)", verr);
    printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "ok", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
