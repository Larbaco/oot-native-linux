/* vkspv_ballot_per_lane (#12, #21, #22): the warp-aggregated counter of shader d2c50cca6e386abc after an early return gets a per-lane ballot, the same
   counter with its result read keeps its ballot, and on a GPU the rewritten shader counts every lane still running. NXRT_TEST_SPV=<a translated .spv>
   checks that file too and NXRT_TEST_SPV_OUT=<path> writes the result (the game's shaders are made from the player's dump and are never in the repository).
   The GPU part is skipped without a device. */
#include "vk_test.h"
#include "vk_shader.h"
#include "test_ballot_comp.h"
#include "test_ballot_used_comp.h"

NXRT_EARLY_RESERVE()

static int count_op(const uint32_t *w, size_t n, uint32_t op) { int c = 0; for (size_t i = 5; i < n && (w[i] >> 16); i += w[i] >> 16) c += (w[i] & 0xffff) == op; return c; }
static uint32_t *copy_words(const uint32_t *w, size_t n) { uint32_t *c = malloc(n * 4); memcpy(c, w, n * 4); return c; }

// One dispatch of groups x 64 lanes; every third lane returns early. The counter after it, or ~0 when it could not run
static uint32_t run(const uint32_t *words, size_t n, uint32_t groups, uint32_t *expect) {
    uint32_t lanes = groups * 64, got = ~0u;
    VkBuffer buf[2] = {0}; vk_alloc mem[2] = {0}; VkDeviceSize size[2] = {4, (VkDeviceSize)lanes * 4};
    for (int k = 0; k < 2; k++) {
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size[k], VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
        if (vkCreateBuffer(g_vk.dev, &bci, NULL, &buf[k])) return got;
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, buf[k], &mr);
        if (vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, &mem[k]) || !mem[k].map) return got;
        vkBindBufferMemory(g_vk.dev, buf[k], mem[k].mem, mem[k].off);
    }
    uint32_t *data = mem[1].map; *(uint32_t *)mem[0].map = 0; *expect = 0;
    for (uint32_t i = 0; i < lanes; i++) { data[i] = i % 3 ? 1 : 0; *expect += data[i]; }
    VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, b};
    VkDescriptorSetLayout dsl; VkPipelineLayout pl; VkShaderModule m; VkPipeline p; VkDescriptorPool dp; VkDescriptorSet ds;
    vkCreateDescriptorSetLayout(g_vk.dev, &dl, NULL, &dsl);
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 0, NULL};
    vkCreatePipelineLayout(g_vk.dev, &pli, NULL, &pl);
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, n * 4, words};
    if (vkCreateShaderModule(g_vk.dev, &mi, NULL, &m)) return got;
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
                                      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", NULL}, pl, VK_NULL_HANDLE, -1};
    if (vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &ci, NULL, &p)) return got;
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps};
    vkCreateDescriptorPool(g_vk.dev, &dpi, NULL, &dp);
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &dsl};
    vkAllocateDescriptorSets(g_vk.dev, &ai, &ds);
    VkDescriptorBufferInfo bi[2] = {{buf[0], 0, 4}, {buf[1], 0, size[1]}};
    VkWriteDescriptorSet wr[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi[0], NULL},
                                  {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi[1], NULL}};
    vkUpdateDescriptorSets(g_vk.dev, 2, wr, 0, NULL);
    VkCommandBuffer cb = vk_once_begin();
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, groups, 1, 1);
    vk_mem_barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    if (!vk_once_end(cb)) got = *(uint32_t *)mem[0].map;
    vkDestroyPipeline(g_vk.dev, p, NULL); vkDestroyShaderModule(g_vk.dev, m, NULL); vkDestroyDescriptorPool(g_vk.dev, dp, NULL);
    vkDestroyPipelineLayout(g_vk.dev, pl, NULL); vkDestroyDescriptorSetLayout(g_vk.dev, dsl, NULL);
    for (int k = 0; k < 2; k++) { vkDestroyBuffer(g_vk.dev, buf[k], NULL); vk_free_mem(&mem[k]); }
    return got;
}

int main(void) {
    crash_install();
    enum { OP_BALLOT = 339, OP_ATOMIC_IADD = 234 };
    // the shape of d2c50cca6e386abc's last block: rewritten
    size_t n = sizeof test_ballot_comp_spv / 4; uint32_t *w = copy_words(test_ballot_comp_spv, n); int nb = -1;
    CHECK(count_op(w, n, OP_BALLOT) == 1, "the test shader has one ballot");
    size_t rn = vkspv_ballot_per_lane(&w, n, &nb);
    CHECK(nb == 1 && rn > n && count_op(w, rn, OP_BALLOT) == 0, "the counter's ballot made per lane (%d rewritten, %zu -> %zu words)", nb, n, rn);
    CHECK(count_op(w, rn, OP_ATOMIC_IADD) == 1 && w[3] > test_ballot_comp_spv[3], "the add stays, the id bound grows (%u -> %u)", test_ballot_comp_spv[3], w[3]);
    // the add's result read: left alone
    size_t un = sizeof test_ballot_used_comp_spv / 4; uint32_t *u = copy_words(test_ballot_used_comp_spv, un); int ub = -1;
    CHECK(vkspv_ballot_per_lane(&u, un, &ub) == un && ub == 0 && !memcmp(u, test_ballot_used_comp_spv, un * 4), "a counter whose result is read keeps its ballot");
    free(u);
    // a translated shader given on the command line's environment (d2c50cca6e386abc from a build folder's shader cache)
    const char *tp = getenv("NXRT_TEST_SPV");
    if (tp && *tp) {
        FILE *f = fopen(tp, "rb"); long sz = 0; uint32_t *t = NULL;
        if (f) { fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET); t = malloc(sz); if (fread(t, 1, sz, f) != (size_t)sz) sz = 0; fclose(f); }
        CHECK(t && sz > 20, "NXRT_TEST_SPV read (%s)", tp);
        if (t && sz > 20) {
            int tb = -1; size_t tn = vkspv_ballot_per_lane(&t, (size_t)sz / 4, &tb);
            printf("%s: %d ballot%s made per lane, %ld -> %zu words, %d ballots left\n", tp, tb, tb == 1 ? "" : "s", sz / 4, tn, count_op(t, tn, OP_BALLOT));
            CHECK(tb >= 1, "the translated shader's counter ballot is found");
            const char *op = getenv("NXRT_TEST_SPV_OUT");
            if (op && *op) { FILE *g = fopen(op, "wb"); if (g) { fwrite(t, 4, tn, g); fclose(g); } }
        }
        free(t);
    }
    // NXRT_TEST_SPV_DIR=<a shader cache's vk folder>: which of its shaders would change (no GPU needed)
    const char *td = getenv("NXRT_TEST_SPV_DIR");
    if (td && *td) {
        char pat[1100]; snprintf(pat, sizeof pat, "%s\\*.spv", td);
        WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd); int seen = 0, changed = 0;
        for (BOOL more = h != INVALID_HANDLE_VALUE; more; more = FindNextFileA(h, &fd)) {
            char p[1400]; snprintf(p, sizeof p, "%s\\%s", td, fd.cFileName);
            FILE *f = fopen(p, "rb"); if (!f) continue;
            fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
            uint32_t *t = malloc(sz > 0 ? sz : 4); size_t got = fread(t, 1, sz, f); fclose(f);
            if (got == (size_t)sz && sz > 20) { int tb = 0; vkspv_ballot_per_lane(&t, (size_t)sz / 4, &tb); seen++; if (tb) { changed++; printf("  would change %s (%d)\n", fd.cFileName, tb); } }
            free(t);
        }
        if (h != INVALID_HANDLE_VALUE) FindClose(h);
        printf("%s: %d of %d shaders would change\n", td, changed, seen);
    }
    // on a GPU: the rewritten shader counts every lane that did not return (the translated one counts the same with 32-lane subgroups)
    int rc = vt_up(0, 1);
    if (!rc) {
        uint32_t expect = 0, got = run(w, rn, 64, &expect);
        printf("GPU (subgroups of %u): the rewritten counter %u, lanes still running %u\n", g_vk.has.subgroup_size, got, expect);
        CHECK(got == expect, "the rewritten counter counts every running lane (%u of %u)", got, expect);
        if (g_vk.has.subgroup_size == 32) {
            uint32_t e2 = 0, orig = run(test_ballot_comp_spv, n, 64, &e2);
            CHECK(orig == e2, "the original counts the same with 32-lane subgroups (%u of %u)", orig, e2);
        }
        uint32_t verr = atomic_load(&g_vk.val_errors);
        vt_down(0);
        CHECK(verr == 0, "no validation errors (%u)", verr);
    }
    free(w);
    if (rc == 77) printf("GPU part skipped (no device)\n");
    printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "ok", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
