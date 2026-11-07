// vk_dispatch_probe: runs ONE compute shader (SPIR-V) once on whatever Vulkan device the loader gives (meant for lavapipe: VK_DRIVER_FILES=lvp_icd.json) with
// dummy resources, and says whether it terminated. Offline check that a translated compute shader (the NDSR ones, e5addddb60498099 and friends) ends and that
// the loop guard (nxs_spv.py LoopGuard) really ends an endless loop. Never use it with a hardware ICD: a shader that does not end hangs the GPU.
//
//   vk_dispatch_probe <shader.spv> <x> <y> <z> <timeout s> <fill> <binding> ...
//     fill     zero | ones | rand | ramp | flag   (flag: word 0 = 1, the rest 0) what the buffers and images hold
//     binding  <n>:<kind>[:<size>]          kind: ubo ssbo cis1d cis2d cis3d cis2da cubes (combined image sampler) img1d img2d img3d img2da (storage image)
//                                           tex1d tex2d tex3d tex2da (sampled image) smp (sampler) utb stb (texel buffers)
//   PROBE_DUMP=<n> prints the first n words of the first storage buffer afterwards ("WORDS a b c")
//   exit 0 and "TERMINATED <ms>" when the dispatch finished; exit 3 and "TIMEOUT" when it did not; 77 when no device; 1 on setup failures
//   build: clang -O1 -o vk_dispatch_probe vk_dispatch_probe.c -lvulkan
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#define VK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s failed: %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)
static VkPhysicalDevice pd; static VkDevice dev; static VkQueue q; static uint32_t qf;
static VkPhysicalDeviceMemoryProperties mp;

static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    fprintf(stderr, "no memory type\n"); exit(1);
}
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 11); }
static void fill(void *p, size_t n, const char *how) {
    uint32_t *w = p; size_t nw = n / 4;
    for (size_t i = 0; i < nw; i++) w[i] = !strcmp(how, "ones") ? 0xffffffffu : !strcmp(how, "rand") ? rnd() : !strcmp(how, "ramp") ? (uint32_t)i : !strcmp(how, "flag") ? (i == 0) : 0;
}
static void mkbuf(size_t size, VkBufferUsageFlags u, const char *how, VkBuffer *b, void **map) {
    VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, u, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VK(vkCreateBuffer(dev, &ci, NULL, b));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, *b, &mr);
    VkMemoryAllocateFlagsInfo fi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    (void)fi;
    VkDeviceMemory m; VK(vkAllocateMemory(dev, &ai, NULL, &m)); VK(vkBindBufferMemory(dev, *b, m, 0));
    VK(vkMapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, map)); fill(*map, size, how);
}
static uint32_t *first_ssbo; static size_t first_ssbo_n;
typedef struct { VkImage im; VkImageView v; } img_t;
static img_t mkimg(int dim, int arrayed, int cube, int storage, const char *how, VkCommandBuffer cb) {
    uint32_t w = 64, h = dim == 0 ? 1 : 64, d = dim == 2 ? 8 : 1, layers = cube ? 6 : arrayed ? 4 : 1;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0, dim == 0 ? VK_IMAGE_TYPE_1D : dim == 2 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
                            VK_FORMAT_R32_SFLOAT, {w, h, d}, 1, layers, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                            (storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0) | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    img_t r; VK(vkCreateImage(dev, &ci, NULL, &r.im));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, r.im, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, memtype(mr.memoryTypeBits, 0)};
    VkDeviceMemory m; VK(vkAllocateMemory(dev, &ai, NULL, &m)); VK(vkBindImageMemory(dev, r.im, m, 0));
    VkImageViewType vt = cube ? VK_IMAGE_VIEW_TYPE_CUBE : dim == 0 ? (arrayed ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D) : dim == 2 ? VK_IMAGE_VIEW_TYPE_3D : arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, r.im, vt, VK_FORMAT_R32_SFLOAT, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers}};
    VK(vkCreateImageView(dev, &vi, NULL, &r.v));
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, r.im, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    if (strcmp(how, "zero")) { VkClearColorValue c; c.float32[0] = !strcmp(how, "ones") ? 1.f : !strcmp(how, "ramp") ? 0.5f : 0.25f; c.float32[1] = c.float32[2] = c.float32[3] = c.float32[0];
        VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers}; vkCmdClearColorImage(cb, r.im, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &sr); }
    return r;
}

int main(int argc, char **argv) {
    if (argc < 7) { fprintf(stderr, "usage: %s shader.spv x y z timeout fill binding...\n", argv[0]); return 1; }
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET); uint32_t *code = malloc(sz); if (fread(code, 1, sz, f) != (size_t)sz) return 1; fclose(f);
    uint32_t gx = atoi(argv[2]), gy = atoi(argv[3]), gz = atoi(argv[4]); double tmo = atof(argv[5]); const char *how = argv[6];

    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "probe", 1, "probe", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    VkInstance inst; if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { puts("no Vulkan instance"); return 77; }
    uint32_t n = 1; if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) { puts("no Vulkan device"); return 77; }
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
    if (props.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU && !getenv("PROBE_ALLOW_GPU")) { printf("refusing to run on %s (not a CPU device): set VK_DRIVER_FILES to lavapipe\n", props.deviceName); return 77; }
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL); VkQueueFamilyProperties qp[16]; if (nq > 16) nq = 16; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    for (qf = 0; qf < nq && !(qp[qf].queueFlags & VK_QUEUE_COMPUTE_BIT); qf++);
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    vkGetPhysicalDeviceFeatures2(pd, &f2); f2.features.robustBufferAccess = 0; f13.robustImageAccess = 0;
    float pr = 1; VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qf, 1, &pr};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &qi};
    VK(vkCreateDevice(pd, &dci, NULL, &dev)); vkGetDeviceQueue(dev, qf, 0, &q);

    VkDescriptorSetLayoutBinding lb[32]; int nb = 0; VkDescriptorPoolSize ps[8]; int nps = 0;
    VkDescriptorBufferInfo bi[32]; VkDescriptorImageInfo ii[32]; VkBufferView bv[32]; VkWriteDescriptorSet wr[32];
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qf}; VkCommandPool cp; VK(vkCreateCommandPool(dev, &cpi, NULL, &cp));
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1}; VkCommandBuffer cb; VK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}; VK(vkBeginCommandBuffer(cb, &cbi));
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT};
    VkSampler smp; VK(vkCreateSampler(dev, &sci, NULL, &smp));
    VkDescriptorType types[32]; int counts[16] = {0}; VkDescriptorType tlist[16]; int ntl = 0;
    for (int a = 7; a < argc; a++) {
        unsigned bnd; char kind[16] = {0}; size_t size = 0; unsigned long sz2 = 0;
        if (sscanf(argv[a], "%u:%15[a-z0-9]:%lu", &bnd, kind, &sz2) < 2) { fprintf(stderr, "bad binding %s\n", argv[a]); return 1; }
        size = sz2;
        VkDescriptorType t; int bi_ = nb;
        wr[nb] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, VK_NULL_HANDLE, bnd, 0, 1};
        if (!strcmp(kind, "ubo")) { t = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; size = size ? size : 65536; VkBuffer b; void *mp_; mkbuf(size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, how, &b, &mp_); bi[nb] = (VkDescriptorBufferInfo){b, 0, size}; wr[nb].pBufferInfo = &bi[nb]; }
        else if (!strcmp(kind, "ssbo")) { t = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; size = size ? size : 4 << 20; VkBuffer b; void *mp_; mkbuf(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, how, &b, &mp_); if (!first_ssbo) { first_ssbo = mp_; first_ssbo_n = size / 4; } bi[nb] = (VkDescriptorBufferInfo){b, 0, size}; wr[nb].pBufferInfo = &bi[nb]; }
        else if (!strcmp(kind, "utb") || !strcmp(kind, "stb")) {
            int st = kind[0] == 's'; t = st ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER; size = size ? size : 65536; VkBuffer b; void *mp_;
            mkbuf(size, st ? VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT : VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT, how, &b, &mp_);
            VkBufferViewCreateInfo vi = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, NULL, 0, b, VK_FORMAT_R32_SFLOAT, 0, size}; VK(vkCreateBufferView(dev, &vi, NULL, &bv[nb])); wr[nb].pTexelBufferView = &bv[nb]; }
        else if (!strcmp(kind, "smp")) { t = VK_DESCRIPTOR_TYPE_SAMPLER; ii[nb] = (VkDescriptorImageInfo){smp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED}; wr[nb].pImageInfo = &ii[nb]; }
        else {
            int cis = !strncmp(kind, "cis", 3) || !strcmp(kind, "cubes"), st = !strncmp(kind, "img", 3);
            const char *dn = cis ? kind + 3 : kind + 3; int cube = !strcmp(kind, "cubes"), dim = cube ? 1 : dn[0] == '1' ? 0 : dn[0] == '3' ? 2 : 1, arr = strstr(dn, "da") != NULL;
            t = st ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : cis ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            img_t im = mkimg(dim, arr, cube, st, how, cb);
            ii[nb] = (VkDescriptorImageInfo){cis ? smp : VK_NULL_HANDLE, im.v, VK_IMAGE_LAYOUT_GENERAL}; wr[nb].pImageInfo = &ii[nb];
        }
        wr[nb].descriptorType = t; lb[nb] = (VkDescriptorSetLayoutBinding){bnd, t, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}; types[nb] = t; nb++; (void)bi_;
        int found = 0; for (int i = 0; i < ntl; i++) if (tlist[i] == t) { counts[i]++; found = 1; } if (!found) { tlist[ntl] = t; counts[ntl++] = 1; }
    }
    for (int i = 0; i < ntl; i++) ps[nps++] = (VkDescriptorPoolSize){tlist[i], (uint32_t)counts[i]};
    VkDescriptorSetLayoutCreateInfo dli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, (uint32_t)nb, lb}; VkDescriptorSetLayout dsl; VK(vkCreateDescriptorSetLayout(dev, &dli, NULL, &dsl));
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, (uint32_t)nps, ps}; VkDescriptorPool dp; VK(vkCreateDescriptorPool(dev, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &dsl}; VkDescriptorSet ds; VK(vkAllocateDescriptorSets(dev, &dai, &ds));
    for (int i = 0; i < nb; i++) wr[i].dstSet = ds;
    if (nb) vkUpdateDescriptorSets(dev, nb, wr, 0, NULL);
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl}; VkPipelineLayout pl; VK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));
    VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, (size_t)sz, code}; VkShaderModule sm; VK(vkCreateShaderModule(dev, &smi, NULL, &sm));
    VkComputePipelineCreateInfo pci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", NULL}, pl};
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    VkPipeline pipe; VK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pci, NULL, &pipe));
    clock_gettime(CLOCK_MONOTONIC, &t1); double cms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe); vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, gx, gy, gz); VK(vkEndCommandBuffer(cb));
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fe; VK(vkCreateFence(dev, &fi, NULL, &fe));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb};
    clock_gettime(CLOCK_MONOTONIC, &t0);
    VK(vkQueueSubmit(q, 1, &si, fe));
    VkResult r = vkWaitForFences(dev, 1, &fe, VK_TRUE, (uint64_t)(tmo * 1e9));
    clock_gettime(CLOCK_MONOTONIC, &t1); double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    if (r == VK_TIMEOUT) { printf("TIMEOUT after %.0f ms (pipeline %.0f ms) on %s\n", ms, cms, props.deviceName); fflush(stdout); _exit(3); }
    if (r != VK_SUCCESS) { printf("wait failed %d\n", r); _exit(1); }
    printf("TERMINATED %.0f ms (pipeline %.0f ms) on %s\n", ms, cms, props.deviceName);
    if (getenv("PROBE_DUMP") && first_ssbo) { size_t k = (size_t)atoi(getenv("PROBE_DUMP")); printf("WORDS"); for (size_t i = 0; i < k && i < first_ssbo_n; i++) printf(" %u", first_ssbo[i]); printf("\n"); }
    fflush(stdout);
    _exit(0);
}
