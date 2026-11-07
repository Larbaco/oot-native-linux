/* Guest setjmp / longjmp (host/guest_jmp.c) without the game. "Guest functions" are host functions void f(NxwGuestCpu *) that keep their state in the cpu
   across calls, as lifted code does: nxrt_guest_setjmp(cpu) is called like sdk's __setjmp (x0 = jmp_buf) and nxrt_guest_longjmp(cpu) like rt_nonlocal calls it
   (x0 = jmp_buf, x1 = value). Checks: setjmp returns 0 and then the longjmp value (1 for 0) with x19-x30, sp and d8-d15 as at the call, through several host
   frames; a copied jmp_buf works; a setjmp in a loop reuses its registration; a returned frame and another host stack are refused; the same inside a fiber
   (and a thread buffer from a fiber is refused); a function whose entry was overwritten by nxrt_sj_patch_entry behaves as setjmp; threads in parallel. */
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
void nxrt_set_cur_cpu(NxwGuestCpu *c) { (void)c; }

extern void nxrt_guest_setjmp(NxwGuestCpu *cpu) __attribute__((returns_twice));
extern const char *nxrt_guest_longjmp(NxwGuestCpu *cpu);
extern int nxrt_sj_patch_entry(void *code, void *target);

static int g_fail;
#define T(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

/* a stand-in for sdk's __setjmp, long enough to take the 14-byte patch */
__asm__(".text\n.globl dummy_setjmp\n.type dummy_setjmp,@function\n.p2align 4\ndummy_setjmp:\n  ret\n  .skip 64, 0x90\n.size dummy_setjmp, .-dummy_setjmp\n");
extern void dummy_setjmp(NxwGuestCpu *cpu) __attribute__((returns_twice));

static const char *g_why;
static volatile int g_first, g_second;

/* the longjmp site: some host frames down, as the callees of the setjmp frame are */
static __attribute__((noinline)) void jump_from(NxwGuestCpu *c, uint64_t buf, uint64_t val, int depth) {
    volatile char pad[160]; pad[0] = (char)depth; (void)pad[0];
    if (depth) { jump_from(c, buf, val, depth - 1); return; }
    c->x[0] = buf; c->x[1] = val;
    g_why = nxrt_guest_longjmp(c);   /* comes back only when it is refused */
}

static uint64_t g_buf[32];
static uint64_t g_buf2[32];

typedef void (*setjmp_fn)(NxwGuestCpu *) __attribute__((returns_twice));
static __attribute__((noinline)) void caller_with(NxwGuestCpu *c, setjmp_fn sj, uint64_t *buf, uint64_t val, int copy) {
    c->x[19] = 0x1919; c->x[20] = 0x2020; c->x[28] = 0x2828; c->x[29] = 0x2929; c->x[30] = 0x3030; c->sp = 0x7000;
    c->q[8][0] = 0x88; c->q[15][7] = 0x77; c->q[9][12] = 0x99;   /* the upper half of v9 is not saved */
    c->x[0] = (uint64_t)(uintptr_t)buf;
    sj(c);
    if (c->x[0] == 0) {
        g_first++;
        c->x[19] = 0xDEAD; c->x[30] = 0xBEEF; c->sp = 0x1234; c->q[8][0] = 0; c->q[15][7] = 0; c->q[9][12] = 0x11;
        uint64_t target = (uint64_t)(uintptr_t)buf;
        if (copy) { memcpy(g_buf2, buf, sizeof g_buf2); target = (uint64_t)(uintptr_t)g_buf2; }
        jump_from(c, target, val, 5);
        T(0);   /* never here */
    } else {
        g_second++;
        T(c->x[0] == ((uint32_t)val ? (uint32_t)val : 1));
        T(c->x[19] == 0x1919 && c->x[20] == 0x2020 && c->x[28] == 0x2828 && c->x[29] == 0x2929 && c->x[30] == 0x3030 && c->sp == 0x7000);
        T(c->q[8][0] == 0x88 && c->q[15][7] == 0x77);
        T(c->q[9][12] == 0x11);   /* only d9's low half returns: the high half keeps what it had */
    }
}

static __attribute__((noinline)) void make_dead(NxwGuestCpu *c, uint64_t *buf) {
    volatile char pad[256]; pad[0] = 1; (void)pad[0];
    c->x[0] = (uint64_t)(uintptr_t)buf;
    nxrt_guest_setjmp(c);
}

static void basic_tests(NxwGuestCpu *c) {
    g_first = g_second = 0;
    caller_with(c, nxrt_guest_setjmp, g_buf, 7, 0);
    T(g_first == 1 && g_second == 1);
    g_first = g_second = 0;
    caller_with(c, nxrt_guest_setjmp, g_buf, 0, 0);          /* longjmp(buf, 0) makes setjmp return 1 */
    T(g_first == 1 && g_second == 1);
    g_first = g_second = 0;
    caller_with(c, nxrt_guest_setjmp, g_buf, 0xFFFFFFFFull | 0x500000000ull, 0);   /* only w1 counts */
    T(g_first == 1 && g_second == 1);
    g_first = g_second = 0;
    caller_with(c, nxrt_guest_setjmp, g_buf, 3, 1);          /* through a copy of the jmp_buf */
    T(g_first == 1 && g_second == 1);
    /* a setjmp in a loop: the same buffer again and again (FreeType renders a glyph band after band), and many nested buffers */
    for (int i = 0; i < 5000; i++) { g_first = g_second = 0; caller_with(c, nxrt_guest_setjmp, g_buf, 1 + i % 5, 0); T(g_first == 1 && g_second == 1); }
    /* the same function, the entry overwritten with a jump to the stub */
    T(nxrt_sj_patch_entry((void *)dummy_setjmp, (void *)nxrt_guest_setjmp) == 0);
    g_first = g_second = 0;
    caller_with(c, dummy_setjmp, g_buf, 9, 0);
    T(g_first == 1 && g_second == 1);
    /* a frame that has returned is refused, a buffer nothing set too */
    make_dead(c, g_buf);
    c->x[0] = (uint64_t)(uintptr_t)g_buf; c->x[1] = 1;
    const char *why = nxrt_guest_longjmp(c);
    T(why && strstr(why, "returned"));
    uint64_t zero[32] = {0};
    c->x[0] = (uint64_t)(uintptr_t)zero; c->x[1] = 1;
    why = nxrt_guest_longjmp(c);
    T(why && strstr(why, "not set"));
}

/* ---- nesting: an outer setjmp stays valid while inner ones come and go ---- */
static uint64_t g_outer[32], g_inner[32], g_many[1000][32];
static volatile int g_stage;
static __attribute__((noinline)) void inner(NxwGuestCpu *c) {
    c->x[19] = 0xAA; c->x[0] = (uint64_t)(uintptr_t)g_inner; nxrt_guest_setjmp(c);
    if (c->x[0] == 0) { jump_from(c, (uint64_t)(uintptr_t)g_inner, 2, 2); T(0); }
    T(c->x[0] == 2 && c->x[19] == 0xAA);
}
static __attribute__((noinline)) void outer(NxwGuestCpu *c) {
    c->x[19] = 0xBB; c->x[0] = (uint64_t)(uintptr_t)g_outer; nxrt_guest_setjmp(c);
    if (c->x[0] == 0) {
        g_stage = 1;
        for (int i = 0; i < 300; i++) inner(c);
        for (int i = 0; i < 1000; i++) make_dead(c, g_many[i]);  /* more registrations of distinct buffers than the table has slots: the dead ones are reclaimed */
        g_stage = 2;
        jump_from(c, (uint64_t)(uintptr_t)g_outer, 5, 3);       /* the outer one is still good */
        T(0);
    }
    T(g_stage == 2 && c->x[0] == 5 && c->x[19] == 0xBB);
    g_stage = 3;
}

/* ---- fibers: the host stack of the setjmp frame is part of its identity ---- */
static NxwGuestCpu g_cpu;
typedef struct { uint64_t slot; uint8_t pad[24]; uint8_t state; uint8_t pad2[47]; uint64_t sp_slot; } fiber_t;
static fiber_t *FB; static uint64_t g_home_slot; static uint64_t g_fib_thread_buf[32], g_fib_buf[32];
static int g_fib_ok;
static void fiber_body(NxwGuestCpu *c) {
    caller_with(c, nxrt_guest_setjmp, g_fib_buf, 4, 0);          /* the whole cycle inside the coroutine */
    c->x[0] = (uint64_t)(uintptr_t)g_fib_thread_buf; c->x[1] = 1;
    const char *why = nxrt_guest_longjmp(c);                     /* a buffer the thread's own stack set */
    T(why && strstr(why, "another host stack"));
    g_fib_ok = 1;
    c->x[0] = (uint64_t)(uintptr_t)&FB->sp_slot; c->x[1] = (uint64_t)(uintptr_t)&g_home_slot;
    FB->state = 4;
    rt_fiber_switch(c);
    T(0);
}
NxwGuestFn nxrt_code_lookup(uint64_t va) { (void)va; return fiber_body; }

static void fiber_tests(NxwGuestCpu *c) {
    g_first = g_second = 0;
    c->x[0] = (uint64_t)(uintptr_t)g_fib_thread_buf; nxrt_guest_setjmp(c);   /* a registration on the thread's stack, from a frame that stays */
    if (c->x[0] == 0) {
        FB = calloc(1, sizeof *FB);
        uint8_t *stk = aligned_alloc(16, 4096); memset(stk, 0, 4096);
        uint64_t *fr = (uint64_t *)(stk + 4096 - 240);
        fr[0] = (uint64_t)(uintptr_t)FB; fr[12] = 0x1000 | 1;
        FB->sp_slot = (uint64_t)(uintptr_t)fr; FB->state = 1;
        g_cpu.x[0] = (uint64_t)(uintptr_t)&g_home_slot; g_cpu.x[1] = (uint64_t)(uintptr_t)&FB->sp_slot;
        rt_fiber_switch(&g_cpu);
        T(g_fib_ok && g_first == 1 && g_second == 1);
        /* and the fiber's buffer from this stack: the coroutine is gone with its registrations */
        g_cpu.x[0] = (uint64_t)(uintptr_t)g_fib_buf; g_cpu.x[1] = 1;
        const char *why = nxrt_guest_longjmp(&g_cpu);
        T(why != NULL);
    }
}

/* ---- threads ---- */
static void *thr(void *arg) {
    NxwGuestCpu c; memset(&c, 0, sizeof c); (void)arg;
    uint64_t buf[32];
    for (int i = 0; i < 20000; i++) { g_first = g_second = 0; caller_with(&c, nxrt_guest_setjmp, buf, 1 + i % 3, 0); }
    return NULL;
}

int main(void) {
    memset(&g_cpu, 0, sizeof g_cpu);
    NxwGuestCpu *c = &g_cpu;
    basic_tests(c);
    g_stage = 0; outer(c); T(g_stage == 3);
    fiber_tests(c);
    pthread_t a, b;
    pthread_create(&a, NULL, thr, NULL); pthread_create(&b, NULL, thr, NULL);
    pthread_join(a, NULL); pthread_join(b, NULL);
    unsigned set, jumps, failed; void nxrt_sj_stats(unsigned *, unsigned *, unsigned *); nxrt_sj_stats(&set, &jumps, &failed);
    T(jumps >= 40000);
    printf(g_fail ? "FAILED\n" : "guest setjmp/longjmp tests ok (%u setjmp, %u longjmp, %u refused)\n", set, jumps, failed);
    return g_fail;
}
