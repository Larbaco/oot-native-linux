// The texture ID inside a descriptor handle (low 20 bits; the sampler ID is above them), checked against the texture pool (vk_replay.c write_set).
// Header only, so tests/test_vk_handle.c can check it without a Vulkan device.
#pragma once
#include <stdint.h>

enum { VKH_OK = 0, VKH_EMPTY = 1, VKH_OFFSET = 2, VKH_RANGE = 3 };
// Returns the pool index the handle names, or 0 for an empty descriptor. *kind: VKH_OK (in the pool), VKH_EMPTY (ID 0), VKH_OFFSET (a Switch 2 handle can hold the descriptor
// header's byte offset, index * 32, instead of the index: Eglantine 049c9633; taken when the ID is past the pool, a multiple of 32 and ID / 32 lies inside it), VKH_RANGE
// (past the pool and not an offset: an empty descriptor instead of a read past the pool, Lichii b34b24cd). pool_n 0 = no pool known yet: nothing to check against
static inline int vkt_handle_index(uint64_t handle, int pool_n, int *kind) {
    int id = (int)(handle & 0xfffff), k = VKH_OK;
    if (id <= 0) { k = VKH_EMPTY; id = 0; }
    else if (pool_n > 0 && id >= pool_n) {
        if (id % 32 == 0 && id / 32 < pool_n) { k = VKH_OFFSET; id /= 32; }
        else { k = VKH_RANGE; id = 0; }
    }
    if (kind) *kind = k;
    return id;
}
