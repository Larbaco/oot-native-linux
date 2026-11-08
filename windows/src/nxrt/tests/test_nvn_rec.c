/* M5 track A: nxnative's NVN recording layer against the null backend, through the generated adapters as the game reaches it (AAPCS64 arguments, objects and
   command memory in the guest heap, on a guest thread so the command buffer's memory callback can be a guest function):
   - the backend is chosen at run time (null; an unknown name is refused);
   - a recording holds nxnative's records in nxnative's format and order, its handle is its first record, and GetCommandMemoryUsed counts what it took;
   - a submit replays it on the null backend: buffer copies, uniform updates, fills, ReportValue and event writes land in memory, counters get the submit's time,
     a recorded fence signals its sync with the submission and not before; draws and dispatches are counted;
   - a segment that runs out calls the game's memory callback (a guest call) and is linked to the new memory by a JUMP; with no callback the host lends a segment;
   - CALL / COPY of another recording, a malformed list (refused, logged), resource registration, texture and program lifetimes reach the backend;
   - a program's Switch 2 key is sha256(control block with +0x20..+0x27 zeroed, code)[:8];
   - texels written into a region read back, and land where the whole-surface s2tex layout puts them (uncompressed and BC1);
   - the run-time shader path (nxs_rt): a program stage the cache lacks is dumped in DM_NVN_SHDUMP's format and logged; once the cache holds it (made by the
     current chain) the next lookup finds it, and an entry of another chain is stale;
   - the Win32 dirty-page tracker: a write to a protected page faults once, is stamped and unprotected. */
#include "guest_test.h"
#include "hle_adapters.h"
#include "hle_registry.h"
#include "../gfx/nvn_obj.h"
#include "../gfx/gfx_null.h"
#include "../gfx/nvn_texio.h"
#include "../gfx/nvn_dirty.h"
#include "../gfx/nxs_rt.h"
#include <bcrypt.h>

NXRT_EARLY_RESERVE()
uint64_t dm_guest_frame(void);                       /* hle_thread.c */
void dm_guest_write_prepare(void *addr, size_t len);   /* crash.c: the hook the dirty tracker installs */

#define P(p) ((uint64_t)(uintptr_t)(p))
static NxwGuestCpu g_a;   /* the cpu adapters are called with (a scratch copy: the guest thread's own cpu runs the callbacks) */
#define CALL(fn, ...) call_(adapter_##fn, (uint64_t[]){0, ##__VA_ARGS__} + 1, (int)(sizeof((uint64_t[]){0, ##__VA_ARGS__}) / 8) - 1)
static uint64_t call_(void (*fn)(NxwGuestCpu *), const uint64_t *a, int n) {
    NxwGuestCpu *c = nxrt_cur_cpu(); g_a = *c;
    for (int i = 0; i < n; i++) g_a.x[i] = a[i];
    fn(&g_a);
    return g_a.x[0];
}
static void callf(void (*fn)(NxwGuestCpu *), uint64_t x0, float f0, float f1) {   /* one pointer and two floats (sampler LOD clamp) */
    NxwGuestCpu *c = nxrt_cur_cpu(); g_a = *c; g_a.x[0] = x0;
    memset(g_a.q[0], 0, 16); memcpy(g_a.q[0], &f0, 4); memset(g_a.q[1], 0, 16); memcpy(g_a.q[1], &f1, 4);
    fn(&g_a);
}

/* the command buffer's memory callback, as the game's: hands out the next chunk of its command pool */
static void *g_cb_pool; static uint64_t g_cb_next, g_cb_chunk = 256; static int g_cb_calls; static uint64_t g_cb_min;
static void mem_callback(NxwGuestCpu *c) {
    uint64_t cmd = c->x[0], ev = c->x[1], min = c->x[2];
    g_cb_calls++; g_cb_min = min;
    T(ev == NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY);
    NxwGuestCpu t = *c; t.x[0] = cmd; t.x[1] = P(g_cb_pool); t.x[2] = g_cb_next; t.x[3] = g_cb_chunk;
    adapter_nvnCommandBufferAddCommandMemory(&t);
    g_cb_next += g_cb_chunk;
}

static void sha256_2(const void *a, size_t na, const void *b, size_t nb, uint8_t out[32]) {
    BCRYPT_ALG_HANDLE alg; BCRYPT_HASH_HANDLE h;
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0); BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0);
    BCryptHashData(h, (PUCHAR)a, (ULONG)na, 0); BCryptHashData(h, (PUCHAR)b, (ULONG)nb, 0); BCryptFinishHash(h, out, 32, 0);
    BCryptDestroyHash(h); BCryptCloseAlgorithmProvider(alg, 0);
}
/* the ops of one recording, following JUMPs (CALLs are not entered) */
static int ops_of(const uint8_t *p, uint32_t *ops, int max) {
    int n = 0;
    for (;;) {
        op_hdr h; memcpy(&h, p, 8);
        if (h.op == OP_JUMP) { memcpy(&p, p + 8, 8); continue; }
        if (n < max) ops[n] = h.op;
        n++;
        if (h.op == OP_END || h.size < 8 || n > 100000) return n;
        p += h.size;
    }
}

int main(void) {
    /* a scratch shader cache for nxs_rt (read at its first use, when the program below is set) */
    char cache[MAX_PATH], tmp[MAX_PATH], p[MAX_PATH + 64]; GetTempPathA(sizeof tmp, tmp);
    snprintf(cache, sizeof cache, "%snxrt_nvn_rec_%lu", tmp, (unsigned long)GetCurrentProcessId());
    CreateDirectoryA(cache, NULL); snprintf(p, sizeof p, "%s/vk", cache); CreateDirectoryA(p, NULL);
    _putenv_s("NXRT_SHADER_CACHE", cache); _putenv_s("NXRT_SHADER_TOOL", "off");
    gt_init();
    /* the backend */
    T(nxrt_gfx_select("vulkan-nonexistent") == -1);
    T(nxrt_gfx_select("null") == 0 && !strcmp(nxrt_gfx_name(), "null"));
    T(nxrt_gfx_select("nosuch") == -1 && !strcmp(nxrt_gfx_name(), "null"));
    gfx_null_counts n0, n1; gfx_null_stats(&n0);

    void *db = gt_alloc(0x100), *dev = gt_alloc(0x1000), *qb = gt_alloc(0x100), *q = gt_alloc(0x2000);
    CALL(nvnDeviceBuilderSetDefaults, P(db)); T((CALL(nvnDeviceInitialize, P(dev), P(db)) & 0xff) == 1);
    CALL(nvnDeviceSetDepthMode, P(dev), 1); CALL(nvnDeviceSetWindowOriginMode, P(dev), 1);
    CALL(nvnQueueBuilderSetDevice, P(qb), P(dev)); CALL(nvnQueueBuilderSetDefaults, P(qb)); T((CALL(nvnQueueInitialize, P(q), P(qb)) & 0xff) == 1);
    gfx_null_stats(&n1); T(n1.depth_zero == 1 && n1.origin_ll == 0);

    /* a pool in the guest heap: buffers, events, counters, textures, command memory */
    uint8_t *store = gt_alloc(8 << 20); void *pb = gt_alloc(0x100), *pool = gt_alloc(0x100);
    CALL(nvnMemoryPoolBuilderSetDefaults, P(pb)); CALL(nvnMemoryPoolBuilderSetDevice, P(pb), P(dev));
    CALL(nvnMemoryPoolBuilderSetStorage, P(pb), P(store), 8 << 20); CALL(nvnMemoryPoolBuilderSetFlags, P(pb), 0x8 | 0x2);
    T((CALL(nvnMemoryPoolInitialize, P(pool), P(pb)) & 0xff) == 1);
    void *buf; uint64_t off;
    T(gfx_addr(P(store) + 0x1000, 0x100, &buf, &off) && buf == store && off == 0x1000);   /* the backend knows the pool */
    T(!gfx_addr(P(store) + (8 << 20) - 8, 16, &buf, &off));

    /* textures, a sampler and a program; the descriptor pools register them with the backend */
    void *tb = gt_alloc(0x100), *rt = gt_alloc(0x80), *tex = gt_alloc(0x80), *bc1 = gt_alloc(0x80);
    CALL(nvnTextureBuilderSetDevice, P(tb), P(dev)); CALL(nvnTextureBuilderSetDefaults, P(tb));
    CALL(nvnTextureBuilderSetSize2D, P(tb), 64, 64); CALL(nvnTextureBuilderSetFormat, P(tb), 0x25);   /* RGBA8 */
    CALL(nvnTextureBuilderSetStorage, P(tb), P(pool), 0x100000); T((CALL(nvnTextureInitialize, P(rt), P(tb)) & 0xff) == 1);
    CALL(nvnTextureBuilderSetStorage, P(tb), P(pool), 0x140000); T((CALL(nvnTextureInitialize, P(tex), P(tb)) & 0xff) == 1);
    CALL(nvnTextureBuilderSetSize2D, P(tb), 128, 64); CALL(nvnTextureBuilderSetFormat, P(tb), 0x42);  /* RGB_DXT1 (BC1: 4x4 blocks of 8 bytes) */
    CALL(nvnTextureBuilderSetStorage, P(tb), P(pool), 0x180000); T((CALL(nvnTextureInitialize, P(bc1), P(tb)) & 0xff) == 1);
    void *tp = gt_alloc(0x40), *sp = gt_alloc(0x40), *sb = gt_alloc(0x100), *smp = gt_alloc(0x100);
    T((CALL(nvnTexturePoolInitialize, P(tp), P(pool), 0x200000, 256) & 0xff) == 1);
    T((CALL(nvnSamplerPoolInitialize, P(sp), P(pool), 0x210000, 256) & 0xff) == 1);
    CALL(nvnTexturePoolRegisterTexture, P(tp), 1, P(tex), 0); CALL(nvnTexturePoolRegisterImage, P(tp), 2, P(rt), 0);
    CALL(nvnSamplerBuilderSetDevice, P(sb), P(dev)); CALL(nvnSamplerBuilderSetDefaults, P(sb));
    callf(adapter_nvnSamplerBuilderSetLodClamp, P(sb), 0.5f, 7.0f);
    float lo = -1, hi = -1; CALL(nvnSamplerBuilderGetLodClamp, P(sb), P(&lo), P(&hi)); T(lo == 0.5f && hi == 7.0f);
    T((CALL(nvnSamplerInitialize, P(smp), P(sb)) & 0xff) == 1);
    CALL(nvnSamplerPoolRegisterSampler, P(sp), 1, P(smp)); CALL(nvnSamplerPoolRegisterSamplerBuilder, P(sp), 2, P(sb));
    gfx_null_stats(&n1);
    T(n1.live_tex - n0.live_tex == 3 && n1.reg_tex - n0.reg_tex == 1 && n1.reg_img - n0.reg_img == 1 && n1.reg_samp - n0.reg_samp == 2);

    /* a Switch 2 program: its key is sha256(control with +0x20..+0x27 zeroed, code)[:8] */
    void *prog = gt_alloc(0x100); uint8_t ctl[0xe0] = {0}; uint8_t *code = store + 0x220000;
    uint32_t w; w = 0xc9; memcpy(ctl + 4, &w, 4); w = 0x11; memcpy(ctl + 8, &w, 4); w = 0x00220015; memcpy(ctl + 0x18, &w, 4); w = 0x180; memcpy(ctl + 0x44, &w, 4);
    memset(ctl + 0x20, 0x5a, 8); ctl[0x60] = 7;
    for (int i = 0; i < 0x200; i++) code[i] = (uint8_t)(i * 13 + 1);
    T((CALL(nvnProgramInitialize, P(prog), P(dev)) & 0xff) == 1);
    struct { uint64_t data; const void *control; } sd = {P(code), ctl};
    T((CALL(nvnProgramSetShaders, P(prog), 1, P(&sd)) & 0xff) == 1);
    uint8_t key[8], want[32], z[0xe0]; memcpy(z, ctl, 0xe0); memset(z + 0x20, 0, 8); sha256_2(z, 0xe0, code, 0x180, want);
    T(prog_key((const prog_t *)prog, 0, key) == 1 && !memcmp(key, want, 8));
    T(prog_key((const prog_t *)prog, 1, key) == 0);

    /* a recording of every kind of record the frame loop makes */
    void *cb = gt_alloc(0x100), *sync = gt_alloc(0x40), *eb = gt_alloc(0x40), *ev = gt_alloc(0x40);
    T((CALL(nvnCommandBufferInitialize, P(cb), P(dev)) & 0xff) == 1);
    T((CALL(nvnSyncInitialize, P(sync), P(dev)) & 0xff) == 1);
    CALL(nvnEventBuilderSetDefaults, P(eb)); CALL(nvnEventBuilderSetStorage, P(eb), P(pool), 0x230000); T((CALL(nvnEventInitialize, P(ev), P(eb)) & 0xff) == 1);
    uint8_t *cmdmem = store + 0x300000;
    CALL(nvnCommandBufferAddCommandMemory, P(cb), P(pool), 0x300000, 0x10000);
    uint8_t *src = store + 0x240000, *dst = store + 0x241000, *ubo = store + 0x242000, *fill = store + 0x243000, *val = store + 0x244000; uint64_t *ctr = (uint64_t *)(store + 0x245000);
    for (int i = 0; i < 256; i++) src[i] = (uint8_t)i;
    memset(ctr, 0xab, 16);
    float clear[4] = {0, 0, 0, 1}; const void *rts[1] = {rt}; uint8_t data[64]; for (int i = 0; i < 64; i++) data[i] = (uint8_t)(0xc0 + i);
    CALL(nvnCommandBufferBeginRecording, P(cb));
    CALL(nvnCommandBufferSetRenderTargets, P(cb), 1, P(rts), 0, 0, 0);
    CALL(nvnCommandBufferClearColor, P(cb), 0, P(clear), 0xf);
    CALL(nvnCommandBufferBindProgram, P(cb), P(prog), 0x1f);
    CALL(nvnCommandBufferBindVertexBuffer, P(cb), 0, P(store) + 0x250000, 0x1000);
    CALL(nvnCommandBufferBindUniformBuffer, P(cb), 0, 2, P(ubo), 0x100);
    CALL(nvnCommandBufferBindTexture, P(cb), 4, 0, 1 | 1ull << 20);
    CALL(nvnCommandBufferDrawElements, P(cb), 4, 2, 36, P(store) + 0x260000);
    CALL(nvnCommandBufferDispatchCompute, P(cb), 4, 2, 1);
    CALL(nvnCommandBufferUpdateUniformBuffer, P(cb), P(ubo), 0x100, 0x20, 64, P(data));
    CALL(nvnCommandBufferCopyBufferToBuffer, P(cb), P(src), P(dst), 256, 0);
    CALL(nvnCommandBufferClearBuffer, P(cb), P(fill), 64, 0x11223344);
    CALL(nvnCommandBufferReportValue, P(cb), 0x1234, P(val));
    CALL(nvnCommandBufferFenceSync, P(cb), P(sync), 0, 0);
    CALL(nvnCommandBufferReportCounter, P(cb), 0x10, P(ctr));   /* TIMESTAMP_TOP */
    CALL(nvnCommandBufferSignalEvent, P(cb), P(ev), 1, 0, 0, 5);   /* ADD 5 */
    uint64_t h = CALL(nvnCommandBufferEndRecording, P(cb));
    T(h == P(cmdmem));
    uint32_t ops[32]; int nops = ops_of((const uint8_t *)(uintptr_t)h, ops, 32);
    static const uint32_t want_ops[] = {OP_SET_RT, OP_CLEAR_COLOR, OP_BIND_PROGRAM, OP_BIND_VBUF1, OP_BIND_UBO_SMALL, OP_BIND_TEX1, OP_DRAW_IDX, OP_DISPATCH, OP_UPDATE_UB,
                                        OP_COPY_B2B, OP_CLEAR_BUF, OP_SIGNAL_EVENT, OP_FENCE_SYNC, OP_REPORT_COUNTER, OP_SIGNAL_EVENT, OP_END};
    T(nops == (int)(sizeof want_ops / sizeof *want_ops) && !memcmp(ops, want_ops, sizeof want_ops));
    /* compact records keep the sizes games measure (nvn_obj.h): a vertex buffer, a uniform buffer and one texture bind take 24 bytes, an indexed draw 40 */
    { const uint8_t *p = (const uint8_t *)(uintptr_t)h; uint32_t sz[16]; for (int i = 0; i < nops && i < 16; i++) { op_hdr hh; memcpy(&hh, p, 8); sz[i] = hh.size; p += hh.size; }
      T(sz[3] == 24 && sz[4] == 24 && sz[5] == 24 && sz[6] == 40 && sz[15] == 8); }
    uint64_t used = CALL(nvnCommandBufferGetCommandMemoryUsed, P(cb));
    T(used > 0 && CALL(nvnCommandBufferGetCommandMemoryFree, P(cb)) == 0x10000 - used);
    /* nothing has happened before the submit */
    T((uint32_t)CALL(nvnSyncWait, P(sync), 0) == NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED);
    T(dst[100] == 0 && *(uint32_t *)val == 0 && (uint32_t)CALL(nvnEventGetValue, P(ev)) == 0 && ctr[1] == 0xababababababababull);
    *(volatile uint32_t *)(store + 0x230000) = 10;
    gfx_null_stats(&n0);
    CALL(nvnQueueSubmitCommands, P(q), 1, P(&h));
    gfx_null_stats(&n1);
    T((uint32_t)CALL(nvnSyncWait, P(sync), 0) == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED);
    T(!memcmp(dst, src, 256));
    T(!memcmp(ubo + 0x20, data, 64));
    { int ok = 1; for (int i = 0; i < 64; i += 4) ok &= *(uint32_t *)(fill + i) == 0x11223344; T(ok); }
    T(*(uint32_t *)val == 0x1234 && (uint32_t)CALL(nvnEventGetValue, P(ev)) == 15);
    T(ctr[0] == ctr[1] && ctr[1] != 0xababababababababull && ctr[1] > 0);
    T(n1.submits - n0.submits == 1 && n1.handles - n0.handles == 1 && n1.draws - n0.draws == 1 && n1.dispatches - n0.dispatches == 1 && n1.records - n0.records == (uint64_t)nops);
    T(n1.copies - n0.copies == 1 && n1.updates - n0.updates == 1 && n1.fills - n0.fills == 1 && n1.events - n0.events == 2 && n1.counters - n0.counters == 1 && n1.fences - n0.fences == 1);
    T(n1.bad == n0.bad);
    /* QueueFenceSync and Finish */
    void *sync2 = gt_alloc(0x40); CALL(nvnSyncInitialize, P(sync2), P(dev)); CALL(nvnQueueFenceSync, P(q), P(sync2), 0, 0);
    T((uint32_t)CALL(nvnSyncWait, P(sync2), 1000000) == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED);
    CALL(nvnQueueFinish, P(q));

    /* CALL and COPY of another recording: replayed twice */
    CALL(nvnCommandBufferBeginRecording, P(cb));
    CALL(nvnCommandBufferDrawArrays, P(cb), 4, 0, 3);
    uint64_t ha = CALL(nvnCommandBufferEndRecording, P(cb));
    CALL(nvnCommandBufferBeginRecording, P(cb));
    CALL(nvnCommandBufferCallCommands, P(cb), 1, P(&ha)); CALL(nvnCommandBufferCopyCommands, P(cb), 1, P(&ha));
    uint64_t hb = CALL(nvnCommandBufferEndRecording, P(cb));
    gfx_null_stats(&n0); CALL(nvnQueueSubmitCommands, P(q), 1, P(&hb)); gfx_null_stats(&n1);
    T(n1.draws - n0.draws == 2 && n1.calls - n0.calls == 2 && n1.bad == n0.bad);

    /* nvnCommandBufferPushComputeConstants (P3b): recorded in order with the dispatches (OP_PUSH_CONST); under 4 bytes and past the 256-byte table are dropped; the Deferred form keeps the pointer, not the words */
    { uint32_t pw[4] = {0x39b60b61, 0x3a21d13a, 0x45340000, 0x44ca8000};
      CALL(nvnCommandBufferBeginRecording, P(cb));
      CALL(nvnCommandBufferPushComputeConstants, P(cb), 0, 0, 16, P(pw));
      CALL(nvnCommandBufferDispatchCompute, P(cb), 1, 1, 1);
      CALL(nvnCommandBufferPushComputeConstants, P(cb), 0, 0, 2, P(pw));
      CALL(nvnCommandBufferPushComputeConstants, P(cb), 0, 0xf8, 16, P(pw));
      CALL(nvnCommandBufferPushComputeConstantsDeferred, P(cb), 3, 8, 16, P(pw));
      CALL(nvnCommandBufferDispatchCompute, P(cb), 2, 1, 1);
      uint64_t hp = CALL(nvnCommandBufferEndRecording, P(cb));
      uint32_t pops[8]; int pn = ops_of((const uint8_t *)(uintptr_t)hp, pops, 8);
      static const uint32_t want_p[] = {OP_PUSH_CONST, OP_DISPATCH, OP_PUSH_CONST, OP_DISPATCH, OP_END};
      T(pn == 5 && !memcmp(pops, want_p, sizeof want_p));
      const uint8_t *r0 = (const uint8_t *)(uintptr_t)hp; op_hdr h0; memcpy(&h0, r0, 8);
      p_push_const pc0; memset(&pc0, 0, sizeof pc0); memcpy(&pc0, r0 + 8, h0.size - 8);
      T(h0.size == 8 + offsetof(p_push_const, w) + 16 && pc0.compute == 1 && pc0.table == 0 && pc0.offset == 0 && pc0.bytes == 16 && pc0.src == 0 && !memcmp(pc0.w, pw, 16));
      const uint8_t *r2 = r0 + h0.size; op_hdr h1; memcpy(&h1, r2, 8); r2 += h1.size; op_hdr h2; memcpy(&h2, r2, 8);
      p_push_const pc2; memset(&pc2, 0, sizeof pc2); memcpy(&pc2, r2 + 8, h2.size - 8);
      T(h2.op == OP_PUSH_CONST && h2.size == 8 + offsetof(p_push_const, w) && pc2.table == 3 && pc2.offset == 8 && pc2.bytes == 16 && pc2.src == P(pw));
      gfx_null_stats(&n0); CALL(nvnQueueSubmitCommands, P(q), 1, P(&hp)); gfx_null_stats(&n1);
      T(n1.op[OP_PUSH_CONST] - n0.op[OP_PUSH_CONST] == 2 && n1.dispatches - n0.dispatches == 2 && n1.bad == n0.bad); }

    /* command memory runs out: the game's callback (a guest function) adds 256-byte chunks; the recording continues across a JUMP */
    g_cb_pool = pool; g_cb_next = 0x400000;
    CALL(nvnCommandBufferSetMemoryCallback, P(cb), gt_fn(mem_callback)); CALL(nvnCommandBufferSetMemoryCallbackData, P(cb), 0x77);
    CALL(nvnCommandBufferAddCommandMemory, P(cb), P(pool), 0x3f0000, 256);
    CALL(nvnCommandBufferBeginRecording, P(cb));
    for (int i = 0; i < 40; i++) CALL(nvnCommandBufferDrawArrays, P(cb), 4, i, 3);
    uint64_t hc = CALL(nvnCommandBufferEndRecording, P(cb));
    printf("40 draws in 256-byte chunks: %d memory callbacks, the last asked for %llu bytes\n", g_cb_calls, (unsigned long long)g_cb_min);
    T(hc == P(store) + 0x3f0000 && g_cb_calls >= 4 && g_cb_min >= 16);
    nops = ops_of((const uint8_t *)(uintptr_t)hc, ops, 32); T(nops == 41);
    gfx_null_stats(&n0); CALL(nvnQueueSubmitCommands, P(q), 1, P(&hc)); gfx_null_stats(&n1);
    T(n1.draws - n0.draws == 40 && n1.op[OP_JUMP] - n0.op[OP_JUMP] == (uint64_t)g_cb_calls && n1.bad == n0.bad);
    /* no callback: the host lends a segment */
    CALL(nvnCommandBufferSetMemoryCallback, P(cb), 0);
    CALL(nvnCommandBufferAddCommandMemory, P(cb), P(pool), 0x500000, 128);
    CALL(nvnCommandBufferBeginRecording, P(cb));
    for (int i = 0; i < 50; i++) CALL(nvnCommandBufferDrawArrays, P(cb), 4, i, 3);
    uint64_t hd = CALL(nvnCommandBufferEndRecording, P(cb));
    gfx_null_stats(&n0); CALL(nvnQueueSubmitCommands, P(q), 1, P(&hd)); gfx_null_stats(&n1);
    T(hd == P(store) + 0x500000 && n1.draws - n0.draws == 50 && n1.bad == n0.bad);

    /* a malformed list is refused with a red line, the rest of the submission still runs */
    uint64_t bad[4] = {(uint64_t)250 | (uint64_t)16 << 32, 0, 0, 0}; uint64_t hs[2] = {P(bad), ha};
    gfx_null_stats(&n0); CALL(nvnQueueSubmitCommands, P(q), 2, P(hs)); gfx_null_stats(&n1);
    T(n1.bad - n0.bad == 1 && n1.draws - n0.draws == 1);

    /* the frame loop: acquire, present (a ticket from the backend), the guest frame ticks */
    void *wb = gt_alloc(0x100), *win = gt_alloc(0x200); const void *texs[2] = {rt, tex}; int idx = -1;
    CALL(nvnWindowBuilderSetDevice, P(wb), P(dev)); CALL(nvnWindowBuilderSetDefaults, P(wb)); CALL(nvnWindowBuilderSetTextures, P(wb), 2, P(texs));
    T((CALL(nvnWindowInitialize, P(win), P(wb)) & 0xff) == 1);
    uint64_t f0 = dm_guest_frame(); gfx_null_stats(&n0);
    for (int k = 0; k < 10; k++) {
        T(CALL(nvnWindowAcquireTexture, P(win), P(sync), P(&idx)) == 0 && idx == k % 2);
        T((uint32_t)CALL(nvnSyncWait, P(sync), 0) == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED);
        CALL(nvnQueueSubmitCommands, P(q), 1, P(&ha));
        CALL(nvnQueuePresentTexture, P(q), P(win), idx);
    }
    gfx_null_stats(&n1);
    T(dm_guest_frame() - f0 == 10 && n1.presents - n0.presents == 10 && ((const win_t *)win)->ticket[1] > ((const win_t *)win)->ticket[0]);

    /* texels: a region written lands where the whole-surface layout (s2tex) puts it, and reads back */
    for (int fmt = 0; fmt < 2; fmt++) {
        const tex_t *t = fmt ? (const tex_t *)bc1 : (const tex_t *)tex;
        int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
        int rx = 16, ry = 8, rw = fmt ? 32 : 24, rh = fmt ? 16 : 12;
        size_t row = (size_t)(rw / bw) * (size_t)bpb, bytes = row * (size_t)(rh / bh);
        uint8_t *in = malloc(bytes), *out = malloc(bytes); for (size_t i = 0; i < bytes; i++) in[i] = (uint8_t)(i * 7 + 3 + fmt);
        int region[6] = {rx, ry, 0, rw, rh, 1};
        CALL(nvnTextureWriteTexels, P(t), 0, P(region), P(in));
        memset(out, 0, bytes); CALL(nvnTextureReadTexels, P(t), 0, P(region), P(out));
        T(!memcmp(in, out, bytes));
        s2tex_surface s; uint64_t base; size_t lrow;
        T(nvn_texio_surface(t, 0, 0, &s, &base, &lrow) == 0 && s.order == S2TEX_SWITCH2);
        size_t lp = (size_t)s.width * s.bytes_per_block; uint8_t *lin = malloc(lp * s.height);
        T(s2tex_deswizzle(&s, t->mem + base, (size_t)s2tex_size(&s), lin, lp, 0) == S2TEX_OK);
        int ok = 1; for (int y = 0; y < rh / bh; y++) ok &= !memcmp(lin + (size_t)(ry / bh + y) * lp + (size_t)(rx / bw) * bpb, in + (size_t)y * row, row);
        T(ok);
        free(in); free(out); free(lin);
    }
    gfx_null_stats(&n1); T(n1.cpu_written >= 2);

    /* the run-time shader path: missing → dumped as DM_NVN_SHDUMP dumps it; then present in the cache → found; another chain's entry → stale */
    {
        T(nxs_rt_enabled());
        const nxs_shader *sh = NULL; char hex[17]; for (int j = 0; j < 8; j++) snprintf(hex + 2 * j, 3, "%02x", key[j]);
        T(nxs_resolve((const prog_t *)prog, 0, &sh) == 0 && !sh);
        snprintf(p, sizeof p, "%s/dumps/%s.ctl", cache, hex); FILE *f = fopen(p, "rb"); uint8_t dc[0x900]; size_t dn = f ? fread(dc, 1, sizeof dc, f) : 0; if (f) fclose(f);
        T(dn == 0x880 && !memcmp(dc, ctl, 0xe0) && dc[0xe0] == 0 && dc[0x87f] == 0);   /* the 0xe0 block, the rest zeros */
        snprintf(p, sizeof p, "%s/dumps/%s.code", cache, hex); f = fopen(p, "rb"); uint8_t dcode[0x400]; dn = f ? fread(dcode, 1, sizeof dcode, f) : 0; if (f) fclose(f);
        T(dn == 0x180 && !memcmp(dcode, code, 0x180));   /* control +0x44 bytes of the code area */
        nxs_counts c0; nxs_counts_get(&c0); T(c0.missing_keys == 1 && c0.dumped == 1 && c0.requested == 0);
        /* nxshader publishes it: CHAIN, <key>.spv, <key>.json (a stand-in module) */
        snprintf(p, sizeof p, "%s/vk/CHAIN", cache); f = fopen(p, "wb"); fputs("00112233aabbccdd", f); fputc(10, f); fclose(f);
        static const uint32_t mod[5] = {0x07230203, 0x00010600, 0, 1, 0};
        snprintf(p, sizeof p, "%s/vk/%s.spv", cache, hex); f = fopen(p, "wb"); fwrite(mod, 1, sizeof mod, f); fclose(f);
        snprintf(p, sizeof p, "%s/vk/%s.json", cache, hex); f = fopen(p, "wb"); fprintf(f, "{\"key\": \"%s\", \"cache\": {\"chain\": \"ffffffffffffffff\"}}", hex); fclose(f);
        Sleep(600);   /* a missing key is looked for again twice a second */
        T(nxs_resolve((const prog_t *)prog, 0, &sh) == 0);   /* another chain's entry: stale */
        f = fopen(p, "wb"); fprintf(f, "{\"key\": \"%s\", \"cache\": {\"chain\": \"00112233aabbccdd\"}}", hex); fclose(f);
        Sleep(600);
        T(nxs_resolve((const prog_t *)prog, 0, &sh) == 1 && sh && sh->spv_size == sizeof mod && !memcmp(sh->spv, mod, sizeof mod) && strstr(sh->json, hex));
        nxs_counts c1; nxs_counts_get(&c1); T(c1.stale >= 1 && c1.found >= 1 && c1.missing_keys == 1);
    }

    /* lifetimes: finalizing reaches the backend */
    gfx_null_stats(&n0);
    CALL(nvnTextureFinalize, P(bc1)); CALL(nvnProgramFinalize, P(prog));
    gfx_null_stats(&n1); T(n0.live_tex - n1.live_tex == 1 && n1.progs_released - n0.progs_released >= 1);
    CALL(nvnMemoryPoolFinalize, P(pool));
    T(!gfx_addr(P(store) + 0x1000, 0x100, &buf, &off));

    /* the dirty-page tracker (Win32 port): a protected page faults once on a write, is stamped and writable again */
    uint8_t *pg = VirtualAlloc(NULL, 1 << 16, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    nvn_dirty_add_range(pg, 1 << 16);
    uint64_t st = nvn_dirty_now();
    nvn_dirty_protect(pg + 4096, 8192);
    T(nvn_dirty_tracked(pg + 4096, 8192) && !nvn_dirty_since(pg + 4096, 8192, st));
    uint64_t fa0, pr0, fa1, pr1; nvn_dirty_stats(&fa0, &pr0);
    pg[4096 + 100] = 1; pg[4096 + 200] = 2;
    nvn_dirty_stats(&fa1, &pr1);
    T(nvn_dirty_since(pg + 4096, 4096, st) && !nvn_dirty_since(pg + 8192, 4096, st) && fa1 - fa0 == 1 && pg[4096 + 100] == 1);
    dm_guest_write_prepare(pg + 8192, 16);   /* a host write: unprotected before, stamped */
    nvn_dirty_stats(&fa0, &pr0); T(pr0 - pr1 == 1 && nvn_dirty_since(pg + 8192, 4096, st));
    nvn_dirty_remove_range(pg);

    printf(gt_fail ? "nxrt.nvn_rec: FAILED\n" : "nxrt.nvn_rec: ok\n");
    return gt_fail;
}
