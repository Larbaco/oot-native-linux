// vk_gmem_probe: runs one compute shader made by nxs_spv.py's gmem_bda on whatever Vulkan device the loader gives, against a page table laid out as vk_mem.c lays it out, over real
// device addresses. Offline check of the look-up and of the physical pointers (lavapipe: VK_DRIVER_FILES=lvp_icd.json).
//   vk_gmem_probe <shader.spv> <table binding>
// The shader reads queries from binding 0: 8 (lo, hi) pairs at word 1, results at word 128; a store's lo, hi, value at words 64-66. Prints "RESULT a b c ..." per run, then "STORED x"
// (what landed at pool A + 0x2000), then "SPARSE 1" when the sparse queries ran. Exit 0 when the run finished, 77 when there is no device with device addresses.
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define VK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s failed: %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)
enum { L1_WORDS = 1 << 19, L2_WORDS = 512, TABLE_WORDS = L1_WORDS + 8 * L2_WORDS, PAGE = 4096 };
static VkPhysicalDevice pd; static VkDevice dev; static VkQueue q; static uint32_t qf; static VkPhysicalDeviceMemoryProperties mp;
static uint64_t *table; static uint64_t next_block = L1_WORDS;

static uint32_t memtype(uint32_t bits) {
    VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    fprintf(stderr, "no memory type\n"); exit(1);
}
static VkDeviceMemory alloc(VkDeviceSize n, uint32_t bits, void **map) {
    VkMemoryAllocateFlagsInfo fl = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, NULL, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fl, n, memtype(bits)};
    VkDeviceMemory m; VK(vkAllocateMemory(dev, &ai, NULL, &m));
    if (map) VK(vkMapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, map));
    return m;
}
static VkBuffer mkbuf(VkDeviceSize n, VkBufferUsageFlags u, void **map) {
    VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, n, u, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer b; VK(vkCreateBuffer(dev, &ci, NULL, &b));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, b, &mr);
    VK(vkBindBufferMemory(dev, b, alloc(mr.size, mr.memoryTypeBits, map), 0));
    return b;
}
static uint64_t devaddr(VkBuffer b) { VkBufferDeviceAddressInfo i = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, NULL, b}; return vkGetBufferDeviceAddress(dev, &i); }
static void map_pages(uint64_t va, uint64_t n, VkBuffer b) {
    uint64_t delta = devaddr(b) - va;
    for (uint64_t k = 0; k < n; k += PAGE) {
        uint64_t v = va + k, *l1 = &table[v >> 21];
        if (!*l1) { *l1 = next_block; next_block += L2_WORDS; }
        table[*l1 + ((v >> 12) & (L2_WORDS - 1))] = delta;
    }
}

int main(int argc, char **argv) {
    if (argc < 3) return 1;
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "gmem probe", 1, "nxrt", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    VkInstance inst; if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { puts("no Vulkan"); return 77; }
    uint32_t n = 1; if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) { puts("no device"); return 77; }
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    if (!f12.bufferDeviceAddress || !f2.features.shaderInt64) { puts("no device addresses"); return 77; }
    uint32_t nq = 8; VkQueueFamilyProperties qp[8]; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    qf = 0; while (qf < nq && !(qp[qf].queueFlags & VK_QUEUE_COMPUTE_BIT)) qf++;
    int sparse = f2.features.sparseBinding && f2.features.sparseResidencyBuffer && (qp[qf].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT);
    float prio = 1; VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qf, 1, &prio};
    VkPhysicalDeviceVulkan12Features en12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES}; en12.bufferDeviceAddress = 1;
    VkPhysicalDeviceFeatures2 en = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &en12}; en.features.shaderInt64 = 1; en.features.sparseBinding = sparse; en.features.sparseResidencyBuffer = sparse;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &en, 0, 1, &qci};
    VK(vkCreateDevice(pd, &dci, NULL, &dev)); vkGetDeviceQueue(dev, qf, 0, &q);

    enum { U = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, ASZ = 4 * PAGE, GRANULE = 2 << 20 };
    uint32_t *wa, *wb, *io; void *m;
    VkBuffer A = mkbuf(ASZ, U, &m); wa = m; VkBuffer B = mkbuf(PAGE, U, &m); wb = m;
    for (uint32_t i = 0; i < ASZ / 4; i++) wa[i] = 0xA0000000u + i;
    for (uint32_t i = 0; i < PAGE / 4; i++) wb[i] = 0xB0000000u + i;
    VkBuffer T = mkbuf(TABLE_WORDS * 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &m); table = m;
    VkBuffer IO = mkbuf(1024, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &m); io = m;
    const uint64_t VA_A = 0x7300001000ull, VA_B = 0x7300401000ull, VA_S = 0x7380000000ull;
    map_pages(VA_A, ASZ, A); map_pages(VA_B, PAGE, B);
    uint64_t qs[8] = {VA_A + 0x10, VA_A + 0x1013, VA_B + 8, VA_A + 0x10000, 0x7100000000ull, 0x10000000010ull, 0, 0};
    if (sparse) {   // a bound granule and an unbound one in one buffer, as a pool mirror has them
        VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT, 2 * GRANULE, U, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
        VkBuffer S; VK(vkCreateBuffer(dev, &ci, NULL, &S));
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, S, &mr);
        void *sm; VkDeviceMemory mem = alloc(GRANULE, mr.memoryTypeBits, &sm);
        ((uint32_t *)sm)[1] = 0xC0000001u;
        VkSparseMemoryBind bind = {0, GRANULE, mem, 0, 0};
        VkSparseBufferMemoryBindInfo bi = {S, 1, &bind};
        VkBindSparseInfo si = {VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, NULL, 0, NULL, 1, &bi};
        VK(vkQueueBindSparse(q, 1, &si, VK_NULL_HANDLE)); VK(vkQueueWaitIdle(q));
        map_pages(VA_S, 2 * GRANULE, S);
        qs[6] = VA_S + 4; qs[7] = VA_S + GRANULE + 4;
        puts("SPARSE 1");
    }

    FILE *f = fopen(argv[1], "rb"); if (!f) return 1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET); uint32_t *code = malloc(sz); if (fread(code, 1, sz, f) != (size_t)sz) return 1; fclose(f);
    uint32_t tb = (uint32_t)atoi(argv[2]);
    VkDescriptorSetLayoutBinding lb[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT}, {tb, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo lc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, lb};
    VkDescriptorSetLayout dsl; VK(vkCreateDescriptorSetLayout(dev, &lc, NULL, &dsl));
    VkPipelineLayoutCreateInfo pc = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl};
    VkPipelineLayout pl; VK(vkCreatePipelineLayout(dev, &pc, NULL, &pl));
    VkShaderModuleCreateInfo sc = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, (size_t)sz, code};
    VkShaderModule sm; VK(vkCreateShaderModule(dev, &sc, NULL, &sm));
    VkComputePipelineCreateInfo cp = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main"}, pl};
    VkPipeline pipe; VK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cp, NULL, &pipe));
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps};
    VkDescriptorPool pool; VK(vkCreateDescriptorPool(dev, &dp, NULL, &pool));
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, pool, 1, &dsl};
    VkDescriptorSet ds; VK(vkAllocateDescriptorSets(dev, &ai, &ds));
    VkDescriptorBufferInfo bi0 = {IO, 0, VK_WHOLE_SIZE}, bi1 = {T, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi0}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, tb, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi1}};
    vkUpdateDescriptorSets(dev, 2, w, 0, NULL);
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qf};
    VkCommandPool cpool; VK(vkCreateCommandPool(dev, &cpi, NULL, &cpool));
    VkCommandBufferAllocateInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb; VK(vkAllocateCommandBuffers(dev, &cbi, &cb));
    for (int run = 0; run < 2; run++) {   // the second run with an empty table: what NXRT_VK_GMEM=0 gives
        memset(io, 0, 1024);
        for (int i = 0; i < 8; i++) { io[1 + 2 * i] = (uint32_t)qs[i]; io[2 + 2 * i] = (uint32_t)(qs[i] >> 32); }
        io[64] = (uint32_t)(VA_A + 0x2000); io[65] = (uint32_t)((VA_A + 0x2000) >> 32); io[66] = run ? 0xdeadbeefu : 0x12345678u;
        if (run) memset(table, 0, TABLE_WORDS * 8);
        VkCommandBufferBeginInfo bg = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK(vkBeginCommandBuffer(cb, &bg));
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
        vkCmdDispatch(cb, 1, 1, 1);
        VK(vkEndCommandBuffer(cb));
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb};
        VK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE)); VK(vkQueueWaitIdle(q));
        printf("RESULT"); for (int i = 0; i < 8; i++) printf(" %u", io[128 + i]); printf("\nSTORED %u\n", wa[0x2000 / 4]);
    }
    return 0;
}
