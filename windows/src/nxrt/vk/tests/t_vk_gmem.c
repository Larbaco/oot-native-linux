/* Untracked global memory on a real device with no game data (lavapipe: VK_DRIVER_FILES=lvp_icd.json): the page table vk_mem.c keeps, its pool mirrors as sparse buffers, the sync of a pointer the replay followed
   (vkm_gmem_touch), and a shader made by nxs_spv.py's gmem_bda going through all of it.
     nxrt-tests-vk-gmem <shader.spv> <table binding>
   The shader is tests/test_nxshader.py's GM_RUN: binding 0 holds eight (lo, hi) queries at word 1, the results land at word 128, and one store's lo, hi, value at words 64-66.
   Exit 0 = pass, 77 = no device or no shader given */
#include "vk_test.h"
#include <sys/mman.h>

NXRT_EARLY_RESERVE()

enum { PG = 4096, NP = 64 };
static VkDevice D; static VkPipeline g_pipe; static VkPipelineLayout pl; static VkDescriptorSet ds; static VkBuffer iob; static uint32_t *io;

static void set_query(int i, uint64_t va) { io[1 + 2 * i] = (uint32_t)va; io[2 + 2 * i] = (uint32_t)(va >> 32); }
static void run(uint64_t touch, uint64_t store_va, uint32_t value) {
    io[64] = (uint32_t)store_va; io[65] = (uint32_t)(store_va >> 32); io[66] = value;
    vks_begin();
    if (touch) vkm_gmem_touch(touch);
    VkCommandBuffer cb = vks_cb();
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, 1, 1, 1);
    vkm_gmem_commit_sync();
    vks_wait(vks_commit(), UINT64_MAX);
}

int main(int argc, char **argv) {
    crash_install();
    if (argc < 3) { printf("no shader given: skipped\n"); return 77; }
    uint8_t *mem = MAP_FAILED;   // where guest memory lives: under 2^40 (the table covers no more), which a plain mmap never returns
    for (uint64_t at = 0x7e00000000ull; at < 0xff00000000ull && mem == MAP_FAILED; at += 1ull << 32) mem = mmap((void *)(uintptr_t)at, 2 * NP * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (mem == MAP_FAILED) { printf("no address under 2^40 free\n"); return 1; }
    uint8_t *mem2 = mem + NP * PG + 8 * PG;   // a second pool in the same level-2 block, a gap between them
    for (uint32_t i = 0; i < NP * PG / 4; i++) ((uint32_t *)mem)[i] = 0xA0000000u + i;
    for (uint32_t i = 0; i < 8 * PG / 4; i++) ((uint32_t *)mem2)[i] = 0xB0000000u + i;
    nvn_dirty_add_range(mem, NP * PG);
    int rc = vt_up(0, 0); if (rc) return rc;
    D = g_vk.dev;
    vkm_pool_add(mem, NP * PG); vkm_pool_add(mem2, 8 * PG);
    VkDeviceSize tsize; VkBuffer table = vkm_gmem_table(&tsize);
    CHECK(table && tsize >= 8u << 20, "the table exists");
    if (!table) return 1;

    // ---- the table against the buffers it names
    uint64_t va = (uint64_t)(uintptr_t)mem, va2 = (uint64_t)(uintptr_t)mem2;
    VkBuffer b1, b2; VkDeviceSize o1, o2;
    CHECK(vkm_addr(va + 0x10, 4, &b1, &o1) && vkm_addr(va2, 4, &b2, &o2) && b1 != b2, "two pools, two mirrors");
    VkBufferDeviceAddressInfo ai = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, NULL, b1};
    uint64_t dev1 = vkGetBufferDeviceAddress(D, &ai); ai.buffer = b2; uint64_t dev2 = vkGetBufferDeviceAddress(D, &ai);
    CHECK(vkm_gmem_lookup(va + 0x10) == dev1 + 0x10, "an address in pool 1 gives its mirror's device address");
    CHECK(vkm_gmem_lookup(va + 0x13) == dev1 + 0x10, "an unaligned address gives its word");
    CHECK(vkm_gmem_lookup(va + 5 * PG) == dev1 + 5 * PG, "another page of the pool");
    CHECK(vkm_gmem_lookup(va2 + 8) == dev2 + 8, "an address in pool 2");
    CHECK(vkm_gmem_lookup(va + NP * PG + PG) == 0, "the gap between the pools is not mapped");
    CHECK(vkm_gmem_lookup(1ull << 40) == 0, "40 bits or more is not mapped");

    // ---- a shader through the table
    FILE *f = fopen(argv[1], "rb"); if (!f) { printf("no %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET); uint32_t *code = malloc((size_t)sz); if (fread(code, 1, (size_t)sz, f) != (size_t)sz) return 1; fclose(f);
    uint32_t tb = (uint32_t)atoi(argv[2]);
    VkDescriptorSetLayoutBinding lb[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT}, {tb, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo lc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, lb};
    VkDescriptorSetLayout dsl; vkCreateDescriptorSetLayout(D, &lc, NULL, &dsl);
    VkPipelineLayoutCreateInfo pc = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl}; vkCreatePipelineLayout(D, &pc, NULL, &pl);
    VkShaderModuleCreateInfo sc = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, (size_t)sz, code}; VkShaderModule sm; vkCreateShaderModule(D, &sc, NULL, &sm);
    VkComputePipelineCreateInfo cp = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main"}, pl};
    CHECK(vkCreateComputePipelines(D, VK_NULL_HANDLE, 1, &cp, NULL, &g_pipe) == VK_SUCCESS, "the pipeline builds");
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 1024, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    vkCreateBuffer(D, &bci, NULL, &iob); VkMemoryRequirements mr; vkGetBufferMemoryRequirements(D, iob, &mr);
    vk_alloc am; vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, &am); vkBindBufferMemory(D, iob, am.mem, am.off); io = am.map;
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}; VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps}; VkDescriptorPool pool; vkCreateDescriptorPool(D, &dp, NULL, &pool);
    VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, pool, 1, &dsl}; vkAllocateDescriptorSets(D, &dai, &ds);
    VkDescriptorBufferInfo bi0 = {iob, 0, 1024}, bi1 = {table, 0, tsize};
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi0}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, tb, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi1}};
    vkUpdateDescriptorSets(D, 2, w, 0, NULL);

    memset(io, 0, 1024);
    set_query(0, va + 0x10); set_query(1, va + 5 * PG + 0x20); set_query(2, va2 + 8); set_query(3, va + NP * PG + PG); set_query(4, 0x1000000ull); set_query(5, 1ull << 40); set_query(6, va + NP * PG - 4); set_query(7, va + 0x10);
    vks_begin(); vkm_gmem_touch(va); vkm_gmem_touch(va2); { VkCommandBuffer cb = vks_cb(); (void)cb; } vks_wait(vks_commit(), UINT64_MAX);   // what follows reads the pools as the replay would have brought them up
    vkm_frame_tick(1);
    run(0, va + 0x100, 0x12345678u);
    CHECK(io[128] == 0xA0000000u + 0x10 / 4, "a followed pointer's pool reads through the table (%#x)", io[128]);
    CHECK(io[129] == 0xA0000000u + (5 * PG + 0x20) / 4, "further into it (%#x)", io[129]);
    CHECK(io[130] == 0xB0000000u + 8 / 4, "the other pool (%#x)", io[130]);
    CHECK(io[131] == 0 && io[132] == 0 && io[133] == 0, "gaps and bad addresses read 0 (%#x %#x %#x)", io[131], io[132], io[133]);
    CHECK(io[134] == 0xA0000000u + (NP * PG - 4) / 4, "the last word of the pool (%#x)", io[134]);

    // the store reached the mirror: a later load sees it, while guest memory does not yet (the open gap: docs FASE5-GMEM section 7)
    memset(io + 1, 0, 8 * 8); set_query(0, va + 0x100);
    run(0, va2 + 0x100, 0x77777777u);
    CHECK(io[128] == 0x12345678u, "a store through the table is read back by the next load (%#x)", io[128]);
    CHECK(((uint32_t *)mem)[0x100 / 4] == 0xA0000000u + 0x100 / 4, "guest memory did not get it: writes through pointers are not written back");

    // a CPU write after the mirror was brought up: a followed pointer sees it at the next submission
    ((uint32_t *)mem)[0x200 / 4] = 0xCAFEF00Du;
    memset(io + 1, 0, 8 * 8); set_query(0, va + 0x200);
    run(va, va2 + 0x104, 1);
    CHECK(io[128] == 0xCAFEF00Du, "a followed pointer brings a CPU write up to date (%#x)", io[128]);

    // a deleted pool leaves the table at once
    vkm_pool_del(mem2);
    CHECK(vkm_gmem_lookup(va2 + 8) == 0, "a deleted pool is gone from the table");
    memset(io + 1, 0, 8 * 8); set_query(0, va2 + 8);
    run(0, va2 + 0x108, 2);
    CHECK(io[128] == 0, "and reads 0 (%#x)", io[128]);
    // an overlapping pool: the newest wins, and the one under it shows again when it goes
    vkm_pool_add(mem + 4 * PG, PG);
    CHECK(vkm_addr(va + 4 * PG, 4, NULL, NULL), "the overlapping pool is registered");
    uint64_t over = vkm_gmem_lookup(va + 4 * PG);
    CHECK(over && over != dev1 + 4 * PG && vkm_gmem_lookup(va + 3 * PG) == dev1 + 3 * PG, "the newest pool wins only on its own pages");
    vkm_pool_del(mem + 4 * PG);
    CHECK(vkm_gmem_lookup(va + 4 * PG) == dev1 + 4 * PG, "the pool under it shows again");

    vkDestroyPipeline(D, g_pipe, NULL); vkDestroyShaderModule(D, sm, NULL); vkDestroyPipelineLayout(D, pl, NULL); vkDestroyDescriptorPool(D, pool, NULL); vkDestroyDescriptorSetLayout(D, dsl, NULL);
    vkDestroyBuffer(D, iob, NULL); vk_free_mem(&am);
    vt_down(0);
    printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "ok", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
