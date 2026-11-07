/* The SVC dispatcher on synthetic register frames (the guest main thread's NxwGuestCpu): memory (heap, QueryMemory, MapMemory as a real alias,
   MapPhysicalMemory), GetInfo, the system tick at 31.25 MHz, events, thread queries, sleep and yield, the defaults; and the host-call path end to end
   (a token through the code table, its thunk, the registry, a ported adapter). */
#include "guest_test.h"

NXRT_EARLY_RESERVE()

typedef struct { uint64_t base, size; uint32_t state, attr, perm, ipc, device, pad; } MemoryInfo;
static MemoryInfo query(NxwGuestCpu *c, uint64_t addr) {
    MemoryInfo *mi = gt_alloc(sizeof *mi);
    c->x[0] = (uint64_t)(uintptr_t)mi; c->x[1] = 0; c->x[2] = addr;
    T(gt_svc(c, 0x06) == 0);
    return *mi;
}
static uint64_t info(NxwGuestCpu *c, uint64_t id) { c->x[0] = 0; c->x[1] = id; c->x[2] = 0; c->x[3] = 0; T(gt_svc(c, 0x29) == 0); return c->x[1]; }

/* A crash on a guest thread: the report names the fault and carries the guest context */
static void crasher(NxwGuestCpu *c) { c->x[19] = 0x1234567; c->pc = NXRT_MODULE_BASE + 0x4444; *(volatile int *)(uintptr_t)0x10 = 1; }
static int crash_child(void) {
    gt_init();
    NxwGuestCpu *c = nxrt_cur_cpu();
    gt_thread(c, crasher, 0, 44, 0);
    Sleep(10000);
    return 3;   /* not reached: the unhandled-exception filter ends the process */
}
static int crash_report_test(void) {
    char exe[MAX_PATH], cmd[MAX_PATH + 16];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    snprintf(cmd, sizeof cmd, "\"%s\" --crash", exe);
    STARTUPINFOA si; PROCESS_INFORMATION pi; ZeroMemory(&si, sizeof si); si.cb = sizeof si;
    uint64_t t0 = now_ns_test();
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    /* the newest crash report in the log folder */
    char dir[MAX_PATH] = ".", pat[MAX_PATH + 8], path[2 * MAX_PATH]; const char *d = getenv("NXRT_LOG_DIR"); if (d) snprintf(dir, sizeof dir, "%s", d);
    snprintf(pat, sizeof pat, "%s\\*.crash.txt", dir);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd); FILETIME best = {0}; path[0] = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do if (CompareFileTime(&fd.ftLastWriteTime, &best) > 0) { best = fd.ftLastWriteTime; snprintf(path, sizeof path, "%s\\%s", dir, fd.cFileName); } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    char buf[8192] = {0}; FILE *f = path[0] ? fopen(path, "r") : NULL;
    if (f) { fread(buf, 1, sizeof buf - 1, f); fclose(f); }
    int ok = code == 128 + 11 && strstr(buf, "exception 0xc0000005") && strstr(buf, "write to 0x10") && strstr(buf, "x19=0000000001234567") && strstr(buf, "guest context");
    printf("crash on a guest thread: exit %lu, report %s %s (%.0f ms)\n", code, path, ok ? "complete" : "INCOMPLETE", (now_ns_test() - t0) / 1e6);
    if (!ok) printf("%s\n", buf);
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--crash")) return crash_child();
    gt_init();
    NxwGuestCpu *c = nxrt_cur_cpu();
    const nxrt_layout *l = nxrt_as_layout();
    T(c && c->tpidrro_el0 && c->tpidr_el0 && c->sp > l->stack && c->sp <= l->stack + l->stack_size);
    T(*(uint32_t *)(uintptr_t)(c->tpidrro_el0 + 0x110) == 0x51);   /* the thread ID in the TLR (SDK 22's mutex owner) */

    /* GetInfo: nxnative's layout and profile */
    T(info(c, 2) == l->alias && info(c, 3) == l->alias_size && info(c, 4) == l->heap && info(c, 5) == l->heap_size);
    T(info(c, 12) == l->as_base && info(c, 13) == l->as_size && info(c, 14) == l->stack && info(c, 15) == l->stack_size);
    T(info(c, 6) == 0x280000000ull && info(c, 7) == (64u << 20) + 0x5600000ull && info(c, 18) == 0x0400486024DEC000ull);
    T(info(c, 33) == 0x1000 && info(c, 23) == 1 && info(c, 24) == 95);   /* 4 KB pages; one game thread of 96 */
    T(info(c, 11) != info(c, 11));   /* entropy */

    /* SetHeapSize: the heap opened by gt_init commits lazily, on first touch */
    MemoryInfo m = query(c, gt_heap + 0x123456);
    T(m.base == l->heap && m.size == (64u << 20) && m.state == 5 && m.perm == 3);
    *(volatile uint64_t *)(uintptr_t)(gt_heap + (60u << 20)) = 0x1122334455667788ull;
    T(*(volatile uint64_t *)(uintptr_t)(gt_heap + (60u << 20)) == 0x1122334455667788ull);
    m = query(c, gt_heap + (64u << 20));
    T(m.state == 0 && m.base == gt_heap + (64u << 20));
    c->x[1] = l->heap_size + 0x200000; T(gt_svc(c, 0x01) == 0xD001);            /* beyond the heap region */
    c->x[1] = 0x280000000ull + 0x200000; T(gt_svc(c, 0x01) == 0x10801);          /* past the pool (no modules loaded here) */
    c->x[1] = 96u << 20; T(gt_svc(c, 0x01) == 0 && c->x[1] == l->heap);          /* grow */
    *(volatile uint32_t *)(uintptr_t)(gt_heap + (90u << 20)) = 7;
    c->x[1] = 64u << 20; T(gt_svc(c, 0x01) == 0);                                /* shrink: the old bytes stay below the end */
    T(*(volatile uint64_t *)(uintptr_t)(gt_heap + (60u << 20)) == 0x1122334455667788ull);
    T(plat_read(gt_heap + (90u << 20), &m.pad, 4) != 0);                          /* beyond the end: closed */

    /* MapMemory: a heap range a second time in the stack region (a view of the same pages) */
    uint64_t src = (uint64_t)(uintptr_t)gt_alloc(0x5000); src = (src + 0xfff) & ~0xfffull;
    uint64_t dst = l->stack + 0x40000;
    memcpy((void *)(uintptr_t)src, "stack bytes", 12);
    c->x[0] = dst; c->x[1] = src; c->x[2] = 0x4000; T(gt_svc(c, 0x04) == 0);
    T(!memcmp((void *)(uintptr_t)dst, "stack bytes", 12));
    *(volatile char *)(uintptr_t)(dst + 0x3fff) = 'z';
    T(*(volatile char *)(uintptr_t)(src + 0x3fff) == 'z');
    m = query(c, dst + 0x10); T(m.base == dst && m.size == 0x4000 && m.state == 0xB && m.perm == 3);
    m = query(c, src); T(m.attr == 1 && m.perm == 0);                             /* the source is borrowed */
    c->x[0] = dst + 0x1000; c->x[1] = src; c->x[2] = 0x1000; T(gt_svc(c, 0x04) == 0xD401);   /* overlaps */
    c->x[0] = dst + 1; c->x[1] = src; c->x[2] = 0x1000; T(gt_svc(c, 0x04) == 0xCC01);
    c->x[0] = dst; c->x[1] = src; c->x[2] = 0; T(gt_svc(c, 0x04) == 0xCA01);
    c->x[0] = l->heap; c->x[1] = src; c->x[2] = 0x1000; T(gt_svc(c, 0x04) == 0xDC01);         /* destination outside the stack region */
    c->x[0] = dst; c->x[1] = src + 0x1000; c->x[2] = 0x4000; T(gt_svc(c, 0x05) == 0xD401);   /* not the mapping */
    c->x[0] = dst; c->x[1] = src; c->x[2] = 0x4000; T(gt_svc(c, 0x05) == 0);
    T(plat_read(dst, &m.pad, 4) != 0);
    m = query(c, dst); T(m.state == 0);
    T(*(volatile char *)(uintptr_t)(src + 0x3fff) == 'z');

    /* MapPhysicalMemory in the alias region; a page mapped again reads zero */
    uint64_t al = l->alias + 0x200000;
    c->x[0] = al; c->x[1] = 0x3000; T(gt_svc(c, 0x2C) == 0);
    *(volatile uint32_t *)(uintptr_t)(al + 0x1000) = 0xfeedu;
    m = query(c, al + 0x1000); T(m.base == al && m.size == 0x3000 && m.state == 5);
    c->x[0] = al; c->x[1] = 0x3000; T(gt_svc(c, 0x2D) == 0);
    m = query(c, al); T(m.state == 0);
    c->x[0] = al; c->x[1] = 0x3000; T(gt_svc(c, 0x2C) == 0);
    T(*(volatile uint32_t *)(uintptr_t)(al + 0x1000) == 0);
    c->x[0] = l->heap; c->x[1] = 0x1000; T(gt_svc(c, 0x2C) == 0xD401);   /* outside the alias region */

    /* the system tick: 31.25 MHz from QueryPerformanceCounter, through the SVC and the counter register */
    T(gt_svc(c, 0x1E) == 0);
    uint64_t t0 = c->x[1], n0 = now_ns_test();
    plat_sleep_ns(100000000);
    uint64_t t1 = rt_mrs(c, NXRT_SR_CNTPCT_EL0), n1 = now_ns_test();
    double hz = (double)(t1 - t0) / ((n1 - n0) / 1e9);
    printf("system tick: %.0f Hz over %.1f ms\n", hz, (n1 - n0) / 1e6);
    T(hz > 31.0e6 && hz < 31.5e6 && rt_mrs(c, NXRT_SR_CNTFRQ_EL0) == 31250000);
    T(rt_mrs(c, NXRT_SR_TPIDRRO_EL0) == c->tpidrro_el0 && rt_mrs(c, NXRT_SR_DCZID_EL0) == 4);
    rt_msr(c, NXRT_SR_TPIDR_EL0, 0x1234); T(c->tpidr_el0 == 0x1234);

    /* events: create, signal, wait, clear, reset, close; wrong handles */
    T(gt_svc(c, 0x45) == 0);
    uint32_t hw = (uint32_t)c->x[1], hr = (uint32_t)c->x[2], *hs = gt_alloc(8);
    hs[0] = hr;
    c->x[1] = (uint64_t)(uintptr_t)hs; c->x[2] = 1; c->x[3] = 0; T(gt_svc(c, 0x18) == R_TIMEOUT);
    c->x[0] = hr; T(gt_svc(c, 0x17) == R_BAD_STATE);    /* ResetSignal of an unsignalled event */
    c->x[0] = hr; T(gt_svc(c, 0x11) == R_BAD_HANDLE);   /* SignalEvent takes the write end */
    c->x[0] = hw; T(gt_svc(c, 0x11) == 0);
    c->x[1] = (uint64_t)(uintptr_t)hs; c->x[2] = 1; c->x[3] = (uint64_t)-1; T(gt_svc(c, 0x18) == 0 && c->x[1] == 0);
    c->x[0] = hr; T(gt_svc(c, 0x17) == 0);
    uint64_t w0 = now_ns_test();
    c->x[1] = (uint64_t)(uintptr_t)hs; c->x[2] = 1; c->x[3] = 5000000; T(gt_svc(c, 0x18) == R_TIMEOUT);
    T(now_ns_test() - w0 >= 4500000);
    c->x[0] = hw; T(gt_svc(c, 0x16) == 0); c->x[0] = hr; T(gt_svc(c, 0x16) == 0);
    hs[0] = 0x1fff; c->x[1] = (uint64_t)(uintptr_t)hs; c->x[2] = 1; c->x[3] = 0; T(gt_svc(c, 0x18) == R_BAD_HANDLE);

    /* thread queries */
    uint32_t me = gt_self();
    c->x[1] = me; T(gt_svc(c, 0x25) == 0 && c->x[1] == 0x51);              /* GetThreadId */
    c->x[1] = 0xFFFF8000; T(gt_svc(c, 0x0C) == 0 && c->x[1] == 44);        /* GetThreadPriority of the current thread */
    c->x[0] = me; c->x[1] = 40; T(gt_svc(c, 0x0D) == 0);
    c->x[1] = me; T(gt_svc(c, 0x0C) == 0 && c->x[1] == 40);
    c->x[0] = me; c->x[1] = (uint64_t)-3; c->x[2] = 0x6; T(gt_svc(c, 0x0F) == 0);   /* SetThreadCoreMask: the ideal core moves into the mask */
    T(gt_svc(c, 0x10) == 1);                                                 /* GetCurrentProcessorNumber */
    c->x[2] = me; T(gt_svc(c, 0x0E) == 0 && c->x[1] == 1 && c->x[2] == 6);
    c->x[0] = 0; T(gt_svc(c, 0x0B) == 0);                                    /* SleepThread(0): a yield */
    w0 = now_ns_test(); c->x[0] = 2000000; gt_svc(c, 0x0B); T(now_ns_test() - w0 >= 1900000);
    c->x[0] = me; c->x[1] = 1; T(gt_svc(c, 0x32) == R_BAD_STATE);           /* SetThreadActivity on itself */

    /* the rest: OutputDebugString, a Break that only notifies, an SVC the host has no handler for */
    char *s = gt_alloc(8); memcpy(s, "hello", 6);
    c->x[0] = (uint64_t)(uintptr_t)s; c->x[1] = 5; T(gt_svc(c, 0x27) == 0);
    c->x[0] = 0x80000000; c->x[1] = 0; c->x[2] = 0; T(gt_svc(c, 0x26) == 0x80000000);
    c->x[0] = 1; T(gt_svc(c, 0x7F) == 0);

    /* a host call through a token: guest call -> code table -> thunk -> registry -> hle_net.c's GetLastErrno (ENETDOWN, 50), then SetLastErrno */
    uint64_t tok = nxrt_token_of("nnsocketGetLastErrno");
    T(tok >= NXRT_TOKEN_BASE && tok < NXRT_TOKEN_BASE + NXRT_TOKEN_SIZE && !strcmp(nxrt_token_name(tok), "nnsocketGetLastErrno"));
    T(*(uint32_t *)(uintptr_t)tok == 0xD429CB00u);   /* BRK #0x4E58 */
    T(nxrt_guest_call(tok, 0, NULL) == 50);
    uint64_t e7 = 7; nxrt_guest_call(nxrt_token_of("nnsocketSetLastErrno"), 1, &e7);
    T(nxrt_guest_call(tok, 0, NULL) == 7);
    T(rt_miss(c, NXRT_RETURN_TO_HOST) == rt_return_to_host);
    T(c->x[30] == NXRT_RETURN_TO_HOST);   /* the guest call restored x30 */

    T(crash_report_test() == 0);
    printf(gt_fail ? "nxrt.svc: FAILED\n" : "nxrt.svc: ok\n");
    return gt_fail;
}
