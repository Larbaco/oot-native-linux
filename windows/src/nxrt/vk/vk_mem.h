// GPU memory (docs/m5-vulkan.md §3): every NVN memory pool gets a device-local VkBuffer of its size (GPU address = guest address - pool base + buffer
// offset), backed lazily in 2 MB granules through sparse binding, so only the parts the GPU uses take video memory. Its contents are kept current from
// guest memory through write-protect dirty tracking (nvn_dirty_win.c): a page is copied when it was written since its last copy, through a staging
// ring, in the submission's upload command buffer. No host memory is ever imported into Vulkan (research/01 lesson 10).
#pragma once
#include "vk_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

int vkm_init(void);
void vkm_shutdown(void);
void vkm_pool_add(const void *mem, uint64_t size);
void vkm_pool_del(const void *mem);
int vkm_peek(uint64_t addr, void *dst, size_t n);   // diagnostics: what the mirror holds at a guest address now (waits); 0 on success
int vkm_addr(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off);   // lookup only: 1 when [addr, addr+len) lies in a pool
// The replay's access to pool memory in the submission being recorded: brings the range's written pages up to date (uploads in this submission's upload
// command buffer, before its main one) and returns the buffer and offset. 1 on success
int vkm_use(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off);
// GPU writes into pool memory are copied back to guest memory once the submission completes (NVN pools are coherent with the CPU)
void vkm_gpu_wrote(uint64_t addr, uint64_t len);
// GPU writes whose bytes the CPU knows (inline uniform updates, fills): guest memory gets the same bytes at completion; data is copied
void vkm_cpu_also(uint64_t addr, uint64_t len, const void *data, int fill, uint32_t value);
// Treats [addr, addr + len) as written since its last copy, once per frame and address: for the 64 KiB uniform banks of native programs (the tracker can lose writes there)
void vkm_recopy_once(uint64_t addr, uint64_t len);
// Every pool's written pages, within the frame's upload budget (the policy until the replay calls vkm_use; then a background prefetch). Returns bytes queued
uint64_t vkm_sync_all(void);
// Untracked global memory (nxs_spv.py gmem_bda): the table of guest pages that shaders read to turn a guest address into the device address of its pool's mirror. Words of 64 bits:
//   [va >> 21]   index of the address's level-2 block, 0 = nothing mapped; [block + ((va >> 12) & 511)]   device address minus guest address for that page, 0 = not mapped
// NULL when the device has no buffer device addresses. NXRT_VK_GMEM=0 leaves it empty: every look-up misses, loads read 0 and stores vanish
VkBuffer vkm_gmem_table(VkDeviceSize *size);
// The replay bound a shader that goes through the table, in the submission being recorded. open: some of its addresses do not come from a pointer the replay can read
void vkm_gmem_used(const char *key, int writes, int open);
// A pointer such a shader goes through: its pool, from there to the end of the mapping and up to 64 MiB (Lichii 4dcbd1cb: records 17.7 MiB past the base), comes up to date; once a submission per pointer
void vkm_gmem_touch(uint64_t ptr);
// At commit: if an open shader ran, every pool comes up to date as far as the frame's upload budget goes (NXRT_VK_GMEM_SYNC=all: for any such shader). Starts the next submission's list of pointers
void vkm_gmem_own(uint64_t ptr);   // a shader that writes through ptr: those pages become the GPU's (CPU writes are applied as the words that changed)
uint64_t vkm_gmem_commit_sync(void);
uint64_t vkm_gmem_lookup(uint64_t va);     // what a shader's look-up gives: the device address of va, 0 on a miss (tests and diagnostics)
int vkm_ind_record(VkCommandBuffer cb, const char *key, uint64_t addr, VkBuffer src, VkDeviceSize soff, int kind);   // kind 0 dispatch, 1 draw, 2 indexed draw   // 12 bytes of indirect-dispatch arguments into a host-visible ring (diagnostics)
void vkm_ind_direct(const char *key, uint64_t x, uint64_t y, uint64_t z);
void vkm_snap(VkCommandBuffer cb, const char *key, const char *tag, uint64_t ptr);   // diagnostics: 128 bytes of the mirror at ptr, logged at completion beside guest memory's
int vkm_ind_is_wanted(const char *key);
void vkm_ind_clamp(VkCommandBuffer cb, VkBuffer src, VkDeviceSize soff, int kind, VkBuffer *buf, VkDeviceSize *off);   // indirect dispatch args: copy to a scratch slot, zeroed when wild
int vkm_ind_report(char *out, size_t cap);   // the last entries, for the watchdog
void vkm_rdwatch_init(void);               // NXRT_VK_RDWATCH diagnostics (vk_rdwatch.c): who reads / writes a range of guest memory
void vkm_rdwatch_frame(uint64_t frame);
void vkm_rdwatch_add(uint64_t addr, uint64_t len, const char *who);   // a range to watch (who wrote it: a shader key)
int vkm_rdwatch_gpu(void);                 // NXRT_VK_RDWATCH has the word gpu: the replay adds every storage buffer a compute shader writes
void vkm_frame_tick(uint64_t frame);       // at each present: the budget restarts, hot pages age, statistics every 300 frames

// The staging ring, shared with texture uploads: room for n bytes that live until the submission being recorded completes (or, outside a submission,
// until the next one-shot). Returns the mapped pointer, the buffer and offset; NULL only when memory is exhausted
void *vkm_staging(VkDeviceSize n, VkDeviceSize align, VkBuffer *buf, VkDeviceSize *off);
uint64_t vkm_budget_left(void);            // bytes of this frame's upload budget not used yet (opportunistic uploads stop at 0)
void vkm_budget_take(uint64_t n);

typedef struct {
    uint64_t pools, pool_bytes, bound_bytes, sparse_pools;
    uint64_t up_bytes, up_pages, up_runs, up_ns, sync_calls;   // cumulative
    uint64_t staging_bytes, staging_overflow, staging_waits;
    uint64_t hot_pages, writeback_bytes, uncommitted_pages;   // uncommitted: pages stepped over (never touched by the game)
    uint64_t faults, fault_ns, preps, recopy_pages;
} vkm_stats_t;
void vkm_stats(vkm_stats_t *s);

#ifdef __cplusplus
}
#endif
