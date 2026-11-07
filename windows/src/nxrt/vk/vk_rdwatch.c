// Diagnostics: who reads (or writes) ranges of guest memory. NXRT_VK_RDWATCH=<hex addr>[:<bytes>][,<addr>[:<bytes>]...] and/or the word "gpu" (every range a compute shader writes through a storage
// buffer, from the replay: the GPU->guest hypothesis "the game reads compute outputs back on the CPU"). The pages of the ranges are armed PROT_NONE; the first fault of a page since it was armed is
// recorded (faulting address, host pc, the guest function that holds the pc, the thread's guest x29/x30) and the page is opened again. A sampler thread arms the pages every
// NXRT_VK_RDWATCH_US microseconds (default 1000), so several accesses per frame show up. Every distinct (guest function, offset, kind, range) is logged once with counts every NXRT_VK_RDWATCH_REP presents.
// Faults on the page beside a range are only opened again. Linux only; the dirty tracker's invariants are bent while a page is armed (a debugging tool: do not use it for measurements of speed).
#include "vk_mem.h"
#ifndef _WIN32
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

void *nxrt_cur_cpu(void);   // the guest thread's NxwGuestCpu (x[31] first), NULL on a host thread
void nxrt_symbolize(uint64_t guest_va, char *out, size_t n);
uint64_t nxrt_host_to_guest(uint64_t host_addr, uint64_t *off);
void dm_fault_trap_register(int (*fn)(void *addr, int is_write, void *ctx));   // host/crash.c: called ahead of the other page fault hooks
int dm_fault_is_wp(void *addr);

enum { MAXR = 256, MAXP = 4096, MAXE = 512, MAXA = 512 };
typedef struct { uintptr_t lo, hi; char who[20]; } range_t;
static range_t g_r[MAXR]; static _Atomic int g_nr;
static uintptr_t g_pg[MAXP]; static _Atomic uint8_t g_armed[MAXP]; static _Atomic int g_np;
typedef struct { uint64_t addr, pc, x29, x30; int tid, w, r; uint32_t val; } ev_t;
static ev_t g_ev[MAXE]; static _Atomic uint32_t g_nev;
static _Atomic uint64_t g_hits, g_lost, g_arm_ok, g_arm_fail; static _Atomic int g_arm_errno;
static uint64_t g_pgsz; static int g_on, g_gpu;

static int trap(void *addr, int is_write, void *ctx) {
    uintptr_t a = (uintptr_t)addr, pg = a & ~(g_pgsz - 1);
    int np = atomic_load_explicit(&g_np, memory_order_acquire), k;
    for (k = 0; k < np; k++) if (g_pg[k] == pg) break;
    if (k == np || !atomic_exchange(&g_armed[k], 0)) return 0;   // not ours / not armed: someone else's fault
    int nr = atomic_load_explicit(&g_nr, memory_order_acquire), r;
    for (r = 0; r < nr; r++) if (a >= g_r[r].lo && a < g_r[r].hi) break;
    atomic_fetch_add(&g_hits, 1);
    mprotect((void *)pg, g_pgsz, dm_fault_is_wp(addr) ? PROT_READ : PROT_READ | PROT_WRITE);
    if (r < nr) {   // inside a watched range (the rest of the page is only opened again)
        ucontext_t *uc = ctx; uint32_t i = atomic_fetch_add(&g_nev, 1);
        if (i < MAXE) {
            ev_t *e = &g_ev[i]; e->addr = a; e->pc = (uint64_t)uc->uc_mcontext.gregs[REG_RIP]; e->tid = (int)syscall(SYS_gettid); e->w = is_write; e->r = r;
            e->val = *(volatile uint32_t *)(a & ~3ull);   // the word as the reader finds it (the access has not happened yet)
            const uint64_t *c = nxrt_cur_cpu(); e->x29 = c ? c[29] : 0; e->x30 = c ? c[30] : 0;
        } else atomic_fetch_add(&g_lost, 1);
    }
    return 1;
}
static void arm_all(void) {
    int np = atomic_load_explicit(&g_np, memory_order_acquire);
    for (int k = 0; k < np; k++) {
        atomic_store(&g_armed[k], 1);   // always protected again: a MAP_FIXED over the page (the guest heap's commit) drops the protection without a fault
        if (mprotect((void *)g_pg[k], g_pgsz, PROT_NONE)) { atomic_store(&g_armed[k], 0); atomic_fetch_add(&g_arm_fail, 1); atomic_store(&g_arm_errno, errno); } else atomic_fetch_add(&g_arm_ok, 1);
    }
}
static useconds_t g_us = 1000;
static void *sampler(void *arg) { (void)arg; for (;;) { usleep(g_us); arm_all(); } return NULL; }

// Called from the replay thread only
void vkm_rdwatch_add(uint64_t addr, uint64_t len, const char *who) {
    if (!g_on || !len) return;
    int nr = atomic_load(&g_nr);
    for (int r = 0; r < nr; r++) if (g_r[r].lo == addr && g_r[r].hi == addr + len) return;
    if (nr >= MAXR) return;
    uintptr_t p0 = (uintptr_t)(addr & ~(g_pgsz - 1)), p1 = (uintptr_t)((addr + len + g_pgsz - 1) & ~(g_pgsz - 1));
    if ((p1 - p0) / g_pgsz > 16) { p1 = p0 + 16 * g_pgsz; len = p1 - addr; }   // a long range: its first 64 KiB
    g_r[nr].lo = (uintptr_t)addr; g_r[nr].hi = (uintptr_t)(addr + len); snprintf(g_r[nr].who, sizeof g_r[nr].who, "%s", who ? who : "");
    atomic_store_explicit(&g_nr, nr + 1, memory_order_release);
    for (uintptr_t p = p0; p < p1; p += g_pgsz) {
        int np = atomic_load(&g_np), k; for (k = 0; k < np; k++) if (g_pg[k] == p) break;
        if (k == np && np < MAXP) { g_pg[np] = p; atomic_store(&g_armed[np], 0); atomic_store_explicit(&g_np, np + 1, memory_order_release); }
    }
}
int vkm_rdwatch_gpu(void) { return g_on && g_gpu; }

typedef struct { uint64_t fn, off; int w, r; uint64_t addr, n, first_frame, nzero, nnan, ninf; uint32_t lastval; char nm[96]; } agg_t;
static agg_t g_agg[MAXA]; static int g_nagg;
void vkm_rdwatch_init(void) {
    const char *e = getenv("NXRT_VK_RDWATCH"); if (!e || !*e) return;
    g_pgsz = (uint64_t)getpagesize(); g_on = 1;
    const char *u = getenv("NXRT_VK_RDWATCH_US"); if (u) g_us = (useconds_t)atoi(u);
    char *s = strdup(e), *sp = NULL;
    for (char *t = strtok_r(s, ",", &sp); t; t = strtok_r(NULL, ",", &sp)) {
        if (!strcmp(t, "gpu")) { g_gpu = 1; VKLOG("rdwatch: every storage buffer a compute shader writes"); continue; }
        char *end; uint64_t a = strtoull(t, &end, 16), n = *end == ':' ? strtoull(end + 1, NULL, 0) : 4;
        vkm_rdwatch_add(a, n, "cmdline"); VKLOG("rdwatch: range %#llx +%llu", (unsigned long long)a, (unsigned long long)n);
    }
    free(s);
    dm_fault_trap_register(trap);
    pthread_t th; pthread_create(&th, NULL, sampler, NULL); pthread_detach(th);
}
void vkm_rdwatch_frame(uint64_t frame) {
    if (!g_on) return;
    uint32_t n = atomic_exchange(&g_nev, 0); if (n > MAXE) n = MAXE;
    for (uint32_t i = 0; i < n; i++) {
        ev_t *e = &g_ev[i]; uint64_t off = 0, g = nxrt_host_to_guest(e->pc, &off);
        uint64_t fn = g ? g : e->pc, fo = g ? off : 0;
        int j; for (j = 0; j < g_nagg; j++) if (g_agg[j].fn == fn && g_agg[j].off == fo && g_agg[j].w == e->w && g_agg[j].r == e->r) break;
        if (j == g_nagg) {
            if (g_nagg >= MAXA) continue;
            agg_t *a = &g_agg[g_nagg++]; memset(a, 0, sizeof *a); a->fn = fn; a->off = fo; a->w = e->w; a->r = e->r; a->addr = e->addr; a->first_frame = frame;
            char s[80], l[80];
            if (g) { nxrt_symbolize(g, s, sizeof s); nxrt_symbolize(e->x30, l, sizeof l); snprintf(a->nm, sizeof a->nm, "guest %s", s); }
            else snprintf(a->nm, sizeof a->nm, "host pc %#llx", (unsigned long long)e->pc);
            VKLOG("rdwatch: NEW %s %#llx (range %#llx +%llu, written by compute %s) by %s+%#llx tid %d%s%s", e->w ? "WRITE" : "read", (unsigned long long)e->addr, (unsigned long long)g_r[e->r].lo,
                  (unsigned long long)(g_r[e->r].hi - g_r[e->r].lo), g_r[e->r].who, a->nm, (unsigned long long)fo, e->tid, g ? ", guest x30 " : "", g ? l : "");
        }
        g_agg[j].n++; if (!e->w) { if (!e->val) g_agg[j].nzero++; if ((e->val & 0x7f800000) == 0x7f800000) { if (e->val & 0x7fffff) g_agg[j].nnan++; else g_agg[j].ninf++; } g_agg[j].lastval = e->val; }
    }
    static uint64_t rep; if (!rep) { const char *e = getenv("NXRT_VK_RDWATCH_REP"); rep = e ? strtoull(e, NULL, 0) : 300; if (!rep) rep = 300; }
    if (frame % rep == 0) {
        int nw = 0, nrd = 0; for (int j = 0; j < g_nagg; j++) { if (g_agg[j].w) nw++; else nrd++; }
        VKLOG("rdwatch: frame %llu: %d ranges on %d pages, arms ok %llu failed %llu (errno %d), %llu faults in all, %llu lost; %d distinct readers, %d distinct writers", (unsigned long long)frame, atomic_load(&g_nr),
              atomic_load(&g_np), (unsigned long long)atomic_load(&g_arm_ok), (unsigned long long)atomic_load(&g_arm_fail), atomic_load(&g_arm_errno), (unsigned long long)atomic_load(&g_hits), (unsigned long long)atomic_load(&g_lost), nrd, nw);
        for (int j = 0; j < g_nagg; j++) VKLOG("rdwatch:   %s %#llx (range %#llx %s) x%llu (first frame %llu; %llu read as zero, %llu as NaN, %llu as inf, last %#x) %s+%#llx", g_agg[j].w ? "W" : "r", (unsigned long long)g_agg[j].addr, (unsigned long long)g_r[g_agg[j].r].lo, g_r[g_agg[j].r].who,
                                  (unsigned long long)g_agg[j].n, (unsigned long long)g_agg[j].first_frame, (unsigned long long)g_agg[j].nzero, (unsigned long long)g_agg[j].nnan, (unsigned long long)g_agg[j].ninf, g_agg[j].lastval, g_agg[j].nm, (unsigned long long)g_agg[j].off);
    }
}
#else
void vkm_rdwatch_init(void) {}
void vkm_rdwatch_frame(uint64_t frame) { (void)frame; }
void vkm_rdwatch_add(uint64_t addr, uint64_t len, const char *who) { (void)addr; (void)len; (void)who; }
int vkm_rdwatch_gpu(void) { return 0; }
#endif
