// from nxnative dm/host/hle_guestthread.c (MIT, © seedds): host-initiated guest threads (host background work that calls the game's own allocator / locks,
// such as the audio renderer's render thread, needs a complete SDK thread context).
// Approach: on a guest thread call the SDK's nn::os::CreateThread / StartThread, with a host function as the entry -- the thread is created by the SDK, so
// ThreadType, thread handle and TLS slots are all in place, and the game's malloc / nn::os locks work as usual (the lock tag word holds this thread's guest handle).
// Windows: the entry is a run-time host-call token (nxrt_host_fn_token) whose adapter runs the host function with x0 as its argument; SDK calls go through GUEST_CALL.
// Constraints: must be called on a guest thread (SDK thread creation goes through rt_svc); not exposed to the game.
// Self-test DM_GUEST_SPAWN_TEST=1: start one thread that does "SDK malloc/free + lock an nn::os mutex and check that the owner is itself", and report the result.
#include "hle.h"
#include "nxrt/runtime.h"
#include <stdatomic.h>

static void *need(const char *n) { void *p = guest_sym(n); if (!p) die("host guest thread: missing symbol %s", n); return p; }
// an entry shim: first register the thread as host-created (hle_thread.c dm_yield_to_core_peers does not wait for them), then run fn
typedef struct { void (*fn)(void *); void *arg; } spawn_arg;
typedef struct { _Alignas(16) uint8_t tt[0x200]; void *stack_alloc; } host_thread;
void dm_thread_mark_host(void);
static void spawn_tramp(void *p) { spawn_arg a = *(spawn_arg *)p; free(p); dm_thread_mark_host(); a.fn(a.arg); }
static void adapter_spawn_tramp(NxwGuestCpu *cpu) { spawn_tramp((void *)(uintptr_t)cpu->x[0]); }
void *dm_guest_thread_spawn(void (*fn)(void *), void *arg, int prio, size_t stack_size) {
    spawn_arg *sa = malloc(sizeof *sa); if (!sa) die("host guest thread: allocation failed"); sa->fn = fn; sa->arg = arg;
    uint64_t entry = nxrt_host_fn_token("host guest thread entry", adapter_spawn_tramp);
    stack_size = (stack_size + 0xfff) & ~(size_t)0xfff;
    host_thread *t = calloc(1, sizeof *t);                       // ThreadType is first, 0x1c0 bytes with headroom
    if (!t) die("host guest thread: allocation failed");
    // CreateThread borrows this range through MapMemory. Host malloc has no guest backing/state; use SDK malloc, retaining the raw pointer until
    // DestroyThread copies back.
    t->stack_alloc = (void *)(uintptr_t)GUEST_CALL(need("malloc"), stack_size + 0xfff);
    if (!t->stack_alloc) die("host guest thread: stack allocation failed");
    void *stack = (void *)(((uintptr_t)t->stack_alloc + 0xfff) & ~(uintptr_t)0xfff);
    int rc = (int)GUEST_CALL(need("_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi"), (uint64_t)(uintptr_t)t->tt, entry, (uint64_t)(uintptr_t)sa,
                             (uint64_t)(uintptr_t)stack, stack_size, (uint64_t)prio);
    if (rc) die("host guest thread: CreateThread failed %#x", rc);
    GUEST_CALL(need("_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE"), (uint64_t)(uintptr_t)t->tt);
    return t;
}
void dm_guest_thread_join(void *tt) {
    GUEST_CALL(need("_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE"), (uint64_t)(uintptr_t)tt);
    GUEST_CALL(need("_ZN2nn2os13DestroyThreadEPNS0_10ThreadTypeE"), (uint64_t)(uintptr_t)tt);
    host_thread *t = tt;
    GUEST_CALL(need("free"), (uint64_t)(uintptr_t)t->stack_alloc);
    free(t);
}

static struct { int malloc_ok, lock_owned, handle; } g_st;
static void selftest_body(void *arg) {
    (void)arg;
    void *p = (void *)(uintptr_t)GUEST_CALL(need("malloc"), 4096);
    if (p) { memset(p, 0x5a, 4096); GUEST_CALL(need("free"), (uint64_t)(uintptr_t)p); g_st.malloc_ok = 1; }
    _Alignas(16) uint8_t mtx[0x40] = {0};           // nn::os::MutexType (0x20 bytes, with headroom)
    GUEST_CALL(need("_ZN2nn2os15InitializeMutexEPNS0_9MutexTypeEbi"), (uint64_t)(uintptr_t)mtx, 0, 0);
    GUEST_CALL(need("_ZN2nn2os9LockMutexEPNS0_9MutexTypeE"), (uint64_t)(uintptr_t)mtx);
    g_st.lock_owned = (int)(GUEST_CALL(need("_ZN2nn2os28IsMutexLockedByCurrentThreadEPKNS0_9MutexTypeE"), (uint64_t)(uintptr_t)mtx) & 1);
    GUEST_CALL(need("_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE"), (uint64_t)(uintptr_t)mtx);
    uint32_t thr_cur_handle(void); g_st.handle = (int)thr_cur_handle();
}
void dm_guest_spawn_selftest(void) {
    const char *e = getenv("DM_GUEST_SPAWN_TEST"); if (!e) return;
    void *tt = dm_guest_thread_spawn(selftest_body, NULL, 16, 64 << 10);
    dm_guest_thread_join(tt);
    uint32_t thr_cur_handle(void); int mine = (int)thr_cur_handle();
    int ok = g_st.malloc_ok && g_st.lock_owned && g_st.handle && g_st.handle != mine;
    LOG("%shost guest thread self-check: SDK malloc/free %s; mutex owner is this thread %s; thread handle %#x (main thread %#x)", ok ? "" : "[RED] ",
        g_st.malloc_ok ? "ok" : "broken", g_st.lock_owned ? "yes" : "no", g_st.handle, mine);
}
