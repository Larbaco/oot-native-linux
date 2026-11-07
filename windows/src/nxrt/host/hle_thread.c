// from nxnative dm/host/hle_thread.c (MIT, © seedds)
// Thread and synchronization SVCs of the Horizon kernel, implemented natively: each guest thread = one host thread owning an NxwGuestCpu; its guest code
// (recompiled) runs on the guest stack given by the SDK, its host frames on the Windows thread's own stack.
// The SDK's nn::os runs as is; only the "kernel" underneath it is replaced by the host:
//   Threads    0x08 CreateThread 0x09 StartThread 0x0A ExitThread 0x0B SleepThread 0x0C/0x0D priority 0x0E/0x0F core mask
//              0x10 GetCurrentProcessorNumber 0x25 GetThreadId 0x19 CancelSynchronization 0x32 SetThreadActivity 0x33 GetThreadContext3
//   Locks      0x1A ArbitrateLock 0x1B ArbitrateUnlock (tag word = owner handle | 0x40000000 has waiters)
//   Condition  0x1C WaitProcessWideKeyAtomic 0x1D SignalProcessWideKey
//   Address    0x34 WaitForAddress 0x35 SignalToAddress
//   Events     0x45 CreateEvent 0x11 SignalEvent 0x12 ClearEvent 0x17 ResetSignal 0x18 WaitSynchronization 0x16 CloseHandle
// Semantics follow the public Horizon kernel behavior (KConditionVariable / KAddressArbiter / KSynchronization of Atmosphère mesosphère).
// All waits hang on one global lock G, with one condition variable per thread; correctness first, performance to be measured later.
// Windows port: pthreads → plat_rwlock_t / pthread_cond_t (posix_win.h); Apple QoS classes → gentle Windows priorities (DESIGN D9, no core pinning: masks are
// bookkeeping); pausing a thread (SetThreadActivity) stops it in an SVC or at a safepoint (rt_yield), since its registers live in its NxwGuestCpu, not
// in the host's; thread names and dumps read guest memory with plat_read. Dropped: the hitch dump and the power-line CPU readings (Mach thread_info).
#include "dm.h"
#include "nxrt/runtime.h"
#include <stdatomic.h>

#define R_TIMEOUT     0xEA01u   // Kernel module 1, description 117
#define R_CANCELLED   0xEC01u   // 118
#define R_BAD_HANDLE  0xE401u   // 114
#define R_BAD_STATE   0xFA01u   // 125
#define R_OUT_OF_RES  0xCE01u   // 103
#define WAITERS_BIT   0x40000000u
#define H_CUR_THREAD  0xFFFF8000u
#define GUEST_THREAD_HOST_STACK (16u << 20)   // host stack reservation: the frames of recompiled code (guest frames live on the guest stack)

void *guest_thread_init(NxwGuestCpu *cpu);   // thread.c
void guest_thread_free(void *own);
void guest_thread_set_id(NxwGuestCpu *cpu, uint64_t id);
void crash_thread_init(void);

enum { O_FREE, O_THREAD, O_EVENT_R, O_EVENT_W };
// Service-layer SystemEvent bindings (see hle_bind_system_event): guest SystemEventType address → the write-end handle of its kernel event.
// An entry lives as long as that handle: CloseHandle on it removes the entry (se_unbind), so a SystemEvent destroyed and created again at the same address gets a new kernel event.
// Lock order: G, then g_se_lock (nothing takes G while holding g_se_lock)
static struct { void *ev; uint32_t wh; uint64_t id; } g_se[512];
static int g_nse;
static uint64_t g_next_se_id = 1;   // g_se_lock: binding identity survives neither address nor handle reuse
static pthread_mutex_t g_se_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t se_find(void *ev) {   // caller holds g_se_lock
    for (int i = 0; i < g_nse; i++) if (g_se[i].ev == ev) return g_se[i].wh;
    return 0;
}
static void se_unbind(uint32_t wh) {   // caller holds G
    pthread_mutex_lock(&g_se_lock);
    for (int i = 0; i < g_nse; ) if (g_se[i].wh == wh) g_se[i] = g_se[--g_nse]; else i++;
    pthread_mutex_unlock(&g_se_lock);
}
typedef struct event { int signaled; int refs; } event_t;
typedef struct thr {
    uint32_t handle; uint64_t id;
    uint64_t entry, arg, stack_top; int prio, core; uint64_t mask;
    uint64_t born_ns, exit_ns;
    int started, exited, cancel;
    uint32_t tid; pthread_cond_t cv;   // the host thread's id
    NxwGuestCpu *cpu; void *tls_own;    // its guest register file (the running fiber's, see nxrt_set_cur_cpu) and TLS blocks
    NxwGuestCpu *cpu_base;              // the register file the thread was created with (what thr_free frees)
    // Current wait: kind 0=none 1=lock 2=condition 3=address 4=synchronization object
    int wkind, woke; uint64_t waddr, wkey; uint32_t wtag; int64_t wres;
    _Atomic int sleeping;      // inside SleepThread's timed sleep (not runnable; used by dm_yield_to_core_peers)
    int yield_pending;         // this caller woke a higher-priority thread pinned to its core: hand off before returning to guest code
    _Atomic uint64_t hand_epoch;   // this thread's current handoff: threads it makes runnable belong to it until its next thread SVC returns (svc_thread)
    struct { struct thr *by; uint64_t epoch; } hand[4]; int nhand;   // the handoffs this thread was made runnable for (its waker's, then those the waker belonged to), until it blocks
    _Atomic int *host_work;    // points at the thread's dm_host_work (registered when the thread starts)
    int native_wait;          // nesting depth of a blocking native HLE wait (G held)
    int host_owned;            // a guest thread the host created itself: it waits in host code and is not one of the game's threads
    uint64_t wseq;             // Sequence number of wait registration: at equal priority first come, first woken (same order as the Horizon kernel's KThread wait tree)
    struct thr *next;          // Wait chain
    struct thr *all;           // All-threads chain (for the state dump)
    int last_svc; uint64_t last_lr;
    int closed;                // Handle closed: the object is freed only when this holds together with exited (Horizon: the thread object goes away with the last reference)
    uint64_t nsvc;             // SVC count (for the state dump)
    uint64_t nyield, nbackoff;   // Yields (SleepThread 0/-1/-2) and those the back-off turned into short sleeps
    uint64_t nspin, spin_site; const char *spin_mod;   // Spin wait passes (dm_spin_wait; those that yield count in nyield too) and where the last one was
    uint64_t srun_svc, srun_t0, spin_ret;     // Current spin wait: nsvc at its passes, when it began, when the last pass returned
    uint64_t yrun_svc, yrun_t0, yrun_sig, yrun_sig2, poll_ret;   // Current run of yields (thr_yield)
    // Pausing (SetThreadActivity, used by garbage collectors to stop the world): pause_req = requested, paused = stopped. A thread stops inside an SVC (its
    // NxwGuestCpu then holds every register) or at a safepoint (rt_yield); pcv lets the requester wait for "stopped" and the paused thread wait for "resume"
    _Atomic int pause_req; int paused;
    uint64_t *_Atomic svc_regs; uint64_t svc_pc;
    // SVC exit without G: bit 0 = inside an SVC, bit 1 = a pause was requested. The exit clears bit 0 with one compare-and-swap when bit 1 is clear, and takes G only otherwise
    _Atomic int svc_state;
    const uint32_t *whs; int whn;   // WaitSynchronization's handle array (guest memory, valid while the thread waits there): a signal wakes only waiters that hold the signalled object
    pthread_cond_t pcv;
} thr_t;
static thr_t *g_all;
_Thread_local _Atomic int dm_host_work;
static _Thread_local thr_t *t_self;               // nxnative: pthread_getspecific(g_keythr)
static _Thread_local NxwGuestCpu *t_cpu;          // the calling thread's register file
static _Atomic uint64_t g_preempt_n, g_preempt_ns;   // same-core handoffs: count and total wait (svc_thread)
static int g_core_preempt;   // also check already-runnable peers after every thread SVC (profile / DM_PREEMPT)

static pthread_mutex_t G = PTHREAD_MUTEX_INITIALIZER;
#define MAX_H 4096
static struct { int type; void *obj; } g_h[MAX_H];
static uint64_t g_next_id = 0x51;
static thr_t *g_wait;          // All waiting threads (lock / condition / address / synchronization)

NxwGuestCpu *nxrt_cur_cpu(void) { return t_cpu; }
void nxrt_set_cur_cpu(NxwGuestCpu *c) { t_cpu = c; if (t_self) t_self->cpu = c; }   // fiber.c: a fiber that migrated runs on a cpu of its own, which is then the thread's

static uint32_t h_new(int type, void *obj) {     // Caller holds G
    for (int i = 1; i < MAX_H; i++) if (!g_h[i].type) { g_h[i].type = type; g_h[i].obj = obj; return 0x1000 + i; }
    die("handle table full");
}
static void *h_peek(uint32_t h, int type) {      // Lookup only, no report
    if (h == H_CUR_THREAD && type == O_THREAD) return t_self;
    unsigned i = h - 0x1000;
    if (i == 0 || i >= MAX_H || g_h[i].type != type) return NULL;
    return g_h[i].obj;
}
static void *h_get(uint32_t h, int type) {       // Reports if not found (a wrong handle is a real error)
    void *o = h_peek(h, type);
    if (!o) LOG("invalid handle %#x (wanted type %d)", h, type);
    return o;
}
static thr_t *cur(void) {
    thr_t *t = t_self;
    if (!t) die("thread SVC called from a non-guest thread");
    return t;
}
// Horizon's per-core priority ordering: a woken higher-priority thread pinned to the caller's core runs until it blocks before the signalling thread continues.
// svc_thread calls it after waking such a thread. Wait for guest blocking, not a host dispatch snapshot or a wall-clock deadline: neither establishes that the work has finished.
void dm_thread_mark_host(void) { thr_t *t = t_self; if (t) t->host_owned = 1; }
// Holding G. wake() clears wkind before the thread resumes; it must already count as runnable in that window.
// Explicit host GPU work and contended native codec locks do not occupy a guest core. Ordinary host dispatch delays and HLE calls do, until a guest wait / sleep / exit says otherwise.
static int thr_runnable(const thr_t *t) {
    return !t->wkind && !t->sleeping && !t->native_wait && !t->pause_req && !(t->host_work && *t->host_work);
}
// Holding G. Whether t was made runnable for self's current handoff (by self, or by a thread that was) and has not blocked since
static int thr_in_handoff(const thr_t *t, const thr_t *self) {
    for (int i = 0; i < t->nhand; i++) if (t->hand[i].by == self && t->hand[i].epoch == self->hand_epoch) return 1;
    return 0;
}
// Holding G. t blocked (a guest wait, a timed sleep, YieldToAnyThread): it gave its core back, so it no longer holds the threads whose handoff made it runnable
static void hand_leave(thr_t *t) { t->nhand = 0; }
// A native HLE condition wait is a guest blocking boundary, like a kernel wait. Called with the HLE's condition mutex held: the waiter cannot miss a signal
// between publishing its blocked state and the condition wait releasing that mutex. On return it is runnable before it goes back to guest code.
void dm_thread_native_wait(int entering) {
    thr_t *t = t_self;
    if (!t) return;
    pthread_mutex_lock(&G);
    if (entering) { t->native_wait++; hand_leave(t); }
    else { if (!t->native_wait) die("unbalanced native HLE wait"); t->native_wait--; }
    pthread_mutex_unlock(&G);
}

// The peers a handoff waits for are the ones it made runnable, not every runnable higher-priority thread of the core (the host has no per-core scheduler; core_preempt aside).
// A caller whose affinity mask allows another core does not wait: Horizon's scheduler moves a preempted thread to another core of its mask that runs nothing of higher priority.
// all = 1 (core_preempt): every runnable higher-priority peer
int dm_yield_to_core_peers(int all) {
    thr_t *self = t_self; if (!self) return 0;
    uint64_t bit = 1ull << (self->core & 63); int waited = 0;
    if (self->mask & ~bit) return 0;
    for (;;) {
        int busy = 0;
        pthread_mutex_lock(&G);
        for (thr_t *t = g_all; t && !busy; t = t->all)
            if (t != self && t->started && !t->exited && !t->host_owned && t->mask == bit && t->prio < self->prio && thr_runnable(t) && (all || thr_in_handoff(t, self))) busy = 1;
        pthread_mutex_unlock(&G);
        if (!busy) return waited;
        plat_sleep_ns(50000); waited = 1;
    }
}
// Take a host lock that an HLE call holds while it runs guest code. A guest thread waiting for it is in a native HLE wait (dm_thread_native_wait), blocked as it would be
// on the guest-visible lock the SDK's own code uses there, so it does not hold a same-core handoff
void dm_host_lock(pthread_mutex_t *m) {
    if (!pthread_mutex_trylock(m)) return;
    dm_thread_native_wait(1);
    pthread_mutex_lock(m);
    dm_thread_native_wait(0);
}

// ---------- Wait primitives (called holding G) ----------
static void wait_unlink(thr_t *t) {
    for (thr_t **p = &g_wait; *p; p = &(*p)->next) if (*p == t) { *p = t->next; break; }
    t->next = NULL;
}
// Pick a waiter in kernel order: numerically lower priority first, at equal priority the earlier registered first (holding G). kind 1 matches by lock address, 2 by condition key, 3 by address
static uint64_t g_wseq;
static thr_t *pick(int kind, uint64_t a) {
    thr_t *b = NULL;
    for (thr_t *t = g_wait; t; t = t->next) {
        if (t->wkind != kind || (kind == 2 ? t->wkey : t->waddr) != a) continue;
        if (!b || t->prio < b->prio || (t->prio == b->prio && t->wseq < b->wseq)) b = t;
    }
    return b;
}
// Holding G. Starting a thread makes it runnable just as waking a waiter does; neither operation promises that an equal-priority or other-core peer has run.
// t joins the waker's current handoff and those the waker is part of
static void note_runnable(thr_t *t) {
    thr_t *self = t_self;
    t->nhand = 0;
    if (!self || self == t || t->host_owned) return;
    t->hand[t->nhand].by = self; t->hand[t->nhand++].epoch = self->hand_epoch;
    for (int i = 0; i < self->nhand && t->nhand < 4; i++)
        if (self->hand[i].by != t && self->hand[i].by->hand_epoch == self->hand[i].epoch) t->hand[t->nhand++] = self->hand[i];   // only handoffs still in progress
    if (t->prio < self->prio && t->mask == 1ull << (self->core & 63) && !(self->mask & ~t->mask)) self->yield_pending = 1;   // a waker that can run on another core is not held
}
static void wake(thr_t *t, int64_t res) {
    wait_unlink(t); t->wkind = 0; t->woke = 1; t->wres = res; pthread_cond_signal(&t->cv);
    note_runnable(t);
}
static void wait_until(uint64_t dl, const int *woke, int64_t asked);
// Suspend until woken or timed out (ns<0 infinite, 0 immediate timeout); returns wres or R_TIMEOUT
static int64_t block(thr_t *t, int kind, int64_t ns) {
    t->wkind = kind; t->woke = 0; t->next = g_wait; g_wait = t; t->wseq = ++g_wseq;
    if (ns == 0) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
    hand_leave(t);
    int det_wait = ns > 0 && dm_detclock_enabled();
    uint64_t guest_deadline = det_wait ? guest_ns() + (uint64_t)ns : 0, deadline = ns > 0 && !det_wait ? now_ns() + (uint64_t)ns : 0;
    while (!t->woke) {
        int rc = 0;
        if (det_wait) {
            uint64_t now = guest_ns();
            if (now >= guest_deadline) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
            rc = pthread_cond_wait(&t->cv, &G);
        } else if (ns > 0) {
            // Horizon's timed waits end on the tick, as nxnative's do on the Mac; a Windows condition variable waits in whole milliseconds (timeBeginPeriod(1))
            // and wakes up to a millisecond late. So the condition variable waits in whole milliseconds while more than 2.5 ms remain, and the last stretch is
            // wait_until's (G released; it ends at a wake as well, which the loop then finds under G), after which the wait has timed out
            uint64_t now = now_ns();
            if (now >= deadline) rc = ETIMEDOUT;
            else if (deadline - now > 2500000) plat_cond_wait_ms(&t->cv, &G, (unsigned)(((deadline - now - 1500000) / 1000000)));
            else { pthread_mutex_unlock(&G); wait_until(deadline, &t->woke, ns); pthread_mutex_lock(&G); rc = ETIMEDOUT; }
        } else rc = pthread_cond_wait(&t->cv, &G);
        if (rc == ETIMEDOUT && !t->woke && (!det_wait || guest_ns() >= guest_deadline)) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
    }
    return t->wres;
}

// ---------- Locks ----------
static inline uint32_t ld32(uint64_t a) { return __atomic_load_n((uint32_t *)a, __ATOMIC_SEQ_CST); }
static inline uint64_t ld64(uint64_t a) { return __atomic_load_n((uint64_t *)a, __ATOMIC_SEQ_CST); }
static inline void st32(uint64_t a, uint32_t v) { __atomic_store_n((uint32_t *)a, v, __ATOMIC_SEQ_CST); }
static inline int cas32(uint64_t a, uint32_t *exp, uint32_t v) { return __atomic_compare_exchange_n((uint32_t *)a, exp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }

// Release the lock at addr: hand it to the first thread waiting on it, or clear it to zero (holding G)
static void lock_handoff(uint64_t addr) {
    thr_t *w = pick(1, addr); int more = 0;
    if (!w) { st32(addr, 0); return; }
    for (thr_t *t = g_wait; t; t = t->next) if (t != w && t->wkind == 1 && t->waddr == addr) { more = 1; break; }
    st32(addr, w->wtag | (more ? WAITERS_BIT : 0));
    wake(w, 0);
}
// Make t take the lock at addr: if free, it gets it directly and is woken, otherwise set the waiters bit and turn into a lock wait (holding G)
static void lock_acquire_for(thr_t *t, uint64_t addr, uint32_t tag) {
    uint32_t v = ld32(addr);
    for (;;) {
        if (v == 0) { if (cas32(addr, &v, tag)) { wake(t, 0); return; } continue; }
        if (cas32(addr, &v, v | WAITERS_BIT)) break;
    }
    t->wkind = 1; t->waddr = addr; t->wtag = tag;      // Still in g_wait, changed to a lock wait
    t->wseq = ++g_wseq;                                  // The kernel inserts it anew into the lock owner's wait queue: at equal priority it goes after the existing lock waiters
}

// ---------- Threads ----------
uint32_t (*dm_thread_tick_fn)(void);
static const char *thr_name(thr_t *t, char *buf, size_t n);
// The guest entry: the host has no use for a return (Horizon threads exit with ExitThread)
static void thread_enter(thr_t *t) {
    NxwGuestCpu *c = t->cpu;
    c->x[0] = t->arg; c->sp = t->stack_top; c->x[29] = 0; c->x[30] = NXRT_RETURN_TO_HOST; c->pc = t->entry;
    NxwGuestFn fn = nxrt_code_lookup(t->entry);
    fn(c);
    die("guest thread %#x entry function returned (Horizon requires ExitThread)", t->handle);
}
static unsigned thread_main(void *p) {
    thr_t *t = p;
    t_self = t; t_cpu = t->cpu;
    crash_thread_init();
    pthread_mutex_lock(&G);
    t->host_work = &dm_host_work;
    plat_thread_set_priority(t->tid = plat_thread_id(), t->prio);
    pthread_mutex_unlock(&G);
    t->tls_own = guest_thread_init(t->cpu); guest_thread_set_id(t->cpu, t->id);
    pthread_mutex_lock(&G); while (!t->started) pthread_cond_wait(&t->cv, &G); pthread_mutex_unlock(&G);
    { char nb[48], hn[64]; snprintf(hn, sizeof hn, "%#x %s", t->handle, thr_name(t, nb, sizeof nb)); pthread_setname_np(hn); }   // the guest's name (set before StartThread)
    thread_enter(t);   // Does not return: the guest must exit through ExitThread
    return 0;
}
static void thr_free(thr_t *t) {   // Holding G; unlink from the all-threads chain and free
    for (thr_t **p = &g_all; *p; p = &(*p)->all) if (*p == t) { *p = t->all; break; }
    for (thr_t *u = g_all; u; u = u->all)   // no handoff entry may point at the freed object (note_runnable reads the waker's epoch)
        for (int i = 0; i < u->nhand; ) if (u->hand[i].by == t) u->hand[i] = u->hand[--u->nhand]; else i++;
    plat_aligned_free(t->cpu_base); free(t);
}
static void wake_sync_waiters(const void *obj);
static _Noreturn void thread_exit(thr_t *t) {
    t->exit_ns = now_ns();
    pthread_mutex_lock(&G);
    t->exited = 1;
    wake_sync_waiters(t);   // Synchronization waiters on this thread re-check
    void *own = t->tls_own; t->tls_own = NULL;
    t_self = NULL; t_cpu = NULL;
    if (t->closed) thr_free(t);
    pthread_mutex_unlock(&G);
    guest_thread_free(own);
    plat_thread_exit();
}

// SetThreadActivity / GetThreadContext3 (garbage collectors stopping the world). A thread stops:
//   · inside an SVC (its NxwGuestCpu holds every register): stopped at once, context = that frame; if still paused when the SVC returns, it waits at the exit for the resume;
//   · in guest code: at its next SVC or safepoint (rt_yield), where the recompiled code has stored its registers. Recompiled code with no SVC or safepoint in a loop
//     cannot be stopped there (nxnative sent SIGUSR2 and read the real registers, which are not the guest's here); the requester warns after a second.
// called on SVC entry / exit (rt_svc in rt.c)
void thr_svc_enter(uint64_t *regs, uint64_t pc) {
    thr_t *t = t_self; if (!t) return;
    t->svc_pc = pc; atomic_store(&t->svc_regs, regs); atomic_fetch_or(&t->svc_state, 1);
    if (atomic_load(&t->pause_req)) { pthread_mutex_lock(&G); if (t->pause_req && !t->paused) { t->paused = 1; pthread_cond_broadcast(&t->pcv); } pthread_mutex_unlock(&G); }
}
void thr_svc_leave(void) {
    thr_t *t = t_self; if (!t) return;
    int in = 1; if (atomic_compare_exchange_strong(&t->svc_state, &in, 0)) { atomic_store(&t->svc_regs, NULL); return; }   // no pause requested: G is not needed
    pthread_mutex_lock(&G);
    while (t->pause_req) { t->paused = 1; pthread_cond_broadcast(&t->pcv); pthread_cond_wait(&t->pcv, &G); }
    t->paused = 0; atomic_fetch_and(&t->svc_state, ~1); atomic_store(&t->svc_regs, NULL);
    pthread_mutex_unlock(&G);
}
// A safepoint in guest code (rt_yield): a requested pause stops here with the cpu as the context
void thr_safepoint(void) {
    thr_t *t = t_self;
    if (!t || !atomic_load(&t->pause_req)) return;
    thr_svc_enter((uint64_t *)t->cpu, t->cpu->pc);
    thr_svc_leave();
}
// Horizon svc::ThreadContext (arm64, 0x320 bytes)
typedef struct { uint64_t r[29], fp, lr, sp, pc; uint32_t pstate, pad; _Alignas(16) uint8_t v[32][16]; uint32_t fpcr, fpsr; uint64_t tpidr; } hz_ctx_t;
_Static_assert(sizeof(hz_ctx_t) == 0x320, "ThreadContext");
static void ctx_fill(const thr_t *t, hz_ctx_t *o) {   // holding G, t is stopped
    memset(o, 0, sizeof *o);
    const NxwGuestCpu *c = (const NxwGuestCpu *)atomic_load(&t->svc_regs);
    if (c) {   // the frame: x0..x30 at +0, q0..q31 at +256, nzcv / fpsr / fpcr at +768 / +776 / +784, sp at +792 (the guest's real sp: nxnative's gate frame held the gate block's)
        for (int i = 0; i < 29; i++) o->r[i] = c->x[i];
        o->fp = c->x[29]; o->lr = c->x[30]; o->sp = c->sp; o->pc = t->svc_pc; o->pstate = (uint32_t)c->nzcv;
        memcpy(o->v, c->q, sizeof o->v); o->fpsr = (uint32_t)c->fpsr; o->fpcr = (uint32_t)c->fpcr; o->tpidr = c->tpidr_el0;
    }
}

uint32_t nxrt_svc_thread_main_init(void);
// Register the main thread and return its handle (passed to nnosInitialize as the main thread handle). Its cpu (nxrt_set_cur_cpu) is set up by the caller
uint32_t nxrt_svc_thread_main_init(void) {
    { const char *e = getenv("DM_PREEMPT"); g_core_preempt = e ? atoi(e) != 0 : DM_GAME_CORE_PREEMPT; }
    thr_t *t = calloc(1, sizeof *t);
    if (!t || !t_cpu) die("svc_thread_main_init: no guest cpu on this thread");
    // Its affinity is its ideal core alone, as Horizon creates every thread (Mesosphere KThread::Initialize)
    t->started = 1; t->prio = 44; t->core = 0; t->mask = 1; t->tid = plat_thread_id(); t->host_work = &dm_host_work; t->cpu = t->cpu_base = t_cpu;
    plat_thread_set_priority(t->tid, t->prio);
    pthread_mutex_lock(&G); t->handle = h_new(O_THREAD, t); t->id = g_next_id++; t->all = g_all; g_all = t; pthread_mutex_unlock(&G);
    t_self = t;
    guest_thread_set_id(t->cpu, t->id);
    pthread_setname_np("guest main thread");
    LOG("main guest thread %#x (priority %d, Windows priority %d)", t->handle, t->prio, plat_priority_for(t->prio));
    return t->handle;
}

// ---------- Synchronization objects ----------
static int sig_obj(uint32_t h, int *bad) {    // 1 = signaled (holding G)
    unsigned i = h - 0x1000;
    if (h == H_CUR_THREAD) return 0;
    if (i == 0 || i >= MAX_H) { *bad = 1; return 0; }
    switch (g_h[i].type) {
    case O_THREAD: return ((thr_t *)g_h[i].obj)->exited;
    case O_EVENT_R: return ((event_t *)g_h[i].obj)->signaled;
    default: *bad = 1; return 0;
    }
}
// A synchronization object became signaled (an event, or a thread that exited): wake the WaitSynchronization waiters whose handle set holds it, so they re-check (holding G)
static void wake_sync_waiters(const void *obj) {
    for (thr_t *w = g_wait; w; ) { thr_t *n = w->next;
        if (w->wkind == 4) { int hit = !w->whs;
            for (int i = 0; i < w->whn && !hit; i++) { unsigned k = w->whs[i] - 0x1000; hit = k > 0 && k < MAX_H && g_h[k].obj == obj; }
            if (hit) wake(w, -1); }
        w = n; }
}

static int64_t now_mono_ns(void) { return (int64_t)now_ns(); }
void dm_guest_clock_advanced(void) {
    if (!dm_detclock_enabled()) return;
    pthread_mutex_lock(&G);
    for (thr_t *t = g_wait; t; t = t->next) if (t->wkind) pthread_cond_signal(&t->cv);
    pthread_mutex_unlock(&G);
}

static const char *thr_name(thr_t *t, char *nb, size_t n) {   // SDK ThreadType+0x1a0 = name pointer
    if (!t->arg) return t->id == 0x51 ? "main thread" : "?";
    uint64_t np = 0;
    if (plat_read(t->arg + 0x1a0, &np, 8) || !np) return "?";
    size_t k = 0;
    for (; k + 1 < n; k++) { char ch; if (plat_read(np + k, &ch, 1) || !ch) break; nb[k] = ch; }
    nb[k] = 0; return k ? nb : "?";
}
// The game's threads that hold a slot of the process's thread resource limit, as Horizon counts them: created (started or not) and not yet exited (GetInfo FreeThreadCount, hle_svc.c).
// Guest threads the host spawns for itself are not the game's
int thr_limit_used(void) {
    int n = 0;
    pthread_mutex_lock(&G);
    for (thr_t *t = g_all; t; t = t->all) n += !t->exited && !t->host_owned;
    pthread_mutex_unlock(&G);
    return n;
}
uint32_t thr_cur_handle(void) { thr_t *t = t_self; return t ? t->handle : 0; }
void thr_counts(int *handles, int *live, int *objs) {
    int nh = 0, nl = 0, no = 0;
    pthread_mutex_lock(&G);
    for (int i = 1; i < MAX_H; i++) nh += g_h[i].type != O_FREE;
    for (thr_t *t = g_all; t; t = t->all) { no++; nl += t->started && !t->exited; }
    pthread_mutex_unlock(&G);
    *handles = nh; *live = nl; *objs = no;
}
// State dump (crash report, watchdog): what each guest thread is waiting on, where its most recent SVC came from, its guest pc and frame chain.
// Takes no lock: it runs when something may already hold G
void svc_thread_dump(void) {
    static const char *kinds[] = {"running/not waiting", "mutex", "condvar", "address", "sync object"};
    int nh = 0; for (int i = 1; i < MAX_H; i++) nh += g_h[i].type != O_FREE;
    LOG("handle table use %d / %d, threads created so far %llu, same-core handoffs %llu (%.1f ms)", nh, MAX_H, (unsigned long long)(g_next_id - 0x51),
        (unsigned long long)g_preempt_n, g_preempt_ns / 1e6);
    for (thr_t *t = g_all; t; t = t->all) {
        char nb[32], lr[96] = "?", pc[96] = "?"; const char *nm = thr_name(t, nb, sizeof nb);
        module_t *m = module_of((void *)(uintptr_t)t->last_lr);
        if (m) snprintf(lr, sizeof lr, "%s+%#llx", m->name, (unsigned long long)(t->last_lr - (uint64_t)(uintptr_t)m->base));
        if (t->cpu && !t->exited) { module_t *pm = module_of((void *)(uintptr_t)t->cpu->pc);
            if (pm) snprintf(pc, sizeof pc, "%s+%#llx", pm->name, (unsigned long long)(t->cpu->pc - (uint64_t)(uintptr_t)pm->base)); }
        char own[96] = "";
        if (t->wkind == 1 && t->waddr) { uint32_t w = 0; if (!plat_read(t->waddr, &w, 4)) snprintf(own, sizeof own, " owner %#x", w & ~WAITERS_BIT); }
        if (t->wkind == 2 && t->waddr) { uint32_t w = 0, k = 0; plat_read(t->waddr, &w, 4); plat_read(t->wkey, &k, 4);
            snprintf(own, sizeof own, " lock word %#x condvar key %#llx=%#x wait #%llu", w, (unsigned long long)t->wkey, k, (unsigned long long)t->wseq); }
        LOG("thread %#x '%s' priority %d core %d mask %#llx CPU %.2fs svc %llu %s%s waits:%s addr=%#llx%s last svc %#x lr=%s pc=%s", t->handle, nm, t->prio, t->core,
            (unsigned long long)t->mask, t->exited ? 0.0 : plat_thread_cpu_seconds(t->tid), (unsigned long long)t->nsvc, t->exited ? "exited " : "", t->started ? "" : "not started ",
            kinds[t->wkind & 7], (unsigned long long)t->waddr, own, t->last_svc, lr, pc);
        if (t->cpu && !t->exited && t->wkind) dm_backtrace(t->cpu->x[29]);
    }
}
void nxrt_thread_dump(void) { svc_thread_dump(); }

// Same-core wake handoffs apply to every game. core_preempt additionally checks already-runnable peers after every thread SVC; DM_PREEMPT overrides that option.
// These are cooperative boundaries, not an asynchronous guest-core scheduler.
static int svc_thread_impl(int num, uint64_t *regs);
// Yield back-off: a guest loop that polls the tick and yields becomes a host busy spin. A run of yields with no other SVC between them (and no progress, thr_yield) is let through as
// a plain yield for its first 2 ms; after that each yield sleeps an eighth of the run so far, between 10 and 250 µs.
// DM_YIELD_BACKOFF=0 turns it off; DM_YIELD_BACKOFF=<cap µs>[:<start µs>] sets the cap and the run length after which it starts (default 250:2000)
static uint64_t yield_backoff_ns(uint64_t run_ns) {
    static int64_t cap = -1, start = 2000000;
    if (cap < 0) { const char *e = getenv("DM_YIELD_BACKOFF"); cap = 250000;
        if (e && *e) { cap = atoll(e) * 1000; const char *c = strchr(e, ':'); if (c) start = atoll(c + 1) * 1000; }
        if (!cap) LOG("DM_YIELD_BACKOFF=0: no back-off on repeated yields (plain yield spin)"); else if (e && *e) LOG("DM_YIELD_BACKOFF: back off after %lld µs of repeated yields, sleeping at most %lld µs at a time", (long long)(start / 1000), (long long)(cap / 1000)); }
    if (!cap || run_ns < (uint64_t)start) return 0;
    uint64_t s = run_ns / 8; if (s < 10000) s = 10000;
    return s < (uint64_t)cap ? s : (uint64_t)cap;
}
// A yield, or a short sleep once the wait has lasted run_ns past the back-off's start
static void yield_for(thr_t *me, uint64_t run_ns) {
    uint64_t bo = yield_backoff_ns(run_ns);
    if (bo) { me->nbackoff++; me->sleeping = 1; plat_sleep_ns(bo); me->sleeping = 0; }
    else sched_yield();
}
// One yield SVC of the calling thread (me may be NULL); regs = the guest registers at the SVC.
// A run of yields is a wait only while the loop makes no progress: it also starts again when the caller's callee-saved registers x19–x28 differ from the last two yields'
static void thr_yield(thr_t *me, const uint64_t *regs) {
    if (!me) { sched_yield(); return; }
    uint64_t y0 = now_ns(), sig = 0xcbf29ce484222325ull;
    for (int i = 19; i <= 28; i++) sig = (sig ^ regs[i]) * 0x100000001b3ull;
    int moved = me->yrun_svc + 1 == me->nsvc && me->yrun_sig != sig && me->yrun_sig2 != sig;   // the loop moved on since its last two yields
    me->nyield++; if (me->yrun_svc + 1 != me->nsvc || moved) me->yrun_t0 = y0; me->yrun_svc = me->nsvc; me->yrun_sig2 = me->yrun_sig; me->yrun_sig = sig;
    if (!moved) yield_for(me, y0 - me->yrun_t0);
    me->poll_ret = now_ns();
}
// Spin waits: a guest loop that polls memory with no SVC (nxanalyze's spin loops; the lifter calls rt_yield on their back edges). A wait (passes with no SVC between them
// and at most SPIN_GAP_NS of guest work between two) polls on for its first DM_SPIN_POLL µs (2000), and then sleeps as a run of yields does.
// DM_SPIN=0: no sleeps either. site = the loop head (guest address)
#define SPIN_GAP_NS 20000
void dm_spin_wait(uint64_t site) {
    static int on = -1; static uint64_t poll_ns;
    if (on < 0) { const char *e = getenv("DM_SPIN"), *p = getenv("DM_SPIN_POLL"); poll_ns = (p && *p ? strtoull(p, 0, 10) : 2000) * 1000; on = !(e && *e && atoi(e) == 0);
                  if (!on) LOG("DM_SPIN=0: spin waits do not yield or sleep"); else if (p && *p) LOG("DM_SPIN_POLL: spin waits poll for %llu µs, then yield", (unsigned long long)(poll_ns / 1000)); }
    thr_t *me = t_self;
    if (!me) return;
    uint64_t now = now_ns();
    if (me->srun_svc != me->nsvc || now - me->spin_ret > SPIN_GAP_NS) me->srun_t0 = now;   // a new wait
    me->srun_svc = me->nsvc; me->nspin++;
    if (site != me->spin_site || !me->spin_mod) { const module_t *m = module_of((void *)(uintptr_t)site); me->spin_mod = m ? m->name : "?"; me->spin_site = site; }
    if (on && now - me->srun_t0 >= poll_ns) { me->nyield++; yield_for(me, now - me->srun_t0); }
    me->spin_ret = now_ns();
}
// Short timed waits as polls. A wait with a timeout under 20 µs (a condition variable, WaitSynchronization or WaitForAddress) that times out is a poll, as a SleepThread that short is:
// it continues the run of yields, and once the run has lasted the back-off's start its timeout becomes the back-off's sleep. → the timeout to wait with
static int64_t poll_wait_begin(int64_t ns) {
    if (ns <= 0 || ns >= 20000 || dm_detclock_enabled()) return ns;
    thr_t *me = t_self; if (!me) return ns;
    uint64_t now = now_ns();
    if (me->yrun_svc + 1 != me->nsvc || now - me->poll_ret > SPIN_GAP_NS) me->yrun_t0 = now;   // a new run
    me->yrun_svc = me->nsvc;
    uint64_t bo = yield_backoff_ns(now - me->yrun_t0);
    if (bo <= (uint64_t)ns) return ns;
    me->nyield++; me->nbackoff++;
    return (int64_t)bo;
}
// After the wait (ns: the guest's timeout): a signalled wait ends the run, the loop moves on
static void poll_wait_end(int64_t ns, int64_t r) {
    if (ns <= 0 || ns >= 20000 || dm_detclock_enabled()) return;
    thr_t *me = t_self; if (!me) return;
    if (r == R_TIMEOUT) me->poll_ret = now_ns(); else me->yrun_svc = 0;
}
// Sleep until dl (host monotonic ns) on time, as nanosleep does on the Mac. Windows' high-resolution waitable timer wakes 0.4–0.6 ms late (measured on this PC) and
// cannot sleep less than about half a millisecond, and Pokopia's main thread waits out each frame with 1 ms sleeps and then 100 µs ones: taken late, its title ran
// at 59.3 Hz instead of 60. So the timer is asked for the remaining time less its own lateness, learned as it goes (a moving average over all threads), and ends on
// the deadline on average; and a wait of at most 150 µs, which the timer would stretch to half a millisecond, is yielded through instead (about 0.1 of a core at
// Pokopia's title, docs/m4-title.md; yielding the last part of every sleep made it exact but cost more than a core). A wake hint (a guest wait's `woke`, read
// without G) ends the yielding early
static _Atomic int64_t g_timer_late = 450000;
static void wait_until(uint64_t dl, const int *woke, int64_t asked) {
    uint64_t now = now_ns();
    if (now >= dl) return;
    if (asked > 150000) {
        int64_t late = atomic_load_explicit(&g_timer_late, memory_order_relaxed), rem = (int64_t)(dl - now);
        if (rem <= late) plat_sleep_ns((uint64_t)rem);
        else {
            uint64_t want = (uint64_t)(rem - late);
            plat_sleep_ns(want);
            int64_t got = (int64_t)(now_ns() - now) - (int64_t)want;
            if (got >= 0 && got < 5000000) atomic_store_explicit(&g_timer_late, late + (got - late) / 16, memory_order_relaxed);
        }
    }
    // Never before the deadline, as on Horizon: when the timer came back early, yield through only what is left (tens of µs), not a fixed last part
    while (now_ns() < dl && !(woke && __atomic_load_n(woke, __ATOMIC_ACQUIRE))) plat_yield();
}
static void sleep_precise(uint64_t dl, int64_t ns) { wait_until(dl, NULL, ns); }
// A guest thread sleeps ns nanoseconds the way SleepThread does: on the guest clock under the deterministic clock, else on time (sleep_precise)
void dm_guest_sleep_ns(int64_t ns) {
    if (ns <= 0) return;
    if (dm_detclock_enabled()) {
        uint64_t deadline = guest_ns() + (uint64_t)ns;
        while (guest_ns() < deadline) plat_sleep_ns(1000000);
        return;
    }
    thr_t *me = t_self; if (me) me->sleeping = 1;
    sleep_precise(now_ns() + (uint64_t)ns, ns);
    if (me) me->sleeping = 0;
}
int svc_thread(int num, uint64_t *regs) {
    int r = svc_thread_impl(num, regs);
    thr_t *me = t_self;
    // The default path hands off only after a wake. The optional broader mode also checks peers that were already runnable
    if (me && (g_core_preempt || me->yield_pending)) {
        uint64_t t0 = now_ns(); int w = dm_yield_to_core_peers(g_core_preempt);
        if (w) { g_preempt_n++; g_preempt_ns += now_ns() - t0; }
    }
    if (me) { me->yield_pending = 0; me->hand_epoch++; }   // the handoff is complete: threads this one made runnable no longer hold it
    return r;
}
static int svc_thread_impl(int num, uint64_t *regs) {
    { thr_t *me = t_self; if (me) { me->last_svc = num; me->last_lr = regs[30]; me->nsvc++; } }
    if (g_flag.thr && num != 0x0B) { thr_t *me = t_self; LOG("[%#x] svc %#x x0=%#llx x1=%#llx x2=%#llx x3=%#llx", me ? me->handle : 0, num, (unsigned long long)regs[0], (unsigned long long)regs[1], (unsigned long long)regs[2], (unsigned long long)regs[3]); }
    switch (num) {
    case 0x08: {  // CreateThread(→w1 handle; x1 entry, x2 argument, x3 stack top, w4 priority, w5 core)
        thr_t *t = calloc(1, sizeof *t);
        NxwGuestCpu *c = plat_aligned_alloc(sizeof *c, 64);
        if (!t || !c) die("CreateThread: out of host memory");
        memset(c, 0, sizeof *c); t->cpu = t->cpu_base = c;
        t->entry = regs[1]; t->arg = regs[2]; t->stack_top = regs[3] & ~0xFull;
        // Core -2 (IdealCoreUseProcessValue) = the process's default ideal core (0 for this game), with an affinity mask of just that core - as in the Horizon kernel
        t->prio = (int)regs[4]; t->core = (int)regs[5] >= 0 ? (int)regs[5] : 0; t->mask = 1ull << t->core;
        t->born_ns = now_ns();
        pthread_mutex_lock(&G); t->handle = h_new(O_THREAD, t); t->id = g_next_id++; pthread_mutex_unlock(&G);
        uint32_t tid = plat_thread_start(thread_main, t, GUEST_THREAD_HOST_STACK);
        if (!tid) die("CreateThread: no host thread");
        pthread_mutex_lock(&G); t->tid = tid; t->all = g_all; g_all = t; pthread_mutex_unlock(&G);
        if (g_flag.thr) LOG("CreateThread h=%#x entry=%#llx arg=%#llx sp=%#llx prio=%d core=%d", t->handle, (unsigned long long)t->entry, (unsigned long long)t->arg, (unsigned long long)t->stack_top, t->prio, t->core);
        regs[0] = 0; regs[1] = t->handle; return 1;
    }
    case 0x09: {  // StartThread(w0)
        pthread_mutex_lock(&G);
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        thr_t *me = t_self; int same_core = t && me && (t->mask & me->mask) != 0;
        if (t) { t->started = 1; pthread_cond_signal(&t->cv); note_runnable(t); }
        pthread_mutex_unlock(&G);
        regs[0] = t ? 0 : R_BAD_HANDLE;
        // The creator goes on only after the new thread has made its first SVC: a freshly started thread reads its entry arguments and its context before it
        // does anything the creator could observe, and a boot stub that reuses its stack arguments right after StartThread (Eglantine 791ac523, Mario Tennis Fever)
        // raced it. On Horizon the new thread runs at once when it has the higher priority on the creator's core; on another core it starts within microseconds.
        // The wait is bounded: 10 x 100 us when the thread shares a core with the creator, up to 5 ms otherwise (a thread that never makes an SVC is not waited for)
        if (t && me && t != me) {
            uint64_t t0 = now_ns(), limit = same_core ? 1000000ull : 5000000ull; int spins = 0;
            while (!__atomic_load_n(&t->nsvc, __ATOMIC_ACQUIRE) && !__atomic_load_n(&t->exited, __ATOMIC_ACQUIRE) && now_ns() - t0 < limit) {
                if (++spins < 20) plat_yield(); else plat_sleep_ns(same_core ? 100000 : 50000);
            }
            if (!__atomic_load_n(&t->nsvc, __ATOMIC_ACQUIRE)) { static _Atomic uint64_t late; if (atomic_fetch_add(&late, 1) < 4) LOG("[thr] StartThread %#x: no SVC from the new thread within %.1f ms (%s core)", t->handle, (now_ns() - t0) / 1e6, same_core ? "same" : "other"); }
        }
        return 1;
    }
    case 0x0A: thread_exit(cur());
    case 0x0B: {  // SleepThread(x0 ns); 0/-1/-2 = the various kinds of yield
        int64_t ns = (int64_t)regs[0];
        if (g_flag.thr) { thr_t *me = t_self; LOG("[%#x] svc 0xb SleepThread %lld ns", me ? me->handle : 0, (long long)ns); }
        // A sleep, and YieldToAnyThread (-2), end its part in a handoff; the other yields (0, -1) pass the core only to threads of its own priority or higher
        if (ns > 0 || ns == -2) { thr_t *me = t_self; if (me && me->nhand) { pthread_mutex_lock(&G); hand_leave(me); pthread_mutex_unlock(&G); } }
        if ((ns > 0 && dm_detclock_enabled()) || ns >= 20000) dm_guest_sleep_ns(ns);
        // Yield (0 / -1 / -2), and sleeps shorter than 20 µs, which sleep_precise would return from at once: as yields they get the back-off
        else thr_yield(t_self, regs);
        return 1;
    }
    case 0x0C: { pthread_mutex_lock(&G); thr_t *t = h_get((uint32_t)regs[1], O_THREAD);
        regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)t->prio : 0; pthread_mutex_unlock(&G); return 1; }
    case 0x0D: { pthread_mutex_lock(&G); thr_t *t = h_get((uint32_t)regs[0], O_THREAD);   // SetThreadPriority; the Windows priority follows (any thread may set another's)
        if (t) { t->prio = (int)regs[1]; if (t->tid) plat_thread_set_priority(t->tid, t->prio); }
        regs[0] = t ? 0 : R_BAD_HANDLE; pthread_mutex_unlock(&G); return 1; }
    case 0x0E: { thr_t *t = h_get((uint32_t)regs[2], O_THREAD);
                 regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)(uint32_t)t->core : 0; regs[2] = t ? t->mask : 0; return 1; }
    case 0x0F: {  // SetThreadCoreMask(w0, w1 ideal core, x2 mask): -2 = use the process default (ideal core 0, mask of just that core), -1 = don't care, -3 = leave the ideal core unchanged; the host only keeps the bookkeeping
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { int c = (int)regs[1]; if (c >= 0) t->core = c; if (c == -2) { t->core = 0; t->mask = 1; } else if (regs[2]) t->mask = regs[2];
            // The ideal core is not in the new mask: as in Horizon, move the ideal core to the lowest core in the mask
            if (t->mask && (t->core < 0 || !(t->mask >> t->core & 1))) t->core = __builtin_ctzll(t->mask); }
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x10: { thr_t *t = cur(); int c = t->core;   // The current core number is never negative; a thread with no ideal core reports the lowest core its mask allows
                 regs[0] = (uint64_t)(uint32_t)(c >= 0 ? c : t->mask ? __builtin_ctzll(t->mask) : 0); return 1; }
    case 0x32: {  // SetThreadActivity(w0 thread, w1 0 runnable / 1 paused): see "SetThreadActivity / GetThreadContext3" above
        thr_t *me = cur(); int act = (int)regs[1];
        pthread_mutex_lock(&G);
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD); int64_t r = 0;
        if (!t) r = R_BAD_HANDLE;
        else if (t == me) r = R_BAD_STATE;
        else if (act == 1) {
            atomic_store(&t->pause_req, 1);
            int st = atomic_fetch_or(&t->svc_state, 2);
            if (!t->started || t->exited || (st & 1)) t->paused = 1;
            uint64_t t0 = now_ns(); int warned = 0;
            while (!t->paused && !t->exited) {
                struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_nsec += 1000000; if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
                pthread_cond_timedwait(&t->pcv, &G, &dl);
                if (!warned && now_ns() - t0 > 1000000000ull) { warned = 1; LOG("[RED] thread %#x still not suspended after more than 1 second (no SVC or safepoint reached)", t->handle); }
            }
        } else { atomic_store(&t->pause_req, 0); atomic_fetch_and(&t->svc_state, ~2); pthread_cond_broadcast(&t->pcv); }
        pthread_mutex_unlock(&G);
        { static int n; if (n++ < 4) LOG("SetThreadActivity %#x → %s (%s)", (uint32_t)regs[0], act ? "pause" : "resume", r ? "failed" : "ok"); }
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x33: {  // GetThreadContext3(x0 out ThreadContext*, w1 thread): only for stopped threads
        pthread_mutex_lock(&G);
        thr_t *t = h_get((uint32_t)regs[1], O_THREAD); int64_t r = 0;
        if (!t) r = R_BAD_HANDLE;
        else if (!t->paused) r = R_BAD_STATE;
        else ctx_fill(t, (hz_ctx_t *)(uintptr_t)regs[0]);
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x25: { thr_t *t = h_get((uint32_t)regs[1], O_THREAD); regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? t->id : 0; return 1; }

    case 0x1A: {  // ArbitrateLock(w0 owner handle, x1 address, w2 this thread's tag)
        thr_t *t = cur(); uint64_t addr = regs[1];
        pthread_mutex_lock(&G);
        int64_t r = 0;
        if (ld32(addr) == ((uint32_t)regs[0] | WAITERS_BIT)) { t->waddr = addr; t->wtag = (uint32_t)regs[2]; r = block(t, 1, -1); }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x1B: pthread_mutex_lock(&G); lock_handoff(regs[0]); pthread_mutex_unlock(&G); regs[0] = 0; return 1;
    case 0x1C: {  // WaitProcessWideKeyAtomic(x0 lock address, x1 condition key, w2 tag, x3 timeout)
        thr_t *t = cur(); uint64_t addr = regs[0], key = regs[1];
        { static int tw = -1; if (tw < 0) tw = getenv("NXRT_TRACE_WAIT") != NULL; static _Atomic uint64_t cnt; if (tw && (t->handle == 0x1064 ? 1 : atomic_fetch_add(&cnt, 1) % 20000 == 0)) LOG("[wait] cond wait #%llu thread %#x timeout %lld ns key %#llx", (unsigned long long)cnt, t->handle, (long long)regs[3], (unsigned long long)key); }
        pthread_mutex_lock(&G);
        // The order matters: first set the condition key = 1, then release the mutex. The SDK's InternalConditionVariable::Signal first reads the condition key in user mode, and does not issue svc 0x1D if it is 0;
        // grabbing the mutex in user mode does not go through G either. Linking into the wait chain happens inside block, still holding G: svc 0x1D has to take G, so it cannot land before this
        st32(key, 1);
        t->wkey = key; t->waddr = addr; t->wtag = (uint32_t)regs[2];
        lock_handoff(addr);
        int64_t r = block(t, 2, poll_wait_begin((int64_t)regs[3]));   // Timeout: the lock is not re-acquired (user mode takes it itself)
        pthread_mutex_unlock(&G);
        poll_wait_end((int64_t)regs[3], r);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x1D: {  // SignalProcessWideKey(x0 condition key, w1 count, <=0 all)
        uint64_t key = regs[0]; int cnt = (int)regs[1], n = 0;
        pthread_mutex_lock(&G);
        for (thr_t *w; (cnt <= 0 || n < cnt) && (w = pick(2, key)); n++) lock_acquire_for(w, w->waddr, w->wtag);   // In kernel order; once turned into a lock wait or woken it is no longer kind 2
        int left = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 2 && w->wkey == key) { left = 1; break; }
        if (!left) st32(key, 0);
        pthread_mutex_unlock(&G);
        regs[0] = 0; return 1;
    }

    case 0x34: {  // WaitForAddress(x0 address, w1 type, x2 value, x3 timeout): 0 wait if less than, 1 decrement if less than then wait, 2 wait if equal, 3 wait if the 64-bit value is equal
        thr_t *t = cur(); uint64_t addr = regs[0]; int type = (int)regs[1]; int32_t val = (int32_t)regs[2];
        pthread_mutex_lock(&G);
        int64_t r; uint32_t v = ld32(addr);
        int ok = type == 3 ? ld64(addr) == regs[2] : type == 2 ? (int32_t)v == val : (int32_t)v < val;
        if (ok && type == 1) { while (!cas32(addr, &v, v - 1)) if (!((int32_t)v < val)) { ok = 0; break; } }
        if (!ok) r = R_BAD_STATE;
        else { t->waddr = addr; r = block(t, 3, poll_wait_begin((int64_t)regs[3])); }
        pthread_mutex_unlock(&G);
        if (ok) poll_wait_end((int64_t)regs[3], r);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x35: {  // SignalToAddress(x0 address, w1 type, w2 value, w3 count): 0 wake, 1 increment if equal and wake, 2 if equal modify the value by waiter count and wake
        uint64_t addr = regs[0]; int type = (int)regs[1]; int32_t val = (int32_t)regs[2]; int cnt = (int)regs[3];
        pthread_mutex_lock(&G);
        int nw = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 3 && w->waddr == addr) nw++;
        int64_t r = 0;
        if (type != 0) {
            int32_t nv = val + 1;
            if (type == 2) {
                // Waking all (count <= 0) with waiters subtracts 2, not 1 (mesosphere KAddressArbiter::SignalAndModifyByWaitingCountIfEqual)
                if (cnt <= 0) nv = nw > 0 ? val - 2 : val + 1;
                else nv = nw == 0 ? val + 1 : nw <= cnt ? val - 1 : val;
            }
            uint32_t e = (uint32_t)val;
            if (!cas32(addr, &e, (uint32_t)nv)) r = R_BAD_STATE;
        }
        if (!r) { int n = 0;
            for (thr_t *w; (cnt <= 0 || n < cnt) && (w = pick(3, addr)); n++) wake(w, 0); }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }

    case 0x45: {  // CreateEvent(→w1 write end, w2 read end)
        event_t *e = calloc(1, sizeof *e); e->refs = 2;
        pthread_mutex_lock(&G); uint32_t hw = h_new(O_EVENT_W, e), hr = h_new(O_EVENT_R, e); pthread_mutex_unlock(&G);
        regs[0] = 0; regs[1] = hw; regs[2] = hr; return 1;
    }
    case 0x11: case 0x12: case 0x17: {  // SignalEvent(write end) / ClearEvent(either end) / ResetSignal(read end, error if not signaled)
        pthread_mutex_lock(&G);
        uint32_t h = (uint32_t)regs[0];
        event_t *e = num == 0x11 ? h_peek(h, O_EVENT_W) : num == 0x17 ? h_peek(h, O_EVENT_R)
                   : (h_peek(h, O_EVENT_W) ? h_peek(h, O_EVENT_W) : h_peek(h, O_EVENT_R));
        int64_t r = 0;
        if (!e) { r = (num == 0x17 && h_peek(h, O_THREAD)) ? R_BAD_STATE : R_BAD_HANDLE;
                  if (r == R_BAD_HANDLE) LOG("event SVC %#x invalid handle %#x", num, h); }
        else if (num == 0x11) { e->signaled = 1; wake_sync_waiters(e); }
        else if (num == 0x17 && !e->signaled) r = R_BAD_STATE;
        else e->signaled = 0;
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x18: {  // WaitSynchronization(→w1 index; x1 handle array, w2 count, x3 timeout)
        thr_t *t = cur(); const uint32_t *hs = (const uint32_t *)(uintptr_t)regs[1]; int n = (int)regs[2]; int64_t ns = poll_wait_begin((int64_t)regs[3]);
        int64_t deadline = ns > 0 ? (dm_detclock_enabled() ? (int64_t)guest_ns() : now_mono_ns()) + ns : 0;
        pthread_mutex_lock(&G);
        int64_t r; int idx = -1;
        for (;;) {
            if (t->cancel) { t->cancel = 0; r = R_CANCELLED; break; }
            int bad = 0;
            for (int i = 0; i < n && idx < 0; i++) if (sig_obj(hs[i], &bad)) idx = i;
            if (bad) { r = R_BAD_HANDLE; break; }
            if (idx >= 0) { r = 0; break; }
            int64_t left = ns < 0 ? -1 : ns == 0 ? 0 : deadline - (dm_detclock_enabled() ? (int64_t)guest_ns() : now_mono_ns());
            if (ns > 0 && left <= 0) { r = R_TIMEOUT; break; }
            t->whs = hs; t->whn = n; int64_t br = block(t, 4, left); t->whs = NULL; t->whn = 0;
            if (br == R_TIMEOUT) { r = R_TIMEOUT; break; }
        }
        pthread_mutex_unlock(&G);
        poll_wait_end((int64_t)regs[3], r);
        regs[0] = (uint64_t)r; regs[1] = idx >= 0 ? (uint64_t)idx : 0; return 1;
    }
    case 0x19: {  // CancelSynchronization(w0 thread)
        pthread_mutex_lock(&G);
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { t->cancel = 1; if (t->wkind == 4) wake(t, -1); }
        pthread_mutex_unlock(&G);
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x16: {  // CloseHandle(w0): an event is freed only when both ends are closed; after a thread handle is closed the slot is recycled, and the object is freed once the thread has also exited
        pthread_mutex_lock(&G);
        unsigned i = (uint32_t)regs[0] - 0x1000;
        if (i > 0 && i < MAX_H && (g_h[i].type == O_EVENT_R || g_h[i].type == O_EVENT_W)) {
            if (g_h[i].type == O_EVENT_W) se_unbind((uint32_t)regs[0]);
            event_t *e = g_h[i].obj; if (--e->refs == 0) free(e);
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
        } else if (i > 0 && i < MAX_H && g_h[i].type == O_THREAD) {
            thr_t *t = g_h[i].obj;
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
            t->closed = 1;
            if (t->exited) thr_free(t);
        }
        pthread_mutex_unlock(&G);
        regs[0] = 0; return 1;
    }
    }
    return 0;
}

// ---------- SystemEvent of the service layer ----------
// Real device: the service hands the read end of a kernel event to the client, and the guest waits on it through SDK nn::os. Host: on a guest thread, call the SDK's own
// CreateSystemEvent(inter-process=true), and the SDK creates the kernel event through SVC 0x45; the write-end handle is recorded, after which any thread can signal it.
void hle_bind_system_event(void *ev, int autoclear) {      // Must be called on a guest thread (it has to execute SDK code)
    pthread_mutex_lock(&g_se_lock); uint32_t wh = se_find(ev); pthread_mutex_unlock(&g_se_lock);
    if (wh) return;
    if (!t_self) die("hle_bind_system_event must be called on a guest thread");
    static void *create, *getw;
    if (!create) { create = guest_sym("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
                   getw = guest_sym("_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE"); }
    uint32_t r = (uint32_t)GUEST_CALL(create, (uint64_t)(uintptr_t)ev, (uint64_t)autoclear, 1);
    if (r) die("CreateSystemEvent failed %#x", r);
    wh = (uint32_t)GUEST_CALL(getw, (uint64_t)(uintptr_t)ev);
    pthread_mutex_lock(&g_se_lock);
    if (g_nse >= (int)(sizeof g_se / sizeof g_se[0])) die("SystemEvent binding table full");
    g_se[g_nse].ev = ev; g_se[g_nse].wh = wh; g_se[g_nse++].id = g_next_se_id++;
    pthread_mutex_unlock(&g_se_lock);
}
uint32_t hle_dup_readable_handle(uint32_t h) {   // A second read end of the same kernel event, as a service's handle copied to its client; 0 if h is no read end
    pthread_mutex_lock(&G);
    event_t *e = h_get(h, O_EVENT_R); uint32_t r = 0;
    if (e) { e->refs++; r = h_new(O_EVENT_R, e); }
    pthread_mutex_unlock(&G);
    return r;
}
uint64_t hle_system_event_binding(void *ev) {
    uint64_t id = 0;
    pthread_mutex_lock(&g_se_lock);
    for (int i = 0; i < g_nse; i++) if (g_se[i].ev == ev) { id = g_se[i].id; break; }
    pthread_mutex_unlock(&g_se_lock);
    return id;
}
int hle_signal_system_event_binding(uint64_t binding) {
    // Match the lifetime and signal under G, just as CloseHandle removes it under G: a stale producer cannot signal a new event that reuses either number
    pthread_mutex_lock(&G);
    uint32_t wh = 0;
    pthread_mutex_lock(&g_se_lock);
    for (int i = 0; i < g_nse; i++) if (g_se[i].id == binding) { wh = g_se[i].wh; break; }
    pthread_mutex_unlock(&g_se_lock);
    event_t *e = wh ? h_get(wh, O_EVENT_W) : NULL;
    if (e) { e->signaled = 1; wake_sync_waiters(e); }
    pthread_mutex_unlock(&G);
    return e != NULL;
}
void hle_signal_system_event(void *ev) {   // An unbound one is bound on the spot as manual-clear (only on a guest thread: off one it is skipped)
    for (int bound = 0; ; bound = 1) {
        pthread_mutex_lock(&G);
        pthread_mutex_lock(&g_se_lock); uint32_t wh = se_find(ev); pthread_mutex_unlock(&g_se_lock);
        if (wh) {
            event_t *e = h_get(wh, O_EVENT_W);
            if (!e) die("SystemEvent write-end handle invalid %#x", wh);
            e->signaled = 1; wake_sync_waiters(e);
            pthread_mutex_unlock(&G);
            return;
        }
        pthread_mutex_unlock(&G);
        if (bound) die("SystemEvent %p still not found after binding", ev);
        if (!t_self) { static _Atomic int once; if (!atomic_exchange(&once, 1)) LOG("[RED] SystemEvent %p signalled off a guest thread after the game destroyed it: skipped", ev); return; }
        hle_bind_system_event(ev, 0);   // guest code (SVCs take G): outside the lock
    }
}
// Signal only a bound one, from any thread; 0 when it is not bound
int hle_signal_bound_system_event(void *ev) {
    pthread_mutex_lock(&G);
    pthread_mutex_lock(&g_se_lock); uint32_t wh = se_find(ev); pthread_mutex_unlock(&g_se_lock);
    event_t *e = wh ? h_get(wh, O_EVENT_W) : NULL;
    if (e) { e->signaled = 1; wake_sync_waiters(e); }
    pthread_mutex_unlock(&G);
    return e != NULL;
}
void hle_clear_system_event(void *ev) {   // Clear a bound service-layer SystemEvent (any thread), as the service does when its condition ends
    pthread_mutex_lock(&G);
    pthread_mutex_lock(&g_se_lock); uint32_t wh = se_find(ev); pthread_mutex_unlock(&g_se_lock);
    if (wh) {
        event_t *e = h_get(wh, O_EVENT_W);
        if (!e) die("SystemEvent write-end handle invalid %#x", wh);
        e->signaled = 0;
    }
    pthread_mutex_unlock(&G);
}
