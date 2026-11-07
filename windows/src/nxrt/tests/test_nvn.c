/* The null NVN device through its generated adapters, as the game reaches it (AAPCS64 arguments in a guest cpu, objects in game memory), without game
   data: the bootstrap hands out tokens; device, queue, memory pool, texture, buffer, command buffer, sync and window initialise; the getters return
   what the builders set; storage sizes follow nxnative's block-linear layout; a recording's handle and accounting; the window hands out its textures
   in turn, and the first acquire and present are logged as the first frame request. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nxrt/runtime.h"
#include "hle_adapters.h"
#include "hle_registry.h"

NXRT_EARLY_RESERVE()

static int g_fail;
#define T(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static NxwGuestCpu g_c;
#define P(p) ((uint64_t)(uintptr_t)(p))
/* call an adapter with integer/pointer arguments; returns x0 */
#define CALL(fn, ...) call_(adapter_##fn, (uint64_t[]){0, ##__VA_ARGS__} + 1, (int)(sizeof((uint64_t[]){0, ##__VA_ARGS__}) / 8) - 1)
static uint64_t call_(void (*fn)(NxwGuestCpu *), const uint64_t *a, int n) {
    memset(&g_c, 0xcd, sizeof g_c);
    for (int i = 0; i < n; i++) g_c.x[i] = a[i];
    fn(&g_c);
    return g_c.x[0];
}
static void *obj(void) { void *p = calloc(1, 0x1000); return p; }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (nxrt_init(&(nxrt_options){.quiet = 1})) { printf("FAIL nxrt_init\n"); return 1; }
    /* names to tokens, unknown names to NULL */
    T(CALL(nvnBootstrapLoader, P("nvnDeviceInitialize")) == nxrt_token_of("nvnDeviceInitialize") && g_c.x[0] != 0);
    T(CALL(nvnBootstrapLoader, P("nvnNoSuchFunction")) == 0);
    T(CALL(nvnDeviceGetProcAddress, 0, P("nvnQueuePresentTexture")) == nxrt_token_of("nvnQueuePresentTexture"));
    const nxrt_hle_entry *e = nxrt_hle_find("nvnQueuePresentTexture", NXRT_HLE_NVN);
    T(e && e->ported);

    void *db = obj(), *dev = obj();
    CALL(nvnDeviceBuilderSetDefaults, P(db));
    T((CALL(nvnDeviceInitialize, P(dev), P(db)) & 0xff) == 1);
    int v = -1;
    CALL(nvnDeviceGetInteger, P(dev), 0, P(&v)); T(v == 55);            /* API major */
    CALL(nvnDeviceGetInteger, P(dev), 0x15, P(&v)); T(v == 512);        /* a measured Tegra X1 constant */

    void *qb = obj(), *q = obj();
    CALL(nvnQueueBuilderSetDevice, P(qb), P(dev)); CALL(nvnQueueBuilderSetDefaults, P(qb));
    T(CALL(nvnQueueBuilderGetQueueMemorySize, P(qb)) == ((65536 + 262144 + 16384 + 4095) & ~4095ull));
    T((CALL(nvnQueueInitialize, P(q), P(qb)) & 0xff) == 1);

    /* a memory pool on game storage: Map and the GPU address are the storage */
    uint8_t *store = _aligned_malloc(4 << 20, 4096); memset(store, 0, 4 << 20);
    void *pb = obj(), *pool = obj();
    CALL(nvnMemoryPoolBuilderSetDefaults, P(pb)); CALL(nvnMemoryPoolBuilderSetDevice, P(pb), P(dev));
    CALL(nvnMemoryPoolBuilderSetStorage, P(pb), P(store), 4 << 20); CALL(nvnMemoryPoolBuilderSetFlags, P(pb), 0x8 | 0x2);
    T((CALL(nvnMemoryPoolInitialize, P(pool), P(pb)) & 0xff) == 1);
    T(CALL(nvnMemoryPoolMap, P(pool)) == P(store) && CALL(nvnMemoryPoolGetBufferAddress, P(pool)) == P(store) && CALL(nvnMemoryPoolGetSize, P(pool)) == 4 << 20);

    /* a 256x256 RGBA8 texture: block-linear size, power-of-two alignment, getters */
    void *tb = obj(), *tex = obj(), *tex2 = obj();
    CALL(nvnTextureBuilderSetDevice, P(tb), P(dev)); CALL(nvnTextureBuilderSetDefaults, P(tb));
    CALL(nvnTextureBuilderSetSize2D, P(tb), 256, 256); CALL(nvnTextureBuilderSetFormat, P(tb), 0x25);   /* NVN_FORMAT_RGBA8 */
    uint64_t size = CALL(nvnTextureBuilderGetStorageSize, P(tb)), align = CALL(nvnTextureBuilderGetStorageAlignment, P(tb));
    printf("256x256 RGBA8: %#llx bytes, alignment %#llx\n", (unsigned long long)size, (unsigned long long)align);
    T(size >= 256 * 256 * 4 && size < 2 * 256 * 256 * 4 && align && !(align & (align - 1)));
    CALL(nvnTextureBuilderSetStorage, P(tb), P(pool), 0);
    T((CALL(nvnTextureInitialize, P(tex), P(tb)) & 0xff) == 1);
    T((CALL(nvnTextureInitialize, P(tex2), P(tb)) & 0xff) == 1);
    T((int)CALL(nvnTextureGetWidth, P(tex)) == 256 && (int)CALL(nvnTextureGetHeight, P(tex)) == 256 && (int)CALL(nvnTextureGetFormat, P(tex)) == 0x25);
    T(CALL(nvnTextureGetTextureAddress, P(tex)) == P(store));

    /* a buffer in the pool */
    void *bb = obj(), *buf = obj();
    CALL(nvnBufferBuilderSetDevice, P(bb), P(dev)); CALL(nvnBufferBuilderSetDefaults, P(bb)); CALL(nvnBufferBuilderSetStorage, P(bb), P(pool), 0x100000, 0x1000);
    T((CALL(nvnBufferInitialize, P(buf), P(bb)) & 0xff) == 1);
    T(CALL(nvnBufferMap, P(buf)) == P(store + 0x100000) && CALL(nvnBufferGetAddress, P(buf)) == P(store + 0x100000) && CALL(nvnBufferGetSize, P(buf)) == 0x1000);

    /* a command buffer: a recording is nxnative's records in the game's command memory, its first record the handle (M5: a draw of 32 bytes, then the 8-byte END) */
    void *cb = obj();
    T((CALL(nvnCommandBufferInitialize, P(cb), P(dev)) & 0xff) == 1);
    CALL(nvnCommandBufferAddCommandMemory, P(cb), P(pool), 0x200000, 0x10000);
    CALL(nvnCommandBufferBeginRecording, P(cb));
    CALL(nvnCommandBufferDrawArrays, P(cb), 4, 0, 3);
    T(CALL(nvnCommandBufferEndRecording, P(cb)) == P(store + 0x200000));
    T(CALL(nvnCommandBufferGetCommandMemoryUsed, P(cb)) == 40 && CALL(nvnCommandBufferGetCommandMemoryFree, P(cb)) == 0x10000 - 40);

    /* sync: signalled once fenced */
    void *sync = obj();
    T((CALL(nvnSyncInitialize, P(sync), P(dev)) & 0xff) == 1);
    T((uint32_t)CALL(nvnSyncWait, P(sync), 0) == 2);   /* TIMEOUT_EXPIRED before any fence */
    CALL(nvnQueueFenceSync, P(q), P(sync), 0, 0);
    T((uint32_t)CALL(nvnSyncWait, P(sync), ~0ull) == 0);   /* ALREADY_SIGNALED */

    /* the window: its textures in turn, then the first frame request */
    void *wb = obj(), *win = obj(); void *texs[2] = {tex, tex2}; int idx = -1;
    CALL(nvnWindowBuilderSetDevice, P(wb), P(dev)); CALL(nvnWindowBuilderSetDefaults, P(wb)); CALL(nvnWindowBuilderSetTextures, P(wb), 2, P(texs));
    T((CALL(nvnWindowInitialize, P(win), P(wb)) & 0xff) == 1);
    T(CALL(nvnWindowGetPresentInterval, P(win)) == 1);
    T(CALL(nvnWindowAcquireTexture, P(win), P(sync), P(&idx)) == 0 && idx == 0);
    CALL(nvnQueuePresentTexture, P(q), P(win), idx);
    T(CALL(nvnWindowAcquireTexture, P(win), P(sync), P(&idx)) == 0 && idx == 1);
    CALL(nvnQueuePresentTexture, P(q), P(win), idx);
    T(CALL(nvnWindowAcquireTexture, P(win), P(sync), P(&idx)) == 0 && idx == 0);
    FILE *f = fopen(nxrt_log_path(), "r"); char line[512]; int acq = 0, pres = 0;
    while (f && fgets(line, sizeof line, f)) { acq += strstr(line, "first frame request: WindowAcquireTexture") != NULL; pres += strstr(line, "first frame request: QueuePresentTexture") != NULL; }
    if (f) fclose(f);
    T(acq == 1 && pres == 1);
    CALL(nvnQueuePresentTexture, P(q), P(win), idx);

    /* M4, the frame loop. Recorded commands whose effects the CPU sees take effect when the recording is submitted (a null GPU finishes at once): a fence
       signals its sync, an event signal writes its value, a counter report writes {counter, timestamp} (a timestamp report: the time in both, nxnative's rule) */
    void *sync2 = obj(), *eb = obj(), *ev = obj();
    T((CALL(nvnSyncInitialize, P(sync2), P(dev)) & 0xff) == 1);
    CALL(nvnEventBuilderSetDefaults, P(eb)); CALL(nvnEventBuilderSetStorage, P(eb), P(pool), 0x310000);
    T((CALL(nvnEventInitialize, P(ev), P(eb)) & 0xff) == 1);
    uint64_t *counter = (uint64_t *)(store + 0x300000); memset(counter, 0xab, 16);
    CALL(nvnCommandBufferBeginRecording, P(cb));
    CALL(nvnCommandBufferFenceSync, P(cb), P(sync2), 0, 0);
    CALL(nvnCommandBufferSignalEvent, P(cb), P(ev), 0, 0, 0, 7);
    CALL(nvnCommandBufferReportCounter, P(cb), 0, P(counter));
    uint64_t handle = CALL(nvnCommandBufferEndRecording, P(cb));
    T((uint32_t)CALL(nvnSyncWait, P(sync2), 0) == 2 && (uint32_t)CALL(nvnEventGetValue, P(ev)) == 0);   /* recorded, not submitted yet */
    CALL(nvnQueueSubmitCommands, P(q), 1, P(&handle));
    T((uint32_t)CALL(nvnSyncWait, P(sync2), 0) == 0);
    T((uint32_t)CALL(nvnEventGetValue, P(ev)) == 7);
    T(counter[0] == counter[1] && counter[1] != 0xababababababababull && counter[1] > 0);
    /* texture views: getters return what the setters set, and a view's offset is its level's */
    uint8_t view[16]; int a = -1, bn = -1;
    CALL(nvnTextureViewSetDefaults, P(view)); CALL(nvnTextureViewSetLevels, P(view), 1, 2);
    T((CALL(nvnTextureViewGetLevels, P(view), P(&a), P(&bn)) & 0xff) == 1 && a == 1 && bn == 2);
    T((CALL(nvnTextureViewGetLayers, P(view), P(&a), P(&bn)) & 0xff) == 0);
    T(CALL(nvnTextureGetViewOffset, P(tex), P(view)) > 0);
    /* texels come from the storage (M5: nvn_texio through s2tex); nothing has written this texture's */
    uint8_t px[64]; memset(px, 0xcd, sizeof px); int region[6] = {0, 0, 0, 4, 4, 1};
    CALL(nvnTextureReadTexels, P(tex), 0, P(region), P(px));
    int zero = 1; for (int i = 0; i < 64; i++) zero &= px[i] == 0;
    T(zero);
    /* 120 frames as fast as the CPU goes: the swap chain holds the producer to the 60 Hz display, the textures come in turn, every present ticks the guest frame */
    uint64_t dm_guest_frame(void), now_ns(void);
    uint64_t f0 = dm_guest_frame(), t0 = now_ns();
    int order_ok = 1, last = idx;
    for (int k = 0; k < 120; k++) {
        T(CALL(nvnWindowAcquireTexture, P(win), P(sync), P(&idx)) == 0);
        order_ok &= idx == (last + 1) % 2; last = idx;
        T((uint32_t)CALL(nvnSyncWait, P(sync), ~0ull) == 0);
        CALL(nvnQueuePresentTexture, P(q), P(win), idx);
    }
    double secs = (now_ns() - t0) / 1e9;
    printf("120 frames through a 2-texture swap chain in %.3f s (%.2f frames/s)\n", secs, 120 / secs);
    T(order_ok);
    T(secs > 1.9 && secs < 2.2);
    T(dm_guest_frame() - f0 == 120);
    printf(g_fail ? "nxrt.nvn: FAILED\n" : "nxrt.nvn: ok\n");
    return g_fail;
}
