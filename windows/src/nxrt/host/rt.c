/* What the game DLL imports (include/nxrt/rt_abi.h): the indirect-branch table, misses, SVCs, system registers, traps, yields; and host-to-guest calls.
   Replaces nxnative's gate machinery (entry.S dm_gate, hle.c gate_dispatch): recompiled code calls rt_svc / rt_mrs directly with its NxwGuestCpu, whose
   first 800 bytes are the gate frame nxnative's handlers already read. */
#include "dm.h"
#include "nxrt/runtime.h"
#include <stdatomic.h>

const uint32_t *rt_code_table;
uint64_t rt_code_base, rt_code_words, rt_host_base;
volatile uint8_t rt_trace_on;
static uint32_t *g_table;
static size_t g_table_bytes;
static uint8_t *g_image;          /* the base the table's RVAs count from: the game DLL (or, in tests, the executable) */
static size_t g_image_size;
static NxwGuestFn (*g_interp)(uint64_t);
/* The game DLL's functions sorted by RVA: host addresses in crash reports name their guest function */
static NxwGameFunction *g_by_rva;
static uint32_t g_nfn;

void nxrt_host_call(NxwGuestCpu *cpu, uint32_t token);   /* hle.c */
void thr_svc_enter(uint64_t *regs, uint64_t pc);          /* hle_thread.c */
void thr_svc_leave(void);
void thr_safepoint(void);
void dm_spin_wait(uint64_t site);
uint64_t svc_dispatch(int num, uint64_t *regs, uint64_t orig_addr);   /* hle_svc.c */
void crash_report(const char *why, const void *ctx);

/* ---- The code table ---- */
/* Pages of the table are committed when first touched: a read of a page nothing was written to sees zeros (a miss) */
static int table_fault(void *addr, int is_write, void *ctx) {
    (void)is_write; (void)ctx;
    uint8_t *a = addr;
    if (!g_table || a < (uint8_t *)g_table || a >= (uint8_t *)g_table + g_table_bytes) return 0;
#ifdef _WIN32
    return VirtualAlloc((void *)((uintptr_t)a & ~(uintptr_t)0xfff), 0x1000, MEM_COMMIT, PAGE_READWRITE) != NULL;
#else
    return 0;   // never faults: the table is ordinary zeroed memory, backed by the kernel where touched
#endif
}
int nxrt_code_table_create(uint64_t base, uint64_t end) {
    if (g_table) return 0;
    rt_code_base = base & ~3ull; rt_code_words = (end - rt_code_base + 3) / 4;
    g_table_bytes = (rt_code_words * 4 + 0xfff) & ~(size_t)0xfff;
#ifdef _WIN32
    g_table = VirtualAlloc(NULL, g_table_bytes, MEM_RESERVE, PAGE_READWRITE);
#else
    g_table = plat_mem_zeroed(g_table_bytes);
#endif
    if (!g_table) return -1;
    dm_fault_hook_register(table_fault);
    rt_code_table = g_table;
    LOG("code table: %#llx-%#llx, %llu slots (%.1f MB reserved, committed on first touch)", (unsigned long long)rt_code_base, (unsigned long long)end,
        (unsigned long long)rt_code_words, g_table_bytes / 1048576.0);
    return 0;
}
static int table_put(uint64_t va, uint32_t rva) {
    uint64_t i = (va - rt_code_base) >> 2;
    if (!g_table || va < rt_code_base || i >= rt_code_words || (va & 3)) return -1;
    uint32_t *slot = &g_table[i];
#ifdef _WIN32
    if (!VirtualAlloc((void *)((uintptr_t)slot & ~(uintptr_t)0xfff), 0x1000, MEM_COMMIT, PAGE_READWRITE)) return -1;
#endif
    *slot = rva;
    return 0;
}

/* The token thunks: "mov <second argument>, k; jmp common", common = "jmp [rip]; .quad nxrt_host_call". A table entry for token k is its thunk's RVA, so an indirect
   call through a token reaches nxrt_host_call(cpu, k) with the first argument (cpu) intact: rcx and edx in the Windows convention, rdi and esi in the System V one. The block must lie within 4 GB above the image */
static uint8_t *thunks_place(size_t n) {
    uint64_t lo = (uint64_t)(uintptr_t)g_image + g_image_size, hi = (uint64_t)(uintptr_t)g_image + 0xFFFFFFFFull;
    return plat_mem_near(lo, hi, n);
}
int nxrt_code_attach(void *image, size_t image_size) {
    if (!g_table) return -1;
    g_image = image; g_image_size = image_size; rt_host_base = (uint64_t)(uintptr_t)image;
    size_t ntok = NXRT_TOKEN_SIZE / NXRT_TOKEN_STRIDE, n = 16 + ntok * 16;
    uint8_t *b = thunks_place(n);
    if (!b) { LOG("[RED] no room for the host-call thunks within 4 GB of %p", image); return -1; }
    b[0] = 0xFF; b[1] = 0x25; memset(b + 2, 0, 4);
    void (*target)(NxwGuestCpu *, uint32_t) = nxrt_host_call; memcpy(b + 6, &target, 8);
    for (size_t k = 0; k < ntok; k++) {
        uint8_t *t = b + 16 + 16 * k;
        int32_t rel = (int32_t)(b - (t + 10));
#ifdef _WIN32
        t[0] = 0xBA;   // mov edx, imm32
#else
        t[0] = 0xBE;   // mov esi, imm32
#endif
        uint32_t kk = (uint32_t)k; memcpy(t + 1, &kk, 4);
        t[5] = 0xE9; memcpy(t + 6, &rel, 4);
        memset(t + 10, 0xCC, 6);
    }
    if (plat_mem_protect(b, n, PLAT_MEM_RX)) return -1;
    for (size_t k = 0; k < ntok; k++)
        if (table_put(NXRT_TOKEN_BASE + k * NXRT_TOKEN_STRIDE, (uint32_t)(b + 16 + 16 * k - (uint8_t *)image))) return -1;
    LOG("code image %p (+%#zx), host-call thunks at %p", image, image_size, (void *)b);
    return 0;
}
int nxrt_code_set(uint64_t va, NxwGuestFn fn) {
    uint64_t d = (uint64_t)(uintptr_t)fn - (uint64_t)(uintptr_t)g_image;
    if (!g_image || (uintptr_t)fn <= (uintptr_t)g_image || d > 0xFFFFFFFFull) return -1;
    return table_put(va, (uint32_t)d);
}
NxwGuestFn nxrt_code_lookup(uint64_t va) {
    uint64_t i = (va - rt_code_base) >> 2;
    uint32_t f = g_table && va >= rt_code_base && i < rt_code_words ? g_table[i] : 0;
    return f ? (NxwGuestFn)(void *)(g_image + f) : rt_miss(nxrt_cur_cpu(), va);
}
void nxrt_set_interpreter(NxwGuestFn (*entry_for)(uint64_t)) { g_interp = entry_for; }

static int by_rva(const void *a, const void *b) {
    uint32_t x = ((const NxwGameFunction *)a)->rva, y = ((const NxwGameFunction *)b)->rva;
    return x < y ? -1 : x > y;
}
/* The guest function whose translated code holds a host address (0 if none): crash reports and backtraces */
uint64_t nxrt_host_to_guest(uint64_t host, uint64_t *off) {
    if (!g_nfn || host < (uint64_t)(uintptr_t)g_image || host >= (uint64_t)(uintptr_t)g_image + g_image_size) return 0;
    uint32_t rva = (uint32_t)(host - (uint64_t)(uintptr_t)g_image), lo = 0, hi = g_nfn;
    while (hi - lo > 1) { uint32_t mid = (lo + hi) / 2; if (g_by_rva[mid].rva <= rva) lo = mid; else hi = mid; }
    if (g_by_rva[lo].rva > rva) return 0;
    if (off) *off = rva - g_by_rva[lo].rva;
    return g_by_rva[lo].guest_va;
}

#ifndef _WIN32
#include <dlfcn.h>
#include <link.h>
struct phdr_look { uint64_t base, size; };
static int phdr_size(struct dl_phdr_info *i, size_t sz, void *p) {
    (void)sz; struct phdr_look *l = p;
    if ((uint64_t)i->dlpi_addr != l->base) return 0;
    for (int k = 0; k < i->dlpi_phnum; k++) if (i->dlpi_phdr[k].p_type == PT_LOAD) {
        uint64_t e = i->dlpi_phdr[k].p_vaddr + i->dlpi_phdr[k].p_memsz; if (e > l->size) l->size = e; }
    return 1;
}
#endif
#ifndef _WIN32
static void guest_jmp_install(void);
#endif
/* The game DLL (the contract in rt_abi.h): its function list fills the table */
int nxrt_attach_game_dll(const char *path) {
    uint64_t t0 = now_ns();
#ifdef _WIN32
    HMODULE h = LoadLibraryA(path);
    if (!h) { LOG("[RED] cannot load the game DLL %s (error %lu)", path, GetLastError()); return -1; }
#else
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { LOG("[RED] cannot load the game's shared object %s: %s", path, dlerror()); return -1; }
#endif
    const uint32_t *abi = (const uint32_t *)plat_lib_sym(h, "nxw_game_abi");
    const NxwGameFunction *fns = (const NxwGameFunction *)plat_lib_sym(h, "nxw_game_functions");
    const uint32_t *nfn = (const uint32_t *)plat_lib_sym(h, "nxw_game_function_count");
    const char *bid = (const char *)plat_lib_sym(h, "nxw_game_build_id");
    if (!abi || !fns || !nfn) { LOG("[RED] %s does not export the nxw_game_* contract", path); return -1; }
    if (*abi != NXW_ABI_VERSION) { LOG("[RED] %s was built for runtime ABI %u, this runtime is %d", path, *abi, NXW_ABI_VERSION); return -1; }
    if (bid && strncmp(bid, dm_main_build(), 32)) { LOG("[RED] %s is for main %s, the loaded main is %s", path, bid, dm_main_build()); return -1; }
#ifdef _WIN32
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)((uint8_t *)h + ((IMAGE_DOS_HEADER *)h)->e_lfanew);
    void *image = h; size_t image_size = nt->OptionalHeader.SizeOfImage;
#else
    // Each entry's rva field holds the function minus the field's own address: from here on, as everywhere, an offset from the load base
    Dl_info di; struct phdr_look look = {0};
    if (!dladdr(fns, &di) || !di.dli_fbase) { LOG("[RED] %s: no load base", path); return -1; }
    look.base = (uint64_t)(uintptr_t)di.dli_fbase;
    dl_iterate_phdr(phdr_size, &look);
    void *image = di.dli_fbase; size_t image_size = look.size;
#endif
    if (nxrt_code_attach(image, image_size)) return -1;
    uint32_t bad = 0;
    g_by_rva = malloc(sizeof *g_by_rva * (*nfn ? *nfn : 1));
    for (uint32_t i = 0; i < *nfn; i++) {
#ifdef _WIN32
        uint32_t rva = fns[i].rva;
#else
        uint32_t rva = (uint32_t)((uint64_t)(uintptr_t)&fns[i].rva + (int64_t)(int32_t)fns[i].rva - (uint64_t)(uintptr_t)image);
#endif
        bad += table_put(fns[i].guest_va, rva) != 0;
        if (g_by_rva) { g_by_rva[i] = fns[i]; g_by_rva[i].rva = rva; }
    }
    if (g_by_rva) { qsort(g_by_rva, *nfn, sizeof *g_by_rva, by_rva); g_nfn = *nfn; }
    LOG("game image %s: base %p, %.1f MB, %u functions (main %.16s), table filled in %.1f ms%s", path, image, image_size / 1048576.0,
        *nfn, bid ? bid : "?", (now_ns() - t0) / 1e6, bad ? " [RED] (some outside the table)" : "");
#ifndef _WIN32
    guest_jmp_install();
#endif
    return bad ? -1 : 0;
}

/* ---- Host-to-guest calls ---- */
uint64_t nxrt_guest_call(uint64_t va, int n, const uint64_t *args) {
    NxwGuestCpu *c = nxrt_cur_cpu();
    if (!c) die("guest call to %#llx from a host thread", (unsigned long long)va);
    if (n > 8) die("guest call to %#llx with %d arguments", (unsigned long long)va, n);
    uint64_t lr = c->x[30], pc = c->pc;
    for (int i = 0; i < n; i++) c->x[i] = args[i];
    c->x[30] = NXRT_RETURN_TO_HOST; c->pc = va;
    nxrt_code_lookup(va)(c);
    c->x[30] = lr; c->pc = pc;
    return c->x[0];
}

/* ---- Exports ---- */
static _Thread_local uint32_t t_token;
static void token_tramp(NxwGuestCpu *cpu) { nxrt_host_call(cpu, t_token); }
void rt_return_to_host(NxwGuestCpu *cpu) { (void)cpu; }

/* Misses inside a module go to the profile list the next build compiles (research/03 §5.4, kirikae's loop) */
static void miss_log(uint64_t va) {
    static plat_rwlock_t lk = PLAT_RWLOCK_INIT; static uint64_t seen[1024]; static int n;
    plat_rw_wrlock(&lk);
    int dup = 0; for (int i = 0; i < n; i++) dup |= seen[i] == va;
    if (!dup && n < 1024) {
        seen[n++] = va;
        char p[MAX_PATH + 16]; snprintf(p, sizeof p, "%s.misses.txt", nxrt_log_path());
        FILE *f = fopen(p, "a");
        module_t *m = module_of((void *)(uintptr_t)va);
        if (f) { fprintf(f, "%s+%#llx\n", m ? m->name : "?", (unsigned long long)(m ? va - (uint64_t)(uintptr_t)m->base : va)); fclose(f); }
    }
    plat_rw_wrunlock(&lk);
}
NxwGuestFn rt_miss(NxwGuestCpu *cpu, uint64_t target) {
    if (target >= NXRT_TOKEN_BASE && target < NXRT_TOKEN_BASE + NXRT_TOKEN_SIZE && !(target & (NXRT_TOKEN_STRIDE - 1))) {
        uint32_t k = (uint32_t)((target - NXRT_TOKEN_BASE) / NXRT_TOKEN_STRIDE);
        if (!k) return rt_return_to_host;
        t_token = k; return token_tramp;
    }
    module_t *m = module_of((void *)(uintptr_t)target);
    if (m && target - (uint64_t)(uintptr_t)m->base < m->text_end && !(target & 3)) {
        miss_log(target);
        if (cpu) cpu->pc = target;   /* the interpreter's trampoline starts at cpu->pc */
        NxwGuestFn f = g_interp ? g_interp(target) : NULL;
        if (f) return f;
    }
    if (cpu) cpu->pc = target;
    char why[160]; snprintf(why, sizeof why, "indirect branch to %s%s%#llx with no translated function%s", m ? m->name : "", m ? "+" : "",
                            (unsigned long long)(m ? target - (uint64_t)(uintptr_t)m->base : target), g_interp ? "" : " (no interpreter attached)");
    LOG("[RED] %s", why);
    crash_report(why, NULL);
    plat_exit(66);
}

void rt_svc(NxwGuestCpu *cpu, uint32_t imm) {
    thr_svc_enter((uint64_t *)cpu, cpu->pc);
    svc_dispatch((int)imm, (uint64_t *)cpu, cpu->pc);
    thr_svc_leave();
}

uint64_t rt_mrs(NxwGuestCpu *cpu, uint32_t sr) {
    switch (sr) {
    case NXRT_SR_CNTPCT_EL0: case NXRT_SR_CNTVCT_EL0: guest_clock_read_wait(); return guest_ticks();
    case NXRT_SR_CNTFRQ_EL0: return 31250000;   /* nothing reads it (research/05); answer the SDK's own constant */
    case NXRT_SR_TPIDR_EL0: return cpu->tpidr_el0;
    case NXRT_SR_TPIDRRO_EL0: return cpu->tpidrro_el0;
    case NXRT_SR_NZCV: return cpu->nzcv;
    case NXRT_SR_FPCR: return cpu->fpcr;
    case NXRT_SR_FPSR: return cpu->fpsr;
    case NXRT_SR_CTR_EL0: return 0x8444c004;    /* DminLine 4 (64-byte lines, nxnative's constant); the other fields as Cortex-A57's */
    case NXRT_SR_DCZID_EL0: return 4;           /* DC ZVA allowed, 64-byte blocks */
    case NXRT_SR_MIDR_EL1: return 0x411fd071;   /* Cortex-A57 r1p1 */
    case NXRT_SR_MPIDR_EL1: return 0x80000000;
    default: {
        static uint8_t seen[64]; uint32_t h = (sr * 2654435761u) >> 26;
        if (!seen[h]) { seen[h] = 1; LOG("[RED] mrs of system register %#x at %#llx: 0", sr, (unsigned long long)cpu->pc); }
        return 0;
    }
    }
}
void rt_msr(NxwGuestCpu *cpu, uint32_t sr, uint64_t v) {
    switch (sr) {
    case NXRT_SR_TPIDR_EL0: cpu->tpidr_el0 = v; return;
    case NXRT_SR_NZCV: cpu->nzcv = v & 0xf0000000u; return;
    case NXRT_SR_FPSR: cpu->fpsr = v; return;
    case NXRT_SR_FPCR:   /* research/05: never written in play. The lifter's exact FP helpers read cpu->fpcr; MXCSR is left at Windows' default (0x1F80) */
        if (v != cpu->fpcr) { static _Atomic int once; if (!atomic_exchange(&once, 1)) LOG("[RED] msr fpcr %#llx at %#llx (only 0 is modelled so far)", (unsigned long long)v, (unsigned long long)cpu->pc); }
        cpu->fpcr = v; return;
    default: {
        static uint8_t seen[64]; uint32_t h = (sr * 2654435761u) >> 26;
        if (!seen[h]) { seen[h] = 1; LOG("[RED] msr of system register %#x = %#llx at %#llx: ignored", sr, (unsigned long long)v, (unsigned long long)cpu->pc); }
    }
    }
}

void rt_yield(NxwGuestCpu *cpu, uint32_t kind, uint64_t site) {
    (void)kind;
    thr_safepoint();
    dm_spin_wait(site ? site : cpu->pc);
}

static const char *trap_name(uint32_t why) {
    switch (why) {
    case NXRT_TRAP_UDF: return "udf"; case NXRT_TRAP_BRK: return "brk"; case NXRT_TRAP_HLT: return "hlt";
    case NXRT_TRAP_UNREACHABLE: return "unreachable code"; case NXRT_TRAP_UNDECODABLE: return "undecodable instruction";
    case NXRT_TRAP_JUMP_TABLE: return "jump-table index outside the decoded table";
    default: return "trap";
    }
}
void rt_trap(NxwGuestCpu *cpu, uint64_t pc, uint32_t why) {
    if (cpu) cpu->pc = pc;
    module_t *m = module_of((void *)(uintptr_t)pc);
    char msg[160]; snprintf(msg, sizeof msg, "%s at %s+%#llx", trap_name(why), m ? m->name : "?", (unsigned long long)(m ? pc - (uint64_t)(uintptr_t)m->base : pc));
    LOG("[RED] %s", msg);
    crash_report(msg, NULL);
    plat_exit(67);
}
/* host/fiber.c is in the static library: nothing else names rt_fiber_switch, which the game's shared object binds to at load */
static void *const k_keep_fiber __attribute__((used)) = (void *)rt_fiber_switch;
/* ---- Guest setjmp / longjmp (host/guest_jmp.c, docs/FASE4-FIBERS.md, section "setjmp / longjmp") ---- */
#ifndef _WIN32
extern void nxrt_guest_setjmp(NxwGuestCpu *cpu);
extern const char *nxrt_guest_longjmp(NxwGuestCpu *cpu);
extern int nxrt_sj_patch_entry(void *code, void *target);
static uint64_t g_longjmp_va[2];
/* The host code of the lifted function at a guest address, and its size up to the next function (0 if unknown) */
static void *host_fn_of(uint64_t va, size_t *size) {
    uint64_t i = (va - rt_code_base) >> 2;
    uint32_t f = g_table && va >= rt_code_base && i < rt_code_words ? g_table[i] : 0;
    if (!f) return NULL;
    *size = 0;
    uint32_t lo = 0, hi = g_nfn;
    while (hi - lo > 1) { uint32_t mid = (lo + hi) / 2; if (g_by_rva[mid].rva <= f) lo = mid; else hi = mid; }
    if (g_nfn && g_by_rva[lo].rva == f) {
        uint32_t nx = lo + 1;
        while (nx < g_nfn && g_by_rva[nx].rva == f) nx++;
        if (nx < g_nfn) *size = g_by_rva[nx].rva - f;
    }
    return g_image + f;
}
/* Called once the game object is attached: sdk's setjmp (the lifted body only has its first return) is replaced by the host-checkpoint one, and longjmp is
   told apart in rt_nonlocal. NXRT_NO_GUEST_LONGJMP=1 keeps the old report-and-stop behaviour */
static void guest_jmp_install(void) {
    const char *e = getenv("NXRT_NO_GUEST_LONGJMP");
    if (e && *e && *e != '0') { LOG("guest setjmp/longjmp: disabled (NXRT_NO_GUEST_LONGJMP)"); return; }
    static const char *const set_names[] = {"__setjmp", "_setjmp", "setjmp"};
    uint64_t done[3] = {0};
    int patched = 0;
    for (int k = 0; k < 3; k++) {
        uint64_t va = nxrt_guest_sym(set_names[k]);
        int dup = 0;
        for (int j = 0; j < k; j++) dup |= done[j] == va;
        if (!va || dup) continue;
        done[k] = va;
        size_t size = 0; void *fn = host_fn_of(va, &size);
        if (!fn || (size && size < 14)) { LOG("[RED] guest setjmp: %s at %#llx has no host code to replace (size %zu)", set_names[k], (unsigned long long)va, size); continue; }
        if (nxrt_sj_patch_entry(fn, (void *)nxrt_guest_setjmp)) { LOG("[RED] guest setjmp: cannot patch the host code of %s at %p", set_names[k], fn); continue; }
        patched++;
    }
    g_longjmp_va[0] = nxrt_guest_sym("longjmp"); g_longjmp_va[1] = nxrt_guest_sym("_longjmp");
    LOG("guest setjmp/longjmp: %d setjmp entr%s replaced by the host-checkpoint stub, longjmp at %#llx", patched, patched == 1 ? "y" : "ies", (unsigned long long)g_longjmp_va[0]);
}
#endif

void rt_nonlocal(NxwGuestCpu *cpu) {
    /* the lifter's context functions arrive with cpu->pc = the function itself (sdk's DispatchFiberContext, UserExceptionHandler, longjmp, jumpto) */
#ifndef _WIN32
    if (cpu->pc && (cpu->pc == g_longjmp_va[0] || cpu->pc == g_longjmp_va[1])) {
        const char *why = nxrt_guest_longjmp(cpu);   /* does not come back unless it cannot jump */
        LOG("[RED] guest longjmp(%#llx, %#llx) from %#llx: %s", (unsigned long long)cpu->x[0], (unsigned long long)cpu->x[1], (unsigned long long)cpu->x[30], why);
    }
#endif
    char fn[160]; nxrt_symbolize(cpu->pc, fn, sizeof fn);
    char msg[320]; snprintf(msg, sizeof msg, "non-local transfer: %s called from %#llx (unwind pc %#llx sp %#llx; fiber switches, C++ exception resume and "
                            "longjmp are not supported yet, DESIGN D5)", fn, (unsigned long long)cpu->x[30],
                            (unsigned long long)cpu->unwind_pc, (unsigned long long)cpu->unwind_sp);
    LOG("[RED] %s", msg);
    crash_report(msg, NULL);
    plat_exit(68);
}

/* ---- The interpreter (src/interp): misses inside a module's text run there until the next build compiles them ---- */
void nxw_interp_trampoline(NxwGuestCpu *cpu);   /* interprets from cpu->pc, which rt_miss sets */
static NxwGuestFn interp_entry(uint64_t va) { (void)va; return nxw_interp_trampoline; }
void nxrt_interp_attach(void) { if (!g_interp && !getenv("NXRT_NO_INTERP")) g_interp = interp_entry; }

/* ---- Function-entry trace (NXRT_TRACE; the hook needs objects lifted with entry tracing, nxlift's default) ----
   NXRT_TRACE=calls:N   the first N function entries of any thread, with x0-x2 and the caller
   NXRT_TRACE=first:N   the first execution of each function, the first N of them, numbered as research/05's executed lists are
   Both: NXRT_TRACE=calls:N,first:M. Written to <log>.trace.txt, a line per entry; the hook turns itself off when both counts run out. */
static FILE *g_trace;
static _Atomic int64_t g_trace_calls, g_trace_first;
static _Atomic uint64_t g_trace_seq;
static _Atomic uint8_t *g_seen;   /* one bit per code-table slot: functions entered so far */
static plat_rwlock_t g_trace_lk = PLAT_RWLOCK_INIT;
static uint64_t g_trace_fns[32];
static int g_trace_nfn;
static int g_trace_dump;

void nxrt_trace_init(void) {
    const char *e = getenv("NXRT_TRACE");
    if (!e || !*e || !g_table) return;
    /* Resolve once before guest threads run. Numeric VAs also select unnamed
       functions, which are common in stripped game modules. */
    const char *m = getenv("NXRT_TRACE_FN");
    while (m && *m && g_trace_nfn < 32) {
        const char *end = strchr(m, ',');
        size_t len = end ? (size_t)(end - m) : strlen(m);
        char name[256]; snprintf(name, sizeof name, "%.*s", (int)(len < 255 ? len : 255), m);
        char *tail; uint64_t va = strtoull(name, &tail, 0);
        if (!len || tail == name || *tail) va = (uint64_t)(uintptr_t)guest_sym(name);
        if (va) g_trace_fns[g_trace_nfn++] = va;
        else if (len) LOG("[RED] NXRT_TRACE_FN: no symbol %s", name);
        m += len + (end != NULL);
    }
    if (getenv("NXRT_TRACE_FN") && !g_trace_nfn) g_trace_nfn = -1;
    g_trace_dump = getenv("NXRT_TRACE_DUMP") != NULL;
    const char *c = strstr(e, "calls:"), *f = strstr(e, "first:");
    if (c) g_trace_calls = strtoll(c + 6, NULL, 0);
    if (f) g_trace_first = strtoll(f + 6, NULL, 0);
    if (!c && !f) g_trace_calls = strtoll(e, NULL, 0);
    if (g_trace_first > 0) g_seen = plat_mem_zeroed(rt_code_words / 8 + 1);
    char p[MAX_PATH + 16]; snprintf(p, sizeof p, "%s.trace.txt", nxrt_log_path());
    g_trace = fopen(p, "w");
    if (!g_trace) { LOG("[RED] NXRT_TRACE: cannot write %s", p); return; }
    LOG("NXRT_TRACE: %lld calls, %lld first entries into %s", (long long)g_trace_calls, (long long)g_trace_first, p);
    rt_trace_on = 1;
}
void rt_trace_entry(NxwGuestCpu *cpu, uint64_t va) {
    int first = 0;
    if (g_trace_nfn) { int hit = 0; for (int i = 0; i < g_trace_nfn; i++) hit |= g_trace_fns[i] == va; if (!hit) return; }
    int64_t c = atomic_load(&g_trace_calls) > 0 ? atomic_fetch_sub(&g_trace_calls, 1) : 0;
    int64_t f = first && atomic_load(&g_trace_first) > 0 ? atomic_fetch_sub(&g_trace_first, 1) : 0;
    if (c <= 0 && f <= 0) {
        if (atomic_load(&g_trace_calls) <= 0 && atomic_load(&g_trace_first) <= 0) rt_trace_on = 0;
        return;
    }
    char nm[160], lr[160];
    nxrt_symbolize(va, nm, sizeof nm); nxrt_symbolize(cpu->x[30], lr, sizeof lr);
    plat_rw_wrlock(&g_trace_lk);
    fprintf(g_trace, "%c%llu t%u %s x0=%llx x1=%llx x2=%llx sp=%llx lr=%s ms=%llu\n", f > 0 ? 'F' : 'C', (unsigned long long)g_trace_seq++,
            plat_thread_id(), nm, (unsigned long long)cpu->x[0], (unsigned long long)cpu->x[1], (unsigned long long)cpu->x[2],
            (unsigned long long)cpu->sp, lr, (unsigned long long)((now_ns() - g_t0_ns) / 1000000));
    if (g_trace_dump) {
        uint64_t words[32];
        if (!plat_read(cpu->x[0], words, sizeof words)) {
            fprintf(g_trace, "  *x0:");
            for (unsigned i = 0; i < 32; i++) fprintf(g_trace, " %llx", (unsigned long long)words[i]);
            fputc('\n', g_trace);
        }
    }
    fflush(g_trace);
    plat_rw_wrunlock(&g_trace_lk);
}
