/* The guest address space (DESIGN D3, research/03 §3.3): identity mapped at the game's own addresses.
   Three placeholder reservations, made as early as possible (a TLS callback of the executable, NXRT_EARLY_RESERVE):
     tokens   NXRT_TOKEN_BASE,  1 MB     guest-visible addresses of the host replacements
     modules  NXRT_MODULE_BASE, 256 MB   main, subsdk0, sdk at nxload's bases
     guest    NXRT_GUEST_AS_BASE, the profile's mem_reserve (11 GB), partitioned as nxnative's hle_svc.c does:
              256 MB unused, then alias (mem_alias), stack (mem_stack), heap (the rest)
   Alias, stack and heap are one pagefile-backed section (SEC_RESERVE: no commit charge until committed); the alias and heap regions are
   views of it, so svcMapMemory can map a heap range a second time into the stack region (a real alias, where nxnative copies bytes).
   Placeholders let those views sit at 4 KB granularity. Everything else in the block stays a placeholder: inaccessible. */
#include "addrspace.h"
#include "dm.h"
#include "nxrt/runtime.h"

#include <psapi.h>

typedef PVOID (WINAPI *va2_t)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
typedef PVOID (WINAPI *mvf3_t)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
typedef BOOL (WINAPI *uvf2_t)(HANDLE, PVOID, ULONG);
static va2_t p_va2;
static mvf3_t p_mvf3;
static uvf2_t p_uvf2;

static nxrt_range g_r[NXRT_AS_COUNT];
static nxrt_layout g_lay;
static int g_done, g_ok;
static HANDLE g_sec;   /* the alias + stack + heap section */
static SRWLOCK g_ph_lock = SRWLOCK_INIT;   /* placeholder splits and coalesces */

static int load_fns(void) {
    if (p_va2) return 1;
    HMODULE k = GetModuleHandleW(L"kernelbase.dll");
    if (!k) return 0;
    p_va2 = (va2_t)(void *)GetProcAddress(k, "VirtualAlloc2");
    p_mvf3 = (mvf3_t)(void *)GetProcAddress(k, "MapViewOfFile3");
    p_uvf2 = (uvf2_t)(void *)GetProcAddress(k, "UnmapViewOfFile2");
    return p_va2 && p_mvf3 && p_uvf2;
}

static void layout_init(void) {
    g_lay.as_base = NXRT_GUEST_AS_BASE; g_lay.as_size = DM_GAME_MEM_RESERVE;
    g_lay.alias = g_lay.as_base + 0x10000000; g_lay.alias_size = DM_GAME_MEM_ALIAS;
    g_lay.stack = g_lay.alias + g_lay.alias_size; g_lay.stack_size = DM_GAME_MEM_STACK;
    g_lay.heap = g_lay.stack + g_lay.stack_size; g_lay.heap_size = g_lay.as_base + g_lay.as_size - g_lay.heap;
    g_lay.app_pool = DM_GAME_APP_POOL;
    g_r[NXRT_AS_TOKENS] = (nxrt_range){.name = "host-call tokens", .base = NXRT_TOKEN_BASE, .size = NXRT_TOKEN_SIZE};
    g_r[NXRT_AS_MODULES] = (nxrt_range){.name = "modules", .base = NXRT_MODULE_BASE, .size = NXRT_MODULE_REGION_SIZE};
    g_r[NXRT_AS_GUEST] = (nxrt_range){.name = "guest block (alias, stack, heap)", .base = g_lay.as_base, .size = g_lay.as_size};
}

/* What holds part of a range we could not reserve: the first allocation inside it */
static void describe_conflict(nxrt_range *r) {
    for (uint64_t a = r->base; a < r->base + r->size;) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery((LPCVOID)(uintptr_t)a, &m, sizeof m)) break;
        uint64_t end = (uint64_t)(uintptr_t)m.BaseAddress + m.RegionSize;
        if (m.State != MEM_FREE) {
            r->conflict_base = (uint64_t)(uintptr_t)m.AllocationBase;
            r->conflict_size = end - r->conflict_base;
            r->conflict_state = m.State; r->conflict_type = m.Type;
            if (!K32GetMappedFileNameA(GetCurrentProcess(), m.BaseAddress, r->conflict_owner, sizeof r->conflict_owner)) {
                HMODULE h = NULL;
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)m.BaseAddress, &h))
                    GetModuleFileNameA(h, r->conflict_owner, sizeof r->conflict_owner);
            }
            return;
        }
        a = end;
    }
}

int nxrt_as_reserve_early(void) {
    if (g_done) return g_ok;
    g_done = 1;
    layout_init();
    if (!load_fns()) return g_ok = 0;   /* VirtualAlloc2 needs Windows 10 1803 or newer */
    g_ok = 1;
    for (int i = 0; i < NXRT_AS_COUNT; i++) {
        nxrt_range *r = &g_r[i];
        void *p = p_va2(GetCurrentProcess(), (PVOID)(uintptr_t)r->base, r->size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
        r->reserved = p == (void *)(uintptr_t)r->base;
        if (!r->reserved) { g_ok = 0; describe_conflict(r); }
    }
    return g_ok;
}
const nxrt_range *nxrt_as_range(int i) { return i >= 0 && i < NXRT_AS_COUNT ? &g_r[i] : NULL; }
int nxrt_as_ok(void) { return g_ok; }
const nxrt_layout *nxrt_as_layout(void) { if (!g_done) nxrt_as_reserve_early(); return &g_lay; }
int as_have_placeholders(void) { return load_fns(); }

/* ---- Placeholder carving ---- */

/* Makes [a, a + n) exactly one placeholder: coalesces the placeholders it spans, then splits it off. Caller holds g_ph_lock.
   VirtualQuery inside a placeholder reports the queried page as the region's base: the placeholder starts at AllocationBase */
static int ph_isolate(uint64_t a, uint64_t n) {
    MEMORY_BASIC_INFORMATION lo, hi;
    if (!VirtualQuery((LPCVOID)(uintptr_t)a, &lo, sizeof lo) || lo.State != MEM_RESERVE) return -1;
    if (!VirtualQuery((LPCVOID)(uintptr_t)(a + n - 1), &hi, sizeof hi) || hi.State != MEM_RESERVE) return -1;
    uint64_t ls = (uint64_t)(uintptr_t)lo.AllocationBase, le = (uint64_t)(uintptr_t)lo.BaseAddress + lo.RegionSize;
    uint64_t hs = (uint64_t)(uintptr_t)hi.AllocationBase, he = (uint64_t)(uintptr_t)hi.BaseAddress + hi.RegionSize;
    if (ls != hs) {   /* several placeholders: coalescing fails if anything mapped lies between them */
        if (!VirtualFree((LPVOID)(uintptr_t)ls, he - ls, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) return -1;
        le = he;
    }
    if (ls == a && le == a + n) return 0;
    return VirtualFree((LPVOID)(uintptr_t)a, n, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) ? 0 : -1;
}

int as_commit_private(uint64_t a, uint64_t n, int prot) {
    if (!load_fns()) return -1;
    AcquireSRWLockExclusive(&g_ph_lock);
    int r = ph_isolate(a, n);
    if (!r && p_va2(GetCurrentProcess(), (PVOID)(uintptr_t)a, n, MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0) != (PVOID)(uintptr_t)a) r = -2;
    ReleaseSRWLockExclusive(&g_ph_lock);
    if (r) return r;
    return VirtualAlloc((LPVOID)(uintptr_t)a, n, MEM_COMMIT, prot) ? 0 : -3;
}
int as_release_private(uint64_t a, uint64_t n) {
    AcquireSRWLockExclusive(&g_ph_lock);
    int r = VirtualFree((LPVOID)(uintptr_t)a, n, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) ? 0 : -1;
    ReleaseSRWLockExclusive(&g_ph_lock);
    return r;
}

/* ---- The guest section ---- */

static int sec_create(void) {
    if (g_sec) return 0;
    uint64_t n = g_lay.alias_size + g_lay.stack_size + g_lay.heap_size;
    g_sec = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | SEC_RESERVE, (DWORD)(n >> 32), (DWORD)n, NULL);
    return g_sec ? 0 : -1;
}
int as_section_offset(uint64_t a, uint64_t n, uint64_t *off) {
    if (a < g_lay.alias || n > g_lay.heap + g_lay.heap_size - a) return -1;
    *off = a - g_lay.alias;
    return 0;
}
int as_map_view(uint64_t a, uint64_t n, uint64_t off) {
    if (!load_fns() || sec_create()) return -1;
    AcquireSRWLockExclusive(&g_ph_lock);
    int r = ph_isolate(a, n);
    if (!r && p_mvf3(g_sec, GetCurrentProcess(), (PVOID)(uintptr_t)a, off, n, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0) != (PVOID)(uintptr_t)a) r = -1;
    ReleaseSRWLockExclusive(&g_ph_lock);
    return r;
}
int as_unmap_view(uint64_t a) {
    AcquireSRWLockExclusive(&g_ph_lock);
    int r = p_uvf2(GetCurrentProcess(), (PVOID)(uintptr_t)a, MEM_PRESERVE_PLACEHOLDER) ? 0 : -1;
    ReleaseSRWLockExclusive(&g_ph_lock);
    return r;
}
/* Merge the placeholders of a free gap again (after unmaps), so later maps split one placeholder. Failure is harmless */
void as_coalesce(uint64_t lo, uint64_t hi) {
    if (hi <= lo) return;
    AcquireSRWLockExclusive(&g_ph_lock);
    VirtualFree((LPVOID)(uintptr_t)lo, hi - lo, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
    ReleaseSRWLockExclusive(&g_ph_lock);
}

/* Map the alias and heap regions as views of the section (once). Pages stay uncommitted until SetHeapSize / MapPhysicalMemory */
int as_guest_views(void) {
    static int done, rc;
    if (done) return rc;
    done = 1;
    if (!g_r[NXRT_AS_GUEST].reserved) return rc = -1;
    if (as_map_view(g_lay.alias, g_lay.alias_size, 0) || as_map_view(g_lay.heap, g_lay.heap_size, g_lay.heap - g_lay.alias)) return rc = -1;
    return rc = 0;
}
/* Commit pages of a view or private range read-write (committing committed pages is harmless; the protection is set again) */
int as_commit(uint64_t a, uint64_t n) {
    if (!n) return 0;
    if (!VirtualAlloc((LPVOID)(uintptr_t)a, n, MEM_COMMIT, PAGE_READWRITE)) return -1;
    DWORD old;
    return VirtualProtect((LPVOID)(uintptr_t)a, n, PAGE_READWRITE, &old) ? 0 : -1;
}
int as_protect(uint64_t a, uint64_t n, int prot) {
    if (!n) return 0;
    DWORD old;
    return VirtualProtect((LPVOID)(uintptr_t)a, n, prot, &old) ? 0 : -1;
}
/* Pages a view can no longer decommit: drop their contents and close them (nxnative: mprotect PROT_NONE + madvise MADV_FREE) */
int as_discard(uint64_t a, uint64_t n) {
    if (!n) return 0;
    DiscardVirtualMemory((PVOID)(uintptr_t)a, n);
    return as_protect(a, n, PAGE_NOACCESS);
}
