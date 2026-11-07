/* The pipeline cache is persisted incrementally (research/01 lesson 16; docs/m5-vulkan.md §5): pipelines created, the file written by the saver thread
   while the process keeps running (no shutdown, as when the game is killed), through a temporary file that does not remain; a new start loads it and
   the same pipelines come from it. Skipped (77) without a GPU. */
#include "vk_test.h"
#include "test_fill_comp.h"

NXRT_EARLY_RESERVE()

enum { NPIPE = 48 };
static VkPipelineLayout g_pl; static VkDescriptorSetLayout g_dsl;
static int make(VkPipeline *out, int n, int *hits) {
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof test_fill_comp_spv, test_fill_comp_spv};
    VkShaderModule m; if (vkCreateShaderModule(g_vk.dev, &mi, NULL, &m)) return -1;
    *hits = 0;
    for (int i = 0; i < n; i++) {
        uint32_t value = (uint32_t)i * 7 + 3;
        VkSpecializationMapEntry me = {0, 0, 4}; VkSpecializationInfo si = {1, &me, 4, &value};
        VkPipelineCreationFeedback fb = {0}, sfb = {0};
        VkPipelineCreationFeedbackCreateInfo fci = {VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO, NULL, &fb, 1, &sfb};
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, &fci, 0,
                                          {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", &si}, g_pl, VK_NULL_HANDLE, -1};
        if (vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &ci, NULL, &out[i])) return -1;
        vkpc_note_created(1);
        if (fb.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) (*hits)++;
    }
    vkDestroyShaderModule(g_vk.dev, m, NULL);
    return 0;
}
static long long file_size(const wchar_t *p) { WIN32_FILE_ATTRIBUTE_DATA a; if (!GetFileAttributesExW(p, GetFileExInfoStandard, &a)) return -1; return ((long long)a.nFileSizeHigh << 32) | a.nFileSizeLow; }

int main(void) {
    crash_install();
    _putenv_s("NXRT_VK_PCACHE_QUIET_MS", "300");
    int rc = vt_up(0, 1);
    if (rc) return rc;
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b};
    vkCreateDescriptorSetLayout(g_vk.dev, &dl, NULL, &g_dsl);
    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &g_dsl, 0, NULL};
    vkCreatePipelineLayout(g_vk.dev, &pl, NULL, &g_pl);

    CHECK(!vkpc_init(), "pipeline cache up");
    const wchar_t *path = vkpc_path(); wchar_t tmp[MAX_PATH]; _snwprintf(tmp, MAX_PATH, L"%ls.tmp", path);
    DeleteFileW(path); vkpc_shutdown(); CHECK(!vkpc_init(), "pipeline cache up, empty");   // a clean start
    uint64_t c0, s0, b0, l0; vkpc_stats(&c0, &s0, &b0, &l0);
    VkPipeline p[NPIPE]; int hits = 0;
    uint64_t t0 = now_ns();
    CHECK(!make(p, NPIPE, &hits), "%d compute pipelines created", NPIPE);
    uint64_t tcold = now_ns() - t0;
    // the saver writes while the process runs: no shutdown, no explicit save
    uint64_t saves = 0, bytes = 0, c, l; int waited = 0;
    for (; waited < 60; waited++) { Sleep(100); vkpc_stats(&c, &saves, &bytes, &l); if (saves > s0 && file_size(path) > 64) break; }
    long long fs = file_size(path);
    printf("created %d pipelines in %.1f ms (cache hits %d); the saver wrote %lld bytes %.1f s later without a shutdown\n", NPIPE, vt_ms(tcold), hits, fs, (waited + 1) / 10.0);
    CHECK(saves > s0 && fs > 64, "the cache is on disk while the process runs (%lld bytes)", fs);
    CHECK(file_size(tmp) < 0, "no temporary file is left behind");
    FILE *f = _wfopen(path, L"rb"); VkPipelineCacheHeaderVersionOne h = {0};
    if (f) { fread(&h, sizeof h, 1, f); fclose(f); }
    CHECK(h.vendorID == g_vk.props.vendorID && h.deviceID == g_vk.props.deviceID && !memcmp(h.pipelineCacheUUID, g_vk.props.pipelineCacheUUID, VK_UUID_SIZE), "the file is this device's cache");
    for (int i = 0; i < NPIPE; i++) vkDestroyPipeline(g_vk.dev, p[i], NULL);
    // a new start: the file is loaded and the same pipelines come from it
    vkpc_shutdown(); CHECK(!vkpc_init(), "pipeline cache up again");
    vkpc_stats(&c, &saves, &bytes, &l);
    CHECK(l > 64, "the saved cache is loaded (%llu bytes)", (unsigned long long)l);
    t0 = now_ns(); CHECK(!make(p, NPIPE, &hits), "pipelines again"); uint64_t twarm = now_ns() - t0;
    printf("second start: %llu bytes loaded, %d pipelines in %.1f ms, %d reported as pipeline cache hits\n", (unsigned long long)l, NPIPE, vt_ms(twarm), hits);
    CHECK(hits == NPIPE || twarm < tcold, "the second start's pipelines come from the cache");
    for (int i = 0; i < NPIPE; i++) vkDestroyPipeline(g_vk.dev, p[i], NULL);
    vkpc_shutdown();
    vkDestroyPipelineLayout(g_vk.dev, g_pl, NULL); vkDestroyDescriptorSetLayout(g_vk.dev, g_dsl, NULL);
    uint32_t verr = atomic_load(&g_vk.val_errors);
    vt_down(0);
    CHECK(verr == 0, "no validation errors (%u)", verr);
    printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "ok", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
