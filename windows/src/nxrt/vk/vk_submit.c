// Submissions, batching and the completion thread (vk_submit.h; docs/m5-vulkan.md §3.4). Every queue submission that signals the GPU timeline goes
// through here, so timeline values reach the queue in increasing order: a value is assigned at commit, and committed submissions are handed to the
// queue in commit order. The ordering rule of the NVN layer (nvn_mtl.h, gfx_backend.h): every visible effect of GPU completion happens only after
// the GPU has actually completed the submission.
#include "vk_submit.h"
#include "vk_tex.h"
#include "vk_mem.h"
#ifdef _WIN32
#include <process.h>
#endif
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NSLOT = 32, AGE_FLUSH_NS = 2000000 };
enum { S_FREE, S_RECORDING, S_PENDING, S_SUBMITTED };
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr; uint32_t type; void (*fn)(void *); void *arg; } post_t;
typedef struct {
    int state;
    VkCommandPool pool; VkCommandBuffer up, main;
    int up_used;
    uint64_t value, t_commit, t_submit;
    post_t *posts; size_t nposts, cap;
    } slot_t;

static slot_t g_slot[NSLOT];
static int g_order[NSLOT], g_norder;        // committed slots (pending, then submitted), in value order
static int g_cur = -1;                      // the NVN submission being recorded
static uint32_t g_cur_tid;                     // the thread recording it (M6 phase B: another thread's uploads go to a one-shot of its own, not into this slot)
static pthread_mutex_t g_lk = PTHREAD_MUTEX_INITIALIZER;         // slots, order, value assignment
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;   // a slot freed / a value completed
static plat_thr_t g_thread; static _Atomic int g_stop;
static int g_batch = 4;                     // NXRT_VK_BATCH: committed submissions held back before one queue submit
static _Atomic uint64_t g_submit_t0;        // when the vkQueueSubmit2 in progress began (0: none), for the watchdog
static unsigned g_wd_ms = 8000;             // NXRT_GPU_WATCHDOG_MS: a submission the GPU has not completed this long after it was queued ends the run with a report (0: off)
static _Atomic uint64_t g_subs, g_qsubs, g_empty, g_posted;
#include "nvn_timestamp.h"   // gfx/: NVN counter records count 614.4 MHz ticks of the guest clock
uint64_t guest_ns(void);

static void slot_init(slot_t *s) {
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, g_vk.qfam};
    vkCreateCommandPool(g_vk.dev, &pci, NULL, &s->pool);
    VkCommandBuffer cbs[2];
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, s->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2};
    vkAllocateCommandBuffers(g_vk.dev, &ai, cbs);
    s->up = cbs[0]; s->main = cbs[1];
}

// A free slot, reset and begun; waits (flushing first) while every slot is in flight. Caller holds g_lk
static int slot_take(void) {
    for (;;) {
        for (int i = 0; i < NSLOT; i++) if (g_slot[i].state == S_FREE) {
            slot_t *s = &g_slot[i];
            s->state = S_RECORDING; s->up_used = 0; s->value = 0; s->nposts = 0;
            vkResetCommandPool(g_vk.dev, s->pool, 0);
            VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
            vkBeginCommandBuffer(s->up, &bi); vkBeginCommandBuffer(s->main, &bi);
            return i;
        }
        pthread_mutex_unlock(&g_lk); vks_flush(); pthread_mutex_lock(&g_lk);
        plat_cond_wait_ms(&g_cv, &g_lk, (unsigned)(50));
    }
}

// Hands every pending slot to the queue in one call. Caller holds g_lk
void vks_device_lost(const char *where);
static void flush_locked(void) {
    VkSubmitInfo2 si[NSLOT]; VkCommandBufferSubmitInfo cbi[NSLOT][2]; VkSemaphoreSubmitInfo sig[NSLOT]; int n = 0; uint64_t last = 0;
    for (int k = 0; k < g_norder; k++) {
        slot_t *s = &g_slot[g_order[k]];
        if (s->state != S_PENDING) continue;
        int nc = 0;
        if (s->up_used) cbi[n][nc++] = (VkCommandBufferSubmitInfo){VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, s->up, 0};
        cbi[n][nc++] = (VkCommandBufferSubmitInfo){VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, s->main, 0};
        sig[n] = (VkSemaphoreSubmitInfo){VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, g_vk.tl, s->value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0};
        si[n] = (VkSubmitInfo2){VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, 0, NULL, (uint32_t)nc, cbi[n], 1, &sig[n]};
        s->state = S_SUBMITTED; s->t_submit = now_ns(); last = s->value; n++;
    }
    if (!n) return;
    plat_rw_wrlock(&g_vk.qlock);
    atomic_store(&g_submit_t0, now_ns());
    VkResult r = vkQueueSubmit2(g_vk.q, (uint32_t)n, si, VK_NULL_HANDLE);
    atomic_store(&g_submit_t0, 0);
    plat_rw_wrunlock(&g_vk.qlock);
    if (r != VK_SUCCESS) vk_lost("a submit", r);   // the fault report (VK_EXT_device_fault / checkpoints), once
    if (r == VK_ERROR_DEVICE_LOST) vks_device_lost("vkQueueSubmit2");
    if (r != VK_SUCCESS) VKRED("vkQueueSubmit2 failed (%d) for %d submissions up to %llu", r, n, (unsigned long long)last);
    atomic_store(&g_vk.tl_submitted, last);
    atomic_fetch_add(&g_qsubs, 1);
}
void vks_flush(void) { pthread_mutex_lock(&g_lk); flush_locked(); pthread_mutex_unlock(&g_lk); }

// The GPU reset this device (a hang, here or in another program): no submission can succeed again, and the game would run on without a picture. What was committed to the save is
// safe; end the run with the reason instead
void vks_device_lost(const char *where) {
    VKRED("the GPU lost the device (%s): the driver or the kernel reset it. Ending the run: the last committed save is intact, and the game can simply be started again", where);
    nxrt_logf("[RED] graphics device lost: ending the run");
    plat_exit(75);
}

static void post_run(post_t *p) {
    if (p->kind == 1) { if (p->mode == 1) *p->val += p->v; else *p->val = p->v; }
    else if (p->kind == 2) {   // NVN's counter record: the counter, then the time. A timestamp report's counter word is 0, as the GPU's semaphore release writes it (a payload of 0 with the time beside it):
        // OoT marks a depth target "cleared this frame" by a TIMESTAMP_TOP report into a per-texture slot it set to 1, and clears again at every bind while the slot's first word is
        // not 0 (main+0x246094). With the time in that word the depth copy its materials test EQUAL against was cleared before every material pass and only the sky was left
        // (FASE5 section 24). NXRT_VK_TS_COUNTER=1 puts the time in the counter word again (the previous behaviour, to compare)
        static int tsc = -1; if (tsc < 0) { const char *e = getenv("NXRT_VK_TS_COUNTER"); tsc = e && *e == '1'; }
        uint64_t t = nvn_timestamp_from_ns(guest_ns()), rec[2] = {tsc && (p->type == 0 || p->type == 0x10) ? t : 0, t};
        void dm_guest_write_prepare(void *addr, size_t len); int nxrt_nvn_eager_ts(void);
        if ((p->type == 0 || p->type == 0x10) && !tsc && nxrt_nvn_eager_ts()) {   // the counter word was written when the report was recorded (hle_nvn.c): only the time now
            dm_guest_write_prepare((void *)(uintptr_t)(p->addr + 8), 8); memcpy((void *)(uintptr_t)(p->addr + 8), &rec[1], 8);
        } else { dm_guest_write_prepare((void *)(uintptr_t)p->addr, sizeof rec); memcpy((void *)(uintptr_t)p->addr, rec, sizeof rec); }
    }
    else if (p->kind == 3) p->fn(p->arg);
}

// The GPU watchdog. A hung shader gets the whole GPU reset by the kernel (amdgpu: "ring gfx_0.0.0 timeout") a long time after the game submitted it, and the game runs on
// meanwhile (2026-11-05: the first run's vkQueueSubmit2 itself blocked until the reset, with g_lk held, so nothing that takes g_lk can watch). Its own thread, no lock: when
// work is queued (timeline submitted > done) and the done value has not moved for g_wd_ms, or one vkQueueSubmit2 call has taken that long, it says which compute dispatches
// were recorded last and ends the run before more work is queued behind the hang. NXRT_SKIP_COMPUTE=<key> skips a shader on the next run
static char g_ring[16][24]; static _Atomic uint32_t g_ring_n;
static plat_thr_t g_wd_thread;
void vks_note(const char *what) {   // a compute shader's key, for the watchdog's report (no lock: a torn line in a report is acceptable)
    uint32_t n = atomic_fetch_add(&g_ring_n, 1); snprintf(g_ring[n & 15], sizeof g_ring[0], "%s", what);
}
// amdgpu's own memory counters (sysfs, Linux only), sampled by the watchdog thread: a submission the kernel cannot make resident fails with "Not enough memory for command
// submission" (VK_ERROR_DEVICE_LOST) with no reset and no hang, and the game's own accounting (video memory in the memory line) does not say how close it was. NXRT_VK_AMDMEM=0 off
static void amd_mem_sample(uint64_t t) {
#ifdef __linux__
    static uint64_t next, lv, lg; static int off = -1; static char dir[64];
    if (off < 0) { const char *e = getenv("NXRT_VK_AMDMEM"); off = e && !atoi(e); if (!off) { off = 1;
        for (int c = 0; c < 8; c++) { char f[96]; snprintf(f, sizeof f, "/sys/class/drm/card%d/device/mem_info_gtt_used", c); FILE *q = fopen(f, "r"); if (q) { fclose(q); snprintf(dir, sizeof dir, "/sys/class/drm/card%d/device/", c); off = 0; break; } } } }
    if (off || t < next) return;
    next = t + 1000000000ull;
    uint64_t v[4] = {0}; static const char *nm[4] = {"mem_info_vram_used", "mem_info_vram_total", "mem_info_gtt_used", "mem_info_gtt_total"};
    for (int i = 0; i < 4; i++) { char f[128]; snprintf(f, sizeof f, "%s%s", dir, nm[i]); FILE *q = fopen(f, "r"); if (q) { unsigned long long x = 0; if (fscanf(q, "%llu", &x) == 1) v[i] = x >> 20; fclose(q); } }
    static uint64_t lastlog;
    if (v[0] + 256 <= lv || v[0] >= lv + 256 || v[2] + 256 <= lg || v[2] >= lg + 256 || t - lastlog > 20000000000ull) {
        lv = v[0]; lg = v[2]; lastlog = t;
        uint64_t bb, dd, bu = 0, bud = 0, bused, hb, pm, pf, pby; vkm_pool_counts(&pm, &pf, &pby); vk_alloc_bytes_nolock(&bb, &dd, &bused, &hb); vk_budget(&bud, &bu);
        static char cen[1400]; vk_alloc_census(cen, sizeof cen);
        nxrt_logf("[vk] amdgpu memory: video %llu / %llu MB, GTT %llu / %llu MB; ours: pool mirrors %llu MB bound live (%llu pools made, %llu freed, %llu MB made in all), images %llu MB, allocator blocks %llu MB (%llu in use, %llu host-visible blocks), dedicated %llu MB, budget usage %llu MB; allocation sites:%s",
                  (unsigned long long)v[0], (unsigned long long)v[1], (unsigned long long)v[2], (unsigned long long)v[3], (unsigned long long)(vkm_bound_total() >> 20), (unsigned long long)pm, (unsigned long long)pf, (unsigned long long)(pby >> 20),
                  (unsigned long long)(vkt_image_bytes() >> 20), (unsigned long long)(bb >> 20), (unsigned long long)(bused >> 20), (unsigned long long)hb, (unsigned long long)(dd >> 20), (unsigned long long)(bu >> 20), cen);
    }
#else
    (void)t;
#endif
}
// The completion thread's own stack, for the report (Linux: a signal to that thread; the addresses are offsets into nxrt: addr2line -f -e bin/nxrt <off>). A GPU that has finished
// everything (the AMD markers say so) while the timeline stands still means the completion thread is stuck in an effect or a lock, and this names where
#ifdef __linux__
#include <signal.h>
#include <execinfo.h>
#include <dlfcn.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
static void stack_sig(int sg) {   // one write per thread, no allocation (the thread may be anywhere)
    (void)sg; void *pc[20]; int n = backtrace(pc, 20); char line[900]; char nm[20] = "?"; prctl(PR_GET_NAME, nm, 0, 0, 0);
    int o = snprintf(line, sizeof line, "[stack] tid %ld '%s':", (long)syscall(SYS_gettid), nm);
    for (int i = 1; i < n && o + 40 < (int)sizeof line; i++) { Dl_info di;
        if (dladdr(pc[i], &di) && di.dli_fbase) o += snprintf(line + o, sizeof line - o, " %s+%#lx", strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname, (unsigned long)((char *)pc[i] - (char *)di.dli_fbase));
        else o += snprintf(line + o, sizeof line - o, " %p", pc[i]); }
    line[o++] = '\n'; (void)!write(2, line, (size_t)o);
}
// Every thread's stack (host frames: nxrt+0x... resolves with addr2line -f -e bin/nxrt; guest code shows as game.so addresses). A thread inside a lock shows which call it sleeps in
static void completion_stack(void) {
    struct sigaction a; memset(&a, 0, sizeof a); a.sa_handler = stack_sig; sigemptyset(&a.sa_mask); sigaction(SIGUSR2, &a, NULL);
    DIR *d = opendir("/proc/self/task"); if (!d) return;
    long me = (long)syscall(SYS_gettid); struct dirent *e;
    while ((e = readdir(d))) { long t = atol(e->d_name); if (t > 0 && t != me) { syscall(SYS_tgkill, (long)getpid(), t, SIGUSR2); usleep(2000); } }
    closedir(d); usleep(300000);
}
#else
static void completion_stack(void) {}
#endif
void nxrt_thread_dump(void);
static unsigned watchdog_main(void *arg) {
    (void)arg; plat_thread_name("vk watchdog");
    uint64_t last_done = 0, since = now_ns();
    while (!atomic_load(&g_stop)) {
        plat_sleep_ms(250);
        amd_mem_sample(now_ns());
        uint64_t done = atomic_load(&g_vk.tl_done), sub = atomic_load(&g_vk.tl_submitted), t = now_ns(), s0 = atomic_load(&g_submit_t0);
        if (done != last_done || sub <= done) { last_done = done; since = t; }
        uint64_t stuck = sub > done ? t - since : 0, insub = s0 ? t - s0 : 0, lim = (uint64_t)g_wd_ms * 1000000ull;
        if (stuck < lim && insub < lim) continue;
        char what[400]; int o = 0; uint32_t n = atomic_load(&g_ring_n);
        for (uint32_t i = n > 12 ? n - 12 : 0; i < n && o + 24 < (int)sizeof what; i++) o += snprintf(what + o, sizeof what - o, " %s", g_ring[i & 15]);
        char ind[700]; ind[0] = 0; vkm_ind_report(ind, sizeof ind);
        nxrt_logf("[RED] GPU watchdog: last indirect-dispatch arguments (key, frame, x y z; deadbeef = copy not run):%s", ind[0] ? ind : " none");
        { static char mk[2400]; mk[0] = 0; if (vk_marker_report(mk, sizeof mk)) nxrt_logf("[RED] GPU watchdog: AMD buffer markers:%s", mk); }
        { uint64_t gpu = vk_tl_value();   // the semaphore's own counter: what the GPU has signalled, whether or not the completion thread has applied it
          if (gpu > done) nxrt_logf("[RED] GPU watchdog: the GPU is NOT hung: its timeline counter is %llu (submissions up to there finished) but the completion thread has applied only %llu, so it is blocked on the CPU "
                                    "side in an effect or a lock (the thread stacks below name where; 2026-11-05: wb_land -> gpu_pending behind a writer-first pool lock)", (unsigned long long)gpu, (unsigned long long)done);
          else nxrt_logf("[RED] GPU watchdog: the GPU's timeline counter is %llu = the completed value: the GPU has not finished submission %llu (a hang, or a wait inside it; NXRT_VK_MARKERS=1 names the command)", (unsigned long long)gpu, (unsigned long long)done + 1); }
        nxrt_logf("[RED] GPU watchdog: %s for %.1f s (timeline done %llu, submitted %llu). Last compute dispatches recorded (%u in all):%s. Ending the run before the kernel resets the GPU; "
                  "NXRT_SKIP_COMPUTE=<key> skips a shader, NXRT_GPU_WATCHDOG_MS=0 turns this off", insub >= lim ? "a vkQueueSubmit2 call has not returned" : "the GPU has completed nothing",
                  (insub >= lim ? insub : stuck) / 1e9, (unsigned long long)done, (unsigned long long)sub, n, o ? what : " none");
        completion_stack();
        nxrt_thread_dump();   // the guest threads: what each waits on
        { const char *h = getenv("NXRT_GPU_WATCHDOG_HOLD"); if (h && atoi(h)) { nxrt_logf("[RED] GPU watchdog: holding (NXRT_GPU_WATCHDOG_HOLD=1) for a debugger: gdb -p %d", (int)getpid()); for (;;) plat_sleep_ms(1000); } }   // debugging: attach and read every thread's stack
        plat_exit(76);
    }
    return 0;
}

// The completion thread: waits for the oldest submitted value, applies its effects in order, frees its slot; flushes submissions held back too long
static unsigned completion_main(void *arg) {
    (void)arg;
    plat_thread_name("vk completion");
    plat_thread_nice(PLAT_PRIO_ABOVE_NORMAL);
    while (!atomic_load(&g_stop)) {
        pthread_mutex_lock(&g_lk);
        uint64_t want = 0, age_from = 0;
        for (int k = 0; k < g_norder; k++) {
            slot_t *s = &g_slot[g_order[k]];
            if (s->state == S_SUBMITTED && !want) want = s->value;
            if (s->state == S_PENDING && !age_from) age_from = s->t_commit;
        }
        if (age_from && now_ns() - age_from > AGE_FLUSH_NS) flush_locked();
        pthread_mutex_unlock(&g_lk);
        if (!want) { vk_retire_poll(); plat_sleep_ms(1); continue; }
        if (!vk_tl_wait(want, 1000000)) continue;
        uint64_t done = vk_tl_value();
        for (;;) {   // apply in value order; effects run outside the lock (an event write can wake a guest thread that submits)
            pthread_mutex_lock(&g_lk);
            int idx = g_norder && g_slot[g_order[0]].state == S_SUBMITTED && g_slot[g_order[0]].value <= done ? g_order[0] : -1;
            pthread_mutex_unlock(&g_lk);
            if (idx < 0) break;
            slot_t *s = &g_slot[idx];
            for (size_t i = 0; i < s->nposts; i++) post_run(&s->posts[i]);
            pthread_mutex_lock(&g_lk);
            atomic_store(&g_vk.tl_done, s->value);
            s->state = S_FREE; memmove(g_order, g_order + 1, (size_t)(--g_norder) * sizeof *g_order);
            pthread_cond_broadcast(&g_cv);
            pthread_mutex_unlock(&g_lk);
        }
        vk_retire_poll();
    }
    return 0;
}

int vks_init(void) {
#ifdef __linux__
    { const char *pt = getenv("NXRT_PTRACE_ANY"); if (pt && atoi(pt)) prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0); }   // diagnostics: lets gdb -p attach under yama ptrace_scope=1 (sampling profiles)
#endif
    const char *e = getenv("NXRT_VK_BATCH"); if (e && atoi(e) > 0) g_batch = atoi(e) < NSLOT / 2 ? atoi(e) : NSLOT / 2;
    if ((e = getenv("NXRT_GPU_WATCHDOG_MS"))) g_wd_ms = (unsigned)atoi(e);
    for (int i = 0; i < NSLOT; i++) slot_init(&g_slot[i]);
    atomic_store(&g_stop, 0);
    if (plat_thr_start(&g_thread, completion_main, NULL)) g_thread = 0;
    if (g_wd_ms && plat_thr_start(&g_wd_thread, watchdog_main, NULL)) g_wd_thread = 0;
    return g_thread ? 0 : -1;
}

void vks_shutdown(void) {
    if (!g_thread) return;
    vks_finish();
    atomic_store(&g_stop, 1); plat_thr_join(g_thread, 5000); g_thread = 0;
    if (g_wd_thread) { plat_thr_join(g_wd_thread, 2000); g_wd_thread = 0; }
    for (int i = 0; i < NSLOT; i++) { if (g_slot[i].pool) vkDestroyCommandPool(g_vk.dev, g_slot[i].pool, NULL); free(g_slot[i].posts); memset(&g_slot[i], 0, sizeof g_slot[i]); }
    g_norder = 0; g_cur = -1;
}

void vks_begin(void) {
    pthread_mutex_lock(&g_lk);
    if (g_cur >= 0) VKRED("submission begun while another is being recorded");
    else { g_cur = slot_take(); g_cur_tid = plat_thread_id(); }
    pthread_mutex_unlock(&g_lk);
}
static int cur_here(void) { return g_cur >= 0 && plat_thread_id() == g_cur_tid; }
int vks_recording(void) { return g_cur >= 0; }
VkCommandBuffer vks_cb(void) { return g_cur >= 0 ? g_slot[g_cur].main : VK_NULL_HANDLE; }
VkCommandBuffer vks_upload_cb(void) { if (!cur_here()) return VK_NULL_HANDLE; g_slot[g_cur].up_used = 1; return g_slot[g_cur].up; }
uint64_t vks_recording_value(void) { return g_cur >= 0 ? atomic_load(&g_vk.tl_assigned) + 1 : 0; }

static void post_add(post_t p) {
    if (g_cur < 0) { VKRED("completion effect posted outside a submission (kind %d): applied now", p.kind); post_run(&p); return; }
    slot_t *s = &g_slot[g_cur];
    if (s->nposts == s->cap) { s->cap = s->cap ? s->cap * 2 : 16; s->posts = realloc(s->posts, s->cap * sizeof *s->posts); }
    s->posts[s->nposts++] = p; atomic_fetch_add(&g_posted, 1);
}
void vks_post_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { post_add((post_t){1, val, mode, v, 0, 0, NULL, NULL}); }
void vks_post_counter(uint64_t addr, uint32_t type) { post_add((post_t){2, NULL, 0, 0, addr, type, NULL, NULL}); }
void vks_post_fn(void (*fn)(void *), void *arg) { post_add((post_t){3, NULL, 0, 0, 0, 0, fn, arg}); }

// Commits slot i: ends its command buffers (a barrier ends the uploads), assigns the next timeline value, queues it in order. Caller holds g_lk
static uint64_t commit_locked(int i) {
    slot_t *s = &g_slot[i];
    if (s->up_used) vk_mem_barrier(s->up, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    s->value = atomic_fetch_add(&g_vk.tl_assigned, 1) + 1;
    vk_marker_end(s->main, s->value);
    vkEndCommandBuffer(s->up); vkEndCommandBuffer(s->main);
    s->t_commit = now_ns(); s->state = S_PENDING;
    void vkm_committed(int owner, uint64_t value); vkm_committed(i, s->value);   // staging the slot used lives until this value
    g_order[g_norder++] = i;
    atomic_fetch_add(&g_subs, 1);
    return s->value;
}
uint64_t vks_commit(void) {
    pthread_mutex_lock(&g_lk);
    if (g_cur < 0) { pthread_mutex_unlock(&g_lk); VKRED("commit without a submission"); return atomic_load(&g_vk.tl_assigned); }
    if (!g_slot[g_cur].up_used) atomic_fetch_add(&g_empty, 1);
    uint64_t v = commit_locked(g_cur); g_cur = -1;
    int pend = 0; for (int k = 0; k < g_norder; k++) pend += g_slot[g_order[k]].state == S_PENDING;
    if (pend >= g_batch) flush_locked();
    pthread_mutex_unlock(&g_lk);
    return v;
}

int vks_wait(uint64_t fence, uint64_t timeout_ns) {
    if (!fence || atomic_load(&g_vk.tl_done) >= fence) return 1;
    if (atomic_load(&g_vk.tl_submitted) < fence) vks_flush();
    if (!timeout_ns) return atomic_load(&g_vk.tl_done) >= fence;
    uint64_t t0 = now_ns();
    pthread_mutex_lock(&g_lk);
    while (atomic_load(&g_vk.tl_done) < fence) {
        uint64_t el = now_ns() - t0;
        if (timeout_ns != UINT64_MAX && el >= timeout_ns) break;
        unsigned ms = timeout_ns == UINT64_MAX ? 100 : (unsigned)((timeout_ns - el) / 1000000 + 1);
        plat_cond_wait_ms(&g_cv, &g_lk, (unsigned)(ms < 100 ? ms : 100));
    }
    pthread_mutex_unlock(&g_lk);
    return atomic_load(&g_vk.tl_done) >= fence;
}
void vks_finish(void) { vks_flush(); vks_wait(atomic_load(&g_vk.tl_assigned), UINT64_MAX); }

void vks_stats(uint64_t *subs, uint64_t *qsubs, uint64_t *empty, uint64_t *posted) {
    *subs = atomic_load(&g_subs); *qsubs = atomic_load(&g_qsubs); *empty = atomic_load(&g_empty); *posted = atomic_load(&g_posted);
}

// ---------- One-shot command buffers (bring-up, readbacks, tests) on a slot of their own, ordered with everything else ----------
static __thread int t_once = -1;
int vks_owner(void) { return t_once >= 0 ? t_once : cur_here() ? g_cur : -1; }   // the slot this thread records into (-1 = none): staging allocations belong to it
VkCommandBuffer vk_once_begin(void) {
    pthread_mutex_lock(&g_lk); t_once = slot_take(); pthread_mutex_unlock(&g_lk);
    return g_slot[t_once].main;
}
int vk_once_end(VkCommandBuffer cb) {
    (void)cb;
    pthread_mutex_lock(&g_lk); uint64_t v = commit_locked(t_once); t_once = -1; flush_locked(); pthread_mutex_unlock(&g_lk);
    int ok = vks_wait(v, 20000000000ull);
    if (!ok) VKRED("one-shot submission %llu did not complete in 20 s", (unsigned long long)v);
    return ok ? 0 : -1;
}
