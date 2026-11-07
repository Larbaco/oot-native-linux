// ASTC on desktop GPUs (research/01 lesson 18): ASTC decoded with ARM's astc-encoder and re-encoded to BC7 with bc7enc, through a persistent cache keyed
// by the source's hash and the quality. A miss is served by a fast first pass at once; the high-quality pass runs on a low-priority worker and replaces
// it (vka_poll hands the results to the texture module).
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { uint64_t lo, hi; } vka_key;
enum { VKA_FAST = 0, VKA_HIGH = 1 };

int vka_init(const char *cache_dir);   // NULL: NXRT_VK_CACHE_DIR or %LOCALAPPDATA%\pokopia-native\vulkan; the worker starts
void vka_shutdown(void);
// ASTC blocks laid out linearly (block rows, slice after slice) -> BC7 blocks (ceil(w/4) x ceil(h/4) per slice). Returns the quality delivered, -1 on error
int vka_get_bc7(const uint8_t *astc, uint32_t w, uint32_t h, uint32_t slices, int bw, int bh, int srgb, uint8_t *bc7, vka_key *key);
int vka_decode_rgba8(const uint8_t *astc, uint32_t w, uint32_t h, uint32_t slices, int bw, int bh, int srgb, uint8_t *rgba);   // 0 on success
int vka_encode_bc7(const uint8_t *rgba, uint32_t w, uint32_t h, int quality, uint8_t *bc7);   // one slice; tests and the worker
size_t vka_bc7_size(uint32_t w, uint32_t h, uint32_t slices);
int vka_decode_bc7_block(const uint8_t *block, uint8_t rgba[64]);   // bc7enc's decoder (tests)
// High-quality results that finished: fn(key, data, size, user) for up to max of them; returns how many
typedef void (*vka_ready_fn)(vka_key key, const uint8_t *bc7, size_t size, void *user);
int vka_poll(vka_ready_fn fn, void *user, int max);
void vka_wait_idle(void);              // tests: until the worker's queue is empty
typedef struct { uint64_t hits_high, hits_fast, misses, fast_ns, fast_texels, high_done, high_ns, high_texels, queued, cache_bytes; } vka_stats_t;
void vka_stats(vka_stats_t *s);

#ifdef __cplusplus
}
#endif
