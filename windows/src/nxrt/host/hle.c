// from nxnative dm/host/hle.c (MIT, © seedds): import resolution (implemented ones go to the host, the rest go to named stubs) and the host's entry from guest code.
// Windows: the registry is generated (gen/out/hle_registry.gen.c, from the same HLE_FORCE / HLE / NVN lines nxnative registers in Mach-O sections), and every
// host replacement has a guest-visible address, its host-call token: NXRT_TOKEN_BASE + 16 × index. Token 0 is RETURN_TO_HOST, 1..N the registry in order, then
// the named stubs (nxnative's 4,096 unimpl_stubs in entry.S), made as imports need them. The gate dispatch is gone: rt.c calls svc_dispatch directly.
#include "dm.h"
#include "addrspace.h"
#include "hle_registry.h"
#include "nxrt/runtime.h"
#include <immintrin.h>
#include <stdatomic.h>

#define MAX_TOKEN (NXRT_TOKEN_SIZE / NXRT_TOKEN_STRIDE)
#define MAX_STUB 4096                 // as nxnative's entry.S
typedef struct { const char *name; NxwGuestFn fn; const nxrt_hle_entry *e; _Atomic uint32_t calls; } token_t;
static token_t g_tok[MAX_TOKEN];
static unsigned g_ntok = 1, g_nstub;
static uint8_t g_stub_seen[MAX_STUB];
static int g_tokens_ready, g_bound_relocs;
static int g_trace_nvn_calls;

static uint32_t name_h(const char *s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }
static unsigned *g_idx, g_mask;   // name → token, open addressing; the first entry of a duplicated name wins
static void idx_put(unsigned k) {
    for (unsigned j = name_h(g_tok[k].name) & g_mask;; j = (j + 1) & g_mask) {
        if (!g_idx[j]) { g_idx[j] = k; return; }
        if (!strcmp(g_tok[g_idx[j]].name, g_tok[k].name)) return;
    }
}
static unsigned idx_find(const char *name) {
    if (!g_mask) return 0;
    for (unsigned j = name_h(name) & g_mask; g_idx[j]; j = (j + 1) & g_mask) if (!strcmp(g_tok[g_idx[j]].name, name)) return g_idx[j];
    return 0;
}

// The token page: what a guest that reads a host function's bytes finds there. Word 0 = BRK #0x4E58 ("NX"), word 1 = the token index; recompiled code never runs it
static void token_page_write(void) {
    uint64_t base = NXRT_TOKEN_BASE;
    if (!nxrt_as_range(NXRT_AS_TOKENS)->reserved) die("the host-call token range %#llx is not reserved", (unsigned long long)base);
    int as_commit_private(uint64_t a, uint64_t n, int prot);
    int rc = as_commit_private(base, NXRT_TOKEN_SIZE, AS_RW);
    if (rc) die("cannot commit the host-call token range (step %d, error %lu)", rc, (unsigned long)errno);
    for (unsigned k = 0; k < MAX_TOKEN; k++) { uint32_t *w = (uint32_t *)(uintptr_t)(base + (uint64_t)k * NXRT_TOKEN_STRIDE); w[0] = 0xD4200000u | (0x4E58u << 5); w[1] = k; }
    as_protect(base, NXRT_TOKEN_SIZE, AS_RO);
}

void nxrt_tokens_init(void) {
    if (g_tokens_ready) return;
    g_tokens_ready = 1;
    g_trace_nvn_calls = getenv("DM_NVN_CALL_TRACE") != NULL;
    token_page_write();
    g_tok[0].name = "RETURN_TO_HOST"; g_tok[0].fn = rt_return_to_host;
    unsigned cap = 16; while (cap < (nxrt_hle_count + MAX_STUB) * 2) cap <<= 1;
    g_idx = calloc(cap, sizeof *g_idx); g_mask = cap - 1;
    for (unsigned i = 0; i < nxrt_hle_count; i++) {
        unsigned k = g_ntok++;
        g_tok[k].name = nxrt_hle_table[i].name; g_tok[k].fn = nxrt_hle_table[i].adapter; g_tok[k].e = &nxrt_hle_table[i];
        idx_put(k);
    }
}
uint64_t nxrt_token_addr(unsigned k) { return NXRT_TOKEN_BASE + (uint64_t)k * NXRT_TOKEN_STRIDE; }
uint64_t nxrt_token_of(const char *name) { nxrt_tokens_init(); unsigned k = idx_find(name); return k ? nxrt_token_addr(k) : 0; }
const char *nxrt_token_name(uint64_t va) {
    if (va < NXRT_TOKEN_BASE || va >= NXRT_TOKEN_BASE + NXRT_TOKEN_SIZE || (va & (NXRT_TOKEN_STRIDE - 1))) return NULL;
    unsigned k = (unsigned)((va - NXRT_TOKEN_BASE) / NXRT_TOKEN_STRIDE);
    return k < g_ntok ? g_tok[k].name : NULL;
}
int nxrt_token_count(void) { return (int)g_ntok; }
const nxrt_hle_entry *nxrt_hle_find(const char *name, int kind) {
    nxrt_tokens_init();
    unsigned k = idx_find(name);
    return k && g_tok[k].e && (!kind || g_tok[k].e->kind == kind) ? g_tok[k].e : NULL;
}

// ---------- Unimplemented imports: named stubs ----------
// An import nothing defines and the host does not implement gets a token of its own that logs "UNIMPL name" once and returns 0 (x0/x1, v0-v3)
uint64_t resolve_import(const char *name) {
    nxrt_tokens_init();
    unsigned k = idx_find(name);
    if (k) return nxrt_token_addr(k);
    if (g_nstub >= MAX_STUB || g_ntok >= MAX_TOKEN) die("stub overflow");
    k = g_ntok++; g_nstub++;
    g_tok[k].name = strdup(name); g_tok[k].fn = NULL;
    idx_put(k);
    return nxrt_token_addr(k);
}
// ---------- Run-time tokens: a host function with a hand-written adapter, at a guest-visible address ----------
// For host code the guest calls through pointers the host plants at run time: the profanity filter's vtable (hle_misc.c) and the entry of a guest thread
// the host starts (hle_guestthread.c). The same adapter always gets the same token; tokens are never freed (there are a handful)
static plat_rwlock_t g_dyn_lock = PLAT_RWLOCK_INIT;
uint64_t nxrt_host_fn_token(const char *name, NxwGuestFn adapter) {
    nxrt_tokens_init();
    plat_rw_wrlock(&g_dyn_lock);
    unsigned k = 0;
    for (unsigned i = (unsigned)nxrt_hle_count + 1; i < g_ntok && !k; i++) if (g_tok[i].fn == adapter) k = i;
    if (!k) {
        if (g_ntok >= MAX_TOKEN) die("host-call token overflow (%s)", name);
        k = g_ntok;
        g_tok[k].name = name; g_tok[k].fn = adapter;
        g_ntok = k + 1;   // published after the entry is complete: nxrt_host_call reads it without the lock
    }
    plat_rw_wrunlock(&g_dyn_lock);
    return nxrt_token_addr(k);
}

// The host implements this import (the swhle table), without allocating a stub for it
int hle_implements(const char *name) { return nxrt_hle_find(name, NXRT_HLE_SWHLE) != NULL; }
void stubs_report(void) { LOG("named stubs=%u", g_nstub); }
void nxrt_hle_note_bound(int n) { g_bound_relocs += n; }

// The host's entry from guest code: a token's thunk (rt.c) or a miss on a token address. Counts the call, runs the adapter, checks MXCSR in debug builds
// (research/05: guest threads need 0x1F80; a driver that sets FTZ/DAZ on the calling thread must not leak into the game)
void nxrt_host_call(NxwGuestCpu *cpu, uint32_t k) {
    if (k >= g_ntok) { LOG("[RED] call to an unassigned host-call token %u (lr %#llx)", k, (unsigned long long)cpu->x[30]); cpu->x[0] = 0; return; }
    token_t *t = &g_tok[k];
    uint32_t previous_calls = atomic_fetch_add_explicit(&t->calls, 1, memory_order_relaxed);
    if (!previous_calls && g_trace_nvn_calls && t->e &&
        (t->e->kind == NXRT_HLE_NVN || !strcmp(t->name, "nvnBootstrapLoader")))
        LOG("[nvn-call] %s x0=%#llx x1=%#llx x2=%#llx x3=%#llx x4=%#llx x5=%#llx lr=%#llx", t->name,
            (unsigned long long)cpu->x[0], (unsigned long long)cpu->x[1], (unsigned long long)cpu->x[2],
            (unsigned long long)cpu->x[3], (unsigned long long)cpu->x[4], (unsigned long long)cpu->x[5], (unsigned long long)cpu->x[30]);
    static int tr = -1; static double tr_from; if (tr < 0) { const char *e = getenv("DM_HLETRACE"); tr = e ? 1 : 0; tr_from = e ? atof(e) : 0; }
    int trace = tr && (now_ns() - g_t0_ns) / 1e9 >= tr_from && t->fn;
    if (trace) { uint64_t a0 = cpu->x[0], a1 = cpu->x[1], a2 = cpu->x[2]; t->fn(cpu);
        LOG("[hle] %s(%#llx,%#llx,%#llx) -> %#llx lr=%#llx", t->name, (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)cpu->x[0], (unsigned long long)cpu->x[30]); }
    else if (t->fn) t->fn(cpu);
    else {
        unsigned s = k - (unsigned)nxrt_hle_count - 1;
        if (s < MAX_STUB && !g_stub_seen[s]) { g_stub_seen[s] = 1;
            LOG("UNIMPL %s (x0=%#llx x1=%#llx x2=%#llx)", t->name, (unsigned long long)cpu->x[0], (unsigned long long)cpu->x[1], (unsigned long long)cpu->x[2]); }
        cpu->x[0] = cpu->x[1] = 0; memset(cpu->q, 0, 4 * 16);
    }
#ifndef NDEBUG
    if (_mm_getcsr() != 0x1F80) { LOG("[RED] MXCSR %#x after host call %s: restored to 0x1F80", _mm_getcsr(), t->name); _mm_setcsr(0x1F80); }
#endif
}

void nxrt_hle_get_stats(nxrt_hle_stats *o) {
    memset(o, 0, sizeof *o);
    for (unsigned i = 0; i < nxrt_hle_count; i++) {
        const nxrt_hle_entry *e = &nxrt_hle_table[i];
        o->forced += e->kind == NXRT_HLE_FORCED; o->swhle += e->kind == NXRT_HLE_SWHLE; o->nvn += e->kind == NXRT_HLE_NVN;
        o->ported += e->ported == 1; o->trivial += e->ported == 2; o->stubbed += e->ported == 0;
    }
    o->named_stubs = (int)g_nstub; o->bound_relocs = g_bound_relocs;
}
// How often each host replacement ran (the log at exit, and tests)
uint32_t nxrt_hle_calls(const char *name) { unsigned k = idx_find(name); return k ? atomic_load(&g_tok[k].calls) : 0; }
// The generated stubs' log: once per name, with the guest call site
void nxrt_stub_log(const char *name, const char *file, uint8_t *seen, const NxwGuestCpu *cpu) {
    if (*seen) return;
    *seen = 1;
    module_t *m = module_of((void *)(uintptr_t)cpu->x[30]);
    LOG("stub %s (%s, not ported yet): neutral result; x0=%#llx x1=%#llx, called from %s+%#llx", name, file, (unsigned long long)cpu->x[0], (unsigned long long)cpu->x[1],
        m ? m->name : "?", (unsigned long long)(m ? cpu->x[30] - (uint64_t)(uintptr_t)m->base : cpu->x[30]));
}
