/* Device, window and swapchain bring-up (docs/m5-vulkan.md §6): a Vulkan 1.3 device with the replay's features, a resizable SDL3 window, FIFO and
   mailbox presentation at the game's pace, the picture scaled with its aspect kept at 1080p, 1440p, 4K and ultrawide window sizes (as large as the
   desktop allows), a texture presented through the texture path, and no validation errors. Skipped (77) without a GPU or a display. */
#include "vk_test.h"

NXRT_EARLY_RESERVE()

static int captured(uint32_t **px, int *w, int *h, int *bgr) {
    for (int i = 0; i < 20; i++) { vkp_enqueue(0, 0, 0, NULL, 0); if (!vkp_capture_next(px, w, h, bgr, 300)) return 0; }
    return -1;
}
static uint32_t pix(const uint32_t *p, int w, int x, int y, int bgr) { uint32_t v = p[y * w + x]; return bgr ? (v & 0xff00ff00u) | (v >> 16 & 0xff) | (v & 0xff) << 16 : v; }   // as RGBA

int main(void) {
    crash_install();
    int rc = vt_up(1, 1);
    if (rc) return rc;
    CHECK(g_vk.f13.dynamicRendering && g_vk.f13.synchronization2 && g_vk.f12.timelineSemaphore && g_vk.f12.descriptorIndexing, "core features");
    CHECK(g_vk.has.corpus_ok, "the features the translated shader corpus declares (docs/m5-record-shaders.md)");
    printf("device %s, vendor %s; sparse buffers %d, custom border %d, xfb %d, depth clip control %d, eds3 %d, NV viewport swizzle %d\n", g_vk.props.deviceName,
           vk_vendor_name(g_vk.props.vendorID), g_vk.has.sparse_buffer, g_vk.has.custom_border, g_vk.has.xfb, g_vk.has.depth_clip_control, g_vk.has.eds3, g_vk.has.viewport_swizzle_nv);
    CHECK(vkpc_init() == 0, "pipeline cache");
    CHECK(vkp_init(VKP_FIFO) == 0, "swapchain (FIFO)");

    // The window may be minimized by the person at the desktop meanwhile: a size request restores it; a run that stays minimized is skipped, not failed
    for (int i = 0; i < 100 && vkw_minimized(); i++) { if (i % 20 == 0) vkw_request_size(1280, 720); Sleep(10); }
    if (vkw_minimized()) { printf("the window is minimized: skipped\n"); vt_down(1); return 77; }
    // FIFO at the game's pace: 120 test-pattern presents of a 60 Hz game, no faster than the period asks
    int hz = vkw_display_hz();
    uint64_t t0 = now_ns(), tk = 0;
    for (int i = 0; i < 120; i++) tk = vkp_enqueue(0, 1.0 / 60, 0, NULL, 0);   // as fast as the queue takes them: the period paces them (the queue holds 4)
    vkp_wait(tk);
    double rate = 120 / ((now_ns() - t0) / 1e9);
    printf("FIFO: 120 presents in %.2f s = %.1f per s (display %d Hz), %llu dropped\n", (now_ns() - t0) / 1e9, rate, hz, (unsigned long long)vkp_dropped());
    CHECK(rate > 50 && rate < 66, "FIFO presents at the game's 60 Hz pace (%.1f)", rate);
    CHECK(vkp_presented_time(vkp_count()) > 0, "presented time recorded");

    // Resolution independence: the 16:9 picture letterboxed into each window shape
    const int sizes[][2] = {{1920, 1080}, {2560, 1440}, {3840, 2160}, {3440, 1440}, {1280, 1024}, {900, 1600}};
    for (unsigned s = 0; s < sizeof sizes / sizeof *sizes; s++) {
        uint32_t ser = vkw_resize_serial();
        vkw_request_size(sizes[s][0], sizes[s][1]);
        for (int i = 0; i < 50 && vkw_resize_serial() == ser; i++) Sleep(10);
        uint32_t *px = NULL; int w = 0, h = 0, bgr = 0;
        if (captured(&px, &w, &h, &bgr)) { CHECK(0, "capture at %dx%d", sizes[s][0], sizes[s][1]); continue; }
        int sw, sh; vkp_info(&sw, &sh, NULL, NULL);
        double sc = fmin(w / 1920.0, h / 1080.0); int vw = (int)floor(1920 * sc + 0.5), vh = (int)floor(1080 * sc + 0.5), vx = (int)floor((w - vw) / 2.0), vy = (int)floor((h - vh) / 2.0);
        // inside the picture: the top-left of the colour bars is light grey (the 0.75 bar; the border line is red), outside it black
        uint32_t in = pix(px, w, vx + vw / 16, vy + vh / 4, bgr), corner = pix(px, w, 0, 0, bgr);
        int letter = vw < w - 2 || vh < h - 2;
        printf("window %dx%d asked, swapchain %dx%d, picture %dx%d at %d,%d: inside %08x, corner %08x\n", sizes[s][0], sizes[s][1], w, h, vw, vh, vx, vy, in, corner);
        CHECK(w == sw && h == sh, "capture is the swapchain's size");
        CHECK((in & 0xff) > 150 && (in >> 8 & 0xff) > 150, "picture content inside the letterbox (%08x)", in);
        if (letter) CHECK((corner & 0xffffff) == 0, "black outside the picture (%08x)", corner);
        free(px);
    }

    // A texture through the texture path: a CPU-written RGBA8 texture with a gradient, presented with a crop
    vkw_request_size(1280, 720); Sleep(200);
    uint32_t tw = 256, th = 128;
    tex_t probe = vt_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, tw, th, 1, 1, 0, 0, NULL);
    uint8_t *mem = _aligned_malloc((size_t)probe.L.size + 4096, 4096); memset(mem, 0, (size_t)probe.L.size);
    tex_t t = vt_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, tw, th, 1, 1, 0, 0, mem);
    uint8_t *lin = malloc((size_t)tw * th * 4);
    for (uint32_t y = 0; y < th; y++) for (uint32_t x = 0; x < tw; x++) { uint8_t *q = lin + (y * tw + x) * 4; q[0] = (uint8_t)x; q[1] = (uint8_t)(y * 2); q[2] = 0x40; q[3] = 0xff; }
    NVNcopyRegion r = {0, 0, 0, (int)tw, (int)th, 1};
    CHECK(nvn_texio_write(&t, 0, 0, &r, lin, 0, 0) == 0, "texture storage written");
    vkt_tex_init(&t); vkt_tex_cpu_written(&t);
    vkt_image *im = vkt_resolve(&t);
    CHECK(im && im->content && im->cls == VKT_STORAGE, "CPU-written texture uploaded");
    uint32_t *px = NULL; int w = 0, h = 0, bgr = 0, got = -1;
    for (int i = 0; i < 20 && got; i++) { vkp_enqueue(t.serial, 0, 0, NULL, 0); got = vkp_capture_next(&px, &w, &h, &bgr, 300); }
    CHECK(!got, "capture of the presented texture");
    if (!got) {
        double sc = fmin(w / (double)tw, h / (double)th); int vw = (int)floor(tw * sc + 0.5), vh = (int)floor(th * sc + 0.5), vx = (w - vw) / 2, vy = (h - vh) / 2;
        uint32_t c = pix(px, w, vx + vw * 3 / 4, vy + vh / 2, bgr);   // texel (192, 64): r 192, g 128, b 64
        printf("presented texture at %dx%d: texel (192,64) shows %08x\n", w, h, c);
        CHECK(abs((int)(c & 0xff) - 192) <= 4 && abs((int)(c >> 8 & 0xff) - 128) <= 6 && abs((int)(c >> 16 & 0xff) - 0x40) <= 2, "the texture's colours reach the window (%08x)", c);
        free(px);
    }
    vkt_tex_release(&t);

    // Mailbox: the swapchain again with the other present mode
    vkp_shutdown();
    CHECK(vkp_init(VKP_MAILBOX) == 0, "swapchain (mailbox)");
    int mode; vkp_info(NULL, NULL, &mode, NULL);
    t0 = now_ns(); tk = 0;
    for (int i = 0; i < 240; i++) tk = vkp_enqueue(0, 0, 0, NULL, 0);
    vkp_wait(tk);
    printf("%s: 240 unpaced presents at %.0f per s\n", mode == VKP_MAILBOX ? "mailbox" : "FIFO (no mailbox on this surface)", 240 / ((now_ns() - t0) / 1e9));
    if (mode == VKP_MAILBOX) CHECK(240 / ((now_ns() - t0) / 1e9) > hz * 1.2, "mailbox runs ahead of the display");

    vkpc_shutdown();
    uint32_t verr = atomic_load(&g_vk.val_errors); int val = g_vk.validation;
    vt_down(1);
    _aligned_free(mem); free(lin);
    printf("validation: %s, %u errors\n", val ? "on" : "not installed", verr);
    CHECK(verr == 0, "no validation errors (%u)", verr);
    printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "ok", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
