/* nn::os fibers (host/fiber.c) without the game: a fiber is an initial frame on a stack of its own (x0 = the FiberType at +0, entry | 1 at +96, as
   nn::os::InitializeFiber builds it), "guest code" is host functions run by a stand-in nxrt_code_lookup. Checks: first switch starts the entry with
   x0 = fiber and sp = frame + 240; switching back resumes the caller after its call with callee-saved registers and sp restored; ping-pong between
   two fibers and the thread; a finished fiber (state byte 4) is freed; a fiber initialised again over a suspended one replaces it. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include "nxw/guest_cpu.h"
#include "nxrt/rt_abi.h"

void nxrt_logf(const char *fmt, ...) { (void)fmt; }
_Noreturn void nxrt_die(const char *fmt, ...) { va_list a; va_start(a, fmt); printf("DIE: "); vprintf(fmt, a); printf("\n"); va_end(a); exit(2); }
unsigned nxrt_fiber_live(void);
static _Thread_local NxwGuestCpu *t_set_cpu;
void nxrt_set_cur_cpu(NxwGuestCpu *c) { t_set_cpu = c; }

static int g_fail;
#define T(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static NxwGuestCpu g_cpu;                     /* the thread's register file */
typedef struct { uint64_t slot; uint8_t pad[24]; uint8_t state; uint8_t pad2[47]; uint64_t sp_slot; } fiber_t;   /* state at +32, context slot at +80 */
static fiber_t *F[3];
static uint64_t g_home_slot;
static int g_log[64], g_n;
static void rec(int v) { if (g_n < 64) g_log[g_n++] = v; }

static void switch_to(uint64_t *from, uint64_t *to) {   /* what SwitchToFiber's tail does: x0/x1 = slots, call, callee-saved survive */
    g_cpu.x[0] = (uint64_t)(uintptr_t)from; g_cpu.x[1] = (uint64_t)(uintptr_t)to;
    rt_fiber_switch(&g_cpu);
}
static void switch_cpu(NxwGuestCpu *c, uint64_t *from, uint64_t *to) {
    c->x[0] = (uint64_t)(uintptr_t)from; c->x[1] = (uint64_t)(uintptr_t)to;
    rt_fiber_switch(c);
}
static void body_a(NxwGuestCpu *c) {            /* fiber 0 */
    T(c != &g_cpu); T(c->x[0] == (uint64_t)(uintptr_t)F[0]);
    T(c->sp == ((uint64_t)(uintptr_t)F[0]->sp_slot + 240));
    rec(1);
    c->x[19] = 0xAAAA; c->x[28] = 0xA28;
    switch_cpu(c, &F[0]->sp_slot, &F[1]->sp_slot);  /* to fiber 1 (an initial frame) */
    T(c->x[19] == 0xAAAA && c->x[28] == 0xA28);  /* resumed with its own registers */
    rec(3);
    switch_cpu(c, &F[0]->sp_slot, &g_home_slot);     /* back to the thread */
    T(0);                                        /* never resumed again */
}
static void body_b(NxwGuestCpu *c) {            /* fiber 1 */
    T(c->x[0] == (uint64_t)(uintptr_t)F[1]);
    rec(2);
    c->x[19] = 0xBBBB;
    switch_cpu(c, &F[1]->sp_slot, &F[0]->sp_slot);   /* back to fiber 0 (suspended) */
    T(0);
}
static void body_c(NxwGuestCpu *c) {            /* fiber 2: finishes at once */
    fiber_t *me = (fiber_t *)(uintptr_t)c->x[0];
    rec(10);
    me->state = 4;
    switch_cpu(c, &me->sp_slot, &g_home_slot);
    T(0);
}
static void body_m(NxwGuestCpu *c);
static NxwGuestFn g_fn[4] = {body_a, body_b, body_c, body_m};
NxwGuestFn nxrt_code_lookup(uint64_t va) { return g_fn[(va - 0x1000) / 2]; }

static fiber_t *mk(int k, uint64_t entry_va) {
    fiber_t *f = calloc(1, sizeof *f);
    uint8_t *stk = aligned_alloc(16, 4096); memset(stk, 0, 4096);
    uint64_t *fr = (uint64_t *)(stk + 4096 - 240);
    fr[0] = (uint64_t)(uintptr_t)f; fr[12] = entry_va | 1;
    f->sp_slot = (uint64_t)(uintptr_t)fr;        /* *(fiber + 80) = sp of the initial frame */
    f->state = 1; (void)k;
    return f;
}

/* ---- a fiber that migrates: suspended by one thread, resumed by another (nn::async's fiber coordinator does that) ---- */
static fiber_t *MF; static uint64_t m_home1, m_home2;
static NxwGuestCpu *m_cpu_a, *m_cpu_b; static uint64_t m_tp_a, m_tp_b; static int m_resumed;
static void body_m(NxwGuestCpu *c) {
    m_cpu_a = c; m_tp_a = c->tpidr_el0;
    c->x[19] = 0x4D4D; c->x[22] = 0x22;
    switch_cpu(c, &MF->sp_slot, &m_home1);                    /* suspended on thread 1 */
    m_cpu_b = c; m_tp_b = c->tpidr_el0; m_resumed = 1;        /* resumed on thread 2 */
    T(c->x[19] == 0x4D4D && c->x[22] == 0x22);
    switch_cpu(c, &MF->sp_slot, &m_home2);                    /* on to thread 2's own context */
    T(0);
}
static void *thread1(void *arg) {
    NxwGuestCpu cpu; memset(&cpu, 0, sizeof cpu); cpu.tpidr_el0 = 0x1111; cpu.sp = 0x9000; (void)arg;
    switch_cpu(&cpu, &m_home1, &MF->sp_slot);                 /* starts the fiber; it hands the thread back */
    T(t_set_cpu == &cpu && !m_resumed);
    return NULL;
}
static void *thread2(void *arg) {
    NxwGuestCpu cpu; memset(&cpu, 0, sizeof cpu); cpu.tpidr_el0 = 0x2222; cpu.sp = 0xA000; (void)arg;
    switch_cpu(&cpu, &m_home2, &MF->sp_slot);                 /* the token thread 1's fiber stored: runs it here */
    T(t_set_cpu == &cpu && m_resumed);
    return NULL;
}
static void migrate_test(void) {
    MF = mk(0, 0x1006);
    pthread_t a, b;
    pthread_create(&a, NULL, thread1, NULL); pthread_join(a, NULL);
    pthread_create(&b, NULL, thread2, NULL); pthread_join(b, NULL);
    T(m_cpu_a && m_cpu_a == m_cpu_b);                         /* the frames of the fiber keep their own register file */
    T(m_tp_a == 0x1111 && m_tp_b == 0x2222);                  /* ... whose thread pointer follows the thread that runs it */
}
int main(void) {
    F[0] = mk(0, 0x1000); F[1] = mk(1, 0x1002); F[2] = mk(2, 0x1004);
    g_cpu.x[19] = 0x1919; g_cpu.sp = 0x7000; g_cpu.q[8][0] = 0x55;
    /* thread -> fiber 0 -> fiber 1 -> fiber 0 -> thread */
    switch_to(&g_home_slot, &F[0]->sp_slot);
    T(g_n == 3 && g_log[0] == 1 && g_log[1] == 2 && g_log[2] == 3);
    T(g_cpu.x[19] == 0x1919 && g_cpu.sp == 0x7000 && g_cpu.q[8][0] == 0x55);   /* the thread's registers are back */
    T(g_home_slot == 0x7000 - 240);
    T(nxrt_fiber_live() == 2);
    /* a fiber that finishes is released */
    switch_to(&g_home_slot, &F[2]->sp_slot);
    T(g_n == 4 && g_log[3] == 10);
    T(nxrt_fiber_live() == 2);
    /* fiber 1 is still suspended: initialising it again over its FiberType starts a new coroutine and drops the old one */
    uint8_t *stk = aligned_alloc(16, 4096); memset(stk, 0, 4096); uint64_t *fr = (uint64_t *)(stk + 4096 - 240);
    fr[0] = (uint64_t)(uintptr_t)F[1]; fr[12] = 0x1004 | 1; F[1]->state = 1; F[1]->sp_slot = (uint64_t)(uintptr_t)fr;
    switch_to(&g_home_slot, &F[1]->sp_slot);
    T(g_n == 5 && g_log[4] == 10);
    T(nxrt_fiber_live() == 1);
    migrate_test();
    printf(g_fail ? "FAILED\n" : "fiber tests ok (%d coroutines created)\n", 5);
    return g_fail;
}
