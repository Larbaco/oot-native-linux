// The Vulkan backend's shared state (src/nxrt/vk; docs/m5-vulkan.md). One device, one universal queue, one GPU timeline.
#pragma once
#include "posix_host.h"
#include "vk_fn.h"
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// The runtime's log (host/log.c) and clock; tests that run without nxrt_core supply their own (tests/vk_test.h)
void nxrt_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
uint64_t now_ns(void);
#define VKLOG(...) nxrt_logf("[vk] " __VA_ARGS__)
#define VKRED(...) nxrt_logf("[vk] [RED] " __VA_ARGS__)

// Optional device extensions and features the replay (M5-C) will want; 1 = enabled on this device
typedef struct {
    int custom_border, custom_border_noformat, xfb, xfb_streams, depth_clip_control, index_u8, cond_render, provoking_last, null_descriptor,
        eds2_logic_op, eds2_patch_points, eds3, viewport_swizzle_nv, memory_budget, present_id, present_wait, swap_maint1, line_raster,
        vertex_divisor, prim_list_restart, stencil_export, fs_interlock, blend_adv, gs_passthrough_nv, viewport_array2_nv, feedback_loop,
        color_write_enable, depth_clamp_01, push_descriptor, gpl, sparse_buffer, sparse_aliased, bc, astc_ldr, mirror_clamp_edge,
        minmax_filter, debug_utils, surface_maint1, fmt_4444, vertex_input_dynamic, device_fault, buffer_marker;
    int checkpoints;   // VK_NV_device_diagnostic_checkpoints: 1 = before each dispatch, 2 = draws too (NXRT_VK_CHECKPOINTS=all), 0 = off (=0, or not NVIDIA)
    int corpus_ok;   // everything the translated corpus declares (docs/m5-record-shaders.md section 7): draw parameters, subgroup ballot / vote / shuffle, formatless storage images, gather, geometry
    uint32_t subgroup_stages, subgroup_ops, subgroup_size;
} vk_caps;

typedef struct {
    VkInstance inst;
    VkPhysicalDevice pd;
    VkDevice dev;
    VkQueue q;
    uint32_t qfam, api;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties memp;
    VkPhysicalDeviceFeatures f10;             // enabled core 1.0 features
    VkPhysicalDeviceVulkan11Features f11;
    VkPhysicalDeviceVulkan12Features f12;
    VkPhysicalDeviceVulkan13Features f13;
    vk_caps has;
    VkDebugUtilsMessengerEXT dbg;
    int validation;
    _Atomic uint32_t val_errors, val_warnings;   // validation messages (tests require 0 errors)
    plat_rwlock_t qlock;                    // external synchronisation of the queue: submits, sparse binds, presents
    VkSemaphore tl;                   // the GPU timeline: submission k signals k
    _Atomic uint64_t tl_assigned;     // highest value handed out (gfx_sub_commit's fence)
    _Atomic uint64_t tl_submitted;    // highest value submitted to the queue
    _Atomic uint64_t tl_done;         // highest value whose GPU work and CPU side effects are complete
    VkPipelineCache pcache;
    _Atomic uint64_t presents;        // presents handed to the GPU: retirement also waits 3 of them (the present thread samples images without the timeline)
    _Atomic int presenting;
    _Atomic int lost;                 // the device was lost (reported once by vk_lost)
    int up;                           // device usable
} vk_device;
extern vk_device g_vk;

typedef struct { int validation; int headless; const char *device_hint; } vk_dev_opts;
int vk_dev_open(const vk_dev_opts *o, const char *const *inst_exts, uint32_t n_inst_exts);   // 0 = up; -1 = no usable Vulkan 1.3 device (logged)
void vk_dev_close(void);
const char *vk_vendor_name(uint32_t vendor);
int vk_mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid);   // -1 = none
void vk_name(uint64_t handle, VkObjectType type, const char *name);   // debug-utils object name (validation builds)
// A lost device (#12, #17): the first VK_ERROR_DEVICE_LOST logs what the GPU was doing (the checkpoints each stage reached) and the driver's fault report
void vk_lost(const char *where, VkResult r);
void vk_checkpoint(VkCommandBuffer cb, char kind, const char *key, uint32_t x, uint32_t y, uint32_t z, int indirect);   // kind 'c' dispatch, 'd' draw
void vk_marker_end(VkCommandBuffer cb, uint64_t value);   // AMD buffer markers (NXRT_VK_MARKERS=1): the end of the submission that signals value
int vk_marker_report(char *out, size_t cap);   // the watchdog's: the commands the GPU had started and not finished (no locks)

// One-shot command buffers on the universal queue (uploads at bring-up, readbacks, tests): begin, record, then submit and wait
VkCommandBuffer vk_once_begin(void);
int vk_once_end(VkCommandBuffer cb);   // submits, waits for completion; 0 on success

// Timeline helpers
int vk_tl_wait(uint64_t value, uint64_t timeout_ns);   // 1 = reached
uint64_t vk_tl_value(void);

// Barriers
void vk_image_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                      VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access);
void vk_mem_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access);

// Device memory: blocks sub-allocated first-fit; big requests get a dedicated allocation
typedef struct { VkDeviceMemory mem; VkDeviceSize off, size; void *map; int block; int tag; } vk_alloc;   // tag: the caller's slot in the allocation census (vk_dev.c)
void vk_alloc_census(char *out, size_t n);

int vk_alloc_mem(const VkMemoryRequirements *r, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid, vk_alloc *out);
void vk_free_mem(vk_alloc *a);
void vk_alloc_bytes_nolock(uint64_t *block_bytes, uint64_t *dedicated_bytes, uint64_t *used, uint64_t *host_blocks);
uint64_t vkm_bound_total(void);
void vkm_pool_counts(uint64_t *made, uint64_t *freed, uint64_t *bytes_made);
void vk_alloc_stats(uint64_t *blocks, uint64_t *block_bytes, uint64_t *used_bytes, uint64_t *dedicated_bytes);
int vk_budget(uint64_t *device_local_budget, uint64_t *device_local_usage);   // VK_EXT_memory_budget; 0 when not available

// Deferred destruction: freed after the GPU passes the timeline value current at the call (vk_retire_poll runs them)
void vk_retire(void (*fn)(void *), void *arg);
void vk_retire_poll(void);

#ifdef __cplusplus
}
#endif
