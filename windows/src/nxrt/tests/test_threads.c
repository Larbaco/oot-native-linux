/* Guest threads and the Horizon synchronisation primitives under stress, with 4 threads at most (no all-core load): each guest thread is a host thread
   owning its NxwGuestCpu, created and started by SVCs with a stack the SDK's way (heap, then MapMemory), and uses only rt_svc:
     1. a mutex (lock word + ArbitrateLock / ArbitrateUnlock) around a shared counter;
     2. a bounded queue with two condition variables (WaitProcessWideKeyAtomic / SignalProcessWideKey), two producers, two consumers;
     3. a barrier on address arbitration (WaitForAddress if equal / SignalToAddress to all);
     4. events across threads, CancelSynchronization, a timed wait, and SetThreadActivity / GetThreadContext3 on a thread waiting in an SVC;
   every thread exits with ExitThread and is joined through WaitSynchronization on its handle. */
#include "guest_test.h"

NXRT_EARLY_RESERVE()

#define LOCK_ITERS 20000
#define QUEUE_ITEMS 20000
#define BARRIER_ROUNDS 2000

typedef struct {
    _Atomic uint32_t lock, not_empty, not_full;
    uint64_t counter;
    uint32_t q[16]; unsigned head, tail, count;
    uint64_t produced_sum, consumed_sum, consumed;
    _Atomic uint32_t bar_count, bar_gen;
    _Atomic uint32_t bar_seen[4], bar_slept;
    uint32_t ev_w, ev_r, ev_ack_w, ev_ack_r;
    _Atomic uint32_t ev_got;
} shared_t;
static shared_t *S;
typedef struct { shared_t *s; int id; } arg_t;

static void lock_worker(NxwGuestCpu *c) {
    arg_t *a = (arg_t *)(uintptr_t)c->x[0];
    for (int i = 0; i < LOCK_ITERS; i++) {
        gt_lock(c, &a->s->lock);
        uint64_t v = a->s->counter; if (!(i & 255)) { c->x[0] = 0; gt_svc(c, 0x0B); }   /* a yield inside the critical section now and then */
        a->s->counter = v + 1;
        gt_unlock(c, &a->s->lock);
    }
    gt_svc(c, 0x0A);   /* ExitThread */
}
static void producer(NxwGuestCpu *c) {
    arg_t *a = (arg_t *)(uintptr_t)c->x[0]; shared_t *s = a->s;
    for (uint32_t i = 0; i < QUEUE_ITEMS / 2; i++) {
        uint32_t item = (uint32_t)a->id << 24 | i;
        gt_lock(c, &s->lock);
        while (s->count == 16) gt_cond_wait(c, &s->lock, &s->not_full, -1);
        s->q[s->tail] = item; s->tail = (s->tail + 1) & 15; s->count++; s->produced_sum += item;
        gt_cond_signal(c, &s->not_empty, 1);
        gt_unlock(c, &s->lock);
    }
    gt_svc(c, 0x0A);
}
static void consumer(NxwGuestCpu *c) {
    arg_t *a = (arg_t *)(uintptr_t)c->x[0]; shared_t *s = a->s;
    for (;;) {
        gt_lock(c, &s->lock);
        while (s->count == 0 && s->consumed < QUEUE_ITEMS) gt_cond_wait(c, &s->lock, &s->not_empty, 50000000);   /* with a timeout too */
        if (s->consumed == QUEUE_ITEMS) { gt_cond_signal(c, &s->not_empty, -1); gt_unlock(c, &s->lock); break; }
        uint32_t item = s->q[s->head]; s->head = (s->head + 1) & 15; s->count--; s->consumed_sum += item; s->consumed++;
        gt_cond_signal(c, &s->not_full, 1);
        if (s->consumed == QUEUE_ITEMS) gt_cond_signal(c, &s->not_empty, -1);
        gt_unlock(c, &s->lock);
    }
    gt_svc(c, 0x0A);
}
static void barrier_worker(NxwGuestCpu *c) {
    arg_t *a = (arg_t *)(uintptr_t)c->x[0]; shared_t *s = a->s;
    for (int r = 0; r < BARRIER_ROUNDS; r++) {
        uint32_t gen = atomic_load(&s->bar_gen);
        atomic_fetch_add(&s->bar_seen[a->id], 1);
        if (atomic_fetch_add(&s->bar_count, 1) + 1 == 4) {
            atomic_store(&s->bar_count, 0);
            atomic_store(&s->bar_gen, gen + 1);
            c->x[0] = (uint64_t)(uintptr_t)&s->bar_gen; c->x[1] = 0; c->x[2] = 0; c->x[3] = (uint64_t)-1;
            T(gt_svc(c, 0x35) == 0);   /* SignalToAddress: wake all */
        } else {
            while (atomic_load(&s->bar_gen) == gen) {
                c->x[0] = (uint64_t)(uintptr_t)&s->bar_gen; c->x[1] = 2; c->x[2] = gen; c->x[3] = (uint64_t)-1;
                uint32_t rc = (uint32_t)gt_svc(c, 0x34);   /* WaitForAddress if equal: R_BAD_STATE when it already moved */
                T(rc == 0 || rc == R_BAD_STATE);
                if (!rc) atomic_fetch_add(&s->bar_slept, 1);
            }
        }
        for (int k = 0; k < 4; k++) T(atomic_load(&s->bar_seen[k]) >= (uint32_t)r);   /* nobody is a round behind */
    }
    gt_svc(c, 0x0A);
}
static void event_worker(NxwGuestCpu *c) {
    arg_t *a = (arg_t *)(uintptr_t)c->x[0]; shared_t *s = a->s;
    uint32_t *hs = gt_alloc(4); hs[0] = s->ev_r;
    for (int i = 0; i < 500; i++) {
        c->x[1] = (uint64_t)(uintptr_t)hs; c->x[2] = 1; c->x[3] = (uint64_t)-1;
        T(gt_svc(c, 0x18) == 0);
        c->x[0] = s->ev_r; T(gt_svc(c, 0x17) == 0);   /* ResetSignal */
        atomic_fetch_add(&s->ev_got, 1);
        c->x[0] = s->ev_ack_w; T(gt_svc(c, 0x11) == 0);
    }
    /* then wait until cancelled */
    c->x[1] = (uint64_t)(uintptr_t)hs; c->x[2] = 1; c->x[3] = (uint64_t)-1;
    uint32_t rc = (uint32_t)gt_svc(c, 0x18);
    T(rc == R_CANCELLED);
    atomic_store(&s->ev_got, 0xC0DE);
    gt_svc(c, 0x0A);
}

static gt_thread_t start(NxwGuestCpu *c, NxwGuestFn fn, int id, int prio, int core) {
    arg_t *a = gt_alloc(sizeof *a); a->s = S; a->id = id;
    return gt_thread(c, fn, (uint64_t)(uintptr_t)a, prio, core);
}

int main(void) {
    gt_init();
    NxwGuestCpu *c = nxrt_cur_cpu();
    S = gt_alloc(sizeof *S);
    gt_thread_t t[4];
    uint64_t t0 = now_ns_test();

    /* 1. mutex: 4 threads, the main thread joins; priorities and cores as a game spreads them */
    for (int i = 0; i < 4; i++) t[i] = start(c, lock_worker, i, 40 + i * 4, i % 3);
    for (int i = 0; i < 4; i++) gt_join(c, &t[i]);
    printf("mutex: counter %llu (want %d), %.0f ms\n", (unsigned long long)S->counter, 4 * LOCK_ITERS, (now_ns_test() - t0) / 1e6);
    T(S->counter == 4 * LOCK_ITERS && atomic_load(&S->lock) == 0);

    /* 2. condition variables */
    t0 = now_ns_test();
    t[0] = start(c, producer, 1, 44, 0); t[1] = start(c, producer, 2, 44, 1);
    t[2] = start(c, consumer, 3, 44, 2); t[3] = start(c, consumer, 4, 45, 0);
    for (int i = 0; i < 4; i++) gt_join(c, &t[i]);
    printf("condvar: %llu items, sums %llx / %llx, %.0f ms\n", (unsigned long long)S->consumed, (unsigned long long)S->produced_sum, (unsigned long long)S->consumed_sum, (now_ns_test() - t0) / 1e6);
    T(S->consumed == QUEUE_ITEMS && S->produced_sum == S->consumed_sum && S->count == 0);

    /* 3. address arbitration */
    t0 = now_ns_test();
    for (int i = 0; i < 4; i++) t[i] = start(c, barrier_worker, i, 44, i % 3);
    for (int i = 0; i < 4; i++) gt_join(c, &t[i]);
    printf("barrier: generation %u (want %d), %u waits slept in WaitForAddress, %.0f ms\n", atomic_load(&S->bar_gen), BARRIER_ROUNDS,
           atomic_load(&S->bar_slept), (now_ns_test() - t0) / 1e6);
    T(atomic_load(&S->bar_gen) == BARRIER_ROUNDS && atomic_load(&S->bar_slept) > 0);

    /* 4. events, cancellation, pausing a thread that waits in an SVC */
    T(gt_svc(c, 0x45) == 0); S->ev_w = (uint32_t)c->x[1]; S->ev_r = (uint32_t)c->x[2];
    T(gt_svc(c, 0x45) == 0); S->ev_ack_w = (uint32_t)c->x[1]; S->ev_ack_r = (uint32_t)c->x[2];
    t[0] = start(c, event_worker, 0, 44, 1);
    uint32_t *hs = gt_alloc(4); hs[0] = S->ev_ack_r;
    for (int i = 0; i < 500; i++) {
        c->x[0] = S->ev_w; T(gt_svc(c, 0x11) == 0);
        c->x[1] = (uint64_t)(uintptr_t)hs; c->x[2] = 1; c->x[3] = 1000000000; T(gt_svc(c, 0x18) == 0);
        c->x[0] = S->ev_ack_r; T(gt_svc(c, 0x17) == 0);
    }
    T(atomic_load(&S->ev_got) == 500);
    plat_sleep_ns(20000000);   /* the worker now waits in WaitSynchronization */
    c->x[0] = t[0].handle; c->x[1] = 1; T(gt_svc(c, 0x32) == 0);   /* SetThreadActivity: pause (it is in an SVC: stops at once) */
    uint8_t *ctx = gt_alloc(0x320);
    c->x[0] = (uint64_t)(uintptr_t)ctx; c->x[1] = t[0].handle; T(gt_svc(c, 0x33) == 0);   /* GetThreadContext3 */
    uint64_t ctx_sp; memcpy(&ctx_sp, ctx + 31 * 8, 8);
    T(ctx_sp > t[0].stack_dst && ctx_sp <= t[0].stack_dst + t[0].stack_size);
    c->x[0] = t[0].handle; c->x[1] = 0; T(gt_svc(c, 0x32) == 0);   /* resume */
    c->x[0] = t[0].handle; T(gt_svc(c, 0x19) == 0);                 /* CancelSynchronization */
    gt_join(c, &t[0]);
    T(atomic_load(&S->ev_got) == 0xC0DE);

    int handles, live, objs; void thr_counts(int *, int *, int *); thr_counts(&handles, &live, &objs);
    printf("threads: %d live, %d objects, %d handles in use\n", live, objs, handles);
    T(live == 1 && objs == 1);   /* only the main thread; every exited thread was closed and freed */
    printf(gt_fail ? "nxrt.threads: FAILED\n" : "nxrt.threads: ok\n");
    return gt_fail;
}
