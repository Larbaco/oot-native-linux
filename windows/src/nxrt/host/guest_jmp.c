/* Guest setjmp / longjmp as a real non-local transfer (DESIGN D5, docs/FASE4-FIBERS.md, section "setjmp / longjmp").

   The lifter keeps every guest register in the NxwGuestCpu around calls (nxw_sem_leave before a call, nxw_sem_enter after it), and a lifted function is a
   host function void f(NxwGuestCpu *) whose frame lives on the host stack. sdk's `longjmp` rewrites SP and x30, which no host stack can follow, so the
   lifter turns it into rt_nonlocal; its `__setjmp` is lifted as an ordinary function (only the first return exists). Both are fixed here without a relift:

     setjmp   The host code of the lifted __setjmp is overwritten with a jump to nxrt_guest_setjmp (nxrt_sj_patch_entry, installed by rt.c once the game
              object is attached). That stub runs *in the caller's frame*: it registers the guest state (x19-x30, sp, d8-d15 of the cpu) and tail-calls the
              host _setjmp, which saves the callee-saved host registers, the host sp and the return address of the lifted caller. The guest jmp_buf only
              gets a magic and an index into the table below (the host state does not fit: SDK 22's jmp_buf is 21 words of guest registers).
     longjmp  rt_nonlocal calls nxrt_guest_longjmp: it finds the registration, checks that the frame is still live (the same host stack, which is the
              fiber's coroutine stack or the thread's own, and above the current sp), puts the guest registers into the cpu of the setjmp frame
              (x0 = value, or 1 if 0) and host-longjmps. The lifted caller then continues after its call, where nxw_sem_enter reloads everything from the cpu:
              setjmp "returns" again with the guest state of the moment it was called, as on hardware.

   Frames between the two are discarded without running anything, as on hardware. A host frame in between (an HLE function that called back into the
   guest) would be skipped too; none exists in the paths seen (FreeType's gray raster, libpng, pthread_exit), and nothing here can see one. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <setjmp.h>
#include <stdatomic.h>
#include "nxw/guest_cpu.h"
#include "nxrt/rt_abi.h"

#ifndef _WIN32
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

extern void nxrt_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern void *nxrt_stack_id(void);   /* fiber.c: the host stack the caller runs on (a fiber's coroutine or the thread's own) */

#define SJ_MAGIC 0x4e58534a4d500001ull   /* "NXSJMP" + 1: word 0 of a registered guest jmp_buf */
#define SJ_CAP 256

typedef struct {
    uint64_t serial;               /* 0 = free; also word 1 of the guest jmp_buf, so a stale buffer is told from a live one */
    void *stack;                   /* nxrt_stack_id() of the setjmp frame */
    uintptr_t sp;                  /* the host sp of that frame once setjmp returned: it is live while this is above the sp of whoever longjmps */
    NxwGuestCpu *cpu;              /* the register file that frame holds */
    uint64_t x[12], gsp;           /* x19..x30 and sp */
    uint64_t d[8];                 /* d8..d15 */
    jmp_buf host;
} sj_t;
static sj_t g_sj[SJ_CAP];
static uint64_t g_serial;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic unsigned g_nset, g_njmp, g_nfail;

/* The setjmp stub. Entered like __setjmp (rdi = cpu, return address on top of the stack): nxrt_sj_enter registers the call, then _setjmp runs on the very
   stack state the caller had, saving its callee-saved registers, sp and return address; its return value (0) goes nowhere, the lifted caller reads x0 */
__asm__(
    ".text\n"
    ".globl nxrt_guest_setjmp\n"
    ".type nxrt_guest_setjmp,@function\n"
    ".p2align 4\n"
    "nxrt_guest_setjmp:\n"
    "  .byte 0xf3,0x0f,0x1e,0xfa\n"          /* endbr64 */
    "  pushq %rdi\n"
    "  leaq 8(%rsp), %rsi\n"                 /* the sp at entry (return address on top) */
    "  call nxrt_sj_enter@PLT\n"
    "  popq %rdi\n"
    "  movq %rax, %rdi\n"                    /* the host jmp_buf of the registration */
    "  jmp _setjmp@PLT\n"
    ".size nxrt_guest_setjmp, .-nxrt_guest_setjmp\n");

static void sj_free_slot(sj_t *e) { e->serial = 0; e->stack = NULL; }

void *nxrt_sj_enter(NxwGuestCpu *cpu, uintptr_t entry_sp) {
    uint64_t *g = (uint64_t *)(uintptr_t)cpu->x[0];
    void *stk = nxrt_stack_id();
    if (!g) { nxrt_logf("[RED] setjmp(NULL)"); abort(); }
    pthread_mutex_lock(&g_mu);
    /* this buffer's previous registration (a setjmp in a loop, or a reused buffer) */
    if (g[0] == SJ_MAGIC && g[2] < SJ_CAP && g_sj[g[2]].serial == g[1]) sj_free_slot(&g_sj[g[2]]);
    /* A registration on this stack whose frame is deeper than this one has returned (a live frame is above its callees): reclaim it. One at the same depth
       may be the same frame (a second setjmp in it) or a sibling that returned: it stays. If the table is full anyway (buffers set and never jumped to,
       call after call at one depth) the victim is the deepest registration of this stack, the oldest among equals: an outer, long-lived frame is the last to go */
    sj_t *e = NULL, *victim = NULL;
    for (unsigned i = 0; i < SJ_CAP; i++) {
        sj_t *s = &g_sj[i];
        if (s->serial && s->stack == stk && s->sp < entry_sp + 8) sj_free_slot(s);
        if (!s->serial) { if (!e) e = s; continue; }
        if (!victim || (s->stack == stk && (victim->stack != stk || s->sp < victim->sp || (s->sp == victim->sp && s->serial < victim->serial))) ||
            (victim->stack != stk && s->stack != stk && s->serial < victim->serial)) victim = s;
    }
    if (!e) e = victim;
    e->serial = ++g_serial; e->stack = stk; e->sp = entry_sp + 8; e->cpu = cpu;
    memcpy(e->x, &cpu->x[19], sizeof e->x);
    e->gsp = cpu->sp;
    for (int i = 0; i < 8; i++) memcpy(&e->d[i], cpu->q[8 + i], 8);
    g[0] = SJ_MAGIC; g[1] = e->serial; g[2] = (uint64_t)(e - g_sj); g[3] = cpu->sp;
    cpu->x[0] = 0;   /* the first return */
    pthread_mutex_unlock(&g_mu);
    const unsigned n = atomic_fetch_add(&g_nset, 1) + 1;
    if (n <= 2 || !(n & (n - 1))) nxrt_logf("[setjmp] #%u: guest setjmp(%#llx) from lr %#llx on the stack %p (%u longjmp so far)", n, (unsigned long long)(uintptr_t)g,
                                           (unsigned long long)cpu->x[30], stk, atomic_load(&g_njmp));
    return e->host;
}

/* The guest longjmp (x0 = jmp_buf, w1 = value). Returns only when it cannot go: the reason */
const char *nxrt_guest_longjmp(NxwGuestCpu *cpu) {
    uint64_t *g = (uint64_t *)(uintptr_t)cpu->x[0];
    uint32_t val = (uint32_t)cpu->x[1];
    if (!g) { atomic_fetch_add(&g_nfail, 1); return "longjmp on a null jmp_buf"; }
    if (g[0] != SJ_MAGIC) { atomic_fetch_add(&g_nfail, 1); return "the jmp_buf was not set by setjmp (or not through the host-checkpoint setjmp)"; }
    void *stk = nxrt_stack_id();
    const uintptr_t here = (uintptr_t)__builtin_frame_address(0);
    pthread_mutex_lock(&g_mu);
    sj_t *e = g[2] < SJ_CAP ? &g_sj[g[2]] : NULL;
    const char *why = NULL;
    if (!e || e->serial != g[1]) why = "the jmp_buf registration is gone (its setjmp frame returned and the entry was reclaimed)";
    else if (e->stack != stk) why = "the setjmp frame is on another host stack (another thread or fiber)";
    else if (e->sp <= here) why = "the setjmp frame has returned (its host frame is below the caller)";
    if (why) { pthread_mutex_unlock(&g_mu); atomic_fetch_add(&g_nfail, 1); return why; }
    NxwGuestCpu *t = e->cpu;
    const uint64_t lr = cpu->x[30];   /* the longjmp caller's, before the registers of the setjmp frame go in */
    memcpy(&t->x[19], e->x, sizeof e->x);
    t->sp = e->gsp;
    for (int i = 0; i < 8; i++) memcpy(t->q[8 + i], &e->d[i], 8);
    t->x[0] = val ? val : 1;
    t->excl_addr = ~0ull;
    jmp_buf *hb = &e->host;
    pthread_mutex_unlock(&g_mu);
    const unsigned n = atomic_fetch_add(&g_njmp, 1) + 1;
    if (n <= 16 || !(n & (n - 1))) nxrt_logf("[longjmp] #%u: guest longjmp(%#llx, %u) from lr %#llx to the setjmp frame (%u setjmp calls so far)", n,
                                           (unsigned long long)(uintptr_t)g, val, (unsigned long long)lr, atomic_load(&g_nset));
    longjmp(*hb, 1);
}

/* A registration whose host stack is being freed (a finished fiber) */
void nxrt_sj_stack_gone(void *stack) {
    pthread_mutex_lock(&g_mu);
    for (unsigned i = 0; i < SJ_CAP; i++) if (g_sj[i].serial && g_sj[i].stack == stack) sj_free_slot(&g_sj[i]);
    pthread_mutex_unlock(&g_mu);
}

/* Overwrite the first 14 bytes of host code with "jmp [rip+0]; .quad target" (the lifted function's entry: its body never runs again) */
int nxrt_sj_patch_entry(void *code, void *target) {
    const long pg = sysconf(_SC_PAGESIZE);
    uintptr_t a = (uintptr_t)code, lo = a & ~(uintptr_t)(pg - 1), hi = (a + 14 + pg - 1) & ~(uintptr_t)(pg - 1);
    uint8_t b[14] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(b + 6, &target, 8);
    if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC)) return -1;
    memcpy(code, b, sizeof b);
    __builtin___clear_cache((char *)code, (char *)code + sizeof b);
    return mprotect((void *)lo, hi - lo, PROT_READ | PROT_EXEC);
}

void nxrt_sj_stats(unsigned *set, unsigned *jumps, unsigned *failed) {
    *set = atomic_load(&g_nset); *jumps = atomic_load(&g_njmp); *failed = atomic_load(&g_nfail);
}
#else
const char *nxrt_guest_longjmp(NxwGuestCpu *cpu) { (void)cpu; return "guest longjmp is not implemented on this host"; }
int nxrt_sj_patch_entry(void *code, void *target) { (void)code; (void)target; return -1; }
void nxrt_guest_setjmp(NxwGuestCpu *cpu) { (void)cpu; }
#endif
