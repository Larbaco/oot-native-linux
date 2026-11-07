// from nxnative dm/host/hle_svc.c (MIT, © seedds)
// Horizon kernel system calls: memory layout + heap + query + debug output + timing; thread and synchronisation SVCs go to hle_thread.c.
// Register convention: inputs w1.. / outputs w0=Result, x1=value (GetInfo / SetHeapSize), consistent with the public Horizon ABI. regs is the thread's NxwGuestCpu.
// Windows port (DESIGN D3): the block is nxnative's, at a fixed address (host/addrspace.c); 4 KB pages, so no host-page rounding. The heap and alias regions
// are views of one pagefile-backed section: svcMapMemory maps a second view of the source into the stack region (a real alias; nxnative copied bytes) and
// copies only when the source is not section-backed. The heap is committed lazily, 1 MB at a time, on first touch (a fault hook), so an 8 GB heap costs
// commit charge only for what the game uses. Dropped: the address-region observation gates (iPhone address-space workarounds: research/02 §2.5).
#include "dm.h"
#include "addrspace.h"
#include "nxrt/runtime.h"
#include <stdatomic.h>
#ifndef _WIN32
#include <sys/mman.h>
#endif

static uint8_t *g_as;              // ASLR region base = the whole block
static size_t g_reserve;           // its size
static uint8_t *g_heap; static _Atomic size_t g_heap_cur; static size_t g_heap_cap;
// The address range lies entirely within the currently opened (readable and writable) part of the guest heap
int dm_guest_heap_readable(uint64_t a, size_t n) { uint64_t b = (uint64_t)(uintptr_t)g_heap; size_t cur = g_heap_cur; return g_heap && a >= b && n <= cur && a - b <= cur - n; }
void dm_guest_heap_range(uint64_t *base, size_t *open_len) {
    if (base) *base = (uint64_t)(uintptr_t)g_heap;
    if (open_len) *open_len = g_heap ? atomic_load(&g_heap_cur) : 0;
}
static uint8_t *g_alias; static size_t g_alias_cap;
static _Atomic uint64_t *g_alias_bits;   // alias region page bitmap (alias_pages)
static uint64_t *g_alias_exposed;   // one bit per page ever opened; protected by g_map_lock
static uint8_t *g_stack; static size_t g_stack_cap;
int dm_guest_as_contains(uintptr_t a) { uintptr_t b = (uintptr_t)g_as; return g_as && a >= b && a - b < g_reserve; }

// ---- Lazy heap commit: one bit per 1 MB chunk committed (and open as far as the heap is open) ----
#define HEAP_CHUNK 0x100000ull
static _Atomic uint64_t *g_heap_chunks;
// Only a chunk committed now is opened here: once committed, its protection belongs to heap_resize and to the graphics layer's write protection
// (gfx/nvn_dirty.c), which re-opening the whole chunk would undo without stamping the pages
static void heap_chunk_open(uint64_t chunk, uint64_t open) {   // commit the chunk, open the part below the heap's end
    uint64_t lo = chunk * HEAP_CHUNK, hi = lo + HEAP_CHUNK;
    if (hi > g_heap_cap) hi = g_heap_cap;
    if (atomic_load(&g_heap_chunks[chunk >> 6]) >> (chunk & 63) & 1) return;
#ifdef _WIN32
    if (!VirtualAlloc(g_heap + lo, hi - lo, MEM_COMMIT, PAGE_NOACCESS)) return;
#endif
    atomic_fetch_or(&g_heap_chunks[chunk >> 6], 1ull << (chunk & 63));
    if (open > lo) as_protect((uint64_t)(uintptr_t)g_heap + lo, (open < hi ? open : hi) - lo, AS_RW);
}
static int heap_fault(void *addr, int is_write, void *ctx) {   // a fault hook: no locks
    (void)ctx;
    uint64_t a = (uint64_t)(uintptr_t)addr, b = (uint64_t)(uintptr_t)g_heap, cur = atomic_load(&g_heap_cur);
    if (!g_heap || a < b || a - b >= cur) return 0;
    uint64_t chunk = (a - b) / HEAP_CHUNK;
    if (atomic_load(&g_heap_chunks[chunk >> 6]) >> (chunk & 63) & 1) {   // committed already
        if (is_write && dm_fault_is_wp(addr)) return 0;   // the dirty tracker's write protection, without a VirtualQuery: its hook comes next
#ifdef _WIN32
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(addr, &mi, sizeof mi)) return 0;
        if (mi.State == MEM_COMMIT && (mi.Protect & 0xff) == PAGE_READONLY) return 0;   // a write to a write-protected page: the dirty tracker's hook
        if (mi.State != MEM_COMMIT || (mi.Protect & 0xff) == PAGE_NOACCESS)            // still being opened by another thread, or closed: this page only
            VirtualAlloc((void *)(uintptr_t)(a & ~0xfffull), 0x1000, MEM_COMMIT, PAGE_READWRITE);
#else
        // Not write-protected by the tracker: a page still being opened by another thread, or closed. Open this page. (Asking the kernel, as Windows does, means reading
        // /proc/self/maps, far too slow for a fault path, and the answer is not needed: opening an open page changes nothing.)
        mprotect((void *)(uintptr_t)(a & ~0xfffull), 0x1000, PROT_READ | PROT_WRITE);
#endif
        return 1;   // accessible now: run the access again
    }
    heap_chunk_open(chunk, cur);
    return 1;
}
// Host writes into guest memory (a file read straight into a guest buffer) do not fault: commit their chunks first
static void heap_write_prep(void *addr, size_t len) {
    uint64_t a = (uint64_t)(uintptr_t)addr, b = (uint64_t)(uintptr_t)g_heap, cur = atomic_load(&g_heap_cur);
    if (!g_heap || !len || a + len <= b || a >= b + cur) return;
    uint64_t lo = a < b ? 0 : a - b, hi = a + len - b < cur ? a + len - b : cur;
    for (uint64_t c = lo / HEAP_CHUNK; c * HEAP_CHUNK < hi; c++) heap_chunk_open(c, cur);
}
// The heap's open end moved: committed chunks change protection at once; the rest commit when touched
static void heap_resize(size_t old, size_t now) {
    uint64_t lo = old < now ? old : now, hi = old < now ? now : old;
    for (uint64_t c = lo / HEAP_CHUNK; c * HEAP_CHUNK < hi; c++) {
        if (!(atomic_load(&g_heap_chunks[c >> 6]) >> (c & 63) & 1)) continue;
        uint64_t cl = c * HEAP_CHUNK < lo ? lo : c * HEAP_CHUNK, ch = (c + 1) * HEAP_CHUNK > hi ? hi : (c + 1) * HEAP_CHUNK;
        as_protect((uint64_t)(uintptr_t)g_heap + cl, ch - cl, now > old ? AS_RW : AS_NONE);
    }
}

static void mem_init_once(void) {
    const nxrt_layout *l = nxrt_as_layout();
    if (!nxrt_as_range(NXRT_AS_GUEST)->reserved) die("the guest block %#llx+%#llx is not reserved (see the start of the log)", (unsigned long long)l->as_base, (unsigned long long)l->as_size);
    if (as_guest_views()) die("cannot map the guest section's views (error %lu)", (unsigned long)errno);
    g_as = (uint8_t *)(uintptr_t)l->as_base; g_reserve = l->as_size;
    g_alias = (uint8_t *)(uintptr_t)l->alias; g_alias_cap = l->alias_size;
    g_stack = (uint8_t *)(uintptr_t)l->stack; g_stack_cap = l->stack_size;
    g_heap = (uint8_t *)(uintptr_t)l->heap; g_heap_cap = l->heap_size;
    if (!(g_alias_bits = calloc((g_alias_cap >> 18) + 1, sizeof *g_alias_bits))) die("alias region page bitmap calloc");
    if (!(g_alias_exposed = calloc(g_alias_cap / 4096 / 64 + 1, sizeof *g_alias_exposed))) die("alias exposure bitmap calloc");
    if (!(g_heap_chunks = calloc(g_heap_cap / HEAP_CHUNK / 64 + 1, sizeof *g_heap_chunks))) die("heap chunk bitmap calloc");
    dm_fault_hook_register(heap_fault);
    dm_guest_write_prep_register(heap_write_prep);
    LOG("guest AS %p +%#llx  heap %p alias %p stack %p", (void *)g_as, (unsigned long long)g_reserve, (void *)g_heap, (void *)g_alias, (void *)g_stack);
}
static void mem_init(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, mem_init_once);
}
void nxrt_mem_init(void) { mem_init(); }

// Application memory pool: at startup the game sets the whole total reported by QueryMemoryInfo less what it keeps as the heap, retrying with 2 MB less on failure:
// heap size = pool − image footprint, it has only as much as the host reports.
#define APP_POOL DM_GAME_APP_POOL   // game profile app_pool
static size_t image_bytes(void) { size_t n = 0; for (int i = 0; i < g_nmod; i++) if (!g_mod[i].nro) n += g_mod[i].size;
    return (n + 0x1fffff) & ~(size_t)0x1fffff; }

void svc_guest_range(uint8_t **base, size_t *size) { *base = g_as; *size = g_reserve; }

enum { I_CoreMask=0, I_PriorityMask=1, I_AliasAddr=2, I_AliasSize=3, I_HeapAddr=4, I_HeapSize=5,
       I_TotalMem=6, I_UsedMem=7, I_DebuggerAttached=8, I_ResourceLimit=9, I_IdleTick=10, I_RandomEntropy=11,
       I_AslrAddr=12, I_AslrSize=13, I_StackAddr=14, I_StackSize=15, I_SysResTotal=16, I_SysResUsed=17,
       I_ProgramId=18, I_InitProcIdRange=19, I_UserExcAddr=20, I_TotalNonSys=21, I_UsedNonSys=22, I_IsApplication=23, I_FreeThreadCount=24 };

static uint8_t g_seen[256];

// Stack destinations and their sources. One lock covers memory validation, protections, copying, budgets and publication; QueryMemory and
// used-memory queries take the same snapshot. g_map_lock is held over a whole lookup + check + change (map_find's index is only valid under it)
#define MAX_MAP 4096
enum { MAP_VIEW = 1, MAP_COPY = 2, MAP_OWN = 3 };   // a view of the source's section pages / a private copy (nxnative's way) / the runtime's own (the main thread's stack)
static struct { uint64_t base, size, src; uint32_t state, kind; } g_map[MAX_MAP];
static int g_nmap;
static pthread_mutex_t g_map_lock = PTHREAD_MUTEX_INITIALIZER;
int dm_guest_heap_write(uint64_t addr, const void *src, size_t len) {
    pthread_mutex_lock(&g_map_lock);
    int out = !dm_guest_heap_readable(addr, len);
    if (!out) { dm_guest_write_prepare((void *)(uintptr_t)addr, len); memcpy((void *)(uintptr_t)addr, src, len); }
    pthread_mutex_unlock(&g_map_lock);
    return out;
}
int dm_guest_heap_read(uint64_t addr, void *out, size_t len) {
    pthread_mutex_lock(&g_map_lock);
    int bad = !dm_guest_heap_readable(addr, len);
    if (!bad) memcpy(out, (void *)(uintptr_t)addr, len);
    pthread_mutex_unlock(&g_map_lock);
    return bad;
}
static int map_find(uint64_t a, uint64_t n) {       // returns a 1-based index, 0 = no overlap; caller holds g_map_lock
    for (int i = 0; i < g_nmap; i++) if (a < g_map[i].base + g_map[i].size && g_map[i].base < a + n) return i + 1;
    return 0;
}
static void map_add(uint64_t b, uint64_t n, uint64_t src, uint32_t st, uint32_t kind) {   // caller holds g_map_lock
    if (g_nmap >= MAX_MAP) die("mapping table full");
    g_map[g_nmap].base = b; g_map[g_nmap].size = n; g_map[g_nmap].src = src; g_map[g_nmap].state = st; g_map[g_nmap].kind = kind; g_nmap++;
}
static void map_del(int i) { g_map[i] = g_map[--g_nmap]; }   // caller holds g_map_lock
// The free gap of the stack region around [a, a + n): from the end of the mapping below it to the start of the one above (caller holds g_map_lock)
static void stack_gap(uint64_t a, uint64_t n, uint64_t *lo, uint64_t *hi) {
    *lo = (uint64_t)(uintptr_t)g_stack; *hi = *lo + g_stack_cap;
    for (int i = 0; i < g_nmap; i++) {
        uint64_t e = g_map[i].base + g_map[i].size;
        if (e <= a && e > *lo) *lo = e;
        if (g_map[i].base >= a + n && g_map[i].base < *hi) *hi = g_map[i].base;
    }
}

// One bit per 4 KB page of the alias region (mapped = 1): Map sets bits, Unmap clears them; returns the number of pages whose state really changed (signed: positive for Map, negative for Unmap)
static int64_t alias_pages(uint64_t off, uint64_t n, int map) {
    int64_t ch = 0;
    for (uint64_t p = off >> 12, e = (off + n + 0xFFF) >> 12; p < e; ) {
        uint64_t sh = p & 63, k = 64 - sh < e - p ? 64 - sh : e - p, m = (k == 64 ? ~0ull : (1ull << k) - 1) << sh;
        uint64_t old = map ? atomic_fetch_or(&g_alias_bits[p >> 6], m) : atomic_fetch_and(&g_alias_bits[p >> 6], ~m);
        ch += __builtin_popcountll((map ? ~old : old) & m);
        p += k;
    }
    return map ? ch : -ch;
}
// Pages of the range not mapped yet (the bitmap is left alone)
static int64_t alias_unmapped(uint64_t off, uint64_t n) {
    int64_t c = 0;
    for (uint64_t p = off >> 12, e = (off + n + 0xFFF) >> 12; p < e; ) {
        uint64_t sh = p & 63, k = 64 - sh < e - p ? 64 - sh : e - p, m = (k == 64 ? ~0ull : (1ull << k) - 1) << sh;
        c += __builtin_popcountll(~atomic_load(&g_alias_bits[p >> 6]) & m);
        p += k;
    }
    return c;
}
static _Atomic int64_t g_alias_mapped;   // bytes mapped in the alias region (page bitmap)

// Word-at-a-time boundaries of the bitmap run containing p (guest page index).
static uint64_t alias_mask(unsigned n) { return n == 64 ? UINT64_MAX : (UINT64_C(1) << n) - 1; }
static uint64_t alias_run_end(uint64_t p, uint64_t end, int mapped) {
    for (uint64_t first = p + 1; first < end;) {
        unsigned shift = first & 63, n = (unsigned)(end - first < 64 - shift ? end - first : 64 - shift);
        uint64_t bits = atomic_load(&g_alias_bits[first >> 6]);
        uint64_t different = (mapped ? ~bits : bits) & (alias_mask(n) << shift);
        if (different) return (first & ~UINT64_C(63)) + __builtin_ctzll(different);
        first += n;
    }
    return end;
}
static void alias_query(uint64_t addr, uint64_t *base, uint64_t *size, int *mapped) {
    uint64_t p = (addr - (uintptr_t)g_alias) >> 12, lo = p;
    *mapped = (atomic_load(&g_alias_bits[p >> 6]) >> (p & 63)) & 1;
    while (lo) {
        uint64_t first = (lo - 1) & ~UINT64_C(63);
        uint64_t bits = atomic_load(&g_alias_bits[first >> 6]);
        uint64_t different = (*mapped ? ~bits : bits) & alias_mask((unsigned)(lo - first));
        if (different) { lo = first + 64 - __builtin_clzll(different); break; }
        lo = first;
    }
    uint64_t hi = alias_run_end(p, g_alias_cap >> 12, *mapped);
    *base = (uintptr_t)g_alias + (lo << 12); *size = (hi - lo) << 12;
}
// Initialize holes using the OLD commit bitmap. Fresh section pages are zero; after any exposure a page may hold stale bytes (discarded pages keep theirs undefined)
static void alias_initialize(uint64_t off, size_t size) {
    uint64_t end = (off + size) >> 12;
    for (uint64_t p = off >> 12; p < end;) {
        int mapped = (atomic_load(&g_alias_bits[p >> 6]) >> (p & 63)) & 1;
        uint64_t next = alias_run_end(p, end, mapped);
        if (!mapped) {
            uint64_t clear = p;
            for (uint64_t q = p; q < next; q++) {
                if (!(g_alias_exposed[q >> 6] & (UINT64_C(1) << (q & 63)))) {
                    if (clear < q) memset(g_alias + (clear << 12), 0, (q - clear) << 12);
                    clear = q + 1;
                }
            }
            if (clear < next) memset(g_alias + (clear << 12), 0, (next - clear) << 12);
        }
        p = next;
    }
    for (uint64_t h = off >> 12, last = (off + size - 1) >> 12; h <= last; ++h)
        g_alias_exposed[h >> 6] |= UINT64_C(1) << (h & 63);
}

static int memory_contains(uint64_t base, uint64_t capacity, uint64_t addr, uint64_t size) {
    return addr >= base && size <= capacity && addr - base <= capacity - size;
}
static uint32_t map_arguments(uint64_t dst, uint64_t src, uint64_t size) {
    if ((dst | src) & 0xfff) return 0xCC01;
    if (!size || (size & 0xfff)) return 0xCA01;
    if (size > UINT64_MAX - src || size > UINT64_MAX - dst) return 0xD401;
    return memory_contains((uintptr_t)g_stack, g_stack_cap, dst, size) ? 0 : 0xDC01;
}
static int map_source_busy(uint64_t addr, uint64_t size) { // g_map_lock held; nonwrapping ranges
    for (int i = 0; i < g_nmap; ++i)
        if (g_map[i].kind != MAP_OWN && addr < g_map[i].src + g_map[i].size && g_map[i].src < addr + size) return 1;
    return 0;
}
// 1: the source is open guest memory; *view: it lies in a section view, so it can be mapped a second time
static int map_source_backed(uint64_t src, uint64_t size, int *view) {
    *view = 0;
    if (memory_contains((uintptr_t)g_heap, g_heap_cur, src, size)) { *view = 1; return 1; }
    if (memory_contains((uintptr_t)g_alias, g_alias_cap, src, size)) { *view = 1; return !alias_unmapped(src - (uintptr_t)g_alias, size); }
    module_t *m = module_of((void *)(uintptr_t)src);
    if (!m || !m->loaded || !memory_contains((uintptr_t)m->base, m->size, src, size)) return 0;
    uint64_t off = src - (uintptr_t)m->base, lo = m->data_off ? m->data_off : (m->ro_off ? m->ro_off : m->text_end);
    return off >= lo && memory_contains(lo, m->size - lo, off, size);   // a module's data and bss (copied)
}
static void map_source_query(uint64_t addr, uint64_t *base, uint64_t *size, uint32_t *attr, uint64_t *perm) {
    uint64_t lo = *base, remaining = *size;
    for (int i = 0; i < g_nmap; ++i) {
        if (g_map[i].kind == MAP_OWN) continue;
        uint64_t src = g_map[i].src, end = src + g_map[i].size;
        uint64_t lower = lo, upper = remaining;
        if (addr >= src && addr < end) {
            if (src > lower) lower = src;
            if (end - lo < upper) upper = end - lo;
            *attr = 1; *perm = 0; // Locked / IsBorrowed, not PermissionLocked (bit 4)
        } else if (end <= addr && end > lower) lower = end;
        else if (src > addr && src - lo < upper) upper = src - lo;
        remaining = upper - (lower - lo); lo = lower;
    }
    *base = lo; *size = remaining;
}

// The guest main thread's stack (the runtime's, as Horizon's loader maps one for the main thread): at the top of the stack region, reported as Stack
uint64_t nxrt_main_stack(size_t size) {
    mem_init();
    size = (size + 0xfff) & ~(size_t)0xfff;
    uint64_t top = (uint64_t)(uintptr_t)g_stack + g_stack_cap, base = top - size, off;
    pthread_mutex_lock(&g_map_lock);
    if (map_find(base, size) || as_section_offset(base, size, &off) || as_map_view(base, size, off) || as_commit(base, size)) die("cannot map the main thread's stack at %#llx", (unsigned long long)base);
    map_add(base, size, 0, 0x0B /*Stack*/, MAP_OWN);
    pthread_mutex_unlock(&g_map_lock);
    return top;
}

int svc_thread(int num, uint64_t *regs);   // hle_thread.c: thread and synchronization kind
void crash_report(const char *why, const void *ctx);

uint64_t svc_dispatch(int num, uint64_t *regs, uint64_t orig_addr) {
    mem_init();
    if (svc_thread(num, regs)) return regs[0];
    switch (num) {
    case 0x01: {  // SetHeapSize(out_addr*, size)  → x1 = heap base
        size_t size = regs[1];
        // Larger than the heap region: ResultOutOfMemory (0xD001), as Horizon's KPageTable::SetHeapSize returns
        if (size > g_heap_cap) { LOG("SetHeapSize OOM: req %#zx > cap %#zx", size, g_heap_cap); regs[0] = 0xD001; return 0; }
        pthread_mutex_lock(&g_map_lock);
        size_t previous = g_heap_cur, image = image_bytes();
        if (size < previous && map_source_busy((uintptr_t)g_heap + size, previous - size)) {
            pthread_mutex_unlock(&g_map_lock); regs[0] = 0xD401; return 0;
        }
        if (image > APP_POOL || size > APP_POOL - image) {   // ResultLimitReached; games retry in 2 MB decrements
            static int once; if (!once++) LOG("[mem] SetHeapSize %#zx + modules %#zx > app pool %#llx (used memory reported %#llx)", size, image_bytes(),
                                              (unsigned long long)APP_POOL, (unsigned long long)DM_GAME_MEM_USED);
            pthread_mutex_unlock(&g_map_lock); regs[0] = 0x10801; return 0;
        }
        uint64_t charged = (uint64_t)DM_GAME_MEM_USED + atomic_load(&g_alias_mapped);
        if (DM_GAME_MEM_MAP_LIMIT && (charged > APP_POOL || size > APP_POOL - charged)) {
            pthread_mutex_unlock(&g_map_lock); regs[0] = 0x10801; return 0;
        }
        heap_resize(g_heap_cur, size);
        if (g_flag.svc) LOG("SetHeapSize %#zx -> base %p", size, (void *)g_heap);
        else if (size > g_heap_cur) LOG("[mem] heap %.2f GB (heap region %.2f GB)", size / 1073741824.0, g_heap_cap / 1073741824.0);
        g_heap_cur = size;
        pthread_mutex_unlock(&g_map_lock);
        regs[0] = 0; regs[1] = (uint64_t)(uintptr_t)g_heap; return 0;
    }
    case 0x02: case 0x03: { // Permission/attribute stubs must not modify a borrowed source.
        uint64_t addr = regs[0], size = regs[1];
        pthread_mutex_lock(&g_map_lock);
        int busy = size > UINT64_MAX - addr || (size && map_source_busy(addr, size));
        pthread_mutex_unlock(&g_map_lock);
        regs[0] = busy ? 0xD401 : 0; return 0;
    }
    case 0x04: {  // MapMemory(dst, src, size): when creating a thread the SDK moves the stack on the heap into the stack region
        uint64_t dst = regs[0], src = regs[1], size = regs[2];
        uint32_t result = map_arguments(dst, src, size);
        if (result) { regs[0] = result; return 0; }
        pthread_mutex_lock(&g_map_lock);
        int view = 0;
        if (map_find(dst, size) || map_source_busy(src, size) || !map_source_backed(src, size, &view)) result = 0xD401;
        else if (g_nmap == MAX_MAP) result = 0xCE01;
        if (result) { pthread_mutex_unlock(&g_map_lock); regs[0] = result; LOG("[RED] MapMemory refused (%#x): %#llx <- %#llx +%#llx", result, (unsigned long long)dst, (unsigned long long)src, (unsigned long long)size); return 0; }
        uint64_t off;
        if (view) {   // the same section pages a second time: commit them at the source (the heap commits lazily), then the view
            dm_guest_write_prepare((void *)(uintptr_t)src, size);
            if (as_section_offset(src, size, &off) || as_map_view(dst, size, off)) die("MapMemory: view %#llx <- %#llx +%#llx failed (error %lu)", (unsigned long long)dst, (unsigned long long)src, (unsigned long long)size, (unsigned long)errno);
            if (as_commit(dst, size)) die("MapMemory: commit %#llx +%#llx failed (error %lu)", (unsigned long long)dst, (unsigned long long)size, (unsigned long)errno);
            map_add(dst, size, src, 0x0B /*Stack*/, MAP_VIEW);
        } else {      // a source outside the section (a module's bss): private pages and a copy, moved back at UnmapMemory (nxnative's way)
            if (as_commit_private(dst, size, AS_RW)) die("MapMemory: private %#llx +%#llx failed (error %lu)", (unsigned long long)dst, (unsigned long long)size, (unsigned long)errno);
            memcpy((void *)(uintptr_t)dst, (void *)(uintptr_t)src, size);   // move semantics: the source region is not accessed while mapped (Horizon makes it inaccessible)
            map_add(dst, size, src, 0x0B /*Stack*/, MAP_COPY);
        }
        pthread_mutex_unlock(&g_map_lock);
        if (g_flag.svc) LOG("MapMemory %#llx <- %#llx +%#llx (%s)", (unsigned long long)dst, (unsigned long long)src, (unsigned long long)size, view ? "view" : "copy");
        regs[0] = 0; return 0;
    }
    case 0x05: {  // UnmapMemory(dst, src, size)
        uint64_t dst = regs[0], src = regs[1], size = regs[2];
        uint32_t result = map_arguments(dst, src, size);
        if (result) { regs[0] = result; return 0; }
        pthread_mutex_lock(&g_map_lock);
        int k = map_find(dst, size);
        if (!k || g_map[k - 1].base != dst || g_map[k - 1].size != size || g_map[k - 1].src != src || g_map[k - 1].kind == MAP_OWN) {
            pthread_mutex_unlock(&g_map_lock); LOG("UnmapMemory does not match a mapping %#llx+%#llx", (unsigned long long)dst, (unsigned long long)size); regs[0] = 0xD401; return 0; }
        if (g_map[k - 1].kind == MAP_VIEW) { if (as_unmap_view(dst)) die("UnmapMemory: unmap %#llx failed (error %lu)", (unsigned long long)dst, (unsigned long)errno); }
        else {
            dm_guest_write_prepare((void *)(uintptr_t)src, size);
            memcpy((void *)(uintptr_t)src, (void *)(uintptr_t)dst, size);
            if (as_release_private(dst, size)) die("UnmapMemory: release %#llx failed (error %lu)", (unsigned long long)dst, (unsigned long)errno);
        }
        map_del(k - 1);
        uint64_t lo, hi; stack_gap(dst, size, &lo, &hi); as_coalesce(lo, hi);
        pthread_mutex_unlock(&g_map_lock);
        regs[0] = 0; return 0;
    }
    case 0x06: {  // QueryMemory(MemoryInfo* out, PageInfo* pout, addr) → x1 = pageinfo
        uint64_t mi = regs[0], addr = regs[2];
        // MemoryInfo: base(0) size(8) state(0x10) attr(0x14) perm(0x18) ...
        uint8_t *a = (uint8_t *)(uintptr_t)addr;
        uint64_t base, size, state, perm;
        uint32_t attr = 0;
        int k;
        pthread_mutex_lock(&g_map_lock);
        uint64_t ab = (uintptr_t)g_alias, ae = ab + g_alias_cap, rb = 0, rn = 0;
        int mapped = 0, in_alias = addr >= ab && addr < ae;
        if (in_alias) alias_query(addr, &rb, &rn, &mapped);
        if (a >= g_heap && a < g_heap + g_heap_cur) { base=(uint64_t)(uintptr_t)g_heap; size=g_heap_cur; state=5/*Heap*/; perm=3; }
        else if ((k = map_find(addr, 1))) { base=g_map[k-1].base; size=g_map[k-1].size; state=g_map[k-1].state; perm=3; }
        else if (in_alias && mapped) { base=rb; size=rn; state=5; perm=3; }
        else if (a >= g_as && a < g_as + g_reserve) {   // free: up to the adjacent mapping/heap boundary
            uint64_t lo = (uint64_t)(uintptr_t)g_as, hi = (uint64_t)(uintptr_t)g_as + g_reserve;
            if (a >= g_heap) lo = (uint64_t)(uintptr_t)g_heap + g_heap_cur;
            else if (hi > (uint64_t)(uintptr_t)g_heap) hi = (uint64_t)(uintptr_t)g_heap;
            if (in_alias) {
                if (rb > ab && rb > lo) lo = rb;
                if (rb + rn < ae && rb + rn < hi) hi = rb + rn;
            } else if (g_alias_cap && addr < ab) {
                alias_query(ab, &rb, &rn, &mapped);
                uint64_t edge = mapped ? ab : rb + rn;
                if ((mapped || edge < ae) && edge < hi) hi = edge;
            } else if (g_alias_cap) {
                alias_query(ae - 1, &rb, &rn, &mapped);
                uint64_t edge = mapped ? ae : rb;
                if ((mapped || edge > ab) && edge > lo) lo = edge;
            }
            for (int i = 0; i < g_nmap; i++) {
                uint64_t e = g_map[i].base + g_map[i].size;
                if (e <= addr && e > lo) lo = e;
                if (g_map[i].base > addr && g_map[i].base < hi) hi = g_map[i].base;
            }
            base=lo; size=hi-lo; state=0; perm=0; }
        else if (module_of(a)) {
            // Loaded module: report the regions in Horizon's layout -- code [0, ro) Code+RX, read-only [ro, data) Code+R, data and bss [data, end) CodeData+RW.
            // The SDK's C++ exception unwinding (nn::ro::detail::GetExceptionInfo) walks back from the PC over "Code+RX" regions to find the module start, forward over Code then CodeData to find its end
            module_t *mm = module_of(a); uint64_t off = (uint64_t)(a - mm->base), mb = (uint64_t)(uintptr_t)mm->base;
            uint64_t ro = mm->ro_off ? mm->ro_off : mm->text_end, da = mm->data_off ? mm->data_off : ro;
            if (off < ro)      { base = mb;      size = ro;            state = 3/*Code*/;     perm = 5; }
            else if (off < da) { base = mb + ro; size = da - ro;       state = 3/*Code*/;     perm = 1; }
            else               { base = mb + da; size = mm->size - da; state = 4/*CodeData*/; perm = 3; }
        }
        // Other host memory (host-call tokens, the host's own): "free, no permissions" (the 4 KiB page around it) as Horizon does for unmapped addresses
        else { base=addr & ~0xFFFull; size=0x1000; state=0/*Free*/; perm=0; }
        map_source_query(addr, &base, &size, &attr, &perm);
        pthread_mutex_unlock(&g_map_lock);
        if (mi) {
            struct { uint64_t base, size; uint32_t state, attr, perm, ipc, device, reserved; }
                out = {base, size, (uint32_t)state, attr, (uint32_t)perm, 0, 0, 0};
            _Static_assert(sizeof out == 40, "MemoryInfo ABI size");
            memcpy((void *)(uintptr_t)mi, &out, sizeof out);
        }
        regs[0] = 0; regs[1] = 0; return 0;
    }
    case 0x2C: case 0x2D: {  // MapPhysicalMemory / UnmapPhysicalMemory(addr, size): only allowed inside the alias region (used by the SDK's VammManager)
        uintptr_t a = regs[0]; size_t size = regs[1];
        if (!size || ((a | size) & 0xfff) || !memory_contains((uintptr_t)g_alias, g_alias_cap, a, size)) {
            LOG("SVC %#x outside the alias region %p+%#zx", num, (void *)a, size); regs[0] = 0xD401; return 0;
        }
        pthread_mutex_lock(&g_map_lock);
        // The SDK extends committed windows across borrowed stack sources. Recommit preserves every live byte; only decommit can invalidate backing required for eventual copyback.
        if (num == 0x2D && map_source_busy(a, size)) { pthread_mutex_unlock(&g_map_lock); regs[0] = 0xD401; return 0; }
        uint64_t off = a - (uintptr_t)g_alias;
        int64_t need = alias_unmapped(off, size) * 4096;
        // Profile mem_map_limit: a map that would take the process past the application pool fails with ResultLimitReached, as on Horizon
        if (DM_GAME_MEM_MAP_LIMIT && num == 0x2C) {
            int64_t room = (int64_t)APP_POOL - (int64_t)(g_heap_cur + DM_GAME_MEM_USED) - atomic_load(&g_alias_mapped);
            if (need && need > room) { pthread_mutex_unlock(&g_map_lock); regs[0] = 0x10801; return 0; }
        }
        int commit = num == 0x2C;
        if (commit && need) {   // only the pages not mapped yet: mapped neighbours stay as they are
            if (as_commit(a, size)) die("SVC %#x commit %p+%#zx error %lu", num, (void *)a, size, (unsigned long)errno);
            alias_initialize(off, size);
        } else if (!commit && (int64_t)size - need) {
            for (uint64_t p = off >> 12, e = (off + size) >> 12; p < e;) {   // the mapped runs only: discarding needs committed pages
                int m = (atomic_load(&g_alias_bits[p >> 6]) >> (p & 63)) & 1; uint64_t nx = alias_run_end(p, e, m);
                if (m) as_discard((uintptr_t)g_alias + (p << 12), (nx - p) << 12);
                p = nx;
            }
        }
        if (g_flag.svc) LOG("%sPhysicalMemory %p+%#zx", commit ? "Map" : "Unmap", (void *)a, size);
        { int64_t d = alias_pages(off, size, commit) * 4096, before = atomic_fetch_add(&g_alias_mapped, d), after = before + d;
          pthread_mutex_unlock(&g_map_lock);
          if (before >> 28 != after >> 28) LOG("[mem] alias region mapped %.2f GB (application pool %.2f GB)", after / 1073741824.0, APP_POOL / 1073741824.0); }
        regs[0] = 0; return 0;
    }
    case 0x26: {  // Break(reason, ptr, size): SDK fatal abort
        uint32_t res = 0;
        if (regs[1] && regs[2] >= 4) plat_read(regs[1], &res, 4);
        module_t *mm = module_of((void *)(uintptr_t)regs[30]);
        static int nb;
        LOG("Break#%d reason=%#llx Result=%#x svc at %#llx, lr %s+%#llx", ++nb, (unsigned long long)regs[0], res, (unsigned long long)orig_addr, mm ? mm->name : "?",
            (unsigned long long)(mm ? regs[30] - (uint64_t)(uintptr_t)mm->base : regs[30]));
        dm_backtrace(regs[29]);
        if (regs[0] & 0x80000000ull) return 0;   // notification only, can continue
        crash_report("svcBreak (the game aborted)", NULL);
        plat_exit(42);
    }
    case 0x1E: { guest_clock_read_wait(); regs[0] = 0; regs[1] = guest_ticks(); return 0; }   // GetSystemTick (hle_os.c: the guest system counter)
    case 0x27: {  // OutputDebugString(str, len)
        if (g_flag.dbg) LOG("[guest] %.*s", (int)regs[1], (char *)(uintptr_t)regs[0]);
        regs[0] = 0; return 0;
    }
    case 0x29: {  // GetInfo(out*, id, handle, subid) → x1 = value
        uint64_t id = regs[1], v = 0;
        switch (id) {
        case I_CoreMask: v = 7; break;
        case I_PriorityMask: v = 0; break;
        case I_AliasAddr: v = (uint64_t)(uintptr_t)g_alias; break;   case I_AliasSize: v = g_alias_cap; break;
        case I_HeapAddr:  v = (uint64_t)(uintptr_t)g_heap;  break;   case I_HeapSize:  v = g_heap_cap;  break;
        case I_AslrAddr:  v = (uint64_t)(uintptr_t)g_as;    break;   case I_AslrSize:  v = g_reserve;   break;
        case I_StackAddr: v = (uint64_t)(uintptr_t)g_stack; break;   case I_StackSize: v = g_stack_cap; break;
        case I_TotalMem: case I_TotalNonSys: v = APP_POOL; break;
        case I_UsedMem:  case I_UsedNonSys:  pthread_mutex_lock(&g_map_lock);
                                             v = g_heap_cur + DM_GAME_MEM_USED;   // profile mem_used
                                             if (DM_GAME_MEM_MAP_LIMIT) v += (uint64_t)atomic_load(&g_alias_mapped);
                                             pthread_mutex_unlock(&g_map_lock); break;
        // the system resource the NPDM declares (profile sys_resource). The SDK's IsVirtualAddressMemoryEnabled is "total != 0"
        case I_SysResTotal: v = DM_GAME_SYS_RESOURCE; break; case I_SysResUsed: v = DM_GAME_SYS_RESOURCE ? 0x400000 : 0; break;
        case I_DebuggerAttached: v = 0; break;
        case I_RandomEntropy: v = dm_entropy64(); break;   // a deterministic stream when DM_DETCLOCK is on (hle_os.c)
        case I_ProgramId: { static uint64_t pid; if (!pid) { const char *e = getenv("DM_PROGRAM_ID"); pid = e ? strtoull(e, NULL, 16) : DM_GAME_PROGRAM_ID; } v = pid; break; }
        case I_IsApplication: v = 1; break;
        // nn::os::GetFreeThreadCount (SDK 11+): the thread resource limit less the threads alive. pm gives an application a limit of 96 threads
        case I_FreeThreadCount: { int thr_limit_used(void); int u = thr_limit_used(); v = u < 96 ? 96 - u : 0; break; }
        case I_UserExcAddr: { static _Alignas(0x1000) uint8_t ctx[0x1000]; v = (uint64_t)(uintptr_t)ctx; break; }   // user exception context area: newer SDKs' SetDyingMessageRegion writes to +0x1c0
        // SWITCH2: the extended SDK's RemoteRegionManager / VAMM queries (ids 29..33): 0 for the remote region/memory ids (no remote mappings) and the page
        // granule for 33, the host's page size: 4 KB here (nxnative answers 16 KB, the Apple page)
        // With a system resource (NPDM; VammManager enabled) 29/30 are the address range VAMM reserves its heap in: the upper three quarters of the alias region
        // (the region MapPhysicalMemory is allowed in, see 0x2C), as Lichii does with the upper half
        case 29: v = DM_GAME_SYS_RESOURCE ? (uint64_t)(uintptr_t)g_alias + g_alias_cap / 4 : 0; break;
        case 30: v = DM_GAME_SYS_RESOURCE ? g_alias_cap / 4 * 3 : 0; break;
        case 31: case 32: v = 0; break;
        case 33: v = 0x1000; break;
        default: v = 0; break;
        }
        regs[0] = 0; regs[1] = v; return 0;
    }
    default:
        if (!g_seen[num & 0xff]) { g_seen[num & 0xff] = 1; LOG("SVC %#x x0=%#llx x1=%#llx x2=%#llx (placeholder returns success)", num, (unsigned long long)regs[0], (unsigned long long)regs[1], (unsigned long long)regs[2]); }
        regs[0] = 0; return 0;
    }
}
