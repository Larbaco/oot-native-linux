/* The run's log (research/01 lesson 15, DESIGN D14): one file per run, flushed every line, with a banner that triages itself:
   the GPU (none until the Vulkan renderer), the CPU and its x86-64-v3 support, the Windows build, the game's build ID and the reserved ranges. */
#include "dm.h"
#include "nxrt/runtime.h"
#include "movie_real.h"

#ifdef _WIN32
#include <direct.h>
#endif
#include <time.h>
#include <stdarg.h>
#include <sys/stat.h>

static FILE *g_log;
static char g_log_path[MAX_PATH];
static int g_quiet;
static plat_rwlock_t g_log_lock = PLAT_RWLOCK_INIT;

const char *nxrt_log_path(void) { return g_log_path; }

static void log_line(const char *prefix, const char *fmt, va_list ap) {
    char buf[4096];
    int n = snprintf(buf, sizeof buf, "[%9.3f] %s", (now_ns() - g_t0_ns) / 1e9, prefix);
    if (n < 0) n = 0;
    int m = vsnprintf(buf + n, sizeof buf - (size_t)n, fmt, ap);
    size_t len = (size_t)n + (m < 0 ? 0 : (size_t)m);
    if (len > sizeof buf - 2) len = sizeof buf - 2;
    buf[len++] = '\n'; buf[len] = 0;
    plat_rw_wrlock(&g_log_lock);
    if (g_log) { fwrite(buf, 1, len, g_log); fflush(g_log); }
    if (!g_quiet || !g_log) { fwrite(buf, 1, len, stderr); fflush(stderr); }
    plat_rw_wrunlock(&g_log_lock);
}
void nxrt_logf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); log_line("", fmt, ap); va_end(ap); }

void crash_report(const char *why, const void *ctx);   /* crash.c */
_Noreturn void nxrt_die(const char *fmt, ...) {
    char msg[1024]; va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    nxrt_logf("FATAL %s", msg);
    crash_report(msg, NULL);
    plat_exit(3);
}

/* The folder: o->log_dir, NXRT_LOG_DIR, .\_local\nxrt when it exists, else the current folder */
int nxrt_log_open(const nxrt_options *o) {
    if (g_log) return 0;
    if (!g_t0_ns) g_t0_ns = now_ns();
    g_quiet = o && o->quiet;
    const char *dir = o && o->log_dir ? o->log_dir : getenv("NXRT_LOG_DIR");
    if (!dir) dir = plat_is_dir("_local" PLAT_SEP "nxrt") ? "_local" PLAT_SEP "nxrt" : ".";
    plat_mkdir(dir);
    time_t now = time(NULL); struct tm t; localtime_r(&now, &t);
    snprintf(g_log_path, sizeof g_log_path, "%s" PLAT_SEP "nxrt-%04d%02d%02d-%02d%02d%02d-%u.log", dir, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, plat_thread_id());
    g_log = fopen(g_log_path, "w");
    if (!g_log) { fprintf(stderr, "[nxrt] cannot write the log %s\n", g_log_path); return -1; }
    return 0;
}

void nxrt_log_banner(void) {
    char cpu[64], win[128];
    plat_cpu_brand(cpu, sizeof cpu); plat_os_version(win, sizeof win);
    LOG("nxrt v0 (runtime ABI %d, built %s %s), log %s", NXW_ABI_VERSION, __DATE__, __TIME__, g_log_path);
    LOG("GPU: the graphics backend's choice (--gfx vulkan: the [vk] lines below name the GPU and driver; --gfx null: none)");
    LOG("movie AV1 decoder: %s", mvr_build_available() ? "built-in FFmpeg/libdav1d available (default; NXRT_MOVIE_DECODE=0 disables)" : "not built; timed no-image stand-in will be used");
    LOG("CPU: %s, %u logical processors, %.1f GB RAM, x86-64-v3 (AVX2+FMA3) %s", cpu, plat_cpu_count(), plat_ram_bytes() / 1073741824.0,
        plat_cpu_x86_64_v3() ? "yes" : "NO: below the minimum CPU (DESIGN §9.1)");
    LOG("OS: %s", win);
    LOG("game profile %s (%s), app pool %.2f GB", DM_GAME_ID, DM_GAME_NAME, DM_GAME_APP_POOL / 1073741824.0);
    for (int i = 0; i < NXRT_AS_COUNT; i++) {
        const nxrt_range *r = nxrt_as_range(i);
        if (r->reserved) LOG("reserved %#llx-%#llx %s (%.2f GB)", (unsigned long long)r->base, (unsigned long long)(r->base + r->size), r->name, r->size / 1073741824.0);
        else LOG("[RED] NOT reserved %#llx-%#llx %s: %s %#llx+%#llx (state %#x type %#x)%s%s", (unsigned long long)r->base, (unsigned long long)(r->base + r->size), r->name,
                 r->conflict_size ? "taken by" : "VirtualAlloc2 unavailable or refused;", (unsigned long long)r->conflict_base, (unsigned long long)r->conflict_size,
                 r->conflict_state, r->conflict_type, *r->conflict_owner ? " " : "", r->conflict_owner);
    }
    const nxrt_layout *l = nxrt_as_layout();
    LOG("guest block: alias %#llx+%#llx, stack %#llx+%#llx, heap %#llx+%#llx", (unsigned long long)l->alias, (unsigned long long)l->alias_size,
        (unsigned long long)l->stack, (unsigned long long)l->stack_size, (unsigned long long)l->heap, (unsigned long long)l->heap_size);
#ifdef _WIN32
    uint8_t *img = (uint8_t *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(img + ((IMAGE_DOS_HEADER *)img)->e_lfanew);
    LOG("executable at %p, high-entropy ASLR %s", (void *)img, nt->OptionalHeader.DllCharacteristics & IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA ? "ON (link with /HIGHENTROPYVA:NO)" : "off");
#endif
}
