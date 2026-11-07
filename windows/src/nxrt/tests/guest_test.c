/* Helpers for the tests' guests (guest_test.h) */
#include "guest_test.h"

int gt_fail;
uint64_t gt_heap, gt_heap_size;
static _Atomic uint64_t g_next_fn = NXRT_MODULE_BASE + 0x1000, g_brk;
uint32_t thr_cur_handle(void);   /* hle_thread.c */

uint64_t gt_fn(NxwGuestFn fn) {
    uint64_t va = atomic_fetch_add(&g_next_fn, 16);
    if (nxrt_code_set(va, fn)) { printf("FAIL: cannot register a guest function at %#llx\n", (unsigned long long)va); gt_fail = 1; }
    return va;
}
uint64_t gt_svc(NxwGuestCpu *c, uint32_t n) { c->pc = NXRT_MODULE_BASE + 0x800; rt_svc(c, n); return c->x[0]; }
void *gt_alloc(size_t n) {
    uint64_t a = atomic_fetch_add(&g_brk, (n + 63) & ~(size_t)63);
    if (a + n > gt_heap + gt_heap_size) { printf("FAIL: test heap exhausted\n"); gt_fail = 1; return NULL; }
    memset((void *)(uintptr_t)a, 0, n);
    return (void *)(uintptr_t)a;
}
uint32_t gt_self(void) { return thr_cur_handle(); }

void gt_init(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (nxrt_init(&(nxrt_options){.quiet = 1})) { printf("FAIL nxrt_init\n"); exit(1); }
    uint8_t *img = (uint8_t *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(img + ((IMAGE_DOS_HEADER *)img)->e_lfanew);
    if (nxrt_code_table_create(NXRT_TOKEN_BASE, NXRT_MODULE_BASE + 0x100000) || nxrt_code_attach(img, nt->OptionalHeader.SizeOfImage)) { printf("FAIL code table\n"); exit(1); }
    nxrt_thread_main_init();
    NxwGuestCpu *c = nxrt_cur_cpu();
    c->x[1] = 64u << 20;
    if (gt_svc(c, 0x01)) { printf("FAIL SetHeapSize\n"); exit(1); }   /* SetHeapSize */
    gt_heap = c->x[1]; gt_heap_size = 64u << 20; g_brk = gt_heap;
}

/* The SDK's InternalCriticalSection on Horizon: the lock word holds the owner's handle, bit 30 when others wait; the kernel hands it over */
#define WAITERS 0x40000000u
void gt_lock(NxwGuestCpu *c, _Atomic uint32_t *w) {
    uint32_t tag = gt_self();
    for (;;) {
        uint32_t v = 0;
        if (atomic_compare_exchange_strong(w, &v, tag)) return;
        if ((v & ~WAITERS) == tag) return;   /* handed over by the kernel */
        if (!(v & WAITERS) && !atomic_compare_exchange_strong(w, &v, v | WAITERS)) continue;
        c->x[0] = v & ~WAITERS; c->x[1] = (uint64_t)(uintptr_t)w; c->x[2] = tag;
        gt_svc(c, 0x1A);   /* ArbitrateLock */
        if ((atomic_load(w) & ~WAITERS) == tag) return;
    }
}
void gt_unlock(NxwGuestCpu *c, _Atomic uint32_t *w) {
    uint32_t v = gt_self();
    if (atomic_compare_exchange_strong(w, &v, 0)) return;
    c->x[0] = (uint64_t)(uintptr_t)w;
    gt_svc(c, 0x1B);   /* ArbitrateUnlock: the next waiter gets it */
}
uint32_t gt_cond_wait(NxwGuestCpu *c, _Atomic uint32_t *lock, _Atomic uint32_t *key, int64_t timeout_ns) {
    c->x[0] = (uint64_t)(uintptr_t)lock; c->x[1] = (uint64_t)(uintptr_t)key; c->x[2] = gt_self(); c->x[3] = (uint64_t)timeout_ns;
    uint32_t r = (uint32_t)gt_svc(c, 0x1C);   /* WaitProcessWideKeyAtomic: releases the lock; on success the kernel gives it back */
    if (r) gt_lock(c, lock);                  /* a timeout returns without it */
    return r;
}
void gt_cond_signal(NxwGuestCpu *c, _Atomic uint32_t *key, int count) {
    if (!atomic_load(key)) return;   /* as the SDK: no waiter, no SVC */
    c->x[0] = (uint64_t)(uintptr_t)key; c->x[1] = (uint64_t)(int64_t)count;
    gt_svc(c, 0x1D);
}

gt_thread_t gt_thread(NxwGuestCpu *c, NxwGuestFn entry, uint64_t arg, int prio, int core) {
    static _Atomic uint64_t next_dst;
    const nxrt_layout *l = nxrt_as_layout();
    gt_thread_t t = {0};
    t.stack_size = 0x10000;
    t.stack_src = (uint64_t)(uintptr_t)gt_alloc(t.stack_size + 0x1000);
    t.stack_src = (t.stack_src + 0xfff) & ~0xfffull;
    t.stack_dst = l->stack + 0x100000 + atomic_fetch_add(&next_dst, 0x20000);
    c->x[0] = t.stack_dst; c->x[1] = t.stack_src; c->x[2] = t.stack_size;
    T(gt_svc(c, 0x04) == 0);   /* MapMemory */
    uint64_t va = gt_fn(entry);
    c->x[1] = va; c->x[2] = arg; c->x[3] = t.stack_dst + t.stack_size; c->x[4] = (uint64_t)prio; c->x[5] = (uint64_t)(int64_t)core;
    T(gt_svc(c, 0x08) == 0);   /* CreateThread */
    t.handle = (uint32_t)c->x[1];
    c->x[0] = t.handle;
    T(gt_svc(c, 0x09) == 0);   /* StartThread */
    return t;
}
void gt_join(NxwGuestCpu *c, gt_thread_t *t) {
    uint32_t *hs = gt_alloc(4); hs[0] = t->handle;
    c->x[1] = (uint64_t)(uintptr_t)hs; c->x[2] = 1; c->x[3] = (uint64_t)-1;
    T(gt_svc(c, 0x18) == 0 && c->x[1] == 0);   /* WaitSynchronization: a thread signals when it exits */
    c->x[0] = t->handle; T(gt_svc(c, 0x16) == 0);   /* CloseHandle */
    c->x[0] = t->stack_dst; c->x[1] = t->stack_src; c->x[2] = t->stack_size;
    T(gt_svc(c, 0x05) == 0);   /* UnmapMemory */
}
