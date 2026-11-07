// from nxnative dm/host/hle_os.c (MIT, © seedds): guest object side table, monotonic clock, guest clock and counter, entropy, getenv.
// Windows: now_ns is QueryPerformanceCounter; the counter keeps nxnative's 31.25 MHz (research/05); arc4random is BCryptGenRandom; host-to-guest
// calls go through GUEST_CALL (the guest's functions are recompiled code behind guest addresses, not host function pointers).
#include <stdatomic.h>
#include "hle.h"

// ---------- Side table ----------
#define OM_SIZE (1u << 18)
static struct { const void *k; void *v; } g_om[OM_SIZE];
static pthread_mutex_t g_om_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned om_h(const void *k) { uint64_t x = (uint64_t)k; x ^= x >> 29; x *= 0xbf58476d1ce4e5b9ull; return (unsigned)(x >> 40) & (OM_SIZE - 1); }
void *objmap_get(const void *k) {
    pthread_mutex_lock(&g_om_lock);
    void *r = NULL;
    for (unsigned i = om_h(k);; i = (i + 1) & (OM_SIZE - 1)) {
        if (g_om[i].k == k) { r = g_om[i].v; break; }
        if (!g_om[i].k) break;
    }
    pthread_mutex_unlock(&g_om_lock);
    return r;
}
void objmap_put(const void *k, void *v) {
    pthread_mutex_lock(&g_om_lock);
    for (unsigned i = om_h(k);; i = (i + 1) & (OM_SIZE - 1))
        if (g_om[i].k == k || !g_om[i].k || g_om[i].k == (void *)1) { g_om[i].k = k; g_om[i].v = v; break; }
    pthread_mutex_unlock(&g_om_lock);
}
void *objmap_del(const void *k) {
    pthread_mutex_lock(&g_om_lock);
    void *r = NULL;
    for (unsigned i = om_h(k);; i = (i + 1) & (OM_SIZE - 1)) {
        if (g_om[i].k == k) { r = g_om[i].v; g_om[i].k = (void *)1; g_om[i].v = NULL; break; }
        if (!g_om[i].k) break;
    }
    pthread_mutex_unlock(&g_om_lock);
    return r;
}
uint64_t now_ns(void) { return plat_now_ns(); }
// Guest clock: the system counters the guest can read (rt_mrs cntpct, svc GetSystemTick) all go through here. Default = now_ns().
// Freeze (only for the test driver): while frozen the guest clock does not advance; after unfreezing the frozen duration is subtracted, the guest clock is continuous with no jump
// DM_DETCLOCK=<step ns> (for replay / test runs; product runs do not set it = wall clock): from the first present on the guest clock advances a fixed step per present (D15's trace diffs).
// Before the first frame the wall clock is still used (the boot period has waits that poll by time, freezing would hang them); at the switch it continues from the guest clock value at that time, no jump.
static _Atomic uint64_t g_gfrozen, g_glag, g_gdet, g_guest_frame; static _Atomic int g_gfz, g_gdet_on; static uint64_t g_gdet_step = 1;
uint64_t dm_guest_frame(void) { return atomic_load(&g_guest_frame); }
static int detclock_cfg(void) {
    static int c = -1; if (c < 0) { const char *e = getenv("DM_DETCLOCK"); g_gdet_step = e ? strtoull(e, 0, 0) : 0; c = g_gdet_step > 0; }
    return c;
}
static uint64_t wall_guest_ns(void) { return atomic_load(&g_gfz) ? atomic_load(&g_gfrozen) : now_ns() - atomic_load(&g_glag); }
uint64_t guest_ns(void) { return atomic_load(&g_gdet_on) ? atomic_load(&g_gdet) : wall_guest_ns(); }
// SWITCH2: the Switch 2 system counter runs at 31.25 MHz (research/05: the SDK's GetSystemTickFrequency is the constant 31,250,000), the Tegra X1's at 19.2 MHz.
// Every guest-visible counter (rt_mrs cntpct / cntvct, svc GetSystemTick) comes from here. DM_TICK_HZ overrides (19200000 = the Tegra X1 counter).
uint64_t guest_ticks(void) {
    static uint64_t hz;
    if (!hz) { const char *e = getenv("DM_TICK_HZ"); uint64_t v = e ? strtoull(e, 0, 0) : 31250000ull; if (!v) v = 31250000ull; hz = v;
               LOG("guest system counter %llu Hz (%s)", (unsigned long long)hz, e ? "DM_TICK_HZ" : "Switch 2 default"); }
    uint64_t ns = guest_ns();
    return ns / 1000000000u * hz + ns % 1000000000u * hz / 1000000000u;   // = ns × hz / 10⁹ without a 128-bit division
}
int dm_detclock_enabled(void) { return detclock_cfg() && atomic_load(&g_gdet_on); }
void dm_guest_frame_tick(void) {
    atomic_fetch_add(&g_guest_frame, 1);
    if (!detclock_cfg()) return;
    if (!atomic_load(&g_gdet_on)) {
        atomic_store(&g_gdet, wall_guest_ns()); atomic_store(&g_gdet_on, 1);
        LOG("DM_DETCLOCK: from the first present on, the guest clock advances by presents, step %llu ns", (unsigned long long)g_gdet_step); return;
    }
    if (!atomic_load(&g_gfz)) { atomic_fetch_add(&g_gdet, g_gdet_step); dm_guest_clock_advanced(); }
}
static void guest_clock_freeze_apply(int on) {
    if (on && !atomic_load(&g_gfz)) { atomic_store(&g_gfrozen, now_ns() - atomic_load(&g_glag)); atomic_store(&g_gfz, 1); }
    else if (!on && atomic_load(&g_gfz)) { atomic_store(&g_glag, now_ns() - atomic_load(&g_gfrozen)); atomic_store(&g_gfz, 0); }
}
// Freezing has several unrelated reasons (0 freeze tick for testing / loading screen, 1 App in background, 2 system keyboard up): freeze if any holds, release only when all are lifted
static pthread_mutex_t g_gfz_mu = PTHREAD_MUTEX_INITIALIZER; static unsigned g_gfz_reasons;
// A suspended application does not run: Horizon suspends the whole process out of focus. Guest reads of the system counter wait while the app is in the background
static pthread_cond_t g_gfz_cv = PTHREAD_COND_INITIALIZER; static _Atomic int g_gfz_bg;
void guest_clock_read_wait(void) {
    if (!atomic_load(&g_gfz_bg) || atomic_load(&dm_host_work)) return;
    pthread_mutex_lock(&g_gfz_mu);
    while (g_gfz_reasons & 2u) pthread_cond_wait(&g_gfz_cv, &g_gfz_mu);
    pthread_mutex_unlock(&g_gfz_mu);
}
void guest_clock_freeze_for(int reason, int on) {
    pthread_mutex_lock(&g_gfz_mu);
    if (on) g_gfz_reasons |= 1u << reason; else g_gfz_reasons &= ~(1u << reason);
    guest_clock_freeze_apply(g_gfz_reasons != 0);
    atomic_store(&g_gfz_bg, (g_gfz_reasons & 2u) != 0);
    if (!(g_gfz_reasons & 2u)) pthread_cond_broadcast(&g_gfz_cv);   // after the clock is running again: a released read sees it continue
    pthread_mutex_unlock(&g_gfz_mu);
}
void guest_clock_freeze(int on) { guest_clock_freeze_for(0, on); }
// Entropy source: true random by default; with DM_DETCLOCK on it becomes a fixed-seed splitmix64 stream (replay pictures byte-for-byte comparable).
// The two outlets (libc getentropy, svc GetInfo RandomEntropy) share one stream; the call count is reported in the log (first call and every 1000 calls)
static _Atomic uint64_t g_detrng = 0x5357444554524e47ull; static _Atomic uint64_t g_detrng_n;
uint64_t dm_entropy64(void) {
    if (!detclock_cfg()) { uint64_t v; plat_random(&v, sizeof v); return v; }
    uint64_t z = atomic_fetch_add(&g_detrng, 0x9E3779B97F4A7C15ull) + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; z ^= z >> 31;
    uint64_t n = atomic_fetch_add(&g_detrng_n, 1) + 1; if (n == 1 || n % 1000 == 0) LOG("DM_DETCLOCK: deterministic entropy call %llu", (unsigned long long)n);
    return z;
}
HLE("getentropy", int, g_getentropy, void *p, size_t n) {
    if (!detclock_cfg()) { plat_random(p, n); return 0; }
    for (size_t i = 0; i < n; i += 8) { uint64_t v = dm_entropy64(); memcpy((uint8_t *)p + i, &v, n - i < 8 ? n - i : 8); }
    return 0;
}
// The SDK's own getentropy fills through nn::crypto::GenerateCryptographicallyRandomBytes, which opens the csrng service.
// The call goes through the sdk's PLT, so the takeover gives it the same stream as getentropy above
HLE_FORCE("_ZN2nn6crypto36GenerateCryptographicallyRandomBytesEPvm", "csrng service; host entropy (the getentropy stream)", void, g_csrng_bytes, void *p, size_t n) {
    g_getentropy(p, n);
}

// ---------- getenv: the game profile's game_env ----------
// Horizon gives a program no environment, and the SDK's C library starts with an empty environ. The profile's game_env (KEY=VALUE, separated by spaces;
// DM_GAME_ENV overrides it) answers getenv for those names; any other name goes to the SDK's own getenv
static const char **g_env_kv;   // "KEY=VALUE" strings, NULL-terminated
static void game_env_init(void) {
    const char *e = getenv("DM_GAME_ENV"); if (!e) e = DM_GAME_ENV;
    int n = 0; for (const char *p = e; *p; p++) n += *p == '=';
    if (!(g_env_kv = calloc((size_t)n + 1, sizeof *g_env_kv))) die("game_env allocation failed");
    char *s = strdup(e), *save = NULL; int k = 0;
    for (char *t = strtok_r(s, " ", &save); t && k < n; t = strtok_r(NULL, " ", &save)) if (strchr(t, '=')) g_env_kv[k++] = t;
    if (k) LOG("game environment (%s): %s", getenv("DM_GAME_ENV") ? "DM_GAME_ENV" : "game profile game_env", e);
}
static const char *const *game_env(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, game_env_init); return g_env_kv; }
HLE_FORCE("getenv", "the game profile's game_env answers its names; the SDK's own getenv (an empty environ on Horizon) the rest", char *, g_getenv, const char *name) {
    static void *orig;
    size_t n = name ? strlen(name) : 0;
    for (const char *const *p = game_env(); n && *p; p++) if (!strncmp(*p, name, n) && (*p)[n] == '=') return (char *)*p + n + 1;
    if (!orig && !(orig = guest_sym("getenv"))) return NULL;
    return (char *)(uintptr_t)GUEST_CALL(orig, (uint64_t)(uintptr_t)name);
}

// ---------- nn::mem::StandardAllocator::Allocate(size): large blocks aligned to 64 KB (profile key alloc_big_align) ----------
// Vanillaware's engine cuts ring heaps out of a malloc'd memory pool at 64 KB alignment relative to its GPU address; on the Switch that address is big-page
// aligned. With the profile key, allocations of 64 KB or more are aligned to 64 KB (Allocate(size, 0x10000)). Other games (Pokopia) forward unchanged
static void *g_sa_alloc, *g_sa_alloc_al;
static int g_sa_big_align;
static pthread_once_t g_sa_once = PTHREAD_ONCE_INIT;
static void sa_init(void) {
    module_t *ms = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "sdk")) ms = &g_mod[i];
    g_sa_alloc = ms ? module_sym(ms, "_ZN2nn3mem17StandardAllocator8AllocateEm") : NULL;
    g_sa_alloc_al = ms ? module_sym(ms, "_ZN2nn3mem17StandardAllocator8AllocateEmm") : NULL;
    if (!g_sa_alloc || !g_sa_alloc_al) die("StandardAllocator::Allocate not found in the SDK");
    g_sa_big_align = DM_GAME_ALLOC_BIG_ALIGN;
    if (g_sa_big_align) LOG("StandardAllocator::Allocate: allocations ≥ 64 KB are 64 KB aligned (game profile alloc_big_align)");
}
HLE_FORCE("_ZN2nn3mem17StandardAllocator8AllocateEm", "passed straight to the SDK; with the profile's alloc_big_align (13 Sentinels, Unicorn Overlord) allocations ≥ 64 KB are 64 KB aligned (memory pool start)", void *, h_mem_SAAllocate, void *self, size_t size) {
    pthread_once(&g_sa_once, sa_init);
    return (void *)(uintptr_t)(g_sa_big_align && size >= 0x10000 ? GUEST_CALL(g_sa_alloc_al, (uint64_t)(uintptr_t)self, size, 0x10000)
                                                                 : GUEST_CALL(g_sa_alloc, (uint64_t)(uintptr_t)self, size));
}

#include "hle_os.adapters.inc"
