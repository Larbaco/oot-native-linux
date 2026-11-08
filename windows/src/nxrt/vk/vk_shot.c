// Screenshots (vk_shot.h). PNG from the specification (RFC 2083): IHDR, one IDAT holding a zlib stream of stored deflate blocks (RFC 1950 / 1951), IEND.
#include "vk_replay.h"
#include "vk_shot.h"
#include "vk_tex.h"
#include "vk_submit.h"
#include "vk_present.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#ifndef _WIN32
#include <unistd.h>
#endif

const char *nxrt_log_path(void);

static uint64_t g_at[64]; static int g_nat; static uint64_t g_every, g_count, g_luma, g_luma_from, g_luma_to = ~0ull;   // g_luma: NXRT_VK_LUMA=<n>
static char g_dir[1024], g_file[1024]; static int g_window;
static plat_rwlock_t g_req_lock = PLAT_RWLOCK_INIT; static char g_req[8][1024]; static int g_nreq;   // vks_shot_request (DM_CTL shot): taken in order, one per present

// ---------- PNG ----------
static uint32_t crc_tab[256];
static uint32_t crc32_of(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1; crc_tab[i] = v; }
    c = ~c; for (size_t i = 0; i < n; i++) c = crc_tab[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return ~c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static int chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
    uint8_t h[8]; be32(h, n); memcpy(h + 4, type, 4);
    uint32_t c = crc32_of(0, (const uint8_t *)type, 4); c = crc32_of(c, data, n);
    uint8_t t[4]; be32(t, c);
    return fwrite(h, 1, 8, f) == 8 && (!n || fwrite(data, 1, n, f) == n) && fwrite(t, 1, 4, f) == 4 ? 0 : -1;
}
// NXRT_VK_PNG_MAXW=<n> (screenshots) / NXRT_VK_TEXDUMP_MAXW=<n> (texture dumps): the image is box-averaged down by a power of two until it is at most n wide. The PNGs are stored
// uncompressed (a 2560x1440 one is 14.7 MB) and the disk can be short of room
static __thread int t_dump;
static int shrink_of(int w) {
    const char *e = getenv(t_dump ? "NXRT_VK_TEXDUMP_MAXW" : "NXRT_VK_PNG_MAXW"); int mw = e ? atoi(e) : 0, k = 1;
    while (mw >= 16 && w / k > mw) k *= 2;
    return k;
}
int vks_png_write(const char *path, const uint8_t *rgba, int w, int h) {
    int k = t_dump < 0 ? 1 : shrink_of(w);
    if (k > 1 && w / k > 0 && h / k > 0) {
        int sw = w / k, sh = h / k; uint8_t *sm = malloc((size_t)sw * sh * 4); if (!sm) return -1;
        for (int y = 0; y < sh; y++) for (int x = 0; x < sw; x++) for (int c = 0; c < 4; c++) {
            unsigned acc = 0; for (int dy = 0; dy < k; dy++) for (int dx = 0; dx < k; dx++) acc += rgba[((size_t)(y * k + dy) * w + (x * k + dx)) * 4 + c];
            sm[((size_t)y * sw + x) * 4 + c] = (uint8_t)(acc / (unsigned)(k * k));
        }
        int saved = t_dump; t_dump = -1; int rc = vks_png_write(path, sm, sw, sh); t_dump = saved; free(sm);   // (t_dump -1: the small image is written as it is)
        return rc;
    }
    if (w <= 0 || h <= 0) return -1;
    size_t row = (size_t)w * 4 + 1, raw = row * (size_t)h, nblk = (raw + 65534) / 65535, zn = 2 + raw + nblk * 5 + 4;
    uint8_t *z = malloc(zn); if (!z) return -1;
    size_t o = 0; z[o++] = 0x78; z[o++] = 0x01;
    uint32_t a = 1, b = 0; size_t done = 0;
    uint8_t *line = malloc(row);
    // the filtered rows (filter 0) streamed into stored blocks
    size_t blk_left = 0;
    for (int y = 0; y < h; y++) {
        line[0] = 0; memcpy(line + 1, rgba + (size_t)y * w * 4, (size_t)w * 4);
        for (size_t i = 0; i < row; i++) {
            if (!blk_left) {
                size_t n = raw - done < 65535 ? raw - done : 65535; blk_left = n;
                z[o++] = done + n == raw ? 1 : 0; z[o++] = (uint8_t)n; z[o++] = (uint8_t)(n >> 8); z[o++] = (uint8_t)~n; z[o++] = (uint8_t)(~n >> 8);
            }
            z[o++] = line[i]; a = (a + line[i]) % 65521; b = (b + a) % 65521; blk_left--; done++;
        }
    }
    free(line);
    be32(z + o, b << 16 | a); o += 4;
    FILE *f = plat_fopen(path, "wb"); if (!f) { free(z); return -1; }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    uint8_t ih[13]; be32(ih, (uint32_t)w); be32(ih + 4, (uint32_t)h); ih[8] = 8; ih[9] = 6; ih[10] = ih[11] = ih[12] = 0;
    int rc = fwrite(sig, 1, 8, f) == 8 && !chunk(f, "IHDR", ih, 13) && !chunk(f, "IDAT", z, (uint32_t)o) && !chunk(f, "IEND", NULL, 0) ? 0 : -1;
    fclose(f); free(z);
    return rc;
}

// ---------- Formats → RGBA8 (the stored values; sRGB bytes stay sRGB, as the window shows them) ----------
static uint8_t f2u8(float v) { if (!(v > 0)) return 0; if (v >= 1) return 255; return (uint8_t)lrintf(v * 255.0f); }
static float half(uint16_t h) {
    uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023, f;
    if (!e) { if (!m) f = s; else { e = 127 - 15 + 1; while (!(m & 1024)) { m <<= 1; e--; } m &= 1023; f = s | e << 23 | m << 13; } }
    else if (e == 31) f = s | 0x7f800000u | m << 13; else f = s | (e + 127 - 15) << 23 | m << 13;
    float r; memcpy(&r, &f, 4); return r;
}
int vks_to_rgba8(VkFormat f, const uint8_t *s, size_t n, uint8_t *d) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: memcpy(d, s, n * 4); return 0;
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: for (size_t i = 0; i < n; i++) { d[4 * i] = s[4 * i + 2]; d[4 * i + 1] = s[4 * i + 1]; d[4 * i + 2] = s[4 * i]; d[4 * i + 3] = s[4 * i + 3]; } return 0;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: for (size_t i = 0; i < n; i++) { uint32_t v; memcpy(&v, s + 4 * i, 4);
            d[4 * i] = (uint8_t)((v & 1023) >> 2); d[4 * i + 1] = (uint8_t)((v >> 10 & 1023) >> 2); d[4 * i + 2] = (uint8_t)((v >> 20 & 1023) >> 2); d[4 * i + 3] = (uint8_t)((v >> 30) * 85); } return 0;
    case VK_FORMAT_R16G16B16A16_SFLOAT: for (size_t i = 0; i < n; i++) for (int c = 0; c < 4; c++) { uint16_t h; memcpy(&h, s + 8 * i + 2 * c, 2); d[4 * i + c] = f2u8(half(h)); } return 0;
    case VK_FORMAT_R16G16B16A16_UNORM: for (size_t i = 0; i < n; i++) for (int c = 0; c < 4; c++) { uint16_t h; memcpy(&h, s + 8 * i + 2 * c, 2); d[4 * i + c] = (uint8_t)(h >> 8); } return 0;
    case VK_FORMAT_R32G32B32A32_SFLOAT: for (size_t i = 0; i < n; i++) for (int c = 0; c < 4; c++) { float v; memcpy(&v, s + 16 * i + 4 * c, 4); d[4 * i + c] = f2u8(v); } return 0;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: for (size_t i = 0; i < n; i++) { uint32_t v; memcpy(&v, s + 4 * i, 4);   // 5-bit exponents, 6 / 6 / 5-bit mantissas
            uint32_t c[3] = {v & 0x7ff, v >> 11 & 0x7ff, v >> 22}; int mb[3] = {6, 6, 5};
            for (int k = 0; k < 3; k++) { uint32_t e = c[k] >> mb[k], m = c[k] & ((1u << mb[k]) - 1); float f = e ? ldexpf(1.0f + (float)m / (float)(1u << mb[k]), (int)e - 15) : ldexpf((float)m, -14 - mb[k]); d[4 * i + k] = f2u8(f); }
            d[4 * i + 3] = 255; } return 0;
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: for (size_t i = 0; i < n; i++) { uint32_t v; memcpy(&v, s + 4 * i, 4);   // 9-bit mantissas (R low) sharing a 5-bit exponent (bias 15)
            float sc = ldexpf(1.0f, (int)(v >> 27) - 15 - 9); for (int k = 0; k < 3; k++) d[4 * i + k] = f2u8((float)(v >> (9 * k) & 511) * sc);
            d[4 * i + 3] = 255; } return 0;
    case VK_FORMAT_R32_SFLOAT: for (size_t i = 0; i < n; i++) { float v; memcpy(&v, s + 4 * i, 4); d[4 * i] = d[4 * i + 1] = d[4 * i + 2] = f2u8(v); d[4 * i + 3] = 255; } return 0;
    case VK_FORMAT_R16_SFLOAT: for (size_t i = 0; i < n; i++) { uint16_t h; memcpy(&h, s + 2 * i, 2); d[4 * i] = d[4 * i + 1] = d[4 * i + 2] = f2u8(half(h)); d[4 * i + 3] = 255; } return 0;
    case VK_FORMAT_R8_UNORM: for (size_t i = 0; i < n; i++) { d[4 * i] = d[4 * i + 1] = d[4 * i + 2] = s[i]; d[4 * i + 3] = 255; } return 0;
    case VK_FORMAT_R8G8_UNORM: for (size_t i = 0; i < n; i++) { d[4 * i] = s[2 * i]; d[4 * i + 1] = s[2 * i + 1]; d[4 * i + 2] = 0; d[4 * i + 3] = 255; } return 0;
    case VK_FORMAT_R16G16_SFLOAT: for (size_t i = 0; i < n; i++) { uint16_t h; memcpy(&h, s + 4 * i, 2); d[4 * i] = f2u8(half(h)); memcpy(&h, s + 4 * i + 2, 2); d[4 * i + 1] = f2u8(half(h)); d[4 * i + 2] = 0; d[4 * i + 3] = 255; } return 0;
    case VK_FORMAT_R5G6B5_UNORM_PACK16: for (size_t i = 0; i < n; i++) { uint16_t v; memcpy(&v, s + 2 * i, 2); d[4 * i] = (uint8_t)((v >> 11) * 255 / 31); d[4 * i + 1] = (uint8_t)((v >> 5 & 63) * 255 / 63); d[4 * i + 2] = (uint8_t)((v & 31) * 255 / 31); d[4 * i + 3] = 255; } return 0;
    default: return -1;
    }
}

int vks_shot_capture(const tex_t *t, const int crop[4], int flip, uint8_t **rgba, int *w, int *h) {
    *rgba = NULL;
    vkt_image *im = t ? vkt_by_serial(t->serial) : NULL;
    if (!im || !im->content) return -1;
    NVNcopyRegion r = {0, 0, 0, (int)im->w, (int)im->h, 1};
    if (crop && crop[2] > 0 && crop[3] > 0 && crop[0] >= 0 && crop[1] >= 0 && (uint32_t)(crop[0] + crop[2]) <= im->w && (uint32_t)(crop[1] + crop[3]) <= im->h) { r.xoffset = crop[0]; r.yoffset = crop[1]; r.width = crop[2]; r.height = crop[3]; }
    size_t n = (size_t)r.width * r.height, bytes = n * im->fi->vbpb;
    uint8_t *raw = malloc(bytes), *out = malloc(n * 4);
    if (!raw || !out) { free(raw); free(out); return -1; }
    vks_finish();   // the frame's GPU work first
    if (vkt_readback_region(im, 0, 0, &r, raw, bytes) || vks_to_rgba8(im->fi->vk, raw, n, out)) {
        static int nf; if (nf++ < 4) VKRED("screenshot: the presented image (format %s) could not be read back", im->fi->name);
        free(raw); free(out); return -1;
    }
    free(raw);
    if (flip) for (int y = 0; y < r.height / 2; y++) { uint8_t *a = out + (size_t)y * r.width * 4, *b = out + (size_t)(r.height - 1 - y) * r.width * 4; for (int x = 0; x < r.width * 4; x++) { uint8_t c = a[x]; a[x] = b[x]; b[x] = c; } }
    *rgba = out; *w = r.width; *h = r.height;
    return 0;
}

static void shot_path(char *out, size_t n, uint64_t frame, const char *suffix) {
    const char *lp = nxrt_log_path();
    if (*g_dir) snprintf(out, n, "%s" PLAT_SEP "shot-%06llu%s.png", g_dir, (unsigned long long)frame, suffix);
    else snprintf(out, n, "%s.shot-%06llu%s.png", lp && *lp ? lp : "nxrt", (unsigned long long)frame, suffix);
}
static int origin_ll(void) { extern int vk_origin_lower_left(void); return vk_origin_lower_left(); }

// NXRT_VK_LUMA=<n>, NXRT_VK_LUMA_FROM/_TO=<present> (diagnostics, flash detection): every n-th present is read back (after the frame's GPU work) and box-averaged to 320x180 RGB8, appended to <shot dir>/luma.bin as
// {u64 present, 320*180*3 bytes}; the mean R,G,B goes to the log ("luma: present N R G B"). tools/flash-detect.py finds the frames whose brightness jumps against both neighbours
static void luma_present(const tex_t *t, uint64_t frame, const int crop[4]) {
    uint8_t *px; int w, h;
    if (vks_shot_capture(t, crop, origin_ll(), &px, &w, &h)) return;
    enum { TW = 320, TH = 180 };
    static uint8_t th[TW * TH * 3]; static FILE *f;
    double m[3] = {0, 0, 0};
    for (int y = 0; y < TH; y++) for (int x = 0; x < TW; x++) {
        int x0 = x * w / TW, x1 = (x + 1) * w / TW, y0 = y * h / TH, y1 = (y + 1) * h / TH; unsigned long a[3] = {0, 0, 0}, n = 0;
        for (int yy = y0; yy < y1; yy++) for (int xx = x0; xx < x1; xx++) { const uint8_t *q = px + ((size_t)yy * w + xx) * 4; a[0] += q[0]; a[1] += q[1]; a[2] += q[2]; n++; }
        for (int c = 0; c < 3; c++) { th[(y * TW + x) * 3 + c] = n ? (uint8_t)(a[c] / n) : 0; m[c] += th[(y * TW + x) * 3 + c]; }
    }
    free(px);
    if (!f) { char p[1100]; snprintf(p, sizeof p, "%s" PLAT_SEP "luma.bin", *g_dir ? g_dir : "."); f = fopen(p, "ab"); }
    if (f) { uint64_t fr = frame; fwrite(&fr, 8, 1, f); fwrite(th, 1, sizeof th, f); fflush(f); }
    VKLOG("luma: present %llu %.4f %.4f %.4f", (unsigned long long)frame, m[0] / (TW * TH * 255.0), m[1] / (TW * TH * 255.0), m[2] / (TW * TH * 255.0));
    {   // NXRT_VK_LUMA_PD=<mean>[,<frames>[,<max triggers>]]: a present whose mean brightness is above <mean> (a white-out) starts a pass dump of the next <frames> frames (default 1), at most <max> times (default 4)
        static double thr = -1; static long pdn = 1; static int maxt = 4, trig;
        if (thr < 0) { const char *e = getenv("NXRT_VK_LUMA_PD"); thr = e && *e ? atof(e) : 2.0; if (e && strchr(e, ',')) { pdn = atol(strchr(e, ',') + 1); const char *e2 = strchr(strchr(e, ',') + 1, ','); if (e2) maxt = atoi(e2 + 1); } }
        double mean = (m[0] + m[1] + m[2]) / (3.0 * TW * TH * 255.0);
        if (mean > thr && trig < maxt) { trig++; vkr_passdump_next(pdn); VKLOG("luma: present %llu is white (mean %.3f): pass dump of the next %ld frame(s) (%d of %d)", (unsigned long long)frame, mean, pdn, trig, maxt); }
    }
    { const char *e = getenv("NXRT_VK_LUMA_EXIT"); if (e && atoi(e) && g_luma_to != ~0ull && frame + g_luma > g_luma_to) { VKLOG("luma: last present of the range captured, exiting"); if (f) fclose(f); fflush(NULL); _exit(0); } }   // diagnostics: a bounded run ends itself
}

void vks_shot_present(const tex_t *t, uint64_t frame, const int crop[4]) {
    int want = 0; char path[1100] = "";
    if (g_luma && frame % g_luma == 0 && frame >= g_luma_from && frame <= g_luma_to) luma_present(t, frame, crop);
    for (int i = 0; i < g_nat; i++) want |= g_at[i] == frame;
    if (g_every && frame % g_every == 0) want = 1;
    if (*g_file) {
                if (plat_is_file(g_file)) {
            want = 1;
            FILE *f = plat_fopen(g_file, "rb");
            if (f) { size_t k = fread(path, 1, sizeof path - 1, f); path[k] = 0; fclose(f); for (char *q = path; *q; q++) if (*q == '\r' || *q == '\n') { *q = 0; break; } }
            plat_unlink(g_file);
        }
    }
    if (!want && g_nreq) {
        plat_rw_wrlock(&g_req_lock);
        if (g_nreq) { want = 1; snprintf(path, sizeof path, "%s", g_req[0]); memmove(g_req[0], g_req[1], (size_t)--g_nreq * sizeof g_req[0]); }
        plat_rw_wrunlock(&g_req_lock);
    }
    if (!want) return;
    if (!*path) shot_path(path, sizeof path, frame, "");
    uint8_t *px; int w, h;
    uint64_t t0 = now_ns();
    if (vks_shot_capture(t, crop, origin_ll(), &px, &w, &h)) { VKLOG("screenshot of present %llu: nothing to read back", (unsigned long long)frame); return; }
    for (size_t i = 0; i < (size_t)w * h; i++) px[4 * i + 3] = 255;   // opaque, as the window shows it: the game leaves alpha 0 under some HUD panels
    int rc = vks_png_write(path, px, w, h);
    free(px);
    g_count++;
    VKLOG("screenshot: present %llu (%dx%d) %s %s (%.1f ms)", (unsigned long long)frame, w, h, rc ? "could not be written to" : "written to", path, (now_ns() - t0) / 1e6);
    if (g_window) {   // the window as shown: the next present's swapchain picture, on a thread of its own (it waits for the present thread)
        uint32_t *wp = NULL; int ww, wh, bgr;
        if (!vkp_capture_next(&wp, &ww, &wh, &bgr, 2000)) {
            uint8_t *o = malloc((size_t)ww * wh * 4);
            for (size_t i = 0; i < (size_t)ww * wh; i++) { uint32_t c = wp[i]; o[4 * i] = (uint8_t)(bgr ? c >> 16 : c); o[4 * i + 1] = (uint8_t)(c >> 8); o[4 * i + 2] = (uint8_t)(bgr ? c : c >> 16); o[4 * i + 3] = 255; }
            char wpth[1200]; snprintf(wpth, sizeof wpth, "%.*s.window.png", (int)(strlen(path) > 4 ? strlen(path) - 4 : strlen(path)), path);
            if (!vks_png_write(wpth, o, ww, wh)) VKLOG("screenshot: the window (%dx%d) written to %s", ww, wh, wpth);
            free(o); free(wp);
        }
    }
}
uint64_t vks_shot_count(void) { return g_count; }
void vks_shot_request(const char *path) {
    plat_rw_wrlock(&g_req_lock);
    int ok = g_nreq < 8; if (ok) snprintf(g_req[g_nreq++], sizeof g_req[0], "%s", path && *path ? path : "");
    plat_rw_wrunlock(&g_req_lock);
    VKLOG("screenshot of the next present %s %s", ok ? "requested:" : "not requested (8 are waiting already):", path && *path ? path : "beside the log");
}
// A texture as it is now (the GPU finished first), level 0, layer z, as PNG: the replay's texture dumps (NXRT_VK_TEXDUMP)
// Compressed images as DDS (DX10 header), the blocks as they are: <path>.dds
static int dump_dds(vkt_image *im, unsigned layer, const char *path) {
    uint32_t dxgi = 0;
    switch (im->fi->vk) {
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: dxgi = 71; break;  case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: dxgi = 72; break;
    case VK_FORMAT_BC2_UNORM_BLOCK: dxgi = 74; break;       case VK_FORMAT_BC2_SRGB_BLOCK: dxgi = 75; break;
    case VK_FORMAT_BC3_UNORM_BLOCK: dxgi = 77; break;       case VK_FORMAT_BC3_SRGB_BLOCK: dxgi = 78; break;
    case VK_FORMAT_BC4_UNORM_BLOCK: dxgi = 80; break;       case VK_FORMAT_BC5_UNORM_BLOCK: dxgi = 83; break;
    case VK_FORMAT_BC6H_UFLOAT_BLOCK: dxgi = 95; break;     case VK_FORMAT_BC7_UNORM_BLOCK: dxgi = 98; break;
    case VK_FORMAT_BC7_SRGB_BLOCK: dxgi = 99; break;        default: return -1;
    }
    uint32_t bw = (im->w + 3) / 4, bh = (im->h + 3) / 4; size_t bytes = (size_t)bw * bh * im->fi->vbpb;
    NVNcopyRegion r = {0, 0, 0, (int)im->w, (int)im->h, 1};   // the whole level: edge blocks may be partial
    uint8_t *raw = malloc(bytes); int rc = -1;
    if (raw && !vkt_readback_region(im, 0, layer, &r, raw, bytes)) {
        char p2[1300]; snprintf(p2, sizeof p2, "%s.dds", path);
        FILE *f = fopen(p2, "wb");
        if (f) {
            uint32_t h[32 + 5] = {0x20534444u, 124, 0x1007 | 0x80000, im->h, im->w, (uint32_t)(bw * im->fi->vbpb), 0, 1};
            h[19] = 32; h[20] = 4; h[21] = 0x30315844u;   // pixel format: FOURCC "DX10"
            h[27] = 0x1000;
            h[32] = dxgi; h[33] = 3; h[34] = 0; h[35] = 1; h[36] = 0;
            rc = fwrite(h, 4, 37, f) == 37 && fwrite(raw, 1, bytes, f) == bytes ? 0 : -1;
            fclose(f);
        }
    }
    free(raw);
    return rc;
}

// Depth images (M6, the shadow maps): the depth as grey (nearest white), and the values as they are in <path>.raw (16-bit for D16, 32-bit float for D32,
// the 24 bits of D24 in 32-bit words), to compare with the Mac host's DM_CTL texraw
static int dump_depth(vkt_image *im, unsigned layer, const char *path) {
    VkFormat f = im->fi->vk; size_t bpt = f == VK_FORMAT_D16_UNORM ? 2 : 4;
    NVNcopyRegion r = {0, 0, 0, (int)im->w, (int)im->h, 1};
    size_t n = (size_t)im->w * im->h; uint8_t *raw = malloc(n * bpt), *out = malloc(n * 4); int rc = -1;
    if (raw && out && !vkt_readback_region(im, 0, layer, &r, raw, n * bpt)) {
        for (size_t i = 0; i < n; i++) {
            double d; if (bpt == 2) { uint16_t v; memcpy(&v, raw + 2 * i, 2); d = v / 65535.0; }
            else if (f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D32_SFLOAT_S8_UINT) { float v; memcpy(&v, raw + 4 * i, 4); d = v; }
            else { uint32_t v; memcpy(&v, raw + 4 * i, 4); d = (v & 0xffffff) / 16777215.0; }
            uint8_t g = (uint8_t)lrint(fmin(fmax(1.0 - d, 0), 1) * 255); out[4 * i] = out[4 * i + 1] = out[4 * i + 2] = g; out[4 * i + 3] = 255;
        }
        rc = vks_png_write(path, out, (int)im->w, (int)im->h);
        char p2[1300]; snprintf(p2, sizeof p2, "%s.raw", path); FILE *fr = fopen(p2, "wb"); if (fr) { fwrite(raw, 1, n * bpt, fr); fclose(fr); }
    }
    free(raw); free(out);
    return rc;
}
// Unclamped RGB of the formats the HDR chain uses (NXRT_VK_TEXDUMP stats: the PNG clamps to 0..1, this does not); 0 when f is not one of them
static int texel_rgb(VkFormat f, const uint8_t *s, size_t i, float c[3]) {
    switch (f) {
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: { uint32_t v; memcpy(&v, s + 4 * i, 4); uint32_t q[3] = {v & 0x7ff, v >> 11 & 0x7ff, v >> 22}; int mb[3] = {6, 6, 5};
        for (int k = 0; k < 3; k++) { uint32_t e = q[k] >> mb[k], m = q[k] & ((1u << mb[k]) - 1); c[k] = e ? ldexpf(1.0f + (float)m / (float)(1u << mb[k]), (int)e - 15) : ldexpf((float)m, -14 - mb[k]); } return 1; }
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: { uint32_t v; memcpy(&v, s + 4 * i, 4); float sc = ldexpf(1.0f, (int)(v >> 27) - 15 - 9); for (int k = 0; k < 3; k++) c[k] = (float)(v >> (9 * k) & 511) * sc; return 1; }
    case VK_FORMAT_R16G16B16A16_SFLOAT: for (int k = 0; k < 3; k++) { uint16_t h; memcpy(&h, s + 8 * i + 2 * k, 2); c[k] = half(h); } return 1;
    case VK_FORMAT_R32_SFLOAT: { float v; memcpy(&v, s + 4 * i, 4); c[0] = c[1] = c[2] = v; return 1; }   // one-channel float targets (linear depth, ...): stats and .f32 dumps
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: for (int k = 0; k < 3; k++) c[k] = s[4 * i + k] / 255.0f; return 1;
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: for (int k = 0; k < 3; k++) c[k] = s[4 * i + 2 - k] / 255.0f; return 1;
    default: return 0;
    }
}
// One log line: the mean / max per channel and the share of texels by luma bin (<.05 <.1 <.25 <.5 <.75 <1 <2 <4 >=4), raw values (sRGB formats: the stored code values)
// NXRT_VK_TEXDUMP_RAW=<max width>: beside each dumped HDR image its linear RGB as float32 (<path>.f32, 3 floats a texel, box-averaged by a power of two down to at most that width, the size in the log line), to redo the tone map offline
static void dump_raw_f32(vkt_image *im, const uint8_t *raw, const char *path) {
    const char *e = getenv("NXRT_VK_TEXDUMP_RAW"); int mw = e ? atoi(e) : 0; float c[3]; if (mw <= 0 || !texel_rgb(im->fi->vk, raw, 0, c)) return;
    uint32_t k = 1; while (im->w / k > (uint32_t)mw) k *= 2;
    uint32_t ow = im->w / k, oh = im->h / k; float *o = calloc((size_t)ow * oh * 3, 4); if (!o) return;
    for (uint32_t y = 0; y < oh * k; y++) for (uint32_t x = 0; x < ow * k; x++) { float t[3]; texel_rgb(im->fi->vk, raw, (size_t)y * im->w + x, t); float *d = o + ((size_t)(y / k) * ow + x / k) * 3; for (int q = 0; q < 3; q++) d[q] += t[q] / (float)(k * k); }
    char p2[1300]; snprintf(p2, sizeof p2, "%s.f32", path); FILE *f = fopen(p2, "wb"); if (f) { fwrite(o, 4, (size_t)ow * oh * 3, f); fclose(f); VKLOG("texture raw: %s %ux%u float32 RGB (box %u)", p2, ow, oh, k); }
    free(o);
}
static void log_stats(vkt_image *im, const uint8_t *raw, size_t n, const char *path) {
    float c[3]; if (!n || !texel_rgb(im->fi->vk, raw, 0, c)) return;
    static const float edge[8] = {.05f, .1f, .25f, .5f, .75f, 1, 2, 4}; double sum[3] = {0, 0, 0}, ysum = 0; float mx[3] = {0, 0, 0}; size_t hist[9] = {0}, nan_ = 0;
    for (size_t i = 0; i < n; i++) {
        texel_rgb(im->fi->vk, raw, i, c); if (!(c[0] == c[0] && c[1] == c[1] && c[2] == c[2])) { nan_++; continue; }
        float y = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; int b = 0; while (b < 8 && y >= edge[b]) b++; hist[b]++; ysum += y;
        for (int k = 0; k < 3; k++) { sum[k] += c[k]; if (c[k] > mx[k]) mx[k] = c[k]; }
    }
    double d = (double)(n - nan_ ? n - nan_ : 1);
    VKLOG("texture stats: %s mean rgb %.4f %.4f %.4f luma %.4f max %.3f %.3f %.3f nan %zu hist%%(<.05 <.1 <.25 <.5 <.75 <1 <2 <4 >=4) %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f",
          path, sum[0] / d, sum[1] / d, sum[2] / d, ysum / d, mx[0], mx[1], mx[2], nan_, 100.0 * hist[0] / d, 100.0 * hist[1] / d, 100.0 * hist[2] / d, 100.0 * hist[3] / d, 100.0 * hist[4] / d, 100.0 * hist[5] / d, 100.0 * hist[6] / d, 100.0 * hist[7] / d, 100.0 * hist[8] / d);
}
// NXRT_VK_DRAWSTATS: a centre block (480x270) of a colour image copied into a host buffer slot by commands recorded in the replay's own command buffer (a readback through a one-shot
// command buffer only sees the submissions already committed, not the one being recorded), reduced to luma statistics once the frame is done
#define ST_SLOTS 900
#define ST_BW 480
#define ST_BH 270
#define ST_SLOT_BYTES 524288u
static VkBuffer g_sb; static vk_alloc g_sm; static int g_nst; static struct { char tag[110]; VkFormat f; } g_stg[ST_SLOTS];
static int cmp_f(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }
int vks_stat_record(VkCommandBuffer cb, vkt_image *im, const char *tag) {
    if (!im || g_nst >= ST_SLOTS || im->w < ST_BW || im->h < ST_BH || im->fi->vbpb != 4) return -1;
    if (!g_sb) {
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, (VkDeviceSize)ST_SLOT_BYTES * ST_SLOTS, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
        if (vkCreateBuffer(g_vk.dev, &bci, NULL, &g_sb) != VK_SUCCESS) { g_sb = VK_NULL_HANDLE; return -1; }
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, g_sb, &mr);
        if (vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0, &g_sm) &&
            vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, &g_sm)) { vkDestroyBuffer(g_vk.dev, g_sb, NULL); g_sb = VK_NULL_HANDLE; return -1; }
        vkBindBufferMemory(g_vk.dev, g_sb, g_sm.mem, g_sm.off);
    }
    vk_image_barrier(cb, im->img, im->aspect, im->rest, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy c = {(VkDeviceSize)g_nst * ST_SLOT_BYTES, 0, 0, {im->aspect, 0, 0, 1}, {(int32_t)(im->w - ST_BW) / 2, (int32_t)(im->h - ST_BH) / 2, 0}, {ST_BW, ST_BH, 1}};
    vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_sb, 1, &c);
    vk_image_barrier(cb, im->img, im->aspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im->rest, VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    vk_mem_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    snprintf(g_stg[g_nst].tag, sizeof g_stg[0].tag, "%s", tag); g_stg[g_nst].f = im->fi->vk; g_nst++;
    return 0;
}
int vks_stat_pending(void) { return g_nst; }
void vks_stat_report(void) {   // the GPU is done with every recorded copy
    float *l = malloc(sizeof(float) * (ST_BW / 4 + 1) * (ST_BH / 4 + 1));
    for (int k = 0; k < g_nst && l; k++) {
        const uint8_t *raw = (const uint8_t *)g_sm.map + (size_t)k * ST_SLOT_BYTES; float c[3]; size_t m = 0, nanc = 0; double sum = 0;
        for (int y = 0; y < ST_BH; y += 4) for (int x = 0; x < ST_BW; x += 4) { if (!texel_rgb(g_stg[k].f, raw, (size_t)y * ST_BW + x, c)) continue;
            float v = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; if (c[0] >= 65536.f || c[1] >= 65536.f || c[2] >= 65536.f || v != v) nanc++; l[m++] = v; sum += v; }
        { static const char *dsub = (const char *)-1; if (dsub == (const char *)-1) dsub = getenv("NXRT_VK_DRAWSTATS_DUMP");   // NXRT_VK_DRAWSTATS_DUMP=<text in the tag>: the raw 480x270 block of those draws is written to <NXRT_VK_DRAWSTATS_DUMP_DIR or .>/dstat-<n>.raw (4 bytes a texel, the target's format) with the tag in dstat.txt
            if (dsub && *dsub && strstr(g_stg[k].tag, dsub)) { static int nd; const char *dd = getenv("NXRT_VK_DRAWSTATS_DUMP_DIR"); char pth[600]; snprintf(pth, sizeof pth, "%s/dstat-%d.raw", dd ? dd : ".", nd);
                FILE *f = fopen(pth, "wb"); if (f) { fwrite(raw, 1, (size_t)ST_BW * ST_BH * 4, f); fclose(f); snprintf(pth, sizeof pth, "%s/dstat.txt", dd ? dd : "."); f = fopen(pth, "a"); if (f) { fprintf(f, "%d %s\n", nd, g_stg[k].tag); fclose(f); } } nd++; } }
        if (!m) continue; qsort(l, m, sizeof(float), cmp_f);
        VKLOG("draw stats %s: centre luma mean %.3f median %.3f p10 %.3f p90 %.3f p99 %.3f max %.1f (inf/nan texels %.1f%%)", g_stg[k].tag, sum / (double)m, l[m / 2], l[m / 10], l[m * 9 / 10], l[m * 99 / 100], l[m - 1], 100.0 * (double)nanc / (double)m);
    }
    free(l); g_nst = 0;
}

// NXRT_VK_NANSCAN=<text in an fs key>: after each draw with such a fragment shader into an R11G11B10F colour target, a full copy of the target (host buffer ring of 40 slots, recorded in the replay's
// own command buffer like DRAWSTATS); at the next frame boundary the copies are scanned for inf/NaN texels (and the centre block alone), a line is logged for every draw with any, the first
// NXRT_VK_NANSCAN_FILES (default 4) go to <NXRT_VK_DRAWSTATS_DUMP_DIR or .>/nanscan-<n>.raw (w*h*4 bytes) with the tag in nanscan.txt
#define SC_SLOTS 40
static VkBuffer g_scb; static vk_alloc g_scm; static int g_nsc; static size_t g_scslot; static struct { char tag[160]; VkFormat f; uint32_t w, h; } g_scg[SC_SLOTS]; static uint64_t g_sc_draws, g_sc_bad_draws, g_sc_texels, g_sc_bad_texels, g_sc_ctr_texels, g_sc_ctr_bad, g_sc_dropped;
int vks_scan_record(VkCommandBuffer cb, vkt_image *im, const char *tag) {
    if (!im || im->fi->vbpb != 4) return -1;
    if (g_nsc >= SC_SLOTS) { g_sc_dropped++; return -1; }
    size_t need = (size_t)im->w * im->h * 4;
    if (!g_scb) {
        g_scslot = (need + 4095) & ~(size_t)4095; g_scslot = g_scslot < (size_t)1920 * 1088 * 4 ? (size_t)1920 * 1088 * 4 : g_scslot;
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, (VkDeviceSize)g_scslot * SC_SLOTS, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
        if (vkCreateBuffer(g_vk.dev, &bci, NULL, &g_scb) != VK_SUCCESS) { g_scb = VK_NULL_HANDLE; return -1; }
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, g_scb, &mr);
        if (vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0, &g_scm) &&
            vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, &g_scm)) { vkDestroyBuffer(g_vk.dev, g_scb, NULL); g_scb = VK_NULL_HANDLE; return -1; }
        vkBindBufferMemory(g_vk.dev, g_scb, g_scm.mem, g_scm.off);
    }
    if (need > g_scslot) return -1;
    vk_image_barrier(cb, im->img, im->aspect, im->rest, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy c = {(VkDeviceSize)g_nsc * g_scslot, 0, 0, {im->aspect, 0, 0, 1}, {0, 0, 0}, {im->w, im->h, 1}};
    vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_scb, 1, &c);
    vk_image_barrier(cb, im->img, im->aspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im->rest, VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    vk_mem_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    snprintf(g_scg[g_nsc].tag, sizeof g_scg[0].tag, "%s", tag); g_scg[g_nsc].f = im->fi->vk; g_scg[g_nsc].w = im->w; g_scg[g_nsc].h = im->h; g_nsc++;
    return 0;
}
int vks_scan_pending(void) { return g_nsc; }
static int r11_bad(uint32_t v) { return ((v & 0x7ff) >> 6) == 31 || (((v >> 11) & 0x7ff) >> 6) == 31 || (v >> 27) == 31; }
void vks_scan_report(void) {   // the GPU is done with every recorded copy
    static int nfiles = -1, maxfiles; if (nfiles < 0) { const char *e = getenv("NXRT_VK_NANSCAN_FILES"); maxfiles = e ? atoi(e) : 2; nfiles = 0; }   // frames (all their scanned draws) written as files
    int anybad = 0;
    for (int k = 0; k < g_nsc; k++) {
        if (g_scg[k].f != VK_FORMAT_B10G11R11_UFLOAT_PACK32) continue;
        const uint32_t *p = (const uint32_t *)((const uint8_t *)g_scm.map + (size_t)k * g_scslot); uint32_t w = g_scg[k].w, h = g_scg[k].h;
        uint64_t bad = 0, cbad = 0; uint32_t x0 = w, x1 = 0, y0 = h, y1 = 0, cx0 = (w - 480) / 2, cy0 = (h - 270) / 2;
        for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) if (r11_bad(p[(size_t)y * w + x])) { bad++; if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
            if (x >= cx0 && x < cx0 + 480 && y >= cy0 && y < cy0 + 270) cbad++; }
        g_sc_draws++; g_sc_texels += (uint64_t)w * h; g_sc_bad_texels += bad; g_sc_ctr_texels += 480 * 270; g_sc_ctr_bad += cbad; if (bad) { g_sc_bad_draws++; anybad = 1; }
        if (bad) VKLOG("nanscan %s: %llu inf/NaN texels of %ux%u (%.4f%%), centre block %llu (%.3f%%), bbox x %u..%u y %u..%u", g_scg[k].tag, (unsigned long long)bad, w, h, 100.0 * (double)bad / ((double)w * h), (unsigned long long)cbad, 100.0 * (double)cbad / (480.0 * 270.0), x0, x1, y0, y1);
    }
    static uint64_t at = (uint64_t)-1; if (at == (uint64_t)-1) { const char *e = getenv("NXRT_VK_NANSCAN_AT"); at = e ? strtoull(e, NULL, 0) : 0; }   // NXRT_VK_NANSCAN_AT=<frame>: also write the first frame at or after it, bad texels or not
    extern uint64_t vks_scan_cur_frame(void); if (!anybad && at && vks_scan_cur_frame() >= at && g_nsc) { anybad = 1; at = 0; }
    if (anybad && nfiles < maxfiles) {
        const char *dd = getenv("NXRT_VK_DRAWSTATS_DUMP_DIR");
        for (int k = 0; k < g_nsc; k++) { char pth[600]; snprintf(pth, sizeof pth, "%s/nanscan-%d-%d.raw", dd ? dd : ".", nfiles, k); FILE *f = fopen(pth, "wb");
            if (f) { fwrite((const uint8_t *)g_scm.map + (size_t)k * g_scslot, 4, (size_t)g_scg[k].w * g_scg[k].h, f); fclose(f); snprintf(pth, sizeof pth, "%s/nanscan.txt", dd ? dd : "."); f = fopen(pth, "a"); if (f) { fprintf(f, "%d-%d %ux%u %s\n", nfiles, k, g_scg[k].w, g_scg[k].h, g_scg[k].tag); fclose(f); } } }
        nfiles++; }
    g_nsc = 0;
}
void vks_scan_summary(void) { if (g_sc_draws) VKLOG("nanscan total: %llu draws scanned (%llu dropped), %llu with inf/NaN; texels %llu bad %llu (%.5f%%); centre-block texels %llu bad %llu (%.4f%%)", (unsigned long long)g_sc_draws, (unsigned long long)g_sc_dropped, (unsigned long long)g_sc_bad_draws, (unsigned long long)g_sc_texels, (unsigned long long)g_sc_bad_texels, 100.0 * (double)g_sc_bad_texels / (double)g_sc_texels, (unsigned long long)g_sc_ctr_texels, (unsigned long long)g_sc_ctr_bad, 100.0 * (double)g_sc_ctr_bad / (double)g_sc_ctr_texels); }
static int dump_image_(vkt_image *im, unsigned layer, const char *path);
int vks_dump_image(vkt_image *im, unsigned layer, const char *path) {
    if (im && !(im->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) && (im->fi->flags & VKF_COMPRESSED) && getenv("NXRT_VK_TEXDUMP_NODDS")) return -1;   // compressed ones are written as raw DDS: big
    t_dump = 1; int rc = dump_image_(im, layer, path); t_dump = 0; return rc;
}
static int dump_image_(vkt_image *im, unsigned layer, const char *path) {
    if (im && (im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) && im->vtype != VK_IMAGE_VIEW_TYPE_3D) return dump_depth(im, layer, path);
    if (im && !(im->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) && (im->fi->flags & VKF_COMPRESSED)) return dump_dds(im, layer, path);
    if (!im || (im->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) || (im->fi->flags & VKF_COMPRESSED)) return -1;
    NVNcopyRegion r = {0, 0, im->vtype == VK_IMAGE_VIEW_TYPE_3D ? (int)layer : 0, (int)im->w, (int)im->h, 1};
    size_t n = (size_t)im->w * im->h, bytes = n * im->fi->vbpb;
    uint8_t *raw = malloc(bytes), *out = malloc(n * 4); int rc = -1;
    if (raw && out && !vkt_readback_region(im, 0, im->vtype == VK_IMAGE_VIEW_TYPE_3D ? 0 : layer, &r, raw, bytes) && !vks_to_rgba8(im->fi->vk, raw, n, out)) { rc = vks_png_write(path, out, (int)im->w, (int)im->h); if (t_dump) { log_stats(im, raw, n, path); dump_raw_f32(im, raw, path); } }
    free(raw); free(out);
    return rc;
}

void vks_shot_init(void) {
    const char *e = getenv("NXRT_VK_SHOT");
    for (const char *q = e; q && *q && g_nat < 64; ) { char *end; unsigned long long v = strtoull(q, &end, 0); if (end == q) break; g_at[g_nat++] = v; q = *end == ',' ? end + 1 : end; }
    if ((e = getenv("NXRT_VK_SHOT_EVERY"))) g_every = strtoull(e, NULL, 0);
    if ((e = getenv("NXRT_VK_LUMA"))) g_luma = strtoull(e, NULL, 0);
    if ((e = getenv("NXRT_VK_LUMA_FROM"))) g_luma_from = strtoull(e, NULL, 0);   // only the presents in [FROM, TO]
    if ((e = getenv("NXRT_VK_LUMA_TO"))) g_luma_to = strtoull(e, NULL, 0);
    if ((e = getenv("NXRT_VK_SHOT_FILE"))) snprintf(g_file, sizeof g_file, "%s", e);
    if ((e = getenv("NXRT_VK_SHOT_DIR"))) { snprintf(g_dir, sizeof g_dir, "%s", e); plat_mkdir(g_dir); }
    g_window = (e = getenv("NXRT_VK_SHOT_WINDOW")) && atoi(e);
    if (g_nat || g_every || *g_file) VKLOG("screenshots: %d presents named, every %llu, control file %s, to %s", g_nat, (unsigned long long)g_every, *g_file ? g_file : "-", *g_dir ? g_dir : "beside the log");
}
