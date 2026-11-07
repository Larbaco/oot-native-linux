// Shared by the Vulkan backend's tests: checks, a GPU bring-up that skips (77) without one, a fake guest texture
#pragma once
#include "vk_internal.h"
#include "nxrt/runtime.h"
#include "vk_window.h"
#include "vk_submit.h"
#include "vk_present.h"
#include "vk_mem.h"
#include "vk_tex.h"
#include "vk_fmt.h"
#include "vk_astc.h"
#include "nvn_dirty.h"
#include "nvn_texio.h"
#include "s2tex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

void crash_install(void);   // host/crash.c: the vectored exception handler the fault hooks hang off
int vkpc_init(void); void vkpc_shutdown(void); int vkpc_save_now(void); void vkpc_note_created(int n);
void vkpc_stats(uint64_t *created, uint64_t *saves, uint64_t *bytes, uint64_t *loaded); const wchar_t *vkpc_path(void);

static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } } while (0)

// The device (and window when asked); returns 0, or 77 after printing why there is no GPU to test on
extern uint64_t g_t0_ns;   // host/main.c: the log's time origin
static inline int vt_up(int window, int validation) {
    g_t0_ns = now_ns();
    uint32_t nx = 0; const char *const *ex = NULL;
    if (window) {
        if (vkw_open("nxrt Vulkan test", 1280, 720, 0)) { printf("no window (no display / SDL3.dll): skipped\n"); return 77; }
        ex = vkw_instance_exts(&nx);
    }
    vk_dev_opts o = {validation, !window, getenv("NXRT_VK_DEVICE")};
    if (vk_dev_open(&o, ex, nx)) { printf("no Vulkan 1.3 device: skipped\n"); return 77; }
    vkf_init();
    if (vks_init() || vkm_init() || vkt_init()) { printf("backend start failed\n"); return 1; }
    return 0;
}
static inline void vt_down(int window) {
    if (window) vkp_shutdown();
    vks_finish(); vkt_shutdown(); vkm_shutdown(); vks_shutdown(); vk_dev_close();
    if (window) vkw_close();
}
static uint64_t vt_rng = 0x9E3779B97F4A7C15ull;
static inline uint32_t vt_rand(void) { vt_rng ^= vt_rng << 13; vt_rng ^= vt_rng >> 7; vt_rng ^= vt_rng << 17; return (uint32_t)(vt_rng >> 11); }
static inline double vt_ms(uint64_t ns) { return ns / 1e6; }

// A guest-like texture over host memory: storage laid out by nvn_tex.c (what the recording layer computes), the pool CPU-writable
static pool_t vt_pool = {0x4c4f4f50, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED, NULL, 0, NULL, 0};
static uint64_t vt_serial = 1000;
static inline tex_t vt_tex(int target, int format, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, int flags, int pkg_bh, uint8_t *mem) {
    tex_t t; memset(&t, 0, sizeof t);
    t.magic = 0x00584554; t.d.target = target; t.d.format = format; t.d.flags = flags; t.d.width = w; t.d.height = h; t.d.depth = d; t.d.levels = levels;
    t.d.pkg_block_h = pkg_bh; t.has_layout = pkg_bh != 0;
    nvn_tex_layout(&t.d, &t.L);
    t.mem = mem; t.pool = &vt_pool; t.off = (int64_t)(uintptr_t)mem; t.serial = ++vt_serial;
    return t;
}
