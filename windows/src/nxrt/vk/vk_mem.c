// GPU memory: pool mirrors, sparse backing, the staging ring, dirty-page uploads, the per-frame budget (vk_mem.h; docs/m5-vulkan.md §3).
// nxnative's Metal backend wraps each pool zero-copy (newBufferWithBytesNoCopy, unified memory); a discrete GPU needs copies, and importing guest memory
// as host memory is what research/01 lesson 10 rules out. Behaviour of pool lookups follows nvn_mtl.m (mtl_pool_add, pool_find); the rest is new.
#include "vk_mem.h"
#include "vk_submit.h"
#include "ind_clamp_comp.h"
#include "nvn_dirty.h"   // gfx/: the dirty-page tracker the recording layer shares (nxnative's, ported by track A)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#endif

void dm_guest_write_prepare(void *addr, size_t len);   // host/crash.c: unprotect before the host writes guest memory

enum { PAGE = 4096, PAGE_SHIFT_ = 12, GRAN = 2u << 20, HOT_STREAK = 3, HOT_COOL_FRAMES = 240, MAXPOOL = 16384, RUN_MAX = 1024, MAXWIN = 32 };
// A pool larger than the device's largest buffer (AMD: 2 GiB; the game's biggest pool is 3.4 GiB) gets several buffers, windows over it. With sparse
// binding and aliasing the windows overlap and share the same memory, so any range up to the overlap fits in one of them; otherwise they are side by side.
typedef struct {
    uintptr_t lo, hi, base;       // the pool [lo, hi) as given; base = lo rounded down to a page
    uint64_t size, npages;        // bytes the buffer covers (from base, whole pages)
    const void *key;
    VkBuffer buf[MAXWIN]; uint64_t win_lo[MAXWIN], win_n[MAXWIN]; int nwin;   // window k covers [win_lo, win_lo + win_n) of the pool, from base
    int sparse;
    vk_alloc *gran; uint64_t ngran, bound;   // sparse: one allocation per bound granule, bound into every window that covers it
    vk_alloc mem[MAXWIN];         // not sparse: one allocation per window
    const void *span;             // the dirty tracker's range (registered by the recording layer right after gfx_pool_add)
    uint64_t *up;                 // per page: the tracker's stamp when its device copy was taken (0 = never copied)
    uint32_t *last_dirty; uint8_t *streak, *hot;   // hot pages: written in HOT_STREAK consecutive frames, left writable and copied at every use
    uint8_t *committed;           // per page: seen committed and readable once (pool_sync asks VirtualQuery only for pages not known yet)
    _Atomic uint32_t *gpu_pending;   // per page: GPU writes recorded whose bytes have not reached guest memory yet (the device copy is the newer one). 32 bits: a page that takes hundreds of small write-backs in flight (inline uniform updates) wrapped a byte to 0 at 256 and looked free
    uint64_t hot_n, cursor;       // vkm_sync_all's position
    uint64_t clean_epoch, pass_ep; int pass_left;   // sync_all_within: the tracker's write count at which a whole pass found nothing left to copy (0 = unknown); the count at the start of the pass in progress, and whether it stepped over pages it could not copy
    uint64_t *unc_until;          // per page: the frame until which a page found uncommitted (and zero-filled) is not asked about again
    uint64_t *hot_sub;            // hot pages: the submission whose upload command buffer last copied the page (all uploads of a submission run before its main commands, so a second copy in it changes nothing the GPU can see)
    uint8_t *owned; uint8_t **shadow;   // GPU-owned pages (written through global memory pointers): guest memory as the mirror last had it, so a CPU write is applied as the words that differ, not as the whole page
    uint64_t *lst;                // per page (lazy): the tracker's stamp after the last write-back landing wrote the page (a later CPU write is the one that counts, see wbm_since)
    uint8_t *rr; uint8_t **rst;   // read-and-reset pages (see rr_land): flag; per page the CPU words a landing leaves pending for the device, the learned reset words and the stamps of the last sync and landing
    uint64_t upb_use, upb_touch, upb_all, upb_first, upb_hot, uncommitted_fill;   // bytes uploaded by reason since the last statistics line (diagnostics)
    uint64_t gmem_delta;          // nonzero when the pool is in the global memory table (the first window's device address minus base)
    uint64_t win_delta[MAXWIN];   // per window: device address of its buffer minus (base + win_lo): what the global memory table holds for the pages it covers
} pool_t_;

// Unbound granules of a sparse pool all map to one zeroed granule (SPARSE_ALIASED): AMD hangs on a read of a sparse page nothing is bound to, and shaders that go through the global memory page
// table can read anywhere in a pool (Eglantine 8e82c282de / eff0fe3bbf / 4506f0c7a0). NXRT_VK_SPARSE_ALIAS=1 enables it; DEFAULT OFF: vkQueueBindSparse(alias) fails (-4) then device lost on a windowed/swapchain run, the stall's real fix was the upload storm
static struct { VkBuffer buf; vk_alloc mem; int ok, tried; } g_zero;
static __thread int t_left;   // pool_sync stepped over pages that still need a copy (uncommitted, or a GPU write in flight): the pool is not known clean
static __thread int t_why;   // 0 use, 1 gmem touch, 2 sync_all / commit sync (diagnostics: who the upload bytes are for)
static _Atomic uint64_t s_t_query, s_t_copy, s_t_prot, s_t_bind, s_t_scan, s_unc_skipped;   // ns inside pool_sync (diagnostics)
static _Atomic uint64_t g_shadow_pages, s_own_pages, s_own_words, s_own_full;
static pool_t_ *g_pool[MAXPOOL]; static int g_npool;
// The pool table. READERS FIRST on purpose: sync_all_within holds it for reading while pool_sync's staging may wait (vks_wait, no timeout) for the GPU's timeline, and the completion thread's
// effects (wb_land -> gpu_pending) take it for reading to get that timeline to advance. A writer-first lock queues a pool_add/del writer between the two: the effect blocks behind the writer,
// the writer behind the waiting reader, the reader behind the effect (the "GPU has completed nothing" watchdog stop 8 s into the 3D scene). Writers are rare (pool create/destroy)
static plat_rwlock_t g_lk = PLAT_RWLOCK_RP_INIT;
static uint64_t g_frame = 1;
static VkBufferUsageFlags g_usage;
static VkDeviceSize g_gran = GRAN, g_max_buffer;
static int g_alias;   // overlapping windows: sparse binding with aliasing (NXRT_VK_NO_ALIAS=1 turns them off, for tests)

// ST_Q: allocation records the ring keeps (one per upload run and overflow buffer). At 4096 a frame of ~1200 small runs, three frames in flight, filled it and every further run waited for the GPU to finish a submission
enum { ST_Q = 16384 };
static struct {
    VkBuffer buf; vk_alloc mem; uint8_t *map; VkDeviceSize size, head, tail; int wrapped;
    struct { uint64_t value; int owner; VkDeviceSize begin, end; VkBuffer ovf; vk_alloc ovf_mem; } q[ST_Q]; unsigned qh, qt;   // allocations in order; value 0 until owner commits
    plat_rwlock_t lk;
} g_st;

static uint64_t g_budget = 64ull << 20, g_budget_used;
static _Atomic uint64_t s_t_stwait, s_t_stovf;   // ns waiting in vkm_staging for the GPU (ring full), ns making overflow buffers (diagnostics)
static _Atomic uint64_t s_up_bytes, s_up_pages, s_up_runs, s_up_ns, s_sync, s_st_bytes, s_st_ovf, s_st_waits, s_wb_bytes, s_uncommitted, s_recopy_pages;

// ---------- Staging ring ----------
static VkDeviceSize g_ovf_live;   // bytes in overflow staging buffers not freed yet (host-visible memory: it counts against the GTT)
static void st_reclaim(void) {   // caller holds g_st.lk
    uint64_t done = atomic_load(&g_vk.tl_done);
    while (g_st.qt != g_st.qh) {
        unsigned i = g_st.qt % ST_Q;
        if (!g_st.q[i].value || g_st.q[i].value > done) break;
        if (g_st.q[i].ovf) { vkDestroyBuffer(g_vk.dev, g_st.q[i].ovf, NULL); vk_free_mem(&g_st.q[i].ovf_mem); g_st.q[i].ovf = VK_NULL_HANDLE; g_ovf_live -= g_st.q[i].end; }
        else g_st.tail = g_st.q[i].end;
        g_st.qt++;
    }
    if (g_st.qt == g_st.qh) { g_st.head = g_st.tail = 0; g_st.wrapped = 0; }
}
// Called by vk_submit at each commit: that slot's allocations now live until its timeline value
void vkm_committed(int owner, uint64_t value) {
    plat_rw_wrlock(&g_st.lk);
    for (unsigned k = g_st.qt; k != g_st.qh; k++) if (!g_st.q[k % ST_Q].value && g_st.q[k % ST_Q].owner == owner) g_st.q[k % ST_Q].value = value;
    plat_rw_wrunlock(&g_st.lk);
}
int vks_owner(void);
void *vkm_staging(VkDeviceSize n, VkDeviceSize align, VkBuffer *buf, VkDeviceSize *off) {
    int owner = vks_owner();
    if (owner < 0) { VKRED("staging asked for outside a submission"); return NULL; }
    if (!align) align = 16;
    n = (n + 15) & ~(VkDeviceSize)15;
    plat_rw_wrlock(&g_st.lk);
    for (int attempt = 0; attempt < 64; attempt++) {
        st_reclaim();
        if (g_st.qh - g_st.qt >= ST_Q - 1) goto wait;
        VkDeviceSize a = (g_st.head + align - 1) / align * align;
        int ok = 0;
        if (!g_st.wrapped) {   // free space: [head, size) then [0, tail)
            if (a + n <= g_st.size) ok = 1;
            else if (n <= g_st.tail) { a = 0; g_st.wrapped = 1; ok = 1; }
        } else if (a + n <= g_st.tail) ok = 1;
        if (ok) {
            unsigned i = g_st.qh++ % ST_Q;
            g_st.q[i].value = 0; g_st.q[i].owner = owner; g_st.q[i].begin = a; g_st.q[i].end = a + n; g_st.q[i].ovf = VK_NULL_HANDLE;
            g_st.head = a + n;
            plat_rw_wrunlock(&g_st.lk);
            atomic_fetch_add(&s_st_bytes, n);
            *buf = g_st.buf; *off = a; return g_st.map + a;
        }
        if (n > g_st.size / 2) break;   // too big for the ring: a buffer of its own
    wait:;
        // the ring is full: wait for the oldest committed allocation's submission; when only the recording's own remain, overflow
        uint64_t v = g_st.qt != g_st.qh ? g_st.q[g_st.qt % ST_Q].value : 0;
        if (!v) break;
        plat_rw_wrunlock(&g_st.lk);
        atomic_fetch_add(&s_st_waits, 1);
        { uint64_t tw = now_ns(); vks_wait(v, UINT64_MAX); atomic_fetch_add(&s_t_stwait, now_ns() - tw); }
        plat_rw_wrlock(&g_st.lk);
    }
    // Overflow: a host buffer for this submission alone, released with it. Bounded: past NXRT_VK_OVERFLOW_MB (default 768) of them in flight, wait for the oldest committed one first
    // (2026-11-05: 11 GB of them at once filled the GTT and the submission failed with "Not enough memory")
    { static VkDeviceSize cap; if (!cap) { const char *e = getenv("NXRT_VK_OVERFLOW_MB"); cap = (e && atoi(e) > 0 ? (VkDeviceSize)atoi(e) : 768) << 20; }
      for (int w = 0; w < 64 && g_ovf_live + n > cap; w++) {
        uint64_t v = 0; for (unsigned k = g_st.qt; k != g_st.qh; k++) if (g_st.q[k % ST_Q].ovf && g_st.q[k % ST_Q].value) { v = g_st.q[k % ST_Q].value; break; }
        if (!v) break;
        plat_rw_wrunlock(&g_st.lk); atomic_fetch_add(&s_st_waits, 1); { uint64_t tw = now_ns(); vks_wait(v, UINT64_MAX); atomic_fetch_add(&s_t_stwait, now_ns() - tw); } plat_rw_wrlock(&g_st.lk); st_reclaim();
      } }
    uint64_t t_ovf = now_ns();
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, n, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer b = VK_NULL_HANDLE; vk_alloc m;
    void *p = NULL;
    if (vkCreateBuffer(g_vk.dev, &bci, NULL, &b) == VK_SUCCESS) {
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, b, &mr);
        if (!vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &m)) {
            vkBindBufferMemory(g_vk.dev, b, m.mem, m.off); p = m.map;
            unsigned i = g_st.qh++ % ST_Q;
            g_st.q[i].value = 0; g_st.q[i].owner = owner; g_st.q[i].begin = 0; g_st.q[i].end = n; g_st.q[i].ovf = b; g_st.q[i].ovf_mem = m; g_ovf_live += n;
        } else { vkDestroyBuffer(g_vk.dev, b, NULL); b = VK_NULL_HANDLE; }
    }
    plat_rw_wrunlock(&g_st.lk);
    if (!p) { VKRED("staging: no host memory for %llu bytes", (unsigned long long)n); return NULL; }
    atomic_fetch_add(&s_st_ovf, 1); atomic_fetch_add(&s_st_bytes, n); atomic_fetch_add(&s_t_stovf, now_ns() - t_ovf);
    *buf = b; *off = 0; return p;
}
uint64_t vkm_budget_left(void) { return g_budget_used >= g_budget ? 0 : g_budget - g_budget_used; }
void vkm_budget_take(uint64_t n) { g_budget_used += n; }

static _Atomic uint64_t g_bound_total, g_pools_made, g_pools_freed, g_pool_bytes_made;   // device memory bound under pool mirrors in all (freed pools not subtracted: a log figure, read by the watchdog thread without a lock)
uint64_t vkm_bound_total(void) { return atomic_load(&g_bound_total); }
void vkm_pool_counts(uint64_t *made, uint64_t *freed, uint64_t *bytes_made) { *made = atomic_load(&g_pools_made); *freed = atomic_load(&g_pools_freed); *bytes_made = atomic_load(&g_pool_bytes_made); }
// ---------- The table of guest pages for untracked global memory (nxs_spv.py gmem_bda; vk_mem.h has the layout) ----------
// A guest GPU address is the pool's own pointer (hle_nvn.c nvnMemoryPoolGetBufferAddress), and each pool has one mirror buffer, so one page of address space has one difference to its
// device address. A binary search over the 2,000-odd pools would cost eleven reads per access, and pools as small as a page sit next to each other, so a coarser table cannot hold them.
// 8-byte aligned stores: a shader reads each word in one load and sees the old or the new value. Blocks are never given back: the address space is small and pools come and go in it.
enum { GMEM_L1_WORDS = 1u << 19, GMEM_L2_WORDS = 512, GMEM_TABLE_BYTES = 64u << 20 };
static struct { VkBuffer buf; vk_alloc mem; _Atomic uint64_t *map; uint64_t next, words; int off, full, sync_all; } g_gt;
static _Atomic int g_gmem_used_now; static _Atomic uint64_t s_gmem_syncs, s_gmem_programs, s_gmem_dirty_bytes; static _Atomic int s_gmem_write_programs;

static int win_of(const pool_t_ *p, uint64_t off) { int k = p->nwin - 1; while (k > 0 && p->win_lo[k] > off) k--; return k; }   // the last window starting at or before off
static int win_for(const pool_t_ *p, uint64_t off, uint64_t n) { int k = win_of(p, off); return off + n <= p->win_lo[k] + p->win_n[k] ? k : -1; }   // the window holding all of it, or -1

static void gmem_init(void) {
    const char *e = getenv("NXRT_VK_GMEM"); g_gt.off = e && !strcmp(e, "0");
    e = getenv("NXRT_VK_GMEM_SYNC"); g_gt.sync_all = e && !strcmp(e, "all");
    if (!g_vk.f12.bufferDeviceAddress || !g_vk.f10.shaderInt64) { VKLOG("global memory: no buffer device addresses or 64-bit integers on this device: programs that need them are skipped"); return; }
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, GMEM_TABLE_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (vkCreateBuffer(g_vk.dev, &bci, NULL, &g_gt.buf) != VK_SUCCESS) { VKRED("global memory: cannot create the table"); g_gt.buf = VK_NULL_HANDLE; return; }
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, g_gt.buf, &mr);
    int r = vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, &g_gt.mem);   // every access reads it: on the GPU's side if it can be
    if (r) r = vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, &g_gt.mem);
    if (r || !g_gt.mem.map) { VKRED("global memory: no host-visible memory for the table"); vkDestroyBuffer(g_vk.dev, g_gt.buf, NULL); g_gt.buf = VK_NULL_HANDLE; return; }
    vkBindBufferMemory(g_vk.dev, g_gt.buf, g_gt.mem.mem, g_gt.mem.off);
    g_gt.map = (_Atomic uint64_t *)g_gt.mem.map; memset((void *)g_gt.map, 0, GMEM_TABLE_BYTES);
    g_gt.words = GMEM_TABLE_BYTES / 8; g_gt.next = GMEM_L1_WORDS;
    vk_name((uint64_t)g_gt.buf, VK_OBJECT_TYPE_BUFFER, "global memory table");
    VKLOG("global memory: page table of %u MB%s", GMEM_TABLE_BYTES >> 20, g_gt.off ? ", left empty (NXRT_VK_GMEM=0)" : "");
}
static void gmem_put(uint64_t va, uint64_t delta) {   // caller holds g_lk for writing
    if (va >> 40) { static int n; if (!n++) VKRED("global memory: a pool at %#llx is past the 40 bits the page table covers: shaders read it as 0", (unsigned long long)va); return; }
    uint64_t i1 = va >> 21, block = atomic_load(&g_gt.map[i1]);
    if (!block) {
        if (g_gt.next + GMEM_L2_WORDS > g_gt.words) { if (!g_gt.full++) VKRED("global memory: the page table is full: pages past %#llx are not mapped", (unsigned long long)va); return; }
        block = g_gt.next; g_gt.next += GMEM_L2_WORDS;
        atomic_store(&g_gt.map[block + ((va >> PAGE_SHIFT_) & (GMEM_L2_WORDS - 1))], delta);   // the entry before the block that leads to it
        atomic_store(&g_gt.map[i1], block);
        return;
    }
    atomic_store(&g_gt.map[block + ((va >> PAGE_SHIFT_) & (GMEM_L2_WORDS - 1))], delta);
}
static void gmem_map(pool_t_ *p) {   // caller holds g_lk for writing; a pool over another one wins, as the last registered
    if (!g_gt.map || g_gt.off) return;
    for (int w = 0; w < p->nwin; w++) {   // a page is read through the last window that starts at or before it (overlapping windows share their memory)
        VkBufferDeviceAddressInfo ai = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, NULL, p->buf[w]};
        p->win_delta[w] = vkGetBufferDeviceAddress(g_vk.dev, &ai) - ((uint64_t)p->base + p->win_lo[w]);
    }
    p->gmem_delta = p->win_delta[0];
    for (uint64_t k = 0; k < p->npages; k++) gmem_put((uint64_t)p->base + k * PAGE, p->win_delta[win_of(p, k * PAGE)]);
}
static void gmem_unmap(pool_t_ *p) {   // caller holds g_lk for writing, p already out of the list. A submission in flight then reads 0 instead of memory about to retire
    if (!g_gt.map || !p->gmem_delta) return;
    for (uint64_t k = 0; k < p->npages; k++) {
        uint64_t va = (uint64_t)p->base + k * PAGE; if (va >> 40) continue;
        uint64_t block = atomic_load(&g_gt.map[va >> 21]); if (!block) continue;
        _Atomic uint64_t *w = &g_gt.map[block + ((va >> PAGE_SHIFT_) & (GMEM_L2_WORDS - 1))];
        if (atomic_load(w) == p->win_delta[win_of(p, k * PAGE)]) atomic_store(w, 0);
    }
    for (int i = 0; i < g_npool; i++) if (g_pool[i]->base < p->base + p->size && p->base < g_pool[i]->base + g_pool[i]->size) gmem_map(g_pool[i]);   // what lay under it shows again
}
VkBuffer vkm_gmem_table(VkDeviceSize *size) { if (size) *size = GMEM_TABLE_BYTES; return g_gt.buf; }
uint64_t vkm_gmem_lookup(uint64_t va) {
    if (!g_gt.map || (va >> 40)) return 0;
    uint64_t block = atomic_load(&g_gt.map[va >> 21]); if (!block) return 0;
    uint64_t d = atomic_load(&g_gt.map[block + ((va >> PAGE_SHIFT_) & (GMEM_L2_WORDS - 1))]);
    return d ? (va & ~3ull) + d : 0;
}
void vkm_gmem_used(const char *key, int writes, int open) {
    if (open || g_gt.sync_all) atomic_store(&g_gmem_used_now, 1);
    static _Atomic int nlog;
    if (atomic_fetch_add(&nlog, 1) < 64) VKLOG("global memory: shader %s goes through the page table%s%s", key, open ? ", from addresses that need every pool in sync" : "", writes ? " and writes through it: those writes reach the mirror, not guest memory" : "");
    atomic_fetch_add(&s_gmem_programs, 1);
    if (writes) atomic_store(&s_gmem_write_programs, 1);
}

// ---------- Pools ----------
static int pool_cmp(const void *a, const void *b) { uintptr_t x = (*(pool_t_ *const *)a)->lo, y = (*(pool_t_ *const *)b)->lo; return x < y ? -1 : x > y; }
static pool_t_ *pool_find(uintptr_t a, uint64_t len) {   // caller holds g_lk (shared): binary search on start, then a scan for overlapping pools
    int lo = 0, hi = g_npool - 1, hit = -1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (g_pool[m]->lo <= a) { hit = m; lo = m + 1; } else hi = m - 1; }
    if (hit >= 0 && a + len <= g_pool[hit]->hi) return g_pool[hit];
    for (int i = 0; i < g_npool; i++) if (a >= g_pool[i]->lo && a + len <= g_pool[i]->hi) return g_pool[i];
    return NULL;
}

static void pool_alias_all(pool_t_ *p);
static void pool_free(pool_t_ *p) {
    atomic_fetch_sub(&g_bound_total, p->bound); atomic_fetch_add(&g_pools_freed, 1);
    for (int k = 0; k < p->nwin; k++) if (p->buf[k]) vkDestroyBuffer(g_vk.dev, p->buf[k], NULL);
    if (p->sparse) { for (uint64_t g = 0; g < p->ngran; g++) vk_free_mem(&p->gran[g]); free(p->gran); }
    else for (int k = 0; k < p->nwin; k++) vk_free_mem(&p->mem[k]);
    if (p->shadow) { for (uint64_t k = 0; k < p->npages; k++) if (p->shadow[k]) { free(p->shadow[k]); atomic_fetch_sub(&g_shadow_pages, 1); } free(p->shadow); }
    if (p->rst) { for (uint64_t k = 0; k < p->npages; k++) free(p->rst[k]); free(p->rst); }
    free(p->rr); free(p->lst);
    free(p->unc_until); free(p->hot_sub); free(p->owned); free(p->up); free(p->last_dirty); free(p->streak); free(p->hot); free((void *)p->gpu_pending); free(p->committed); free(p);
}
static void pool_free_cb(void *arg) { pool_free(arg); }

void vkm_pool_add(const void *mem, uint64_t size) {
    if (!g_vk.up || !mem || !size) return;
    pool_t_ *p = calloc(1, sizeof *p);
    p->lo = (uintptr_t)mem; p->hi = p->lo + size; p->key = mem;
    p->base = p->lo & ~(uintptr_t)(PAGE - 1);
    p->size = ((p->hi - p->base) + PAGE - 1) & ~(uint64_t)(PAGE - 1); p->npages = p->size / PAGE;
    p->up = calloc(p->npages, sizeof *p->up); p->last_dirty = calloc(p->npages, sizeof *p->last_dirty); p->streak = calloc(p->npages, 1); p->hot = calloc(p->npages, 1); p->gpu_pending = calloc(p->npages, sizeof *p->gpu_pending);
    p->committed = calloc(p->npages, 1);
    // Sparse backing comes in 2 MB granules: a pool smaller than that (OoT makes ~3,900 of them, most of a page to a few hundred KB) would be backed by a whole granule once touched,
    // 3,900 x 2 MB = 7.8 GB of VRAM for 5.4 GB of pools (2026-11-05: the submission failed with "Not enough memory for command submission"). Pools below NXRT_VK_SPARSE_MIN_MB
    // (default 4) are backed whole, at their own size, from the allocator's blocks
    static uint64_t sparse_min = ~0ull; if (sparse_min == ~0ull) { const char *e = getenv("NXRT_VK_SPARSE_MIN_MB"); sparse_min = (e ? (uint64_t)atoi(e) : 4) << 20; }
    p->sparse = g_vk.has.sparse_buffer && p->size >= sparse_min;
    VkDeviceSize bsize = p->sparse ? (p->size + g_gran - 1) / g_gran * g_gran : p->size, w = bsize, ov = 0;
    if (g_max_buffer && bsize > g_max_buffer) {   // windows start on granules (or pages), so a granule never straddles two
        w = p->sparse ? g_max_buffer / g_gran * g_gran : g_max_buffer / PAGE * PAGE;
        if (p->sparse && g_alias) { ov = w / 4 < (256ull << 20) ? w / 4 : (256ull << 20); ov = ov / g_gran * g_gran; }
    }
    for (uint64_t at = 0; w > ov && p->nwin < MAXWIN; at += w - ov) {
        p->win_lo[p->nwin] = at; p->win_n[p->nwin] = bsize - at < w ? bsize - at : w; p->nwin++;
        if (at + w >= bsize) break;
    }
    if (!p->nwin || p->win_lo[p->nwin - 1] + p->win_n[p->nwin - 1] < bsize) {
        VKRED("memory pool %p+%#llx does not fit in %d buffers of the device's largest size (%#llx): not mirrored", mem, (unsigned long long)size, MAXWIN, (unsigned long long)g_max_buffer);
        p->nwin = 0; pool_free(p); return;
    }
    // SPARSE_ALIASED: overlapping windows share memory, and NXRT_VK_SPARSE_ALIAS=1's zero block is bound under every unbound granule
    VkBufferCreateFlags fl = p->sparse ? VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT | (g_vk.has.sparse_aliased ? VK_BUFFER_CREATE_SPARSE_ALIASED_BIT : 0) : 0;
    int ok = p->up && p->hot && p->committed;
    for (int k = 0; ok && k < p->nwin; k++) {
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, fl, p->win_n[k], g_usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
        ok = vkCreateBuffer(g_vk.dev, &bci, NULL, &p->buf[k]) == VK_SUCCESS;
        if (ok) vk_name((uint64_t)p->buf[k], VK_OBJECT_TYPE_BUFFER, "nvn memory pool");
    }
    if (!ok) { VKRED("cannot create the buffer of memory pool %p+%#llx", mem, (unsigned long long)size); pool_free(p); return; }
    if (p->sparse) { p->ngran = bsize / g_gran; p->gran = calloc(p->ngran, sizeof *p->gran); for (uint64_t g = 0; g < p->ngran; g++) p->gran[g].block = -1; pool_alias_all(p); }
    else {   // without sparse binding: all of it now
        for (int k = 0; k < p->nwin; k++) {
            VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, p->buf[k], &mr);
            if (vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &p->mem[k])) { VKRED("no device memory for memory pool %p+%#llx", mem, (unsigned long long)size); pool_free(p); return; }
            vkBindBufferMemory(g_vk.dev, p->buf[k], p->mem[k].mem, p->mem[k].off);
        }
        p->bound = p->size; atomic_fetch_add(&g_bound_total, p->size);
    }
    if (p->nwin > 1) VKLOG("memory pool %p+%#llx: %d buffers of up to %llu MB%s (the device's largest buffer is %llu MB)", mem, (unsigned long long)size, p->nwin,
                           (unsigned long long)(w >> 20), ov ? ", overlapping by the same memory" : ", side by side", (unsigned long long)(g_max_buffer >> 20));
    plat_rw_wrlock(&g_lk);
    if (g_npool < MAXPOOL) { gmem_map(p); g_pool[g_npool++] = p; qsort(g_pool, (size_t)g_npool, sizeof *g_pool, pool_cmp); p = NULL; }
    plat_rw_wrunlock(&g_lk);
    if (p) { VKRED("too many memory pools (%d)", MAXPOOL); pool_free(p); return; }
    atomic_fetch_add(&g_pools_made, 1); atomic_fetch_add(&g_pool_bytes_made, size);
    static int nlog; if (nlog++ < 32) VKLOG("memory pool %p+%#llx: device-local mirror%s", mem, (unsigned long long)size, g_vk.has.sparse_buffer ? ", backed in 2 MB granules on first use" : "");
}

void vkm_pool_del(const void *mem) {
    pool_t_ *p = NULL;
    plat_rw_wrlock(&g_lk);
    for (int i = 0; i < g_npool; i++) if (g_pool[i]->key == mem) { p = g_pool[i]; memmove(&g_pool[i], &g_pool[i + 1], (size_t)(g_npool - i - 1) * sizeof *g_pool); g_npool--; break; }
    if (p) gmem_unmap(p);
    plat_rw_wrunlock(&g_lk);
    if (p) vk_retire(pool_free_cb, p);   // submissions recorded so far may still read it
}

// The window of p holding [addr, addr+len): buffer and offset, or 0 when the range spans two windows that do not overlap that far (logged once)
static int pool_window(pool_t_ *p, uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off) {
    uint64_t o = addr - p->base; int k = win_for(p, o, len ? len : 1);
    if (k < 0) {
        static _Atomic int said; if (!atomic_exchange(&said, 1))
            VKRED("a range %#llx+%#llx spans two of memory pool %p's buffers (the device's largest buffer is %llu MB): it reads zeros (later ones are not logged)",
                  (unsigned long long)addr, (unsigned long long)len, p->key, (unsigned long long)(g_max_buffer >> 20));
        return 0;
    }
    if (buf) *buf = p->buf[k];
    if (off) *off = o - p->win_lo[k];
    return 1;
}

int vkm_addr(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off) {
    plat_rw_rdlock(&g_lk);
    pool_t_ *p = pool_find((uintptr_t)addr, len ? len : 1);
    int ok = p && pool_window(p, addr, len, buf, off);
    plat_rw_rdunlock(&g_lk);
    return ok;
}

static int zero_block(void) {
    if (g_zero.tried) return g_zero.ok;
    g_zero.tried = 1;
    const char *e = getenv("NXRT_VK_SPARSE_ALIAS"); if (!(e && atoi(e)) || !g_vk.has.sparse_aliased) return 0;   // default off, see above
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, g_gran, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (vkCreateBuffer(g_vk.dev, &bci, NULL, &g_zero.buf) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, g_zero.buf, &mr);
    VkMemoryRequirements gr = {g_gran, mr.alignment, mr.memoryTypeBits};
    if (vk_alloc_mem(&gr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &g_zero.mem)) { vkDestroyBuffer(g_vk.dev, g_zero.buf, NULL); return 0; }
    vkBindBufferMemory(g_vk.dev, g_zero.buf, g_zero.mem.mem, g_zero.mem.off);
    VkCommandBuffer cb = vk_once_begin(); vkCmdFillBuffer(cb, g_zero.buf, 0, g_gran, 0); vk_once_end(cb);
    g_zero.ok = 1; VKLOG("sparse pools: unbound granules alias one zeroed %llu KB block (memory %p offset %#llx, buffer alignment %llu)", (unsigned long long)(g_gran >> 10), (void *)g_zero.mem.mem, (unsigned long long)g_zero.mem.off, (unsigned long long)mr.alignment);
    return 1;
}
// Binds every granule of p to the zero block (they are replaced by real memory as pool_bind backs them)
static void pool_alias_all(pool_t_ *p) {
    if (!zero_block()) return;
    for (int w = 0; w < p->nwin; w++) {
        uint64_t gw = p->win_n[w] / g_gran;   // window starts are granule multiples
        for (uint64_t g0 = 0; g0 < gw; g0 += 256) {
            VkSparseMemoryBind binds[256]; uint32_t nb = 0;
            for (uint64_t g = g0; g < gw && nb < 256; g++) binds[nb++] = (VkSparseMemoryBind){g * g_gran, g_gran, g_zero.mem.mem, g_zero.mem.off, 0};
            VkSparseBufferMemoryBindInfo bi = {p->buf[w], nb, binds};
            VkBindSparseInfo si = {VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, NULL, 0, NULL, 1, &bi, 0, NULL, 0, NULL, 0, NULL};
            VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence f; vkCreateFence(g_vk.dev, &fci, NULL, &f);
            plat_rw_wrlock(&g_vk.qlock);
            VkResult r = vkQueueBindSparse(g_vk.q, 1, &si, f);
            plat_rw_wrunlock(&g_vk.qlock);
            if (r == VK_SUCCESS) vkWaitForFences(g_vk.dev, 1, &f, VK_TRUE, UINT64_MAX); else { VKRED("vkQueueBindSparse (alias) failed (%d)", r); vk_lost("an alias bind", r); }
            vkDestroyFence(g_vk.dev, f, NULL);
            if (r != VK_SUCCESS) return;
        }
    }
}

// Whether pages are readable (committed), asked of the OS: plat_mem_query probes page by page with a system call each (an uncommitted page one by one), which took 123 s in 140 s of the scene when
// shaders bound to multi-GB ranges asked about millions of uncommitted pages a frame. On Linux the answer comes from /proc/self/maps, read at most every 250 ms into a sorted list of readable
// ranges (a page committed since then reads as zeros for that long; pages seen committed are remembered and stay so)
#ifdef __linux__
static struct { uint64_t (*r)[2]; int n, cap; uint64_t t; pthread_mutex_t lk; int init; } g_mp;
static void maps_refresh(void) {
    FILE *f = fopen("/proc/self/maps", "r"); if (!f) return;
    int n = 0; char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned long a, b; char perm[8];
        if (sscanf(line, "%lx-%lx %7s", &a, &b, perm) != 3 || perm[0] != 'r') continue;
        if (n && g_mp.r[n - 1][1] == a) { g_mp.r[n - 1][1] = b; continue; }
        if (n == g_mp.cap) { g_mp.cap = g_mp.cap ? g_mp.cap * 2 : 4096; g_mp.r = realloc(g_mp.r, (size_t)g_mp.cap * sizeof *g_mp.r); }
        g_mp.r[n][0] = a; g_mp.r[n][1] = b; n++;
    }
    fclose(f); g_mp.n = n; g_mp.t = now_ns();
}
// Linux 6.11+: PROCMAP_QUERY asks the kernel for one VMA by address (binary search), where the list above is the whole of /proc/self/maps parsed (50 ms a refresh once the dirty tracker's protections have
// split the pools into tens of thousands of VMAs; 4 refreshes a second were 4 % of the scene). NXRT_VK_PROCMAP=0 keeps the list. Fresh answers instead of up to 250 ms old ones
struct procmap_query_ { uint64_t size, query_flags, query_addr, vma_start, vma_end, vma_flags, vma_page_size, vma_offset, inode; uint32_t dev_major, dev_minor, vma_name_size, build_id_size; uint64_t vma_name_addr, build_id_addr; };
#define PROCMAP_QUERY_ _IOWR('f', 17, struct procmap_query_)
static int procmap_q(uint64_t a, uint64_t flags, uint64_t *start, uint64_t *end, uint64_t *vflags) {   // 0 = a VMA (covering a, or the next one: flag 0x10), 1 = none, -1 = not supported
    static int fd = -2; static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER; static int on = -1;
    if (on < 0) { const char *e = getenv("NXRT_VK_PROCMAP"); on = !(e && *e && !atoi(e)); }
    if (!on) return -1;
    pthread_mutex_lock(&lk);
    if (fd == -2) fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    int r = -1;
    if (fd >= 0) {
        struct procmap_query_ q; memset(&q, 0, sizeof q); q.size = sizeof q; q.query_addr = a; q.query_flags = flags | 0x10;
        if (!ioctl(fd, PROCMAP_QUERY_, &q)) { *start = q.vma_start; *end = q.vma_end; *vflags = q.vma_flags; r = 0; }
        else if (errno == ENOENT) r = 1;
        else { close(fd); fd = -1; on = 0; }   // ENOTTY / EINVAL: an older kernel: the list
    }
    pthread_mutex_unlock(&lk);
    return r;
}
static int vkm_mem_query(const void *addr, uint64_t *end, size_t look) {
    uint64_t a = (uint64_t)(uintptr_t)addr & ~(uint64_t)(PAGE - 1), stop = a + (look ? look : PAGE);
    { uint64_t vs, ve, vf; int r = procmap_q(a, 0, &vs, &ve, &vf);
      if (r >= 0) {
        if (r == 0 && vs <= a && (vf & 1)) {   // readable: through the readable VMAs that follow without a gap
            uint64_t e = ve; int n = 0;
            while (e < stop && n++ < 4096) { uint64_t s2, e2, f2; if (procmap_q(e, 0, &s2, &e2, &f2) || s2 != e || !(f2 & 1)) break; e = e2; }
            if (end) *end = e < stop ? e : stop;
            return 1;
        }
        uint64_t s2, e2, f2;   // not readable: the gap lasts until the next readable VMA
        uint64_t e = procmap_q(a, 1, &s2, &e2, &f2) == 0 ? s2 : stop;
        if (end) *end = e < stop ? e : stop;
        return 0;
      } }
    if (!g_mp.init) { g_mp.init = 1; pthread_mutex_init(&g_mp.lk, NULL); }
    pthread_mutex_lock(&g_mp.lk);
    if (!g_mp.n || now_ns() - g_mp.t > 250000000ull) maps_refresh();
    int lo = 0, hi = g_mp.n - 1, hit = -1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (g_mp.r[m][0] <= a) { hit = m; lo = m + 1; } else hi = m - 1; }
    int readable = hit >= 0 && a < g_mp.r[hit][1]; uint64_t e;
    if (readable) e = g_mp.r[hit][1]; else e = hit + 1 < g_mp.n ? g_mp.r[hit + 1][0] : stop;
    pthread_mutex_unlock(&g_mp.lk);
    if (end) *end = e < stop ? e : stop;
    return readable;
}
#else
static int vkm_mem_query(const void *addr, uint64_t *end, size_t look) { return plat_mem_query(addr, end, look); }
#endif

// Sparse backing for [o, o+n) of the buffer: unbound granules get memory and are bound before anything is recorded against them (a bind is a queue
// operation; it is waited for here, which happens once per 2 MB granule)
static int pool_bind(pool_t_ *p, uint64_t o, uint64_t n) {
    if (!p->sparse) return 0;
    uint64_t g0 = o / g_gran, g1 = (o + n + g_gran - 1) / g_gran;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, p->buf[0], &mr);
    VkMemoryRequirements gr = {g_gran, mr.alignment, mr.memoryTypeBits};
    for (;;) {
        uint64_t fresh[32]; int nf = 0, rc = 0;   // granules backed in this round; each is bound into every window that covers it (two where windows overlap)
        for (uint64_t g = g0; g < g1 && g < p->ngran && nf < 32; g++) {
            if (p->gran[g].mem) continue;
            if (vk_alloc_mem(&gr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &p->gran[g])) { VKRED("no device memory to back pool %p at %#llx", p->key, (unsigned long long)(g * g_gran)); rc = -1; break; }
            fresh[nf++] = g;
            p->bound += g_gran; atomic_fetch_add(&g_bound_total, g_gran);
        }
        if (!nf) return rc;
        VkSparseMemoryBind binds[64]; VkSparseBufferMemoryBindInfo bis[MAXWIN]; int nb = 0, nbi = 0;
        for (int k = 0; k < p->nwin; k++) {
            int first = nb;
            for (int i = 0; i < nf; i++) {
                uint64_t at = fresh[i] * g_gran;
                if (at >= p->win_lo[k] && at < p->win_lo[k] + p->win_n[k] && nb < 64)
                    binds[nb++] = (VkSparseMemoryBind){at - p->win_lo[k], g_gran, p->gran[fresh[i]].mem, p->gran[fresh[i]].off, 0};
            }
            if (nb > first) bis[nbi++] = (VkSparseBufferMemoryBindInfo){p->buf[k], (uint32_t)(nb - first), binds + first};
        }
        VkBindSparseInfo si = {VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, NULL, 0, NULL, (uint32_t)nbi, bis, 0, NULL, 0, NULL, 0, NULL};
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence f; vkCreateFence(g_vk.dev, &fci, NULL, &f);
        plat_rw_wrlock(&g_vk.qlock);
        VkResult r = vkQueueBindSparse(g_vk.q, 1, &si, f);
        plat_rw_wrunlock(&g_vk.qlock);
        if (r == VK_SUCCESS) vkWaitForFences(g_vk.dev, 1, &f, VK_TRUE, UINT64_MAX); else { VKRED("vkQueueBindSparse failed (%d)", r); vk_lost("a sparse bind", r); rc = -1; }
        vkDestroyFence(g_vk.dev, f, NULL);
        if (rc) return rc;
    }
}

// Guest memory is read with a guard: a page that is not mapped any more (a pool the game freed without finalizing) reads as zeros instead of crashing
typedef struct { void *d; const void *s; size_t n; } copy_args;
static void copy_run(void *p) { copy_args *a = p; memcpy(a->d, a->s, a->n); }
static int guest_copy(void *dst, const void *src, size_t n) {
    copy_args a = {dst, src, n};
    if (!plat_guarded(copy_run, &a)) return 0;
    memset(dst, 0, n); return -1;
}

static void page_rewritten(pool_t_ *p, uint64_t pg) {   // a page copied before and written again: hot after HOT_STREAK frames in a row
    if (p->last_dirty[pg] == (uint32_t)g_frame) return;
    p->streak[pg] = p->last_dirty[pg] + 1 == (uint32_t)g_frame ? (uint8_t)(p->streak[pg] < 255 ? p->streak[pg] + 1 : 255) : 1;
    p->last_dirty[pg] = (uint32_t)g_frame;
    if (p->streak[pg] >= HOT_STREAK && !p->hot[pg]) { p->hot[pg] = 1; p->hot_n++; }
}
// Whether page pg's device copy is behind guest memory: never copied, hot, or written (a fault stamped it) since its copy; untracked pages always are
static uint64_t s_blk_lo, s_blk_hi;   // NXRT_VK_BLKWATCH=<hex addr>,<bytes>: log each such page in the range
static int s_blk_on; static _Atomic uint64_t s_blk_pages, s_blk_first; static uint64_t s_blk_last[16];   // NXRT_VK_FRAMESTATS: pages a sync stepped over because of a GPU write in flight while the CPU had newer data in them (the device copy is stale until the landing)
// ---------- Read-and-reset pages ----------
// A "read and reset" page is a small buffer the GPU accumulates into (atomics) that the game's CPU reads back once per frame and zeroes (OoT's auto-exposure reductions). Overlapped frames land their
// write-back ~2 frames late, and a GPU write in flight keeps the sync from uploading the page: the CPU's zero never reaches the device copy, which keeps accumulating (1x..3x, exposure blows up, one
// white frame). A page that is classified this way is treated as GPU-owned: only the words the CPU changed since the mirror had them go over (also while a write is in flight); a landing keeps the
// words the CPU changed and no sync has delivered yet as pending words for the device (the guest gets the GPU's bytes, the device the CPU's reset); the words learned as resets (the CPU wrote ZERO over a landed
// nonzero word) are sent again while the CPU writes the page, even when it wrote zero over zero.
// CLASSIFICATION (vk_rr_class.h) comes from the landings, not from blocked syncs: a small write-back (NXRT_VK_EXPO_MAXLEN bytes, default 16384) whose page the CPU then zeroes (nonzero landed -> zero) with no
// other CPU data in it, in NXRT_VK_EXPO_FRAMES landings (default 3). A buffer the CPU fills with data (streaming, dynamic) shows CPU-written nonzero words and never converts.
// NXRT_VK_EXPO_FIX=0 turns it all off, NXRT_VK_EXPO_PAGES=<n> (default 64) caps the converted pages, NXRT_VK_EXPO_RESEND=0 turns the unchanged re-sends off, NXRT_VK_EXPO_LOG=1 logs each conversion and the top pages
#include "vk_rr_class.h"
static pthread_mutex_t g_rr_mu = PTHREAD_MUTEX_INITIALIZER;
// p->rst[pg]: [0, 4096) values of the CPU words pending for the device, [4096, +128) their mask, [+128, +256) the mask of words learned as resets, then two u64 stamps: the last sync's and the last landing's
enum { RR_PEND = PAGE, RR_LEARN = PAGE + 128, RR_SSTAMP = PAGE + 256, RR_LSTAMP = PAGE + 264, RR_BLOCK = PAGE + 272 };
static _Atomic uint64_t s_rr_pages, s_rr_words, s_rr_pend, s_rr_landed, s_rr_cands, s_rr_resent, s_rr_first, s_rr_capped, s_rr_stream;   // converted pages, words sent, words a landing left pending, landings over converted pages, candidate pages, unchanged learned resets sent, first conversion frame, conversions refused by the cap, candidates that showed CPU-written data
static _Atomic int s_rr_any;
static int rr_cfg(const char *name, int def) { const char *e = getenv(name); return e && *e ? atoi(e) : def; }
static int rr_fix_on(void) { static int on = -1; if (on < 0) on = rr_cfg("NXRT_VK_EXPO_FIX", 1) != 0; return on; }
// The candidates: pages that took small write-backs, with the words the last landing left in them. Open addressing on the page address; a full table takes no new candidates
enum { RR_CAND_N = 2048 };
static struct { uint64_t page; rrc_cand *c; } g_cand[RR_CAND_N];
static unsigned g_ncand;
static rrc_cand *rr_cand_get(uint64_t page, int create) {   // caller holds g_rr_mu
    unsigned h = (unsigned)((page >> 12) * 0x9e3779b1u) & (RR_CAND_N - 1);
    for (unsigned n = 0; n < RR_CAND_N; n++, h = (h + 1) & (RR_CAND_N - 1)) {
        if (g_cand[h].page == page) return g_cand[h].c;
        if (!g_cand[h].page) {
            if (!create || g_ncand >= RR_CAND_N / 2) return NULL;
            rrc_cand *c = calloc(1, sizeof *c); if (!c) return NULL;
            g_cand[h].page = page; g_cand[h].c = c; g_ncand++; atomic_fetch_add(&s_rr_cands, 1); return c;
        }
    }
    return NULL;
}
static int rr_convert(pool_t_ *p, uint64_t pg, const rrc_cand *c) {   // caller holds g_rr_mu; 1 when the page is now a read-and-reset page
    static int cap = -1, log = -1; if (cap < 0) { cap = rr_cfg("NXRT_VK_EXPO_PAGES", 64); log = rr_cfg("NXRT_VK_EXPO_LOG", 0); }
    if (p->rr && p->rr[pg]) return 1;
    if ((int)atomic_load(&s_rr_pages) >= cap) { atomic_fetch_add(&s_rr_capped, 1); return 0; }
    if (!p->owned) { p->shadow = calloc(p->npages, sizeof *p->shadow); atomic_thread_fence(memory_order_release); p->owned = calloc(p->npages, 1); }
    if (!p->rst) p->rst = calloc(p->npages, sizeof *p->rst);
    if (!p->rr) { uint8_t *r = calloc(p->npages, 1); atomic_thread_fence(memory_order_release); p->rr = r; }
    if (!p->shadow || !p->owned || !p->rst || !p->rr) return 0;
    if (!p->shadow[pg]) {   // the CPU's current words are what the mirror is taken to have: the next upload sends what differs from them
        uint8_t *sh = malloc(PAGE); if (!sh) return 0;
        if (guest_copy(sh, (const void *)(p->base + pg * PAGE), PAGE)) { free(sh); return 0; }
        p->shadow[pg] = sh; atomic_fetch_add(&g_shadow_pages, 1);
    }
    if (!p->rst[pg]) p->rst[pg] = calloc(1, RR_BLOCK);
    if (!p->rst[pg]) return 0;
    memcpy(p->rst[pg] + RR_LEARN, c->reset, RRC_WORDS / 8);   // the words seen reset so far
    if (!p->owned[pg]) { p->owned[pg] = 1; atomic_fetch_add(&s_own_pages, 1); }
    p->rr[pg] = 1; if (!atomic_fetch_add(&s_rr_pages, 1)) atomic_store(&s_rr_first, g_frame); atomic_store(&s_rr_any, 1);
    if (log) VKLOG("expo-fix: frame %llu page %#llx is a read-and-reset page (%u reset words, %u good landings, %u with CPU data)", (unsigned long long)g_frame, (unsigned long long)(p->base + pg * PAGE), rrc_count(c->reset), c->good, c->bad);
    return 1;
}
// A landing over words of the pool (data is the landing's bytes, changed in place; kind 0 = a readback). Candidates are looked at first (the guest still holds the CPU's words); then, for read-and-reset pages: words the CPU
// changed and no sync has handed to the device stay pending for it (value kept in rst), and the shadow takes the landed bytes
static void rr_land(uint64_t addr, uint64_t len, uint8_t *data, int kind) {
    static int maxlen = -1; if (maxlen < 0) maxlen = rr_cfg("NXRT_VK_EXPO_MAXLEN", 16384);
    static int need = -1; if (need < 0) need = rr_cfg("NXRT_VK_EXPO_FRAMES", 3);
    if (!data || !rr_fix_on()) return;
    int cand = kind == 0 && len <= (uint64_t)maxlen;
    if (!cand && !atomic_load(&s_rr_any)) return;
    plat_rw_rdlock(&g_lk);
    pool_t_ *p = pool_find((uintptr_t)addr, len);
    if (p) for (uint64_t pg = (addr - p->base) / PAGE; pg <= (addr + len - 1 - p->base) / PAGE && pg < p->npages; pg++) {
        uint64_t pa = p->base + pg * PAGE, a0 = addr > pa ? addr : pa, a1 = addr + len < pa + PAGE ? addr + len : pa + PAGE;
        a0 = (a0 + 3) & ~3ull; a1 &= ~3ull;
        if (a1 <= a0) continue;
        pthread_mutex_lock(&g_rr_mu);
        if (!(p->rr && p->rr[pg]) && cand) {
            rrc_cand *c = rr_cand_get(pa, 1);
            if (c && !c->converted) {
                uint32_t guest[RRC_WORDS]; unsigned n = (unsigned)((a1 - a0) / 4);
                for (unsigned i = 0; i < n; i++) guest[i] = *(volatile uint32_t *)(uintptr_t)(a0 + 4 * i);
                uint32_t bad0 = c->bad;
                if (rrc_observe(c, (unsigned)((a0 - pa) / 4), n, guest, (const uint32_t *)(data + (a0 - addr)), (unsigned)need) && !rr_convert(p, pg, c)) c->converted = 0;   // not converted (cap): looked at again
                if (c->bad == 3 && bad0 == 2 && !c->good) atomic_fetch_add(&s_rr_stream, 1);
            }
        }
        if (p->rr && p->rr[pg]) {
            uint32_t *sh = (uint32_t *)p->shadow[pg];
            static int resend = -1; if (resend < 0) resend = rr_cfg("NXRT_VK_EXPO_RESEND", 1);
            uint8_t *lm = p->rst[pg] + RR_LEARN; int wrote = 0;
            if (resend) { uint64_t ss = *(uint64_t *)(p->rst[pg] + RR_SSTAMP), ls = *(uint64_t *)(p->rst[pg] + RR_LSTAMP), since = ss > ls ? ss : ls; wrote = p->hot[pg] || !since || nvn_dirty_since_span(p->span, (const void *)pa, PAGE, since); }   // the CPU wrote the page since the last sync and landing (possibly the same bytes again)
            for (uint64_t a = a0; a < a1; a += 4) {
                uint32_t *d = (uint32_t *)(data + (a - addr)); unsigned i = (unsigned)((a - pa) / 4);
                uint32_t gv = *(volatile uint32_t *)(uintptr_t)a;
                if (gv != sh[i] || (wrote && gv == 0 && (lm[i >> 3] & (1u << (i & 7))))) {   // the CPU wrote this word since the mirror had it, or a learned reset word (zero) in a page it wrote: the device gets it at the next sync
                    uint32_t *rv = (uint32_t *)p->rst[pg]; uint8_t *mk = p->rst[pg] + RR_PEND;
                    if (!(mk[i >> 3] & (1u << (i & 7)))) { mk[i >> 3] |= (uint8_t)(1u << (i & 7)); rv[i] = gv; atomic_fetch_add(&s_rr_pend, 1); }
                }
                sh[i] = *d;
            }
            atomic_fetch_add(&s_rr_landed, 1);
        }
        pthread_mutex_unlock(&g_rr_mu);
    }
    plat_rw_rdunlock(&g_lk);
}
// A landing over read-and-reset pages must apply in full (their resets are handled by rr_land / the sync; keeping the CPU's zero would hide the GPU's counts from it): the merged bytes there go back to the landing's
static void rr_restore(uint64_t addr, uint64_t len, uint8_t *merged, const uint8_t *raw) {
    if (!atomic_load(&s_rr_any)) return;
    plat_rw_rdlock(&g_lk);
    pool_t_ *p = pool_find((uintptr_t)addr, len);
    if (p && p->rr) for (uint64_t pg = (addr - p->base) / PAGE; pg <= (addr + len - 1 - p->base) / PAGE && pg < p->npages; pg++) {
        if (!p->rr[pg]) continue;
        uint64_t pa = p->base + pg * PAGE, lo = addr > pa ? addr : pa, hi = addr + len < pa + PAGE ? addr + len : pa + PAGE;
        memcpy(merged + (lo - addr), raw + (lo - addr), (size_t)(hi - lo));
    }
    plat_rw_rdunlock(&g_lk);
}
static void rr_landed(uint64_t addr, uint64_t len) {   // the landing has written the guest: later CPU writes are the ones newer than this stamp
    if (!atomic_load(&s_rr_any)) return;
    plat_rw_rdlock(&g_lk);
    pool_t_ *p = pool_find((uintptr_t)addr, len);
    if (p && p->rr) for (uint64_t pg = (addr - p->base) / PAGE; pg <= (addr + len - 1 - p->base) / PAGE && pg < p->npages; pg++) {
        if (!p->rr[pg]) continue;
        pthread_mutex_lock(&g_rr_mu);
        if (p->rst[pg]) *(uint64_t *)(p->rst[pg] + RR_LSTAMP) = nvn_dirty_now();
        pthread_mutex_unlock(&g_rr_mu);
    }
    plat_rw_rdunlock(&g_lk);
}
// NXRT_VK_EXPO_LOG=1: the converted pages with their reset-word counts (at most 16), at the every-300-frames summary
static void rr_top_log(void) {
    pthread_mutex_lock(&g_rr_mu); int shown = 0;
    for (unsigned h = 0; h < RR_CAND_N && shown < 16; h++) if (g_cand[h].page && g_cand[h].c->converted) { rrc_cand *c = g_cand[h].c; shown++;
        VKLOG("expo-fix:   page %#llx: %u reset words, %u good landings, %u with CPU data", (unsigned long long)g_cand[h].page, rrc_count(c->reset), c->good, c->bad); }
    pthread_mutex_unlock(&g_rr_mu);
}
static inline int page_needs(pool_t_ *p, uint64_t pg) {
    if (atomic_load(&p->gpu_pending[pg])) {   // a GPU write in flight: the device copy is ahead until its bytes are written back
        static int rr = -1; if (rr < 0) rr = rr_cfg("NXRT_VK_EXPO_FIX", 1);
        if (rr && p->rr && p->rr[pg]) goto normal;   // a read-and-reset page: only the CPU's changed words go over, so the write in flight is no reason to wait
        if (s_blk_on) { int cpu_newer = !p->up[pg] || (p->span && nvn_dirty_since_span(p->span, (const void *)(p->base + pg * PAGE), PAGE, p->up[pg]));   // diagnostics: the CPU has data in the page that the device copy lacks
                        if (cpu_newer && s_blk_lo) { uint64_t a = p->base + pg * PAGE; if (a >= s_blk_lo && a < s_blk_hi) VKLOG("blk: frame %llu page %#llx not uploaded: a GPU write is in flight (pending %u), up %llu, CPU newer", (unsigned long long)g_frame, (unsigned long long)a, (unsigned)atomic_load(&p->gpu_pending[pg]), (unsigned long long)p->up[pg]); }
                        if (cpu_newer) { uint64_t n = atomic_fetch_add(&s_blk_pages, 1); s_blk_last[n % 16] = p->base + pg * PAGE; if (!p->up[pg]) atomic_fetch_add(&s_blk_first, 1); } }
        return 0;
    }
normal:
    if (p->hot[pg] && p->up[pg] && p->hot_sub && p->hot_sub[pg] && p->hot_sub[pg] == vks_recording_value()) return 0;   // already copied for this submission
    if (!p->up[pg] || p->hot[pg] || !p->span) return 1;
    return nvn_dirty_since_span(p->span, (const void *)(p->base + pg * PAGE), PAGE, p->up[pg]);
}
// A clean group of 64 pages at once: one generation scan from the group's oldest copy
static int group_clean(pool_t_ *p, uint64_t g0) {
    if (!p->span || g0 + 64 > p->npages) return 0;
    uint64_t lo = UINT64_MAX;
    for (uint64_t k = g0; k < g0 + 64; k++) { if (!p->up[k] || p->hot[k] || atomic_load(&p->gpu_pending[k])) return 0; if (p->up[k] < lo) lo = p->up[k]; }
    return !nvn_dirty_since_span(p->span, (const void *)(p->base + g0 * PAGE), 64 * PAGE, lo);
}

typedef struct { VkBufferCopy r[256]; int n; VkBuffer src, dst; } copy_batch;
static void batch_flush(copy_batch *b, VkCommandBuffer cb) { if (b->n) vkCmdCopyBuffer(cb, b->src, b->dst, (uint32_t)b->n, b->r); b->n = 0; }
// [off, off+n) of the pool from src at so: one copy per window it crosses (windows that overlap share their memory, so one of them is enough)
static void batch_put(copy_batch *b, pool_t_ *p, VkCommandBuffer cb, VkBuffer src, VkDeviceSize so, uint64_t off, uint64_t n) {
    while (n) {
        int k = win_of(p, off); uint64_t end = p->win_lo[k] + p->win_n[k], m = off + n <= end ? n : end - off;
        if (b->n == 256 || (b->n && (b->src != src || b->dst != p->buf[k]))) batch_flush(b, cb);
        b->src = src; b->dst = p->buf[k]; b->r[b->n++] = (VkBufferCopy){so, off - p->win_lo[k], m};
        so += m; off += m; n -= m;
    }
}
static void pool_fill_zero(pool_t_ *p, VkCommandBuffer cb, uint64_t off, uint64_t n) {
    while (n) { int k = win_of(p, off); uint64_t end = p->win_lo[k] + p->win_n[k], m = off + n <= end ? n : end - off; vkCmdFillBuffer(cb, p->buf[k], off - p->win_lo[k], m, 0); off += m; n -= m; }
}

// Copies the pages among [p0, p1) of pool p whose device copy is behind into cb, until budget bytes; *stop_at (optional) is where it stopped.
// Order per run (nxnative's): take the stamp, write-protect, then read guest memory: a write after the protection faults and is seen at the next check
static __thread int s_only_dirty;   // set around pool_sync by vkm_gmem_commit_sync: only pages copied before whose device copy is now behind (CPU rewrites), never the first copies
#define PAGE_NEEDS(p, pg) (page_needs((p), (pg)) && (!s_only_dirty || (p)->up[(pg)]))
// NXRT_VK_WB_LOCK=1 (experiment): a write-back's landing (guest write, pending count, adoption) and a pool sync (the copy of guest pages into upload runs) exclude each other
static pthread_mutex_t g_wbl_mu = PTHREAD_MUTEX_INITIALIZER;
static int wb_lock_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("NXRT_VK_WB_LOCK"); on = e && atoi(e); } return on; }
static uint64_t pool_sync_(pool_t_ *p, uint64_t p0, uint64_t p1, VkCommandBuffer cb, uint64_t budget, uint64_t *stop_at);
static uint64_t pool_sync(pool_t_ *p, uint64_t p0, uint64_t p1, VkCommandBuffer cb, uint64_t budget, uint64_t *stop_at) {
    if (!wb_lock_on()) return pool_sync_(p, p0, p1, cb, budget, stop_at);
    pthread_mutex_lock(&g_wbl_mu); uint64_t r = pool_sync_(p, p0, p1, cb, budget, stop_at); pthread_mutex_unlock(&g_wbl_mu); return r;
}
static uint64_t pool_sync_(pool_t_ *p, uint64_t p0, uint64_t p1, VkCommandBuffer cb, uint64_t budget, uint64_t *stop_at) {
    if (!p->span) p->span = nvn_dirty_span((const void *)p->lo, p->hi - p->lo);
    if (p1 > p->npages) p1 = p->npages;
    copy_batch b; b.n = 0; b.src = b.dst = VK_NULL_HANDLE;
    uint64_t queued = 0, t0 = now_ns(), pg = p0;
    while (pg < p1 && queued < budget) {
        if (!(pg & 63) && pg + 64 <= p1 && group_clean(p, pg)) { pg += 64; continue; }
        if (!PAGE_NEEDS(p, pg)) { if (atomic_load(&p->gpu_pending[pg])) t_left = 1; pg++; continue; }
        uint64_t run0 = pg;
        while (pg < p1 && pg - run0 < RUN_MAX && PAGE_NEEDS(p, pg)) { if (p->up[pg] && !p->hot[pg]) page_rewritten(p, pg); pg++; }
        // Pages the game never touched are not committed (the heap commits lazily): nothing to read or protect there. A use reads them as zeros and
        // keeps asking (they stay "never copied"); the background sync steps over the whole uncommitted region. VirtualQuery walks the address space's
        // regions, which the tracker's protections split finely: in gameplay it was 70 % of the render thread's time. Pages seen committed are remembered
        // (a pool's memory stays committed while the pool exists) and only a run that starts on a page not known yet is asked about
        uint64_t known = run0; while (known < pg && p->committed[known]) known++;
        if (known > run0) pg = known;   // the known part now, the rest in the next run
        else {
            if (p->unc_until && p->unc_until[run0] > g_frame) {   // found uncommitted a moment ago and zero-filled: not asked again yet
                uint64_t e = run0; while (e < pg && p->unc_until[e] > g_frame) e++;
                pg = e; atomic_fetch_add(&s_unc_skipped, pg - run0); t_left = 1; continue;
            }
            uint64_t region_end = 0, tq = now_ns();
            int readable = vkm_mem_query((const void *)(p->base + run0 * PAGE), &region_end, (size_t)((p1 - run0) * PAGE));
            atomic_fetch_add(&s_t_query, now_ns() - tq);
            uint64_t rend = (region_end - p->base) / PAGE;
            if (rend < pg) pg = rend > run0 ? rend : run0 + 1;
            if (!readable) {
                if (budget == UINT64_MAX) { uint64_t tb = now_ns(); if (pool_bind(p, run0 * PAGE, (pg - run0) * PAGE)) break; atomic_fetch_add(&s_t_bind, now_ns() - tb); batch_flush(&b, cb); pool_fill_zero(p, cb, run0 * PAGE, (pg - run0) * PAGE);
                    if (!p->unc_until) p->unc_until = calloc(p->npages, sizeof *p->unc_until);
                    if (p->unc_until) for (uint64_t k = run0; k < pg; k++) p->unc_until[k] = g_frame + 8; }
                else if (rend > pg) pg = rend < p1 ? rend : p1;
                p->uncommitted_fill += (pg - run0) * PAGE; t_left = 1;
                atomic_fetch_add(&s_uncommitted, pg - run0);
                continue;
            }
            memset(p->committed + run0, 1, (size_t)(pg - run0));
        }
        uint64_t off = run0 * PAGE, n = (pg - run0) * PAGE;
        if (pool_bind(p, off, n)) break;
        uint64_t stamp = nvn_dirty_now(), tp = now_ns();
        for (uint64_t q0 = run0; q0 < pg; ) {   // protect the pages that are not hot
            if (p->hot[q0]) { q0++; continue; }
            uint64_t q1 = q0; while (q1 < pg && !p->hot[q1]) q1++;
            if (p->span) nvn_dirty_protect((const void *)(p->base + q0 * PAGE), (size_t)((q1 - q0) * PAGE));
            q0 = q1;
        }
        atomic_fetch_add(&s_t_prot, now_ns() - tp);
        VkBuffer sb; VkDeviceSize so;
        uint64_t tg = now_ns(); uint8_t *dst = vkm_staging(n, 16, &sb, &so); atomic_fetch_add(&s_t_bind, now_ns() - tg);
        if (!dst) { pg = run0; t_left = 1; break; }
        if (s_blk_lo && p->base + off < s_blk_hi && p->base + off + n > s_blk_lo) VKLOG("blk: frame %llu upload %#llx+%llu in submission %llu", (unsigned long long)g_frame, (unsigned long long)(p->base + off), (unsigned long long)n, (unsigned long long)vks_recording_value());
        { uint64_t tc = now_ns(); guest_copy(dst, (const void *)(p->base + off), (size_t)n); atomic_fetch_add(&s_t_copy, now_ns() - tc); }
        { uint64_t first = 0, hot = 0; for (uint64_t k = run0; k < pg; k++) { first += !p->up[k]; hot += p->hot[k]; }
          p->upb_first += first * PAGE; p->upb_hot += hot * PAGE; if (t_why == 1) p->upb_touch += n; else if (t_why == 2) p->upb_all += n; else p->upb_use += n; }
        for (uint64_t k = run0; k < pg; k++) p->up[k] = stamp;
        { uint64_t sub = vks_recording_value(); if (sub) { int any = 0; for (uint64_t k = run0; k < pg && !any; k++) any = p->hot[k]; if (any) { if (!p->hot_sub) p->hot_sub = calloc(p->npages, sizeof *p->hot_sub); if (p->hot_sub) for (uint64_t k = run0; k < pg; k++) p->hot_sub[k] = sub; } } }
        for (uint64_t a = run0; a < pg; ) {
            if (p->rr && p->rr[a] && p->shadow && p->shadow[a]) {   // a read-and-reset page: the words the CPU changed (and the ones a landing handed over, see rr_land) go over, nothing else
                uint8_t *pd = dst + (a - run0) * PAGE; uint32_t *g = (uint32_t *)pd; uint32_t cur[PAGE / 4]; static __thread uint8_t send[PAGE / 4];
                pthread_mutex_lock(&g_rr_mu);
                guest_copy(pd, (const void *)(p->base + a * PAGE), PAGE);   // the guest as it is now, under the lock a landing takes (the copy of the run may be older than a landing)
                memcpy(cur, pd, PAGE);
                uint32_t *sh = (uint32_t *)p->shadow[a];
                {   // words the CPU changed are learned as reset words; while the CPU writes the page (since the last sync and the last landing) every learned reset word is sent again with its value, even
                    // when it did not change: a read-and-reset that finds the same bytes again (the landing is late, it read its own zero) is still a reset in the game's order, and the device
                    // must not keep accumulating from the one before
                    static int resend = -1; if (resend < 0) resend = rr_cfg("NXRT_VK_EXPO_RESEND", 1);
                    uint8_t *blk = p->rst[a]; uint8_t *lm = blk + RR_LEARN; uint64_t *ss = (uint64_t *)(blk + RR_SSTAMP), *ls = (uint64_t *)(blk + RR_LSTAMP);
                    uint64_t since = *ss > *ls ? *ss : *ls;
                    int wrote = resend && (p->hot[a] || !since || nvn_dirty_since_span(p->span, (const void *)(p->base + a * PAGE), PAGE, since));
                    for (unsigned i = 0; i < PAGE / 4; i++) {
                        int ch = cur[i] != sh[i];
                        if (ch && cur[i] == 0) lm[i >> 3] |= (uint8_t)(1u << (i & 7));   // zero over a nonzero word: a reset
                        send[i] = (uint8_t)ch;
                        if (!ch && wrote && cur[i] == 0 && (lm[i >> 3] & (1u << (i & 7)))) { send[i] = 1; atomic_fetch_add(&s_rr_resent, 1); }
                    }
                    *ss = stamp;
                }
                if (p->rst && p->rst[a]) { const uint32_t *rv = (const uint32_t *)p->rst[a]; uint8_t *mk = p->rst[a] + PAGE; for (unsigned i = 0; i < PAGE / 4; i++) if (mk[i >> 3] & (1u << (i & 7))) { g[i] = rv[i]; send[i] = 1; } memset(mk, 0, 128); }
                for (unsigned i = 0; i < PAGE / 4; ) {
                    if (!send[i]) { i++; continue; }
                    unsigned j = i; while (j < PAGE / 4 && send[j]) j++;
                    batch_put(&b, p, cb, sb, so + (a - run0) * PAGE + i * 4, a * PAGE + i * 4, (j - i) * 4);
                    atomic_fetch_add(&s_rr_words, j - i); i = j;
                }
                memcpy(sh, cur, PAGE);
                pthread_mutex_unlock(&g_rr_mu);
                a++; continue;
            }
            if (p->owned && p->owned[a]) {   // a GPU-owned page: only the words the CPU changed since the mirror had it go over; the rest is the GPU's
                const uint32_t *g = (const uint32_t *)(dst + (a - run0) * PAGE);
                if (!p->shadow[a]) { p->shadow[a] = malloc(PAGE); atomic_fetch_add(&g_shadow_pages, 1); memcpy(p->shadow[a], g, PAGE); batch_put(&b, p, cb, sb, so + (a - run0) * PAGE, a * PAGE, PAGE); atomic_fetch_add(&s_own_full, 1); }
                else {
                    uint32_t *sh = (uint32_t *)p->shadow[a];
                    for (unsigned i = 0; i < PAGE / 4; ) {
                        if (g[i] == sh[i]) { i++; continue; }
                        unsigned j = i; while (j < PAGE / 4 && g[j] != sh[j]) j++;
                        batch_put(&b, p, cb, sb, so + (a - run0) * PAGE + i * 4, a * PAGE + i * 4, (j - i) * 4);
                        atomic_fetch_add(&s_own_words, j - i); i = j;
                    }
                    memcpy(sh, g, PAGE);
                }
                a++; continue;
            }
            uint64_t e = a + 1; while (e < pg && !(p->owned && p->owned[e])) e++;
            batch_put(&b, p, cb, sb, so + (a - run0) * PAGE, a * PAGE, (e - a) * PAGE);
            a = e;
        }
        queued += n;
        atomic_fetch_add(&s_up_pages, pg - run0); atomic_fetch_add(&s_up_runs, 1);
    }
    batch_flush(&b, cb);
    if (stop_at) *stop_at = pg;
    atomic_fetch_add(&s_up_bytes, queued); atomic_fetch_add(&s_up_ns, now_ns() - t0);
    return queued;
}

// A shader writes through the pointer ptr (untracked global memory): the pool's pages from there, to the end of the mapping and up to 64 MiB, are the GPU's from now on (Lichii f9acc60a,
// d854ff0d, 25567e08: never refilled from guest memory; Eglantine 575d0a5bfd: CPU writes applied in place). Over-marking read-only parts is harmless: nothing but CPU-changed words ever goes over
void vkm_gmem_own(uint64_t ptr) {
    if (!ptr || g_gt.off || !g_gt.map) return;
    static uint64_t cap_pages = ~0ull; if (cap_pages == ~0ull) { const char *e = getenv("NXRT_VK_OWN_MB"); cap_pages = ((e ? (uint64_t)atoi(e) : 256) << 20) / PAGE; }
    plat_rw_rdlock(&g_lk);
    pool_t_ *p = pool_find((uintptr_t)ptr, 1);
    if (p) {
        uint64_t pg0 = (ptr - p->base) / PAGE, pg1 = pg0 + ((64u << 20) / PAGE); if (pg1 > p->npages) pg1 = p->npages;
        if (!p->owned) { p->shadow = calloc(p->npages, sizeof *p->shadow); atomic_thread_fence(memory_order_release); p->owned = calloc(p->npages, 1); }
        for (uint64_t k = pg0; k < pg1; k++) {
            if (p->owned[k]) continue;
            if (atomic_load(&g_shadow_pages) >= cap_pages) { static int n; if (!n++) VKRED("GPU-owned pages: the shadow limit of %llu MB is reached (NXRT_VK_OWN_MB): further pages stay refilled from guest memory", (unsigned long long)(cap_pages * PAGE >> 20)); break; }
            if (p->up[k] && p->span && !page_needs(p, k)) {   // the mirror matches guest memory: its content is the shadow
                uint8_t *sh = malloc(PAGE);
                if (!guest_copy(sh, (const void *)(p->base + k * PAGE), PAGE) && !nvn_dirty_since_span(p->span, (const void *)(p->base + k * PAGE), PAGE, p->up[k])) { p->shadow[k] = sh; atomic_fetch_add(&g_shadow_pages, 1); }
                else { free(sh); p->up[k] = 0; p->clean_epoch = 0; }   // written meanwhile: copied afresh at the next use (the shadow comes from that copy)
            } else { p->up[k] = 0; p->clean_epoch = 0; }   // not copied yet (or untracked): the first copy takes the shadow
            p->owned[k] = 1; atomic_fetch_add(&s_own_pages, 1);
        }
    }
    plat_rw_rdunlock(&g_lk);
}

int vkm_use(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off) {
    plat_rw_rdlock(&g_lk);
    pool_t_ *p = pool_find((uintptr_t)addr, len ? len : 1);
    int ok = p && pool_window(p, addr, len, buf, off);
    plat_rw_rdunlock(&g_lk);
    if (!ok) return 0;
    VkCommandBuffer cb = vks_upload_cb();
    if (!cb) return 1;
    atomic_fetch_add(&s_sync, 1);
    pool_sync(p, (addr - p->base) / PAGE, (addr + (len ? len : 1) - p->base + PAGE - 1) / PAGE, cb, UINT64_MAX, NULL);
    return 1;
}

// A 64 KiB uniform bank that a native (Switch 2) program reads with indirect offsets: the dirty tracker can miss writes to it (Eglantine 0b2affc9 / 5c7da271: the
// tracker lost writes, tables next to the code read stale), so every use is also treated as written: its pages are copied again, once per frame and address
enum { NRECOPY = 256 };
static struct { uint64_t addr, frame; } g_recopy[NRECOPY]; static unsigned g_recopy_n; static uint64_t g_recopy_frame;
static plat_rwlock_t g_rcl = PLAT_RWLOCK_INIT;
void vkm_recopy_once(uint64_t addr, uint64_t len) {
    if (!addr || !len) return;
    plat_rw_wrlock(&g_rcl);
    if (g_recopy_frame != g_frame) { g_recopy_frame = g_frame; g_recopy_n = 0; }
    for (unsigned i = 0; i < g_recopy_n; i++) if (g_recopy[i].addr == addr) { plat_rw_wrunlock(&g_rcl); return; }
    int full = g_recopy_n >= NRECOPY;
    if (!full) { g_recopy[g_recopy_n].addr = addr; g_recopy[g_recopy_n++].frame = g_frame; }
    plat_rw_wrunlock(&g_rcl);
    if (full) return;
    plat_rw_rdlock(&g_lk);
    pool_t_ *p = pool_find((uintptr_t)addr, len);
    if (p) for (uint64_t pg = (addr - p->base) / PAGE; pg <= (addr + len - 1 - p->base) / PAGE && pg < p->npages; pg++) if (p->up[pg]) { p->up[pg] = 0; p->clean_epoch = 0; atomic_fetch_add(&s_recopy_pages, 1); }
    plat_rw_rdunlock(&g_lk);
}

// Every pool's pages behind guest memory, round robin from where the last call stopped, within the frame's upload budget and a scan limit
static uint64_t g_scan_pages = 262144;   // NXRT_VK_SCAN_PAGES: pages looked at per call (1 GB)
static int g_rr;
static uint64_t sync_all_within(uint64_t g_scan_pages);
uint64_t vkm_sync_all(void) { return sync_all_within(g_scan_pages); }
// A submission that went through the table: every pool, since the table does not say which ones the addresses it was asked for lie in (a page behind guest memory reads old bytes)
enum { GMEM_TOUCHES = 1024, GMEM_TOUCH_BYTES = 64u << 20 };
static uint64_t g_touched[GMEM_TOUCHES]; static unsigned g_ntouched; static _Atomic uint64_t s_gmem_touches;
void vkm_gmem_touch(uint64_t ptr) {   // on the replay's thread
    if (!ptr || g_gt.off || !g_gt.map) return;
    uint64_t page = ptr & ~(uint64_t)(PAGE - 1);
    for (unsigned i = g_ntouched; i-- > 0;) if (g_touched[i] == page) return;
    if (g_ntouched == GMEM_TOUCHES) { atomic_store(&g_gmem_used_now, 1); return; }   // more pointers than the list holds in one submission: every pool instead
    g_touched[g_ntouched++] = page;
    plat_rw_rdlock(&g_lk);
    pool_t_ *p = pool_find((uintptr_t)ptr, 1);
    uint64_t n = p ? p->hi - ptr : 0;
    plat_rw_rdunlock(&g_lk);
    if (!n) return;
    atomic_fetch_add(&s_gmem_touches, 1);
    t_why = 1; vkm_use(ptr, n < GMEM_TOUCH_BYTES ? n : GMEM_TOUCH_BYTES, NULL, NULL); t_why = 0;
}
uint64_t vkm_gmem_commit_sync(void) {
    g_ntouched = 0;
    if (!atomic_exchange(&g_gmem_used_now, 0)) return 0;
    atomic_fetch_add(&s_gmem_syncs, 1);
    uint64_t total = sync_all_within(UINT64_MAX >> 1);
    // A program that reads and writes through addresses nobody traced (the culling producer 1a29cd3b: its lists and the dispatch arguments it fills are CPU-written each frame, pages
    // the CPU rewrites every frame) must see this frame's CPU writes whatever the frame's upload budget has left: the pages copied before and rewritten since go over now, unbudgeted
    // (they are the few hot pages, not first copies). With the budget gone they used to be copied by the first vkm_use of a later submission, over what the GPU had written meanwhile.
    static int dirty_all = -1; if (dirty_all < 0) { const char *e = getenv("NXRT_VK_GMEM_DIRTY"); dirty_all = e && atoi(e); }   // off by default: measured 50 GB in 126 s (the hot pages go every submission)
    if (dirty_all) {
        VkCommandBuffer cb = vks_upload_cb();
        if (cb) {
            uint64_t n = 0; t_why = 2; s_only_dirty = 1;
            plat_rw_rdlock(&g_lk);
            for (int i = 0; i < g_npool; i++) { pool_t_ *p = g_pool[i]; n += pool_sync(p, 0, p->npages, cb, UINT64_MAX, NULL); }
            plat_rw_rdunlock(&g_lk); s_only_dirty = 0; t_why = 0;
            atomic_fetch_add(&s_gmem_dirty_bytes, n); total += n;
        }
    }
    return total;
}
// NXRT_VK_SYNC_SKIP (default 1; 0 off, 2 = check): sync_all_within (the background prefetch, and the sync after a submission that went through the global memory table) walked every page of every
// pool (5 GB, 1.4 M pages) each time to find the few the CPU had written. A pool whose tracker range has had no page stamped since a pass that copied everything and stepped over nothing is skipped
// (the tracker's write count, nvn_dirty_span_epoch). Pools with hot pages, uncommitted pages or a GPU write in flight are not. 2 also counts, in each skipped pool, the pages copied before that are
// behind guest memory now (there must be none)
static _Atomic uint64_t s_sync_skipped, s_sync_skip_bad;
static int sync_skip_mode(void) { static int m = -1; if (m < 0) { const char *e = getenv("NXRT_VK_SYNC_SKIP"); m = e ? atoi(e) : 1; } return m; }
static void sync_skip_check(pool_t_ *p) {
    uint64_t bad = 0, first = 0;
    for (uint64_t pg = 0; pg < p->npages; pg++) if (p->up[pg] && !p->hot[pg] && !(p->owned && p->owned[pg]) && !atomic_load(&p->gpu_pending[pg]) && nvn_dirty_since_span(p->span, (const void *)(p->base + pg * PAGE), PAGE, p->up[pg])) { if (!bad++) first = pg; }
    if (bad && atomic_fetch_add(&s_sync_skip_bad, 1) < 20) VKLOG("[RED] sync skip: pool %p+%#llx was skipped as clean, but %llu pages copied before are behind guest memory (first page %llu)", (void *)p->lo, (unsigned long long)(p->hi - p->lo), (unsigned long long)bad, (unsigned long long)first);
}
static uint64_t sync_all_within(uint64_t g_scan_pages) {
    VkCommandBuffer cb = vks_upload_cb();
    if (!cb) return 0;
    uint64_t total = 0, scanned = 0; t_why = 2;
    plat_rw_rdlock(&g_lk);
    for (int k = 0; k < g_npool && vkm_budget_left() && scanned < g_scan_pages; k++) {
        int i = (g_rr + k) % g_npool;
        pool_t_ *p = g_pool[i];
        uint64_t ep = sync_skip_mode() && p->span ? nvn_dirty_span_epoch(p->span) : 0;
        if (ep && ep == p->clean_epoch && !p->cursor && !p->hot_n) {   // nothing in the pool was written since a pass found it all copied: no scan
            if (sync_skip_mode() == 2) sync_skip_check(p);
            atomic_fetch_add(&s_sync_skipped, 1); g_rr = (i + 1) % g_npool; continue;
        }
        uint64_t from = p->cursor < p->npages ? p->cursor : 0, to = from + (g_scan_pages - scanned), stop = from;
        if (!from) { p->pass_ep = ep; p->pass_left = 0; }
        t_left = 0;
        uint64_t n = pool_sync(p, from, to, cb, vkm_budget_left(), &stop);
        p->pass_left |= t_left;
        vkm_budget_take(n); total += n; scanned += stop - from;
        p->cursor = stop >= p->npages ? 0 : stop;
        if (stop >= p->npages && p->pass_ep && !p->pass_left && !p->hot_n) p->clean_epoch = p->pass_ep;
        if (p->cursor) { g_rr = i; break; }   // continue in this pool next time
        g_rr = (i + 1) % g_npool;
    }
    plat_rw_rdunlock(&g_lk); t_why = 0;
    atomic_fetch_add(&s_sync, 1);
    return total;
}

// GPU writes in flight over [a, a+len): +1 when recorded, -1 when their bytes have reached guest memory. The pages are copied again from guest memory after
// that (the write back stamps them), which also brings in anything the CPU wrote beside the GPU's bytes
static void gpu_pending(uint64_t a, uint64_t len, int d) {
    plat_rw_rdlock(&g_lk);
    pool_t_ *p = pool_find((uintptr_t)a, len);
    if (p) for (uint64_t pg = (a - p->base) / PAGE; pg <= (a + len - 1 - p->base) / PAGE && pg < p->npages; pg++) atomic_fetch_add(&p->gpu_pending[pg], (uint32_t)d);
    plat_rw_rdunlock(&g_lk);
}

// ---------- GPU writes back to guest memory ----------
typedef struct wb_s { uint64_t addr, len; VkBuffer buf; vk_alloc mem; const uint8_t *cpu; uint32_t fill; int kind; uint64_t stamp, seq; char key[17];
                      uint8_t *g0; struct wb_s *next, *prev; } wb_t;   // kind 0 readback, 1 known bytes, 2 fill; g0 = the guest's bytes at record time (small ranges), the list = records not landed yet, in order
#include "vk_wb_merge.h"
void vkp_order_stats(uint64_t *n, uint64_t *back, uint64_t *same_val, uint64_t *same_tex);
// NXRT_VK_WB_MERGE (default 1; 0 = off, the old landing that puts its bytes over everything): a landing keeps the words the CPU wrote after the write-back was recorded (see vk_wb_merge.h). The guest's bytes at record
// time are kept for ranges up to NXRT_VK_WB_KEEP_MAX bytes (default 65536); bigger ones only count the pages they overwrote that the CPU had written since (the tracker's generation)
static pthread_mutex_t g_wbp_mu = PTHREAD_MUTEX_INITIALIZER; static wb_t *g_wbp_tail;
static _Atomic uint64_t s_wm_lands, s_wm_kept_lands, s_wm_kept_words, s_wm_big_kept, s_wm_big_edge, s_wm_total;   // landings, landings that kept CPU words, words kept, pages of bigger ranges overwritten though the CPU wrote them since the record
static int wb_merge_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("NXRT_VK_WB_MERGE"); on = !(e && *e && !atoi(e)); } return on; }
static uint64_t wb_keep_max(void) { static uint64_t v = ~0ull; if (v == ~0ull) { const char *e = getenv("NXRT_VK_WB_KEEP_MAX"); v = e ? strtoull(e, NULL, 0) : 65536; } return v; }
static void wb_keep(wb_t *w) {   // at record time: remember the guest's bytes, queue the record
    if (wb_merge_on() && w->len <= wb_keep_max() && w->len) { w->g0 = malloc((size_t)w->len); if (w->g0 && guest_copy(w->g0, (const void *)(uintptr_t)w->addr, (size_t)w->len)) { free(w->g0); w->g0 = NULL; } }
    pthread_mutex_lock(&g_wbp_mu);
    w->prev = g_wbp_tail; w->next = NULL; if (g_wbp_tail) g_wbp_tail->next = w; g_wbp_tail = w;
    pthread_mutex_unlock(&g_wbp_mu);
}
static void wb_unkeep(wb_t *w, const uint8_t *landed) {   // at landing: leave the queue; the records recorded after this one see the bytes it put in the guest
    pthread_mutex_lock(&g_wbp_mu);
    if (w->prev) w->prev->next = w->next;
    if (w->next) w->next->prev = w->prev; else g_wbp_tail = w->prev;
    if (landed) for (wb_t *r = w->next; r; r = r->next) if (r->g0) wbm_patch(r->g0, r->addr, r->len, w->addr, w->len, landed);
    pthread_mutex_unlock(&g_wbp_mu);
}
// NXRT_VK_NANFIX=<hex address>[,<hex address>...] (an experiment): a write-back that starts at one of those addresses has its NaN words replaced by the last finite value that word had (0 before
// one): "what the CPU would read if the shader never summed a NaN". Only for ranges up to 4160 bytes
static int nanfix_hit(uint64_t addr, uint64_t len, int *slot) {
    static uint64_t list[8]; static int n = -1;
    if (n < 0) { n = 0; const char *e = getenv("NXRT_VK_NANFIX"); while (e && *e && n < 8) { char *end; list[n] = strtoull(e, &end, 16); if (list[n]) n++; e = *end == ',' ? end + 1 : NULL; } }
    if (len > 4160) return 0;
    for (int i = 0; i < n; i++) if (list[i] == addr) { *slot = i; return 1; }
    return 0;
}
static void nanfix(wb_t *w) {
    static float last[8][1040]; static uint64_t fixed[8]; int slot;
    if (!nanfix_hit(w->addr, w->len, &slot)) return;
    float *f = (float *)w->mem.map; uint64_t nw = w->len / 4;
    for (uint64_t k = 0; k < nw && k < 1040; k++) { if (f[k] != f[k]) { f[k] = last[slot][k]; fixed[slot]++; } else last[slot][k] = f[k]; }
    if (!(fixed[slot] & (fixed[slot] - 1)) && fixed[slot]) VKLOG("nanfix: %llu NaN words replaced so far in the write-backs at %#llx", (unsigned long long)fixed[slot], (unsigned long long)w->addr);
}
// NXRT_VK_WB_ADOPT (default 1): a write-back that lands on whole pages makes guest memory equal to the mirror there, so the pages need no copy back to the device. The write itself stamped them as
// written by the CPU (dm_guest_write_prepare), and every use after a compute pass re-uploaded them (measured: 63 of the 70 MB/frame of upload in the forest were the pages the GPU had just written).
// Per page fully inside the range, with no GPU write in flight, not hot, not GPU-owned and copied before: take the tracker's stamp, write-protect the page again, then compare it with the bytes that
// landed (a CPU write before the protection shows up there, one after it faults and gets a newer generation); equal: the device copy is current as of that stamp
static _Atomic uint64_t s_wb_adopted, s_wb_adopt_miss, s_wb_same;   // pages adopted, pages the CPU had changed, bytes of write-back that already matched guest memory (not written)
static int wb_adopt_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("NXRT_VK_WB_ADOPT"); on = !(e && *e && !atoi(e)); } return on; }
// NXRT_VK_WB_DIFF (default 1, with WB_ADOPT): the bytes of a write-back that guest memory already holds are not written: most of what a compute pass writes back each frame is what it wrote back the frame before, and writing it
// meant un-protecting its pages (a system call and a new generation each) and copying them. Only the page-sized chunks that differ are written; the rest of the range is read, not written
// NXRT_VK_WB_RACE=1 (diagnostics): at landing, pages the write-back is about to change that the CPU wrote after the write-back was recorded (their generation is newer than the stamp taken at record time): the
// landing would put the device's older bytes over the CPU's newer ones. Counted and the first ones logged with both values (writes by other landings count too: the values tell them apart)
static _Atomic uint64_t s_wr_pages, s_wr_chunks;
static int wb_race_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("NXRT_VK_WB_RACE"); on = e && atoi(e); } return on; }
static void wb_race(uint64_t addr, uint64_t len, const uint8_t *src, uint64_t stamp, int kind) {
    if (!wb_race_on() || !stamp) return;
    for (uint64_t o = 0; o < len; ) {
        uint64_t pe = ((addr + o) | (PAGE - 1)) + 1 - addr; if (pe > len) pe = len;
        const uint8_t *g = (const uint8_t *)(uintptr_t)(addr + o);
        if (memcmp(g, src + o, (size_t)(pe - o)) && nvn_dirty_since((const void *)g, 1, stamp)) {
            uint64_t n = atomic_fetch_add(&s_wr_pages, 1);
            if (n < 60 || !(n % 1000)) { uint64_t d0 = 0; while (d0 < pe - o && g[d0] == src[o + d0]) d0++; uint64_t gv = 0, sv = 0; memcpy(&gv, g + (d0 & ~7ull), 8); memcpy(&sv, src + o + (d0 & ~7ull), 8);
                VKLOG("wb race #%llu: kind %d range %#llx+%llu lands over page %#llx that the CPU wrote since it was recorded; first difference at +%llu: guest %016llx, device %016llx", (unsigned long long)n, kind, (unsigned long long)addr, (unsigned long long)len, (unsigned long long)(addr + o) & ~(uint64_t)(PAGE - 1), (unsigned long long)d0, (unsigned long long)gv, (unsigned long long)sv); }
        }
        o = pe;
    }
}
// NXRT_VK_WB_PAGEKEEP (default 1; 0 = a landing writes every page that differs, as before): pages of a BIG write-back (no g0) that the CPU wrote since the record are not overwritten (wbm_page_action). Hot pages count as written (the
// tracker does not see them); GPU-owned and read-and-reset pages are written as ever. NXRT_VK_WB_COUNT=1 also counts, without changing anything, the pages the no-adoption path overwrites though the CPU wrote them (it
// otherwise compares nothing)
static int wb_pagekeep_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("NXRT_VK_WB_PAGEKEEP"); on = !(e && *e && !atoi(e)); } return on; }
static int wb_count_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("NXRT_VK_WB_COUNT"); on = e && atoi(e); } return on; }
static pthread_mutex_t g_lst_mu = PTHREAD_MUTEX_INITIALIZER;
static int wb_cpu_newer(pool_t_ *p, uint64_t pa, uint64_t stamp) {   // caller holds g_lk for reading when p != NULL
    uint64_t since = stamp;
    if (p && pa >= p->base && pa < p->base + p->size) {
        uint64_t pg = (pa - p->base) / PAGE;
        if ((p->owned && p->owned[pg]) || (p->rr && p->rr[pg])) return 0;
        if (p->hot[pg]) return 1;
        if (p->lst) since = wbm_since(stamp, p->lst[pg]);
    }
    return nvn_dirty_since((const void *)(uintptr_t)pa, 1, since);
}
static void wb_note_landing(pool_t_ *p, uint64_t addr, uint64_t len) {   // the pages [addr, addr+len) were just written by a landing
    if (!p || addr < p->base || addr + len > p->base + p->size) return;
    if (!p->lst) { pthread_mutex_lock(&g_lst_mu); if (!p->lst) { uint64_t *l = calloc(p->npages, sizeof *l); atomic_thread_fence(memory_order_release); p->lst = l; } pthread_mutex_unlock(&g_lst_mu); if (!p->lst) return; }
    uint64_t ls = nvn_dirty_now();
    for (uint64_t pg = (addr - p->base) / PAGE; pg <= (addr + len - 1 - p->base) / PAGE && pg < p->npages; pg++) p->lst[pg] = ls;
}
static void wb_apply(uint64_t addr, uint64_t len, const uint8_t *src, uint64_t stamp, int big) {
    static int on = -1; if (on < 0) { const char *e = getenv("NXRT_VK_WB_DIFF"); on = !(e && *e && !atoi(e)); }
    uint8_t *d = (uint8_t *)(uintptr_t)addr;
    int pk = big && stamp && wb_pagekeep_on();
    if (!on || !wb_adopt_on()) {
        if (big && stamp && wb_count_on()) {   // count only: the differing pages a CPU write is overwritten on
            plat_rw_rdlock(&g_lk); pool_t_ *p = pool_find((uintptr_t)addr, len);
            for (uint64_t o = 0; o < len; ) { uint64_t pe = ((addr + o) | (PAGE - 1)) + 1 - addr; if (pe > len) pe = len; if (memcmp(d + o, src + o, (size_t)(pe - o)) && wb_cpu_newer(p, (addr + o) & ~(uint64_t)(PAGE - 1), stamp)) atomic_fetch_add(&s_wm_big_edge, 1); o = pe; }
            plat_rw_rdunlock(&g_lk);
        }
        dm_guest_write_prepare(d, (size_t)len); memcpy(d, src, (size_t)len); return;
    }
    plat_rw_rdlock(&g_lk);
    pool_t_ *p = pk ? pool_find((uintptr_t)addr, len) : NULL;
    uint64_t o = 0, same = 0, run0 = ~0ull;
#define WB_FLUSH() do { if (run0 != ~0ull) { dm_guest_write_prepare(d + run0, (size_t)(o - run0)); memcpy(d + run0, src + run0, (size_t)(o - run0)); if (pk) wb_note_landing(p, addr + run0, o - run0); run0 = ~0ull; } } while (0)
    while (o < len) {
        uint64_t pe = ((addr + o) | (PAGE - 1)) + 1 - addr; if (pe > len) pe = len;
        if (!memcmp(d + o, src + o, (size_t)(pe - o))) { WB_FLUSH(); same += pe - o; o = pe; continue; }
        uint64_t pa = (addr + o) & ~(uint64_t)(PAGE - 1); int full = pa >= addr && pa + PAGE <= addr + len;
        int act = pk ? wbm_page_action(1, full, wb_cpu_newer(p, pa, stamp)) : WBP_WRITE;
        if (act == WBP_KEEP) { WB_FLUSH(); atomic_fetch_add(&s_wm_big_kept, 1); o = pe; continue; }   // the guest's page is newer than the device's: it stays, and stays unadopted (it differs), so the next sync uploads it
        if (act == WBP_EDGE_DIRTY) atomic_fetch_add(&s_wm_big_edge, 1);
        if (run0 == ~0ull) run0 = o;
        o = pe;
    }
    WB_FLUSH();
#undef WB_FLUSH
    plat_rw_rdunlock(&g_lk);
    atomic_fetch_add(&s_wb_same, same);
}
// NXRT_VK_WB_VERIFY=1 (diagnostics): a ring of the pages adopted; every 15 frames wb_verify reads some of them back from the device and compares them with guest memory (a page the CPU has not written
// since, no GPU write in flight): a difference is a device copy that was taken for current and is not
static struct { uint64_t addr[2048]; unsigned n; pthread_mutex_t lk; } g_av = {.lk = PTHREAD_MUTEX_INITIALIZER};
static _Atomic uint64_t s_av_checked, s_av_bad;
static int wb_verify_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("NXRT_VK_WB_VERIFY"); on = e ? atoi(e) : 0; } return on; }
static void wb_verify(void) {
    if (!wb_verify_on()) return;
    uint64_t pick[256]; int np = 0, want = wb_verify_on() > 1 ? (wb_verify_on() < 256 ? wb_verify_on() : 256) : 16;   // NXRT_VK_WB_VERIFY=<n> > 1: n pages a check (up to 256), every 3 frames
    pthread_mutex_lock(&g_av.lk);
    for (int i = 0; i < want && g_av.n; i++) { unsigned k = (unsigned)rand() % (g_av.n < 2048 ? g_av.n : 2048); pick[np++] = g_av.addr[k]; }
    g_av.n = 0;
    pthread_mutex_unlock(&g_av.lk);
    for (int i = 0; i < np; i++) {
        uint64_t a = pick[i]; uint8_t mir[PAGE];
        plat_rw_rdlock(&g_lk); pool_t_ *p = pool_find((uintptr_t)a, PAGE); uint64_t pg = p ? (a - p->base) / PAGE : 0;
        int ok = p && p->span && p->up[pg] && !p->hot[pg] && !(p->owned && p->owned[pg]) && !atomic_load(&p->gpu_pending[pg]) && !nvn_dirty_since_span(p->span, (const void *)a, PAGE, p->up[pg]);
        plat_rw_rdunlock(&g_lk);
        if (!ok || vkm_peek(a, mir, PAGE)) continue;
        plat_rw_rdlock(&g_lk); p = pool_find((uintptr_t)a, PAGE);
        ok = p && !atomic_load(&p->gpu_pending[pg]) && !nvn_dirty_since_span(p->span, (const void *)a, PAGE, p->up[pg]);
        plat_rw_rdunlock(&g_lk);
        if (!ok) continue;
        atomic_fetch_add(&s_av_checked, 1);
        if (memcmp(mir, (const void *)(uintptr_t)a, PAGE)) {
            unsigned nd = 0, first = PAGE; for (unsigned k = 0; k < PAGE; k++) if (mir[k] != ((const uint8_t *)(uintptr_t)a)[k]) { nd++; if (first == PAGE) first = k; }
            if (atomic_fetch_add(&s_av_bad, 1) < 20) VKLOG("[RED] wb verify: adopted page %#llx: the device copy differs from guest memory in %u bytes (first at +%u: device %02x guest %02x)", (unsigned long long)a, nd, first, mir[first], ((const uint8_t *)(uintptr_t)a)[first]);
        }
    }
}
// NXRT_VK_ADOPT_SKIP=<key prefix>[,...] / NXRT_VK_ADOPT_ONLY=<key prefix>[,...] (diagnostics): the write-backs of those shaders are (not) adopted. The per-shader adoption totals are logged with the adoption line when either is set or NXRT_VK_FRAMESTATS=1
static struct { char key[17]; uint64_t pages, lands; } g_ak[96]; static int g_nak; static pthread_mutex_t g_ak_mu = PTHREAD_MUTEX_INITIALIZER;
static int adopt_key_ok(const char *key) {
    static const char *skip, *only; static int init;
    if (!init) { init = 1; skip = getenv("NXRT_VK_ADOPT_SKIP"); only = getenv("NXRT_VK_ADOPT_ONLY"); }
    if (!skip && !only) return 1;
    #define LISTED(l) ({ int hit = 0; if (l) { const char *q = (l); while (*q) { size_t n = strcspn(q, ","); if (n && !strncmp(key, q, n)) hit = 1; q += n + (q[n] ? 1 : 0); } } hit; })
    if (only) return LISTED(only);
    return !LISTED(skip);
}
static void adopt_key_note(const char *key, uint64_t pages) {
    pthread_mutex_lock(&g_ak_mu); int i; for (i = 0; i < g_nak && strcmp(g_ak[i].key, key); i++) ;
    if (i == g_nak && g_nak < 96) { snprintf(g_ak[g_nak].key, 17, "%s", key); g_nak++; }
    if (i < g_nak) { g_ak[i].pages += pages; g_ak[i].lands++; }
    pthread_mutex_unlock(&g_ak_mu);
}
// NXRT_VK_ADOPT_STRICT (default 1; 0 = the old adoption): the pages a landing wrote (the dirty ones) are NOT marked current: they stay behind guest memory and the next sync copies them to the device, as the no-adoption
// path does for every page of a write-back (which is the path that shows no stale frames). The diff writes stay (pages whose bytes already matched are neither written nor copied): only the few pages a landing changed are
// uploaded again (~80 a frame in the forest), not the whole range. The adoption marked a page current from the landing's bytes alone, a claim the device's copy never had to back; NXRT_VK_WB_VERIFY=256 with STRICT=0 checks it
static _Atomic uint64_t s_wb_adopt_strict;   // pages left for re-upload by the strict mode
static int adopt_strict(void) { static int on = -1; if (on < 0) { const char *e = getenv("NXRT_VK_ADOPT_STRICT"); on = !(e && *e && !atoi(e)); } return on; }
static void wb_adopt(uint64_t addr, uint64_t len, const uint8_t *data, const char *key) {
    if (!wb_adopt_on() || !data || !adopt_key_ok(key)) return;
    uint64_t adopted0 = atomic_load(&s_wb_adopted);
    plat_rw_rdlock(&g_lk);
    pool_t_ *p = pool_find((uintptr_t)addr, len);
    if (p && p->span && addr >= p->base) {
        uint64_t pg0 = (addr - p->base + PAGE - 1) / PAGE, pg1 = (addr + len - p->base) / PAGE;
        if (pg1 > p->npages) pg1 = p->npages;
        for (uint64_t pg = pg0; pg < pg1; ) {
            uint64_t run0 = pg;
            while (pg < pg1 && p->up[pg] && !p->hot[pg] && !(p->owned && p->owned[pg]) && !atomic_load(&p->gpu_pending[pg])) pg++;
            if (pg == run0) { pg++; continue; }
            for (uint64_t k = run0; k < pg; ) {   // the pages written (or otherwise behind) since their copy; a page whose bytes matched and is still protected is current as it is
                if (!nvn_dirty_since_span(p->span, (const void *)(p->base + k * PAGE), PAGE, p->up[k])) { k++; continue; }
                uint64_t k0 = k; while (k < pg && nvn_dirty_since_span(p->span, (const void *)(p->base + k * PAGE), PAGE, p->up[k])) k++;
                if (adopt_strict()) { atomic_fetch_add(&s_wb_adopt_strict, k - k0); continue; }   // left dirty: the next sync copies them from guest memory
                uint64_t stamp = nvn_dirty_now();
                static int mode = -1; if (mode < 0) { const char *e = getenv("NXRT_VK_ADOPT_MODE"); mode = e ? atoi(e) : 0; }   // experiments: 1 = protect and compare but never mark current, 2 = mark current without protecting
                if (mode != 2) nvn_dirty_protect((const void *)(p->base + k0 * PAGE), (size_t)((k - k0) * PAGE));
                for (uint64_t q = k0; q < k; q++) {
                    const void *gp = (const void *)(p->base + q * PAGE), *dp = data + (p->base + q * PAGE - addr);
                    if (!memcmp(gp, dp, PAGE) && !atomic_load(&p->gpu_pending[q])) { if (mode != 1) p->up[q] = stamp; atomic_fetch_add(&s_wb_adopted, 1);
                        if (wb_verify_on()) { pthread_mutex_lock(&g_av.lk); if (g_av.n < 2048) g_av.addr[g_av.n++] = (uint64_t)(uintptr_t)gp; pthread_mutex_unlock(&g_av.lk); } }
                    else atomic_fetch_add(&s_wb_adopt_miss, 1);
                }
            }
        }
    }
    plat_rw_rdunlock(&g_lk);
    adopt_key_note(key, atomic_load(&s_wb_adopted) - adopted0);
}
static void wb_land_(wb_t *w);
static void wb_land(void *arg) {
    wb_t *w = arg;
    if (wb_lock_on()) { pthread_mutex_lock(&g_wbl_mu); wb_land_(w); pthread_mutex_unlock(&g_wbl_mu); } else wb_land_(w);
}
// NXRT_VK_NANLOG=1 (diagnostics): the float words with an all-ones exponent (NaN / Inf, or other bits that look like it) in each compute write-back, summed per shader key and logged once a frame ("nan: ...")
static struct { char key[17]; uint64_t words, lands, first_off; uint64_t addr, len; } g_nl[64]; static int g_nnl; static pthread_mutex_t g_nl_mu = PTHREAD_MUTEX_INITIALIZER;
static int nanlog_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("NXRT_VK_NANLOG"); on = e && atoi(e); } return on; }
static void nanlog_scan(const wb_t *w) {
    const uint32_t *u = (const uint32_t *)w->mem.map; uint64_t n = w->len / 4, c = 0, first = 0;
    for (uint64_t i = 0; i < n; i++) if ((u[i] & 0x7f800000u) == 0x7f800000u) { if (!c++) first = i * 4; }
    if (!c) return;
    pthread_mutex_lock(&g_nl_mu); int i; for (i = 0; i < g_nnl && (strcmp(g_nl[i].key, w->key) || g_nl[i].addr != w->addr); i++) ;
    if (i == g_nnl && g_nnl < 64) { snprintf(g_nl[g_nnl].key, 17, "%s", w->key); g_nl[g_nnl].addr = w->addr; g_nl[g_nnl].len = w->len; g_nnl++; }
    if (i < g_nnl) { g_nl[i].words += c; g_nl[i].lands++; g_nl[i].first_off = first; }
    pthread_mutex_unlock(&g_nl_mu);
}
static void nanlog_tick(uint64_t frame) {
    if (!g_nnl) return;
    pthread_mutex_lock(&g_nl_mu);
    for (int i = 0; i < g_nnl; i++) if (g_nl[i].lands) { VKLOG("nan: frame %llu shader %s addr %#llx len %llu: %llu words in %llu landings (first at +%llu)", (unsigned long long)frame, g_nl[i].key, (unsigned long long)g_nl[i].addr, (unsigned long long)g_nl[i].len, (unsigned long long)g_nl[i].words, (unsigned long long)g_nl[i].lands, (unsigned long long)g_nl[i].first_off); g_nl[i].words = g_nl[i].lands = 0; }
    pthread_mutex_unlock(&g_nl_mu);
}
static void wb_land_(wb_t *w) {
    if (w->kind == 0 && nanlog_on()) nanlog_scan(w);
    {   // NXRT_VK_EXLOG=1 (diagnostics): the landings of the exposure reductions (2cfd3d8d, 5a6f9e2a): the first four words as they land and as the CPU had them
        static int on = -1; if (on < 0) { const char *e = getenv("NXRT_VK_EXLOG"); on = e && atoi(e); }
        if (on && w->kind == 0 && w->len >= 16 && (!strncmp(w->key, "2cfd3d8d", 8) || !strncmp(w->key, "5a6f9e2a", 8))) {
            const uint32_t *d = (const uint32_t *)w->mem.map, *g = (const uint32_t *)(uintptr_t)w->addr;
            VKLOG("blk: frame %llu land %.8s addr %#llx len %llu data %08x %08x %08x %08x guest-before %08x %08x %08x %08x", (unsigned long long)g_frame, w->key, (unsigned long long)w->addr, (unsigned long long)w->len, d[0], d[1], d[2], d[3], g[0], g[1], g[2], g[3]);
        }
    }
    if (w->kind == 0) nanfix(w);
    uint8_t *d = (uint8_t *)(uintptr_t)w->addr;
    const uint8_t *raw = w->kind == 0 ? w->mem.map : w->kind == 1 ? w->cpu : NULL;   // the bytes landing (a fill: w->fill, below)
    uint8_t *fillbuf = NULL, *merged = NULL; const uint8_t *eff = raw;
    if (w->kind == 2 && w->g0) { fillbuf = malloc((size_t)w->len); if (fillbuf) { for (uint64_t i = 0; i < w->len; i++) fillbuf[i] = (uint8_t)(w->fill >> 8 * (i & 3)); raw = eff = fillbuf; } }
    atomic_fetch_add(&s_wm_lands, 1);
    if (w->g0 && raw) {   // the words the CPU wrote since the record stay
        uint8_t *cur = malloc((size_t)w->len); merged = malloc((size_t)w->len);
        if (cur && merged && !guest_copy(cur, d, (size_t)w->len)) {
            wbm_merge(merged, w->addr, w->len, cur, w->g0, raw);
            rr_restore(w->addr, w->len, merged, raw);
            uint64_t kept = 0; for (uint64_t i = 0; i < w->len; i += 4) { uint64_t n = w->len - i < 4 ? w->len - i : 4; kept += memcmp(merged + i, raw + i, (size_t)n) != 0; }   // words that stay as the CPU has them and differ from the landing
            if (kept) { atomic_fetch_add(&s_wm_kept_lands, 1); atomic_fetch_add(&s_wm_kept_words, kept); eff = merged; }
        }
        free(cur);
    }
    if (w->kind <= 1) rr_land(w->addr, w->len, (uint8_t *)eff, w->kind);
    if (w->kind <= 1) wb_race(w->addr, w->len, w->kind ? w->cpu : w->mem.map, w->stamp, w->kind);
    if (eff == merged && merged) wb_apply(w->addr, w->len, eff, w->stamp, 0);
    else if (w->kind == 0) wb_apply(w->addr, w->len, w->mem.map, w->stamp, !w->g0);
    else if (w->kind == 1) wb_apply(w->addr, w->len, w->cpu, w->stamp, !w->g0);
    else if (fillbuf) wb_apply(w->addr, w->len, fillbuf, w->stamp, 0);
    else { dm_guest_write_prepare((void *)(uintptr_t)w->addr, (size_t)w->len); for (uint64_t i = 0; i < w->len; i++) d[i] = (uint8_t)(w->fill >> 8 * (i & 3)); }
    wb_unkeep(w, raw ? eff : NULL);
    free(merged); free(fillbuf); free(w->g0);
    gpu_pending(w->addr, w->len, -1);
    if (w->kind <= 1) rr_landed(w->addr, w->len);
    if (w->kind == 0) wb_adopt(w->addr, w->len, w->mem.map, w->key); else if (w->kind == 1) wb_adopt(w->addr, w->len, w->cpu, w->key);
    atomic_fetch_add(&s_wb_bytes, w->len);
    if (w->buf) { vkDestroyBuffer(g_vk.dev, w->buf, NULL); vk_free_mem(&w->mem); }
    free((void *)w->cpu); free(w);
}
// NXRT_VK_WBLOG=1: the distinct (address, size) of every GPU write-back with its count and bytes, listed every 600 presents (the biggest first)
static struct { uint64_t addr, len, n, first; const char *why; } g_wbl[256]; static int g_nwbl; static int g_wbl_on = -1;
static uint64_t g_wbl_total;
static void wbl_note(uint64_t addr, uint64_t len) {
    if (g_wbl_on < 0) { const char *e = getenv("NXRT_VK_WBLOG"); g_wbl_on = e && atoi(e); } if (!g_wbl_on) return;
    g_wbl_total += len;
    for (int i = 0; i < g_nwbl; i++) if (g_wbl[i].addr == addr && g_wbl[i].len == len) { g_wbl[i].n++; return; }
    if (g_nwbl < 256) { g_wbl[g_nwbl].addr = addr; g_wbl[g_nwbl].len = len; g_wbl[g_nwbl].n = 1; g_wbl[g_nwbl].first = g_frame; g_nwbl++; }
}
static void wbl_report(uint64_t frame) {
    if (!g_wbl_on || frame % 600 || !g_nwbl) return;
    VKLOG("wblog: frame %llu: %d distinct write-back ranges, %.1f MB in all", (unsigned long long)frame, g_nwbl, g_wbl_total / 1048576.0);
    for (int pass = 0; pass < 16; pass++) {   // the 16 biggest by bytes
        int best = -1; uint64_t bb = 0; for (int i = 0; i < g_nwbl; i++) if (g_wbl[i].n * g_wbl[i].len > bb && g_wbl[i].why == NULL) { bb = g_wbl[i].n * g_wbl[i].len; best = i; }
        if (best < 0) break; g_wbl[best].why = "x";
        VKLOG("wblog:   %#llx +%llu x%llu = %.1f MB (first frame %llu)", (unsigned long long)g_wbl[best].addr, (unsigned long long)g_wbl[best].len, (unsigned long long)g_wbl[best].n, bb / 1048576.0, (unsigned long long)g_wbl[best].first);
    }
    for (int i = 0; i < g_nwbl; i++) g_wbl[i].why = NULL;
}
void vkm_gpu_wrote(uint64_t addr, uint64_t len) { vkm_gpu_wrote_k(addr, len, "?"); }
void vkm_gpu_wrote_k(uint64_t addr, uint64_t len, const char *key) {
    VkBuffer pb; VkDeviceSize po;
    wbl_note(addr, len);
    VkCommandBuffer cb = vks_cb();
    if (!cb || !len || !vkm_addr(addr, len, &pb, &po)) return;
    wb_t *w = calloc(1, sizeof *w); w->addr = addr; w->len = len; w->stamp = nvn_dirty_now(); snprintf(w->key, sizeof w->key, "%s", key);
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, len, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    vkCreateBuffer(g_vk.dev, &bci, NULL, &w->buf);
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, w->buf, &mr);
    if (vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0, &w->mem) &&
        vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, &w->mem)) { vkDestroyBuffer(g_vk.dev, w->buf, NULL); free(w); return; }
    vkBindBufferMemory(g_vk.dev, w->buf, w->mem.mem, w->mem.off);
    vk_mem_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferCopy c = {po, 0, len};
    vkCmdCopyBuffer(cb, pb, w->buf, 1, &c);
    vk_mem_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    gpu_pending(addr, len, 1);
    if (s_blk_lo && addr < s_blk_hi && addr + len > s_blk_lo) VKLOG("blk: frame %llu record write-back %#llx+%llu (%s) in submission %llu", (unsigned long long)g_frame, (unsigned long long)addr, (unsigned long long)len, key, (unsigned long long)vks_recording_value());
    wb_keep(w);
    vks_post_fn(wb_land, w);
}
// Writes whose bytes the CPU knows (inline uniform updates, fills): the GPU writes the mirror in order, guest memory gets the same bytes at completion
void vkm_cpu_also(uint64_t addr, uint64_t len, const void *data, int fill, uint32_t value) {
    wb_t *w = calloc(1, sizeof *w); w->addr = addr; w->len = len; w->kind = fill ? 2 : 1; w->fill = value; w->stamp = nvn_dirty_now();
    if (!fill) { uint8_t *c = malloc((size_t)len); memcpy(c, data, (size_t)len); w->cpu = c; }
    gpu_pending(addr, len, 1);
    wb_keep(w);
    vks_post_fn(wb_land, w);
}

// ---------- Indirect arguments seen by the GPU (2026-11-05: the foliage culling compute 71cca4c5 stalls the GPU; its dispatch is indirect) ----------
// Before each vkCmdDispatchIndirect the replay copies the 12 argument bytes from the mirror into a host-visible ring (this copy is in the same command stream, so it holds what the GPU
// itself was about to read); the completion thread logs interesting ones (keys named in NXRT_VK_INDLOG, default the foliage pair; anything over NXRT_VK_IND_WARN workgroups) and
// the watchdog's report prints the last ones, even for a submission that never completed (a word still 0xdeadbeef: its copy had not run yet)
enum { IND_N = 256 };
typedef struct { char key[20]; uint64_t addr, seq, frame, off; uint32_t slot, kind; } ind_meta;   // kind 0 dispatch (3 words), 1 draw (4), 2 indexed draw (5)
static struct { VkBuffer buf; vk_alloc mem; volatile uint32_t *map; ind_meta meta[IND_N]; _Atomic uint64_t seq; int ok, off; char want[256]; uint64_t warn, clamp_n; } g_ind;
static void ind_init(void) {
    const char *e = getenv("NXRT_VK_INDRING"); g_ind.off = e && !atoi(e);
    e = getenv("NXRT_VK_INDLOG"); snprintf(g_ind.want, sizeof g_ind.want, ",%s,", e ? e : "71cca4c5dca75177,3f1e6bf13d5f50af");
    e = getenv("NXRT_VK_IND_WARN"); g_ind.warn = e ? strtoull(e, NULL, 0) : 65535;
    if (g_ind.off) return;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, IND_N * 32, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (vkCreateBuffer(g_vk.dev, &bci, NULL, &g_ind.buf) != VK_SUCCESS) return;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, g_ind.buf, &mr);
    if (vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0, &g_ind.mem) &&
        vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, &g_ind.mem)) { vkDestroyBuffer(g_vk.dev, g_ind.buf, NULL); g_ind.buf = VK_NULL_HANDLE; return; }
    vkBindBufferMemory(g_vk.dev, g_ind.buf, g_ind.mem.mem, g_ind.mem.off);
    g_ind.map = (volatile uint32_t *)g_ind.mem.map; for (int i = 0; i < IND_N * 8; i++) g_ind.map[i] = 0xdeadbeef;
    g_ind.ok = 1;
}
typedef struct { uint32_t slot; uint64_t seq; } ind_post;
static int ind_wanted(const char *key) { char pat[24]; snprintf(pat, sizeof pat, ",%s,", key); return strstr(g_ind.want, pat) != NULL || strstr(g_ind.want, ",all,") != NULL; }
static uint64_t g_cl_draw_total = 1u << 28, g_cl_draw_inst = 1u << 20, g_cl_draw_count = 1u << 24;   // limits of an indirect draw (NXRT_VK_IND_DRAW_MAX = vertices * instances)
static uint32_t ind_count(const char *key) {   // calls so far of this key (small table; the completion thread and the replay thread may race on a count: it is a log counter)
    static struct { char key[20]; uint32_t n; } t[32]; static int nt;
    for (int i = 0; i < nt; i++) if (!strcmp(t[i].key, key)) return t[i].n++;
    if (nt < 32) { snprintf(t[nt].key, sizeof t[nt].key, "%s", key); t[nt].n = 1; nt++; }
    return 0;
}
// A direct dispatch of a wanted key: its sizes (nothing to copy)
void vkm_ind_direct(const char *key, uint64_t x, uint64_t y, uint64_t z) {
    if (!ind_wanted(key)) return;
    static _Atomic int lk; while (atomic_exchange(&lk, 1)) ;
    uint32_t n = ind_count(key); atomic_store(&lk, 0);
    if (n < 40 || !(n % 300)) VKLOG("direct dispatch %s (frame %llu): %llu x %llu x %llu (call %u)", key, (unsigned long long)g_frame, (unsigned long long)x, (unsigned long long)y, (unsigned long long)z, n);
}
static void ind_land(void *arg) {
    ind_post *p = arg; ind_meta *m = &g_ind.meta[p->slot];
    if (m->seq == p->seq) {
        volatile uint32_t *w = &g_ind.map[p->slot * 8]; uint32_t a[5] = {w[0], w[1], w[2], w[3], w[4]};
        uint64_t tot = m->kind == 0 ? (uint64_t)a[0] * a[1] * a[2] : (uint64_t)a[0] * a[1];
        static _Atomic uint32_t nbig; int want = ind_wanted(m->key);
        uint32_t nw = want ? ind_count(m->key) : 0;
        uint64_t lim = m->kind == 0 ? g_ind.warn : g_cl_draw_total;
        if (tot > lim || (m->kind && a[1] > g_cl_draw_inst) || (m->kind && a[0] > g_cl_draw_count)) { if (atomic_fetch_add(&nbig, 1) < 200) VKRED("indirect %s %s (frame %llu, args at %#llx): %x %x %x %x %x: over the limits, zeroed before the GPU reads it", m->kind ? "draw, vertex shader" : "dispatch", m->key, (unsigned long long)m->frame, (unsigned long long)m->addr, a[0], a[1], a[2], a[3], a[4]); }
        else if (want && (nw < 40 || !(nw % 300))) VKLOG("indirect %s %s (frame %llu, args at %#llx): %x %x %x %x %x (call %u)", m->kind ? "draw" : "dispatch", m->key, (unsigned long long)m->frame, (unsigned long long)m->addr, a[0], a[1], a[2], a[3], a[4], nw);
        else if (m->kind) { static _Atomic uint32_t nd; uint32_t k = atomic_fetch_add(&nd, 1); if (k < 60 || !(k % 600)) VKLOG("indirect draw, vertex shader %s (frame %llu, args at %#llx): %x %x %x %x %x (call %u)", m->key, (unsigned long long)m->frame, (unsigned long long)m->addr, a[0], a[1], a[2], a[3], a[4], k); }
    }
    free(p);
}
// Records the copy of the 12 bytes at src+soff into the ring, in cb (outside a render pass). Returns 1 when recorded
int vkm_ind_record(VkCommandBuffer cb, const char *key, uint64_t addr, VkBuffer src, VkDeviceSize soff, int kind) {
    if (!g_ind.ok || !cb) return 0;
    uint64_t seq = atomic_fetch_add(&g_ind.seq, 1) + 1; uint32_t slot = (uint32_t)(seq % IND_N);
    ind_meta *m = &g_ind.meta[slot]; snprintf(m->key, sizeof m->key, "%s", key); m->addr = addr; m->seq = seq; m->frame = g_frame; m->slot = slot; m->kind = (uint32_t)kind;
    for (int i = 0; i < 5; i++) g_ind.map[slot * 8 + i] = 0xdeadbeef;
    VkBufferCopy c = {soff, slot * 32, kind == 0 ? 12 : kind == 1 ? 16 : 20}; vkCmdCopyBuffer(cb, src, g_ind.buf, 1, &c);
    vk_mem_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT);
    ind_post *p = malloc(sizeof *p); p->slot = slot; p->seq = seq; vks_post_fn(ind_land, p);
    return 1;
}
// Snapshots of what the mirror holds at the start of each pointer a wanted shader goes through (diagnostics, NXRT_VK_INDLOG keys): 128 bytes, before and after the dispatch, logged at completion
// beside what guest memory holds at the same moment of recording
enum { SNAP_N = 64, SNAP_BYTES = 128 };
static struct { VkBuffer buf; vk_alloc mem; volatile uint32_t *map; _Atomic uint64_t seq; int ok; } g_snap;
typedef struct { char key[20]; const char *tag; uint64_t ptr, frame, seq; uint32_t slot, guest[SNAP_BYTES / 4]; int have_guest; } snap_post;
static void snap_init(void) {
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, SNAP_N * SNAP_BYTES, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (vkCreateBuffer(g_vk.dev, &bci, NULL, &g_snap.buf) != VK_SUCCESS) return;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, g_snap.buf, &mr);
    if (vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0, &g_snap.mem) &&
        vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, &g_snap.mem)) { vkDestroyBuffer(g_vk.dev, g_snap.buf, NULL); g_snap.buf = VK_NULL_HANDLE; return; }
    vkBindBufferMemory(g_vk.dev, g_snap.buf, g_snap.mem.mem, g_snap.mem.off);
    g_snap.map = (volatile uint32_t *)g_snap.mem.map; g_snap.ok = 1;
}
static void snap_land(void *arg) {
    snap_post *p = arg; static _Atomic uint32_t n;
    uint32_t k = atomic_fetch_add(&n, 1);
    if (k < 120 || !(k % 150)) {
        char line[900]; int o = snprintf(line, sizeof line, "snapshot %s %s ptr %#llx frame %llu mirror:", p->key, p->tag, (unsigned long long)p->ptr, (unsigned long long)p->frame);
        for (int i = 0; i < 16; i++) o += snprintf(line + o, sizeof line - o, " %x", g_snap.map[p->slot * (SNAP_BYTES / 4) + i]);
        o += snprintf(line + o, sizeof line - o, " | guest:");
        for (int i = 0; i < 16 && p->have_guest; i++) o += snprintf(line + o, sizeof line - o, " %x", p->guest[i]);
        VKLOG("%s", line);
    }
    free(p);
}
void vkm_snap(VkCommandBuffer cb, const char *key, const char *tag, uint64_t ptr) {
    if (!g_snap.ok || !cb || !ind_wanted(key)) return;
    VkBuffer b; VkDeviceSize o;
    if (!vkm_addr(ptr & ~3ull, SNAP_BYTES, &b, &o)) return;
    uint64_t seq = atomic_fetch_add(&g_snap.seq, 1) + 1; uint32_t slot = (uint32_t)(seq % SNAP_N);
    snap_post *p = calloc(1, sizeof *p); snprintf(p->key, sizeof p->key, "%s", key); p->tag = tag; p->ptr = ptr; p->frame = g_frame; p->slot = slot; p->seq = seq;
    p->have_guest = !plat_read(ptr & ~3ull, p->guest, SNAP_BYTES);
    VkBufferCopy c = {o, slot * SNAP_BYTES, SNAP_BYTES}; vkCmdCopyBuffer(cb, b, g_snap.buf, 1, &c);
    vk_mem_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT);
    vks_post_fn(snap_land, p);
}
int vkm_ind_is_wanted(const char *key) { return ind_wanted(key); }
// Diagnostics (pass dump): the bytes the mirror holds at a guest address now (a GPU copy, waited for); 0 on success
int vkm_peek(uint64_t addr, void *dst, size_t n) {
    VkBuffer b; VkDeviceSize o; if (!n || n > 65536 || !vkm_addr(addr, n, &b, &o)) return -1;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, n, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer hb; vk_alloc m; if (vkCreateBuffer(g_vk.dev, &bci, NULL, &hb) != VK_SUCCESS) return -1;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, hb, &mr);
    if (vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0, &m) &&
        vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, &m)) { vkDestroyBuffer(g_vk.dev, hb, NULL); return -1; }
    vkBindBufferMemory(g_vk.dev, hb, m.mem, m.off);
    VkCommandBuffer cb = vk_once_begin(); VkBufferCopy c = {o, 0, n}; vkCmdCopyBuffer(cb, b, hb, 1, &c);
    vk_mem_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    int rc = vk_once_end(cb); if (!rc) memcpy(dst, m.map, n);
    vkDestroyBuffer(g_vk.dev, hb, NULL); vk_free_mem(&m); return rc;
}
// ---------- Indirect dispatch argument clamp ----------
// The game's dispatch arguments are written by the GPU (culling passes): a garbage count (stale, or never reset) is a wild dispatch, which hangs AMD's GPU. Before each indirect dispatch a
// one-thread compute pass copies the 12 bytes to a scratch slot, zeroing them when a dimension is over the device's limit or the total over NXRT_VK_IND_CAP groups (default 1 << 20);
// the dispatch then reads the slot. NXRT_VK_IND_CLAMP=0 dispatches from the pool directly
enum { CL_SLOTS = 16384 };
static struct { VkBuffer buf; vk_alloc mem; uint64_t addr; VkPipeline pipe; VkPipelineLayout lay; _Atomic uint64_t seq; int ok; float cap; } g_cl;
static void clamp_init(void) {
    const char *e = getenv("NXRT_VK_IND_CLAMP"); if (e && !atoi(e)) return;
    if (!g_vk.f12.bufferDeviceAddress) return;
    e = getenv("NXRT_VK_IND_CAP"); g_cl.cap = e ? (float)strtod(e, NULL) : (float)(1u << 20);
    g_ind.warn = (uint64_t)g_cl.cap;
    e = getenv("NXRT_VK_IND_DRAW_MAX"); if (e) g_cl_draw_total = strtoull(e, NULL, 0);
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, CL_SLOTS * 32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (vkCreateBuffer(g_vk.dev, &bci, NULL, &g_cl.buf) != VK_SUCCESS) return;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, g_cl.buf, &mr);
    if (vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &g_cl.mem)) { vkDestroyBuffer(g_vk.dev, g_cl.buf, NULL); g_cl.buf = VK_NULL_HANDLE; return; }
    vkBindBufferMemory(g_vk.dev, g_cl.buf, g_cl.mem.mem, g_cl.mem.off);
    VkBufferDeviceAddressInfo ai = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, NULL, g_cl.buf}; g_cl.addr = vkGetBufferDeviceAddress(g_vk.dev, &ai);
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 56};
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 1, &pr};
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof ind_clamp_comp_spv, ind_clamp_comp_spv}; VkShaderModule m;
    if (vkCreatePipelineLayout(g_vk.dev, &li, NULL, &g_cl.lay) != VK_SUCCESS || vkCreateShaderModule(g_vk.dev, &mi, NULL, &m) != VK_SUCCESS) { VKRED("indirect clamp: cannot create its pipeline layout / module"); return; }
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", NULL}, g_cl.lay, VK_NULL_HANDLE, 0};
    VkResult r = vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &ci, NULL, &g_cl.pipe);
    vkDestroyShaderModule(g_vk.dev, m, NULL);
    if (r != VK_SUCCESS) { VKRED("indirect clamp: pipeline creation failed (%d)", r); return; }
    g_cl.ok = 1;
    VKLOG("indirect dispatch arguments are clamped by a compute pass (groups per dimension <= %u/%u/%u, total <= %.0f)", g_vk.props.limits.maxComputeWorkGroupCount[0], g_vk.props.limits.maxComputeWorkGroupCount[1], g_vk.props.limits.maxComputeWorkGroupCount[2], g_cl.cap);
}
// Records the clamp of the 12 bytes at src (device address of the pool mirror + offset) into a scratch slot, outside a render pass; returns the buffer and offset the dispatch reads, or src itself without the pass
void vkm_ind_clamp(VkCommandBuffer cb, VkBuffer src, VkDeviceSize soff, int kind, VkBuffer *buf, VkDeviceSize *off) {
    if (!g_cl.ok) { *buf = src; *off = soff; return; }
    VkBufferDeviceAddressInfo ai = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, NULL, src};
    uint64_t sa = vkGetBufferDeviceAddress(g_vk.dev, &ai) + soff;
    uint32_t slot = (uint32_t)((atomic_fetch_add(&g_cl.seq, 1)) % CL_SLOTS);
    struct { uint64_t src, dst; uint32_t mx, my, mz; float cap; uint32_t mode; uint32_t lim_inst, lim_count; float lim_total; } pc =
        {sa, g_cl.addr + slot * 32ull, g_vk.props.limits.maxComputeWorkGroupCount[0], g_vk.props.limits.maxComputeWorkGroupCount[1], g_vk.props.limits.maxComputeWorkGroupCount[2], g_cl.cap, (uint32_t)kind,
         (uint32_t)g_cl_draw_inst, (uint32_t)g_cl_draw_count, (float)g_cl_draw_total};
    vk_mem_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_cl.pipe);
    vkCmdPushConstants(cb, g_cl.lay, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, 1, 1, 1);
    vk_mem_barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    *buf = g_cl.buf; *off = slot * 32ull;
}
// For the watchdog thread (no locks): the last entries, newest last
int vkm_ind_report(char *out, size_t cap) {
    int o = 0; if (!g_ind.ok) return 0;
    uint64_t seq = atomic_load(&g_ind.seq);
    for (uint64_t k = seq > 10 ? seq - 9 : 1; k <= seq && o + 80 < (int)cap; k++) {
        ind_meta m = g_ind.meta[k % IND_N]; if (m.seq != k) continue;
        uint32_t a[5]; for (int i = 0; i < 5; i++) a[i] = g_ind.map[(k % IND_N) * 8 + i];
        o += snprintf(out + o, cap - o, " [%s %.8s f%llu %x,%x,%x,%x,%x]", m.kind ? "draw" : "disp", m.key, (unsigned long long)m.frame, a[0], a[1], a[2], a[3], a[4]);
    }
    return o;
}

// ---------- Frames, statistics ----------
void vkm_stats(vkm_stats_t *s) {
    memset(s, 0, sizeof *s);
    plat_rw_rdlock(&g_lk);
    for (int i = 0; i < g_npool; i++) { s->pools++; s->pool_bytes += g_pool[i]->hi - g_pool[i]->lo; s->bound_bytes += g_pool[i]->bound; s->sparse_pools += g_pool[i]->sparse; s->hot_pages += g_pool[i]->hot_n; }
    plat_rw_rdunlock(&g_lk);
    s->up_bytes = atomic_load(&s_up_bytes); s->up_pages = atomic_load(&s_up_pages); s->up_runs = atomic_load(&s_up_runs); s->up_ns = atomic_load(&s_up_ns);
    s->sync_calls = atomic_load(&s_sync); s->staging_bytes = atomic_load(&s_st_bytes); s->staging_overflow = atomic_load(&s_st_ovf); s->staging_waits = atomic_load(&s_st_waits);
    s->recopy_pages = atomic_load(&s_recopy_pages); s->writeback_bytes = atomic_load(&s_wb_bytes); s->uncommitted_pages = atomic_load(&s_uncommitted);
    nvn_dirty_stats(&s->faults, &s->preps);
    uint64_t mc, mpg; nvn_dirty_mp_stats(&mc, &mpg, &s->fault_ns);   // the tracker's protection changes (faults and protects)
}

static void upload_report(uint64_t df, int always) {
    // who the uploads were for: the five pools with the most bytes in this period (zeroed afterwards)
    {
        pool_t_ *top[5] = {0}; uint64_t tb[5] = {0}, ub[4] = {0};
        plat_rw_rdlock(&g_lk);
        for (int i = 0; i < g_npool; i++) { pool_t_ *q = g_pool[i]; uint64_t t = q->upb_use + q->upb_touch + q->upb_all; ub[0] += q->upb_use; ub[1] += q->upb_touch; ub[2] += q->upb_all; ub[3] += q->upb_first;
            for (int k = 0; k < 5; k++) if (t > tb[k]) { for (int j = 4; j > k; j--) { top[j] = top[j - 1]; tb[j] = tb[j - 1]; } top[k] = q; tb[k] = t; break; } }
        if (!always && ub[0] + ub[1] + ub[2] < (1000ull << 20)) { for (int i = 0; i < g_npool; i++) { pool_t_ *q = g_pool[i]; if (q->upb_use + q->upb_touch + q->upb_all) { } } plat_rw_rdunlock(&g_lk); return; }
        VKLOG("uploads in these %llu frames: %.0f MB for uses, %.0f MB for global memory pointers, %.0f MB for pool syncs (of which %.0f MB pages never copied before)", (unsigned long long)df, ub[0] / 1048576.0, ub[1] / 1048576.0, ub[2] / 1048576.0, ub[3] / 1048576.0);
        for (int k = 0; k < 5 && top[k]; k++) { pool_t_ *q = top[k];
            VKLOG("  pool %p+%#llx%s: %.0f MB uploaded (uses %.0f, pointers %.0f, syncs %.0f; first copies %.0f, hot pages %.0f), %.0f MB zero-filled, %llu hot pages, span %s", (void *)q->lo, (unsigned long long)(q->hi - q->lo), q->sparse ? " sparse" : "", tb[k] / 1048576.0, q->upb_use / 1048576.0, q->upb_touch / 1048576.0,
                  q->upb_all / 1048576.0, q->upb_first / 1048576.0, q->upb_hot / 1048576.0, q->uncommitted_fill / 1048576.0, (unsigned long long)q->hot_n, q->span ? "tracked" : "UNTRACKED"); }
        for (int i = 0; i < g_npool; i++) { pool_t_ *q = g_pool[i]; q->upb_use = q->upb_touch = q->upb_all = q->upb_first = q->upb_hot = q->uncommitted_fill = 0; }
        plat_rw_rdunlock(&g_lk);
    }
}

void vkm_frame_tick(uint64_t frame) {
    static vkm_stats_t prev; static uint64_t prev_frame, t_prev;
    { static int bi; if (!bi) { bi = 1; const char *e = getenv("NXRT_VK_BLKWATCH"); if (e) { char *end; s_blk_lo = strtoull(e, &end, 16); s_blk_hi = s_blk_lo + (*end == ',' ? strtoull(end + 1, NULL, 0) : 4096); s_blk_on = 1; } } }
    wbl_report(frame); nanlog_tick(frame);
    { static int on = -1; if (on < 0) { const char *e = getenv("NXRT_VK_FRAMESTATS"); on = e && atoi(e); }   // one line a frame: what the write-back landings did since the last one (flash hunting)
      if (on) { static uint64_t pa, pm, ps, pw, pu, pbl; uint64_t a = atomic_load(&s_wb_adopted), m = atomic_load(&s_wb_adopt_miss), sm = atomic_load(&s_wb_same), wb = atomic_load(&s_wb_bytes), up = atomic_load(&s_up_bytes), bl = atomic_load(&s_blk_pages);
          s_blk_on = 1;
          VKLOG("fm: frame %llu blocked %llu (never copied %llu in all) last %#llx adopted %llu miss %llu same %llu KB wb %llu KB uploaded %llu KB", (unsigned long long)frame, (unsigned long long)(bl - pbl), (unsigned long long)atomic_load(&s_blk_first), (unsigned long long)s_blk_last[(bl + 15) % 16],
                (unsigned long long)(a - pa), (unsigned long long)(m - pm), (unsigned long long)((sm - ps) >> 10), (unsigned long long)((wb - pw) >> 10), (unsigned long long)((up - pu) >> 10));
          pa = a; pm = m; ps = sm; pw = wb; pu = up; pbl = bl; } }
    g_frame = frame + 1; g_budget_used = 0;
    if (frame % (wb_verify_on() > 1 ? 3 : 15) == 0) wb_verify();
    if (frame % HOT_COOL_FRAMES == 0) {   // hot pages cool down: protected again at their next copy; written again, they come back within HOT_STREAK frames
        plat_rw_rdlock(&g_lk);
        for (int i = 0; i < g_npool; i++) { pool_t_ *p = g_pool[i]; for (uint64_t k = 0; k < p->npages; k++) if (p->hot[k]) { p->hot[k] = 0; p->up[k] = 0; p->streak[k] = 0; } p->hot_n = 0; p->clean_epoch = 0; }   // copied (and protected) again at the next sync
        plat_rw_rdunlock(&g_lk);
    }
    { static uint64_t last_rep; if (frame % 100 == 0 && frame != last_rep) { upload_report(frame - last_rep > 1000 ? 100 : frame - last_rep, 0); last_rep = frame; } }
    {   // always, next to the "frame pacing" line (the same every-300-frames cadence): whether the read-and-reset detection fired, and when
        static uint64_t last_rr; if (frame % 300 == 0 && frame != last_rr) { last_rr = frame; static uint64_t pw, pr, pl; uint64_t w = atomic_load(&s_rr_words), r = atomic_load(&s_rr_resent), l = atomic_load(&s_rr_landed);
          VKLOG("expo-fix up to frame %llu: %s; %llu read-and-reset pages (first at frame %llu, %llu refused by the cap) of %llu candidate pages (%llu showed CPU-written data); words sent from the CPU %llu (+%llu), of them %llu unchanged learned resets (+%llu); %llu landings over them (+%llu), %llu words left pending by a landing; WB_ADOPT %s",
                (unsigned long long)frame, rr_fix_on() ? "on" : "OFF", (unsigned long long)atomic_load(&s_rr_pages), (unsigned long long)atomic_load(&s_rr_first), (unsigned long long)atomic_load(&s_rr_capped), (unsigned long long)atomic_load(&s_rr_cands), (unsigned long long)atomic_load(&s_rr_stream),
                (unsigned long long)w, (unsigned long long)(w - pw), (unsigned long long)r, (unsigned long long)(r - pr), (unsigned long long)l, (unsigned long long)(l - pl), (unsigned long long)atomic_load(&s_rr_pend), wb_adopt_on() ? "on" : "OFF");
          pw = w; pr = r; pl = l; if (rr_cfg("NXRT_VK_EXPO_LOG", 0)) rr_top_log();
          uint64_t pn, pb, psv, pst; vkp_order_stats(&pn, &pb, &psv, &pst); static uint64_t lpn, lpb, lpsv, lpst, llands, lkl, lkw, lbig, ledge; uint64_t ll = atomic_load(&s_wm_lands), kl = atomic_load(&s_wm_kept_lands), kw = atomic_load(&s_wm_kept_words), big = atomic_load(&s_wm_big_kept), edge = atomic_load(&s_wm_big_edge);
          VKLOG("rewind-check up to frame %llu: WB_MERGE %s; write-back landings %llu (+%llu), of them %llu (+%llu) held words the CPU wrote after the record that were kept (%llu words, +%llu), %llu (+%llu) pages of big write-backs KEPT because the CPU wrote them since the record, %llu (+%llu) edge/uncovered pages overwritten though the CPU had written them; presents drawn %llu (+%llu): timeline value went BACK %llu (+%llu), did not advance %llu (+%llu), same texture twice %llu (+%llu)",
                (unsigned long long)frame, wb_merge_on() ? "on" : "OFF", (unsigned long long)ll, (unsigned long long)(ll - llands), (unsigned long long)kl, (unsigned long long)(kl - lkl), (unsigned long long)kw, (unsigned long long)(kw - lkw), (unsigned long long)big, (unsigned long long)(big - lbig), (unsigned long long)edge, (unsigned long long)(edge - ledge),
                (unsigned long long)pn, (unsigned long long)(pn - lpn), (unsigned long long)pb, (unsigned long long)(pb - lpb), (unsigned long long)psv, (unsigned long long)(psv - lpsv), (unsigned long long)pst, (unsigned long long)(pst - lpst));
          lpn = pn; lpb = pb; lpsv = psv; lpst = pst; llands = ll; lkl = kl; lkw = kw; lbig = big; ledge = edge; }
    }
    if (frame % 300 || frame == prev_frame) return;
    vkm_stats_t s; vkm_stats(&s);
    uint64_t df = frame - prev_frame, now = now_ns(); double secs = t_prev ? (now - t_prev) / 1e9 : 0;
    uint64_t budget = 0, usage = 0; vk_budget(&budget, &usage);
    VKLOG("memory up to frame %llu: %llu pools %.1f MB (backed %.1f MB, %llu sparse); per frame: uploaded %.1f KB in %.1f runs (%.3f ms CPU), %.1f write faults (protection changes %.3f ms), "
          "%.1f host pre-writes, written back %.1f KB, %.0f MB of uncommitted pool stepped over; %.1f pages recopied; hot pages %llu; staging overflow %llu waits %llu; video memory %.0f / %.0f MB%s",
          (unsigned long long)frame, (unsigned long long)s.pools, s.pool_bytes / 1048576.0, s.bound_bytes / 1048576.0, (unsigned long long)s.sparse_pools,
          (s.up_bytes - prev.up_bytes) / 1024.0 / df, (double)(s.up_runs - prev.up_runs) / df, (s.up_ns - prev.up_ns) / 1e6 / df,
          (double)(s.faults - prev.faults) / df, (s.fault_ns - prev.fault_ns) / 1e6 / df, (double)(s.preps - prev.preps) / df,
          (s.writeback_bytes - prev.writeback_bytes) / 1024.0 / df, (s.uncommitted_pages - prev.uncommitted_pages) * 4096.0 / 1048576.0 / df, (double)(s.recopy_pages - prev.recopy_pages) / df, (unsigned long long)s.hot_pages, (unsigned long long)s.staging_overflow,
          (unsigned long long)s.staging_waits, usage / 1048576.0, budget / 1048576.0, secs > 0 ? "" : "");
    VKLOG("pool_sync time in all: %.0f ms asking the OS about pages, %.0f ms copying guest memory, %.0f ms protecting pages, %.0f ms binding sparse memory / waiting for staging (of which %.0f ms waiting for the GPU to free the ring, %.0f ms making overflow buffers); %llu MB of uncommitted pages skipped (known zero)", atomic_load(&s_t_query) / 1e6, atomic_load(&s_t_copy) / 1e6, atomic_load(&s_t_prot) / 1e6, atomic_load(&s_t_bind) / 1e6, atomic_load(&s_t_stwait) / 1e6, atomic_load(&s_t_stovf) / 1e6, (unsigned long long)(atomic_load(&s_unc_skipped) * PAGE >> 20));
    if (atomic_load(&s_sync_skipped)) VKLOG("pool scans up to frame %llu: %llu skipped as unwritten since a complete pass (%llu found behind guest memory by the check)", (unsigned long long)frame, (unsigned long long)atomic_load(&s_sync_skipped), (unsigned long long)atomic_load(&s_sync_skip_bad));
    if (g_nak && frame % 900 == 0) { pthread_mutex_lock(&g_ak_mu); for (int i = 0; i < g_nak; i++) VKLOG("adoption by shader %s: %llu pages in %llu landings", g_ak[i].key, (unsigned long long)g_ak[i].pages, (unsigned long long)g_ak[i].lands); pthread_mutex_unlock(&g_ak_mu); }
    if (atomic_load(&s_wr_pages)) VKLOG("wb race up to frame %llu: %llu pages overwritten by a landing though the CPU wrote them after the write-back was recorded", (unsigned long long)frame, (unsigned long long)atomic_load(&s_wr_pages));
    if (atomic_load(&s_wb_adopted) + atomic_load(&s_wb_adopt_miss) + atomic_load(&s_wb_adopt_strict)) VKLOG("write-back adoption (STRICT %s: %llu pages left for re-upload) up to frame %llu: %llu pages kept current without a copy back (%.0f MB), %llu not (CPU wrote them first); %.0f MB of the write-backs already matched guest memory and were not written; verify: %llu pages checked, %llu differ", adopt_strict() ? "on" : "OFF", (unsigned long long)atomic_load(&s_wb_adopt_strict), (unsigned long long)frame, (unsigned long long)atomic_load(&s_wb_adopted), atomic_load(&s_wb_adopted) * PAGE / 1048576.0, (unsigned long long)atomic_load(&s_wb_adopt_miss), atomic_load(&s_wb_same) / 1048576.0, (unsigned long long)atomic_load(&s_av_checked), (unsigned long long)atomic_load(&s_av_bad));
    if (atomic_load(&s_own_pages)) VKLOG("GPU-owned pages up to frame %llu: %llu pages (%llu shadows), %llu words applied in place from the CPU, %llu whole pages first copied", (unsigned long long)frame, (unsigned long long)atomic_load(&s_own_pages), (unsigned long long)atomic_load(&g_shadow_pages), (unsigned long long)atomic_load(&s_own_words), (unsigned long long)atomic_load(&s_own_full));
    if (atomic_load(&s_gmem_programs)) VKLOG("global memory up to frame %llu: %llu programs bound, %llu pointers followed, %llu pool syncs for them (%.0f MB of CPU-rewritten pages sent unbudgeted), page table %llu of %llu words", (unsigned long long)frame,
          (unsigned long long)atomic_load(&s_gmem_programs), (unsigned long long)atomic_load(&s_gmem_touches), (unsigned long long)atomic_load(&s_gmem_syncs), atomic_load(&s_gmem_dirty_bytes) / 1048576.0, (unsigned long long)g_gt.next, (unsigned long long)g_gt.words);
    prev = s; prev_frame = frame; t_prev = now;
}

int vkm_init(void) {
    plat_rw_init(&g_st.lk);
    const char *e = getenv("NXRT_VK_UPLOAD_MB"); if (e && atoi(e) > 0) g_budget = (uint64_t)atoi(e) << 20;
    e = getenv("NXRT_VK_SCAN_PAGES"); if (e && atoi(e) > 0) g_scan_pages = (uint64_t)atoi(e);
    VkDeviceSize ring = g_budget < (16u << 20) ? (16u << 20) : g_budget > (256u << 20) ? (256u << 20) : g_budget;
    e = getenv("NXRT_VK_STAGING_MB"); if (e && atoi(e) > 0) ring = (VkDeviceSize)atoi(e) << 20;
    g_usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
              VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
    if (g_vk.f12.bufferDeviceAddress) g_usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    gmem_init(); ind_init(); snap_init(); clamp_init();
    if (g_vk.has.xfb) g_usage |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT;
    if (g_vk.has.cond_render) g_usage |= VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT;
    VkPhysicalDeviceMaintenance4Properties m4 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &m4};
    vkGetPhysicalDeviceProperties2(g_vk.pd, &p2);
    g_max_buffer = m4.maxBufferSize;
    e = getenv("NXRT_VK_MAX_BUFFER_MB");   // test switch: pretend the device's largest buffer is smaller (AMD's 2048, to try its pool windows on another card)
    if (e && atoi(e) > 0 && ((VkDeviceSize)atoi(e) << 20) < g_max_buffer) g_max_buffer = (VkDeviceSize)atoi(e) << 20;
    g_alias = g_vk.has.sparse_aliased && !(getenv("NXRT_VK_NO_ALIAS") && atoi(getenv("NXRT_VK_NO_ALIAS")));
    if (g_vk.has.sparse_buffer) {   // the granule is a whole number of the device's sparse blocks
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT, GRAN, g_usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
        VkBuffer b; VkMemoryRequirements mr;
        if (vkCreateBuffer(g_vk.dev, &bci, NULL, &b) == VK_SUCCESS) { vkGetBufferMemoryRequirements(g_vk.dev, b, &mr); vkDestroyBuffer(g_vk.dev, b, NULL);
            if (mr.alignment > g_gran) g_gran = mr.alignment; else g_gran = (GRAN + mr.alignment - 1) / mr.alignment * mr.alignment; }
    }
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, ring, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (vkCreateBuffer(g_vk.dev, &bci, NULL, &g_st.buf) != VK_SUCCESS) return -1;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, g_st.buf, &mr);
    if (vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &g_st.mem)) { VKRED("no host memory for the staging ring"); return -1; }
    vkBindBufferMemory(g_vk.dev, g_st.buf, g_st.mem.mem, g_st.mem.off);
    g_st.map = g_st.mem.map; g_st.size = ring;
    vk_name((uint64_t)g_st.buf, VK_OBJECT_TYPE_BUFFER, "staging ring");
    VKLOG("memory: staging ring %llu MB, upload budget %llu MB a frame, pools %s (granule %llu KB), largest buffer %.0f GB",
          (unsigned long long)(ring >> 20), (unsigned long long)(g_budget >> 20), g_vk.has.sparse_buffer ? "sparse" : "fully backed", (unsigned long long)(g_gran >> 10), g_max_buffer / 1073741824.0);
    return 0;
}

void vkm_shutdown(void) {
    plat_rw_wrlock(&g_lk);
    for (int i = 0; i < g_npool; i++) pool_free(g_pool[i]);
    g_npool = 0;
    plat_rw_wrunlock(&g_lk);
    if (g_gt.buf) { vkDestroyBuffer(g_vk.dev, g_gt.buf, NULL); vk_free_mem(&g_gt.mem); memset(&g_gt, 0, sizeof g_gt); }
    plat_rw_wrlock(&g_st.lk);
    for (unsigned k = g_st.qt; k != g_st.qh; k++) { unsigned i = k % ST_Q; if (g_st.q[i].ovf) { vkDestroyBuffer(g_vk.dev, g_st.q[i].ovf, NULL); vk_free_mem(&g_st.q[i].ovf_mem); } }
    g_st.qt = g_st.qh = 0;
    if (g_st.buf) { vkDestroyBuffer(g_vk.dev, g_st.buf, NULL); vk_free_mem(&g_st.mem); g_st.buf = VK_NULL_HANDLE; }
    plat_rw_wrunlock(&g_st.lk);
}
