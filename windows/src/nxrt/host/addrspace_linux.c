/* The guest address space on Linux (DESIGN D3): identity mapped at the game's own addresses, as host/addrspace.c does on Windows.
     tokens   NXRT_TOKEN_BASE,  1 MB     guest-visible addresses of the host replacements
     modules  NXRT_MODULE_BASE, 256 MB   main, subsdk0, sdk at nxload's bases
     guest    NXRT_GUEST_AS_BASE, the profile's mem_reserve (11 GB), partitioned as nxnative's hle_svc.c does:
              256 MB unused, then alias (mem_alias), stack (mem_stack), heap (the rest)
   Reserved ranges are PROT_NONE anonymous mappings. Alias, stack and heap are one sparse memfd: the alias and heap regions are shared mappings of it, so svcMapMemory
   can map a heap range a second time into the stack region (a real alias). A page costs memory only once it is touched, so "commit" is just opening its protection. */
#define _GNU_SOURCE
#include "addrspace.h"
#include "dm.h"
#include "nxrt/runtime.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>

static nxrt_range g_r[NXRT_AS_COUNT];
static nxrt_layout g_lay;
static int g_done, g_ok, g_sec = -1;
/* The views' lengths, for as_unmap_view, which is given only an address */
#define MAX_VIEW 256
static struct { uint64_t a, n; } g_view[MAX_VIEW];
static pthread_mutex_t g_view_lock = PTHREAD_MUTEX_INITIALIZER;

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
/* What holds part of a range we could not reserve */
static void describe_conflict(nxrt_range *r) {
    FILE *f = fopen("/proc/self/maps", "r"); if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned long long lo, hi; int at = 0;
        if (sscanf(line, "%llx-%llx %*s %*s %*s %*s %n", &lo, &hi, &at) < 2 || hi <= r->base || lo >= r->base + r->size) continue;
        r->conflict_base = lo; r->conflict_size = hi - lo;
        snprintf(r->conflict_owner, sizeof r->conflict_owner, "%s", at ? line + at : "");
        break;
    }
    fclose(f);
}
int nxrt_as_reserve_early(void) {
    if (g_done) return g_ok;
    g_done = 1;
    layout_init();
    g_ok = 1;
    for (int i = 0; i < NXRT_AS_COUNT; i++) {
        nxrt_range *r = &g_r[i];
        void *p = mmap((void *)(uintptr_t)r->base, r->size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
        r->reserved = p == (void *)(uintptr_t)r->base;
        if (!r->reserved) { if (p != MAP_FAILED) munmap(p, r->size); g_ok = 0; describe_conflict(r); }
    }
    return g_ok;
}
const nxrt_range *nxrt_as_range(int i) { return i >= 0 && i < NXRT_AS_COUNT ? &g_r[i] : NULL; }
int nxrt_as_ok(void) { return g_ok; }
const nxrt_layout *nxrt_as_layout(void) { if (!g_done) nxrt_as_reserve_early(); return &g_lay; }
int as_have_placeholders(void) { return 1; }

static int prot_of(int p) { return p == AS_RW ? PROT_READ | PROT_WRITE : p == AS_RO ? PROT_READ : p == AS_RX ? PROT_READ | PROT_EXEC : PROT_NONE; }
static int reserve_again(uint64_t a, uint64_t n) {   /* back to a reservation: the contents go */
    return mmap((void *)(uintptr_t)a, n, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0) == (void *)(uintptr_t)a ? 0 : -1;
}
int as_commit_private(uint64_t a, uint64_t n, int prot) {
    return mmap((void *)(uintptr_t)a, n, prot_of(prot), MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == (void *)(uintptr_t)a ? 0 : -1;
}
int as_release_private(uint64_t a, uint64_t n) { return reserve_again(a, n); }

static int sec_create(void) {
    if (g_sec >= 0) return 0;
    uint64_t n = g_lay.alias_size + g_lay.stack_size + g_lay.heap_size;
    int fd = (int)syscall(SYS_memfd_create, "nxrt-guest", 1u /* MFD_CLOEXEC */);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)n)) { close(fd); return -1; }
    g_sec = fd;
    return 0;
}
int as_section_offset(uint64_t a, uint64_t n, uint64_t *off) {
    if (a < g_lay.alias || n > g_lay.heap + g_lay.heap_size - a) return -1;
    *off = a - g_lay.alias;
    return 0;
}
int as_map_view(uint64_t a, uint64_t n, uint64_t off) {
    if (sec_create()) return -1;
    if (mmap((void *)(uintptr_t)a, n, PROT_NONE, MAP_SHARED | MAP_FIXED, g_sec, (off_t)off) != (void *)(uintptr_t)a) return -1;
    pthread_mutex_lock(&g_view_lock);
    int k = 0; while (k < MAX_VIEW && g_view[k].n) k++;
    if (k < MAX_VIEW) { g_view[k].a = a; g_view[k].n = n; }
    pthread_mutex_unlock(&g_view_lock);
    return k < MAX_VIEW ? 0 : -1;
}
int as_unmap_view(uint64_t a) {
    uint64_t n = 0;
    pthread_mutex_lock(&g_view_lock);
    for (int k = 0; k < MAX_VIEW; k++) if (g_view[k].n && g_view[k].a == a) { n = g_view[k].n; g_view[k].n = 0; break; }
    pthread_mutex_unlock(&g_view_lock);
    return n ? reserve_again(a, n) : -1;
}
void as_coalesce(uint64_t lo, uint64_t hi) { (void)lo; (void)hi; }   /* the kernel merges adjacent mappings by itself */

int as_guest_views(void) {
    static int done, rc;
    if (done) return rc;
    done = 1;
    if (!g_r[NXRT_AS_GUEST].reserved) return rc = -1;
    if (as_map_view(g_lay.alias, g_lay.alias_size, 0) || as_map_view(g_lay.heap, g_lay.heap_size, g_lay.heap - g_lay.alias)) return rc = -1;
    return rc = 0;
}
int as_commit(uint64_t a, uint64_t n) { return !n || !mprotect((void *)(uintptr_t)a, n, PROT_READ | PROT_WRITE) ? 0 : -1; }
int as_protect(uint64_t a, uint64_t n, int prot) { return !n || !mprotect((void *)(uintptr_t)a, n, prot_of(prot)) ? 0 : -1; }
/* Pages a view can no longer use: their contents go back to the system (MADV_REMOVE punches the hole in the memfd, a view of the same pages sees zeros), and they close */
int as_discard(uint64_t a, uint64_t n) {
    if (!n) return 0;
    if (madvise((void *)(uintptr_t)a, n, MADV_REMOVE)) madvise((void *)(uintptr_t)a, n, MADV_DONTNEED);
    return as_protect(a, n, AS_NONE);
}
