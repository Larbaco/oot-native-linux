/* Memory pools larger than the device's largest buffer (AMD's 2 GiB, issue #14): with NXRT_VK_MAX_BUFFER_MB=8 a 40 MB pool gets several buffers (windows).
   Guest pages reach the device copy wherever they lie and read back equal through the buffer vkm_use names: inside a window, in the overlap of two windows,
   across a window edge. Overlapping windows share their memory: a page uploaded through one window reads back through the other. Side by side
   (NXRT_VK_NO_ALIAS=1, the second ctest run) a range across an edge is refused instead. Skipped (77) without a GPU. */
#include "vk_test.h"

NXRT_EARLY_RESERVE()

enum { PG = 4096, MB = 1 << 20, POOL = 40 * MB };

// [addr, addr+n) through vkm_use (which uploads what guest memory has newer), read back from the buffer it names: 1 equal, 0 different, -1 refused
static int through_use(uint64_t addr, size_t n) {
    VkBuffer pb; VkDeviceSize po;
    vks_begin(); int ok = vkm_use(addr, n, &pb, &po); vks_wait(vks_commit(), UINT64_MAX);
    if (!ok) return -1;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, n, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer b; vk_alloc m; vkCreateBuffer(g_vk.dev, &bci, NULL, &b);
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, b, &mr);
    vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, &m); vkBindBufferMemory(g_vk.dev, b, m.mem, m.off);
    VkCommandBuffer cb = vk_once_begin();
    VkBufferCopy c = {po, 0, n}; vkCmdCopyBuffer(cb, pb, b, 1, &c);
    vk_mem_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    vk_once_end(cb);
    int same = !memcmp(m.map, (const void *)(uintptr_t)addr, n);
    vkDestroyBuffer(g_vk.dev, b, NULL); vk_free_mem(&m);
    return same;
}

int main(void) {
    crash_install();
    _putenv_s("NXRT_VK_MAX_BUFFER_MB", "8");
    int r = vt_up(0, 0); if (r) return r;
    const char *na = getenv("NXRT_VK_NO_ALIAS");
    int alias = g_vk.has.sparse_buffer && g_vk.has.sparse_aliased && !(na && atoi(na));
    printf("pool windows: %s\n", alias ? "overlapping (sparse aliasing)" : "side by side");

    uint8_t *mem = VirtualAlloc(NULL, POOL, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    for (size_t i = 0; i < POOL; i += 4) *(uint32_t *)(mem + i) = vt_rand();
    vkm_pool_add(mem, POOL);
    uint64_t a = (uint64_t)(uintptr_t)mem;

    VkBuffer b0, b1; VkDeviceSize o0, o1;
    CHECK(vkm_addr(a, PG, &b0, &o0) && o0 == 0, "the pool's start is in its first buffer at offset 0");
    CHECK(vkm_addr(a + POOL - PG, PG, &b1, &o1) && b1 != b0, "the pool's end is in another buffer (a 40 MB pool over an 8 MB largest buffer)");
    CHECK(through_use(a + 12 * PG, 3 * PG) == 1, "a range inside the first window reads back equal");
    CHECK(through_use(a + 6 * MB - 2 * PG, 4 * PG) == 1, "a range just inside the first window's end reads back equal");
    CHECK(through_use(a + POOL - 5 * PG, 5 * PG) == 1, "the pool's last pages read back equal");
    CHECK(through_use(a + 20 * MB + 123, 1000) == 1, "an unaligned range in the middle reads back equal");
    int edge = through_use(a + 8 * MB - PG, 2 * PG);   // across 8 MB: the first window's end
    if (alias) {
        CHECK(edge == 1, "overlapping: a range across the first window's end reads back equal (%d)", edge);
        // A page uploaded through the second window (it starts at 6 MB) reads back through the first one, which also covers it: the same memory
        for (size_t i = 6 * MB + 64 * PG; i < 6 * MB + 65 * PG; i += 4) *(uint32_t *)(mem + i) = vt_rand();
        CHECK(through_use(a + 6 * MB + 64 * PG, PG) == 1, "the rewritten page reads back equal through the second window");
        VkBuffer bf; VkDeviceSize of; CHECK(vkm_addr(a + 5 * MB, 2 * MB, &bf, &of) && bf == b0, "a range starting before 6 MB is served by the first window");
        CHECK(through_use(a + 5 * MB, 2 * MB) == 1, "and through the first window the page uploaded through the second is there");
    } else {
        CHECK(edge == -1, "side by side: a range across a window edge is refused, not read from the wrong place (%d)", edge);
        CHECK(through_use(a + 8 * MB, 2 * PG) == 1, "the second window's first pages read back equal");
    }
    vkm_pool_del(mem);
    vt_down(0);
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
