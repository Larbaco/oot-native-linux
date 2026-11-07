/* nn::os fibers (DESIGN D5): sdk's DispatchFiberContext(from_slot, to_slot) is one of the four "context functions" the lifter cannot follow (it rewrites SP
   and x30 on a stack of its own). The lifter now lifts it as a call of rt_fiber_switch, which does the same thing for a host that runs guest calls on
   its own stack:
     - every guest fiber is a host coroutine (a ucontext on a stack of its own) next to the guest stack the SDK gives it; the thread's own stack is
       the coroutine that was running when the first switch happened (its "home");
     - every coroutine runs on a NxwGuestCpu of its own: the lifted frames on its host stack hold that pointer, so it must stay valid and must not be shared
       with whatever else runs on the thread meanwhile. The guest register state the SDK saves in the context frame (x19-x30, sp, d8-d15) is kept by the
       runtime in the coroutine's record and loaded into its cpu on resume;
     - *from_slot gets a token (the guest sp the real function would have stored); *to_slot is either a token of a suspended coroutine, or the initial frame
       nn::os::InitializeFiber builds on the fiber's stack: x0 (the FiberType) at +0, the entry (bit 0 set marks "initial") at +96.
   A fiber whose entry has finished (FiberType state byte == 4 when it switches away) is never resumed: its coroutine is freed once the thread is on
   another stack.
   Fibers MIGRATE: the game's coroutine scheduler (nn::async::FiberTaskCoordinator on nn.npln.Worker threads) resumes a fiber on whichever worker takes it, not
   on the one that suspended it. So the list of suspended coroutines is global, and what belongs to the host thread rather than to the fiber follows the
   thread: the guest thread pointers (tpidr_el0 / tpidrro_el0 of the thread the coroutine runs on now), fpcr / fpsr, the kernel's cur_cpu (nxrt_set_cur_cpu) and
   the per-thread fiber state, which is read again after every context switch (fs(): the compiler must not reuse a TLS address across swapcontext).
   A coroutine joins the suspended list only once the thread has left its stack (after the swapcontext that saved it), or another thread could resume it
   half saved; a lookup that misses while such a switch is in flight waits for it.
   Context layout of the SDK 22 frame (240 bytes): [+0] x0 / zero, x19..x29 at +8.., [+96] x30, [+104] checksum, [+112] q8..q15. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifndef _WIN32
#include <sys/mman.h>
#include <ucontext.h>
#include <pthread.h>
#include <sched.h>
#endif
#include <stdatomic.h>
#include "nxw/guest_cpu.h"
#include "nxrt/rt_abi.h"

extern void nxrt_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern _Noreturn void nxrt_die(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern NxwGuestFn nxrt_code_lookup(uint64_t guest_va);

#define FIB_FRAME 240u
#define FIB_STACK (4u << 20)       /* host stack of a coroutine: lifted frames (virtual: pages are committed when touched) */
#define FIB_STATE_OFF 32u          /* FiberType: state byte (1 ready, 2 running, 4 finished) */
#define FIB_DONE 4u

extern void nxrt_set_cur_cpu(NxwGuestCpu *c);   /* hle_thread.c: the calling thread's cpu as nxrt_cur_cpu() answers it */

typedef struct fib {
    struct fib *next;
    uint64_t token;                /* the *slot value that resumes this coroutine (0 while it runs) */
    uint64_t fiber;                /* its FiberType (0 for a thread's own context) */
    uint64_t arg, entry, sp0;      /* a fresh coroutine's start */
    uint64_t x[31 - 19], sp;       /* x19..x30 and sp while it is suspended */
    uint8_t q[8][16];              /* v8..v15 */
    NxwGuestCpu *cpu;              /* the register file its frames run on (a fresh coroutine's own, else the one the last switch call was given) */
    NxwGuestCpu *own_cpu;          /* what to free with it (fresh coroutines only) */
#ifndef _WIN32
    ucontext_t uc;
    void *stack;
#endif
} fib_t;

/* Per host thread: what runs on it right now. Everything else (the suspended list) is global */
struct fibstate {
    fib_t *cur;                    /* the running context */
    fib_t *pub;                    /* the context this thread just left: joins the suspended list once the thread is on the next stack */
    fib_t *zombie;                 /* finished, waiting to be freed from another stack */
    fib_t *home;                   /* the thread's own stack */
};
#ifndef _WIN32
static _Thread_local struct fibstate t_f;
static fib_t *g_susp;              /* suspended coroutines of every thread */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_pending;      /* switches that left a context not yet on the list */
static _Atomic unsigned g_live, g_created, g_migrated;

/* The thread's state, looked up anew: a coroutine can be resumed by another thread than the one that suspended it, and a TLS address computed before
   swapcontext is the old thread's. The empty asm keeps the compiler from treating the function as pure and merging calls across a switch */
static __attribute__((noinline)) struct fibstate *fs(void) { __asm__ volatile("" ::: "memory"); return &t_f; }

/* guest_jmp.c: setjmp registrations made on a host stack that goes away (weak: the fiber test links this file alone) */
extern void nxrt_sj_stack_gone(void *stack) __attribute__((weak));
static void fib_free(fib_t *f) {
    if (nxrt_sj_stack_gone) nxrt_sj_stack_gone(f);
#ifndef _WIN32
    if (f->stack) munmap(f->stack, FIB_STACK + 4096);
#endif
    free(f->own_cpu);
    free(f);
}
static void reap(struct fibstate *s) {
    if (s->zombie) { fib_free(s->zombie); s->zombie = NULL; }
}
/* On the next stack: the context this thread left is complete now, others may resume it */
static void publish(struct fibstate *s) {
    fib_t *p = s->pub;
    if (!p) return;
    s->pub = NULL;
    pthread_mutex_lock(&g_mu);
    p->next = g_susp; g_susp = p;
    pthread_mutex_unlock(&g_mu);
    atomic_fetch_sub(&g_pending, 1);
}
static fib_t *take(uint64_t token) {     /* unlink the suspended coroutine whose token it is */
    pthread_mutex_lock(&g_mu);
    fib_t *r = NULL;
    for (fib_t **p = &g_susp; *p; p = &(*p)->next) if ((*p)->token == token) { r = *p; *p = r->next; break; }
    pthread_mutex_unlock(&g_mu);
    return r;
}
static void save_regs(fib_t *f, const NxwGuestCpu *c) {
    memcpy(f->x, &c->x[19], sizeof f->x);
    f->sp = c->sp;
    memcpy(f->q, &c->q[8], sizeof f->q);
}
static void load_regs(const fib_t *f, NxwGuestCpu *c) {
    memcpy(&c->x[19], f->x, sizeof f->x);
    c->sp = f->sp;
    memcpy(&c->q[8], f->q, sizeof f->q);
}
/* What belongs to the host thread, not to the fiber: the coroutine about to run on this thread gets the thread's guest thread pointers (TLS, TLR and so the
   thread object and id the SDK reads there), floating-point control and kernel thread link, from the cpu the thread is running now */
static void thread_bits(NxwGuestCpu *to, const NxwGuestCpu *from) {
    if (to == from) return;
    to->tpidr_el0 = from->tpidr_el0; to->tpidrro_el0 = from->tpidrro_el0;
    to->fpcr = from->fpcr; to->fpsr = from->fpsr; to->thread = from->thread;
    to->excl_addr = ~0ull;
}

static void fib_main(void) {
    struct fibstate *s = fs();
    publish(s); reap(s);
    fib_t *f = s->cur; NxwGuestCpu *c = f->cpu;
    nxrt_set_cur_cpu(c);
    memset(c->x, 0, sizeof c->x[0] * 19);
    c->x[0] = f->arg; c->x[29] = 0; c->x[30] = NXRT_RETURN_TO_HOST; c->sp = f->sp0; c->pc = f->entry;
    nxrt_code_lookup(f->entry)(c);
    nxrt_die("a fiber's entry %#llx returned to the host (the SDK's entry never returns: it switches away)", (unsigned long long)f->entry);
}
static fib_t *fib_create(uint64_t fiber, uint64_t arg, uint64_t entry, uint64_t sp0) {
    fib_t *f = calloc(1, sizeof *f);
    if (!f) nxrt_die("fiber: out of memory");
    void *m = NULL;
    if (posix_memalign(&m, 64, sizeof(NxwGuestCpu))) nxrt_die("fiber: out of memory");
    memset(m, 0, sizeof(NxwGuestCpu));
    f->cpu = f->own_cpu = m;
    f->stack = mmap(NULL, FIB_STACK + 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
    if (f->stack == MAP_FAILED) nxrt_die("fiber: cannot map a %u KB host stack", FIB_STACK >> 10);
    mprotect(f->stack, 4096, PROT_NONE);   /* guard */
    f->fiber = fiber; f->arg = arg; f->entry = entry; f->sp0 = sp0;
    getcontext(&f->uc);
    f->uc.uc_stack.ss_sp = (char *)f->stack + 4096;
    f->uc.uc_stack.ss_size = FIB_STACK;
    f->uc.uc_link = NULL;
    makecontext(&f->uc, fib_main, 0);
    return f;
}

/* Identity of the host stack the caller runs on: its coroutine, or the thread's own (the "home" context, made now if no switch has happened yet). guest_jmp.c
   tells with it whether a setjmp frame can still be returned to */
void *nxrt_stack_id(void) {
    struct fibstate *s = fs();
    if (!s->home) {
        s->home = calloc(1, sizeof *s->home);
        if (!s->home) nxrt_die("fiber: out of memory");
    }
    if (!s->cur) s->cur = s->home;
    return s->cur;
}

/* Number of live coroutines (tests, diagnostics) */
unsigned nxrt_fiber_live(void) { return atomic_load(&g_live); }

void rt_fiber_switch(NxwGuestCpu *cpu) {
    uint64_t *from = (uint64_t *)(uintptr_t)cpu->x[0], *to = (uint64_t *)(uintptr_t)cpu->x[1];
    if (!from || !to) nxrt_die("DispatchFiberContext with a null context slot (%p, %p)", (void *)from, (void *)to);
    struct fibstate *s = fs();
    if (!s->home) {
        s->home = calloc(1, sizeof *s->home);
        if (!s->home) nxrt_die("fiber: out of memory");
    }
    if (!s->cur) s->cur = s->home;
    fib_t *me = s->cur;
    me->cpu = cpu;                           /* the frames of this context hold this pointer */
    save_regs(me, cpu);
    me->token = cpu->sp - FIB_FRAME;
    *from = me->token;                       /* what the SDK would have stored: the sp of the saved frame */
    const uint64_t want = *to;
    fib_t *next = take(want);
    for (int spin = 0; !next && atomic_load(&g_pending) > 0 && spin < 20000; spin++) {   /* a context another thread is just leaving: it lands in microseconds */
        sched_yield();
        next = take(want);
    }
    if (next) {
        load_regs(next, next->cpu);
        next->cpu->x[0] = 0; next->cpu->x[1] = 0;
        if (next->cpu->tpidr_el0 != cpu->tpidr_el0) {   /* suspended by another thread: log the first and then the powers of two */
            const unsigned n = atomic_fetch_add(&g_migrated, 1) + 1;
            if (n <= 4 || !(n & (n - 1))) nxrt_logf("[fiber] migration #%u: the coroutine of FiberType %#llx, suspended by a thread with tpidr %#llx, resumes on one with %#llx (%u live, %u created)",
                                                    n, (unsigned long long)next->fiber, (unsigned long long)next->cpu->tpidr_el0, (unsigned long long)cpu->tpidr_el0,
                                                    atomic_load(&g_live), atomic_load(&g_created));
        }
    } else {
        /* an initial frame: x0 = the FiberType, x30 = entry | 1 (the SDK signs it; the runtime's PAC keeps pointers as they are) */
        const uint64_t *fr = (const uint64_t *)(uintptr_t)want;
        const uint64_t entry = want ? fr[12] & 0x0000FFFFFFFFFFFEull : 0;
        if (!want || !(fr[12] & 1) || !entry) {
            if (want) nxrt_logf("[RED] fiber switch to %#llx: no coroutine of any thread has that token (%u live, %u created, %u resumed on another thread); the frame there starts %#llx %#llx, x30 slot %#llx",
                                (unsigned long long)want, atomic_load(&g_live), atomic_load(&g_created), atomic_load(&g_migrated),
                                (unsigned long long)fr[0], (unsigned long long)fr[1], (unsigned long long)fr[12]);
            nxrt_die("DispatchFiberContext to %#llx: neither a suspended context nor an initial frame (x30 slot %#llx)", (unsigned long long)want,
                     (unsigned long long)(want ? fr[12] : 0)); }
        /* a fiber initialised again over a suspended one (FinalizeFiber without finishing): the old coroutine is dead */
        for (;;) {
            fib_t *old = NULL;
            pthread_mutex_lock(&g_mu);
            for (fib_t **p = &g_susp; *p; p = &(*p)->next) if ((*p)->fiber && (*p)->fiber == fr[0]) { old = *p; *p = old->next; break; }
            pthread_mutex_unlock(&g_mu);
            if (!old) break;
            fib_free(old); atomic_fetch_sub(&g_live, 1);
        }
        next = fib_create(fr[0], fr[0], entry, want + FIB_FRAME);
        atomic_fetch_add(&g_live, 1); atomic_fetch_add(&g_created, 1);
    }
    thread_bits(next->cpu, cpu);
    /* the context that finished its fiber is never resumed */
    const int done = me != s->home && me->fiber && *(volatile uint8_t *)(uintptr_t)(me->fiber + FIB_STATE_OFF) == FIB_DONE;
    if (done) { reap(s); me->token = 0; s->zombie = me; atomic_fetch_sub(&g_live, 1); }
    else { s->pub = me; atomic_fetch_add(&g_pending, 1); }
    s->cur = next; next->token = 0;
    if (swapcontext(&me->uc, &next->uc)) nxrt_die("fiber: swapcontext failed");
    /* back on `me`, maybe on another thread than the one that left it (never reached for a finished fiber) */
    s = fs();
    publish(s); reap(s);
    nxrt_set_cur_cpu(me->cpu);
}
#else
unsigned nxrt_fiber_live(void) { return 0; }
void *nxrt_stack_id(void) { return NULL; }
void rt_fiber_switch(NxwGuestCpu *cpu) { (void)cpu; nxrt_die("fiber switches are not implemented on this host"); }
#endif
