/* ASTC -> BC7 (docs/m5-vulkan.md §4.3, §6): images compressed to ASTC with astc-encoder, decoded by astc-encoder as the reference, transcoded to BC7 by
   vk_astc (fast pass, then the high-quality pass from the background worker), decoded with bc7enc's decoder and compared with the reference within a
   tolerance; the cache serves the high-quality result after a restart; the passes' speed on this PC. Needs no GPU. */
#include "vk_test.h"
#include "third_party/astcenc/astcenc.h"

NXRT_EARLY_RESERVE()

static void make_image(int kind, uint32_t w, uint32_t h, uint8_t *px) {
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
        uint8_t *p = px + ((size_t)y * w + x) * 4; double u = x / (double)w, v = y / (double)h;
        switch (kind) {
        case 0: p[0] = (uint8_t)(255 * u); p[1] = (uint8_t)(255 * v); p[2] = (uint8_t)(255 * (1 - u) * v); p[3] = 255; break;   // smooth gradient
        case 1: { double s = sin(u * 23) * cos(v * 17) * 0.5 + 0.5, t = sin((u + v) * 41) * 0.5 + 0.5;                       // "photo-like" bands
            p[0] = (uint8_t)(255 * s); p[1] = (uint8_t)(255 * (0.3 + 0.6 * t * s)); p[2] = (uint8_t)(255 * t); p[3] = (uint8_t)(255 * (0.5 + 0.5 * sin(u * 9))); break; }
        case 2: { uint32_t r = vt_rand(); p[0] = (uint8_t)r; p[1] = (uint8_t)(r >> 8); p[2] = (uint8_t)(r >> 16); p[3] = 255; break; }   // noise: the worst case
        default: { int c = ((x / 8) ^ (y / 8)) & 1; p[0] = p[1] = p[2] = c ? 230 : 20; p[3] = c ? 255 : 128; }               // sharp edges and alpha (UI)
        }
    }
}
static uint8_t *astc_compress(const uint8_t *px, uint32_t w, uint32_t h, int bw, int bh, int srgb, size_t *size) {
    struct astcenc_config cfg; struct astcenc_context *c = NULL;
    if (astcenc_config_init(srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR, (unsigned)bw, (unsigned)bh, 1, ASTCENC_PRE_MEDIUM, 0, &cfg) || astcenc_context_alloc(&cfg, 1, &c, NULL)) return NULL;
    *size = (size_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * 16;
    uint8_t *out = malloc(*size); void *plane = (void *)px;
    struct astcenc_image img = {w, h, 1, ASTCENC_TYPE_U8, &plane};
    struct astcenc_swizzle sw = {ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    enum astcenc_error e = astcenc_compress_image(c, &img, &sw, out, *size, 0);
    astcenc_context_free(c);
    if (e) { free(out); return NULL; }
    return out;
}
static void bc7_decode(const uint8_t *bc7, uint32_t w, uint32_t h, uint8_t *px) {
    uint32_t bx = (w + 3) / 4; uint8_t blk[64];
    for (uint32_t by = 0; by < (h + 3) / 4; by++) for (uint32_t x0 = 0; x0 < bx; x0++) {
        vka_decode_bc7_block(bc7 + ((size_t)by * bx + x0) * 16, blk);
        for (uint32_t y = 0; y < 4; y++) for (uint32_t x = 0; x < 4; x++) if (x0 * 4 + x < w && by * 4 + y < h) memcpy(px + (((size_t)by * 4 + y) * w + x0 * 4 + x) * 4, blk + (y * 4 + x) * 4, 4);
    }
}
static double psnr(const uint8_t *a, const uint8_t *b, size_t n, int *maxerr) {
    double se = 0; int mx = 0;
    for (size_t i = 0; i < n; i++) { int d = a[i] - b[i]; se += d * d; if (abs(d) > mx) mx = abs(d); }
    *maxerr = mx; return se ? 10 * log10(255.0 * 255.0 * n / se) : 99;
}

extern uint64_t g_t0_ns;
int main(void) {
    crash_install(); g_t0_ns = now_ns();
    char dir[MAX_PATH]; const char *e = getenv("NXRT_VK_CACHE_DIR");
    snprintf(dir, sizeof dir, "%s\\astc-test-%lu", e && *e ? e : ".", GetCurrentProcessId());
    CreateDirectoryA(e && *e ? e : ".", NULL); CreateDirectoryA(dir, NULL);
    vka_init(dir);
    static const struct { int bw, bh, srgb; } blocks[] = {{4, 4, 0}, {5, 5, 0}, {6, 6, 0}, {8, 8, 0}, {10, 10, 0}, {12, 12, 0}, {4, 4, 1}, {8, 8, 1}};
    static const struct { int kind; uint32_t w, h; double min_fast, min_high; const char *name; } imgs[] = {
        {0, 256, 256, 45, 45, "gradient"}, {1, 250, 130, 33, 33, "bands, 250x130"}, {3, 96, 64, 45, 45, "edges and alpha"}, {2, 64, 64, 15, 15, "noise"}};   // noise: incompressible, a sanity bound
    double worst_fast = 99, worst_high = 99;
    for (unsigned i = 0; i < sizeof imgs / sizeof *imgs; i++)
        for (unsigned k = 0; k < sizeof blocks / sizeof *blocks; k++) {
            uint32_t w = imgs[i].w, h = imgs[i].h; size_t n = (size_t)w * h * 4, as = 0;
            uint8_t *src = malloc(n), *ref = malloc(n), *dec = malloc(n), *bc = malloc(vka_bc7_size(w, h, 1));
            make_image(imgs[i].kind, w, h, src);
            uint8_t *astc = astc_compress(src, w, h, blocks[k].bw, blocks[k].bh, blocks[k].srgb, &as);
            CHECK(astc != NULL, "astc-encoder compressed %s %dx%d", imgs[i].name, blocks[k].bw, blocks[k].bh);
            if (!astc) continue;
            CHECK(!vka_decode_rgba8(astc, w, h, 1, blocks[k].bw, blocks[k].bh, blocks[k].srgb, ref), "astc-encoder decode");
            vka_key key; int q = vka_get_bc7(astc, w, h, 1, blocks[k].bw, blocks[k].bh, blocks[k].srgb, bc, &key);
            CHECK(q == VKA_FAST, "first use: the fast pass (%d)", q);
            bc7_decode(bc, w, h, dec); int mf; double pf = psnr(ref, dec, n, &mf);
            vka_wait_idle();
            q = vka_get_bc7(astc, w, h, 1, blocks[k].bw, blocks[k].bh, blocks[k].srgb, bc, &key);
            CHECK(q == VKA_HIGH, "after the worker: the high-quality pass from the cache (%d)", q);
            bc7_decode(bc, w, h, dec); int mh; double ph = psnr(ref, dec, n, &mh);
            printf("%-16s ASTC %2dx%-2d%s: BC7 against astc-encoder's decode: fast %.2f dB (max %3d), high %.2f dB (max %3d)\n", imgs[i].name, blocks[k].bw, blocks[k].bh,
                   blocks[k].srgb ? " sRGB" : "     ", pf, mf, ph, mh);
            CHECK(pf >= imgs[i].min_fast, "%s %dx%d fast pass within tolerance (%.2f dB >= %.0f)", imgs[i].name, blocks[k].bw, blocks[k].bh, pf, imgs[i].min_fast);
            CHECK(ph >= imgs[i].min_high && ph >= pf - 0.25, "%s %dx%d high-quality pass within tolerance (%.2f dB)", imgs[i].name, blocks[k].bw, blocks[k].bh, ph);
            if (pf < worst_fast) worst_fast = pf;
            if (ph < worst_high) worst_high = ph;
            free(astc); free(src); free(ref); free(dec); free(bc);
        }
    // The cache across a restart, and speed on a large texture
    uint32_t W = 1024, H = 1024; size_t n = (size_t)W * H * 4, as = 0;
    uint8_t *src = malloc(n), *bc = malloc(vka_bc7_size(W, H, 1)), *rgba = malloc(n);
    make_image(1, W, H, src);
    uint8_t *astc = astc_compress(src, W, H, 6, 6, 1, &as);
    uint64_t t0 = now_ns(); vka_decode_rgba8(astc, W, H, 1, 6, 6, 1, rgba); uint64_t td = now_ns() - t0;
    t0 = now_ns(); vka_encode_bc7(rgba, W, H, VKA_FAST, bc); uint64_t tf = now_ns() - t0;
    t0 = now_ns(); vka_encode_bc7(rgba, W, H, VKA_HIGH, bc); uint64_t th = now_ns() - t0;
    printf("1024x1024 ASTC 6x6 sRGB: decode %.1f ms (%.0f Mtexel/s), BC7 fast pass %.1f ms (%.0f Mtexel/s, up to 4 threads), high quality %.0f ms (%.1f Mtexel/s, one thread)\n",
           vt_ms(td), W * H / (td / 1e3), vt_ms(tf), W * H / (tf / 1e3), vt_ms(th), W * H / (th / 1e3));
    vka_key key; vka_get_bc7(astc, W, H, 1, 6, 6, 1, bc, &key); vka_wait_idle();
    vka_shutdown(); vka_init(dir);
    vka_stats_t s0, s1; vka_stats(&s0);
    t0 = now_ns(); int q = vka_get_bc7(astc, W, H, 1, 6, 6, 1, bc, &key); uint64_t tc = now_ns() - t0; vka_stats(&s1);
    CHECK(q == VKA_HIGH && s1.hits_high == s0.hits_high + 1, "after a restart the cache serves the high-quality pass");
    printf("cache: a 1024x1024 texture's high-quality BC7 read back from the cache in %.2f ms; worst PSNR fast %.2f dB, high %.2f dB\n", vt_ms(tc), worst_fast, worst_high);
    vka_shutdown();
    free(src); free(bc); free(rgba); free(astc);
    printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "ok", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
