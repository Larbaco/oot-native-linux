// Vulkan instance and device bring-up, device memory, one-shot command buffers, deferred destruction (docs/m5-vulkan.md §2).
// Written from the Vulkan 1.3 specification. The list of optional features follows what the NVN replay needs; Ryujinx's VulkanInitialization.cs (MIT,
// git.ryujinx.app) was read for which of them a Switch GPU's SPIR-V commonly uses.
#include "vk_internal.h"
#include "game_profile.h"
#include <stdio.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

vk_device g_vk;

const char *vk_vendor_name(uint32_t v) {
    switch (v) { case 0x10de: return "NVIDIA"; case 0x1002: return "AMD"; case 0x8086: return "Intel"; case 0x13b5: return "Arm"; case 0x5143: return "Qualcomm"; case 0x106b: return "Apple"; }
    return "other";
}

static VKAPI_ATTR VkBool32 VKAPI_CALL on_debug(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)type; (void)user;
    if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) { uint32_t n = atomic_fetch_add(&g_vk.val_errors, 1); if (n < 200) VKRED("validation: %s", d->pMessage); }
    else if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) { uint32_t n = atomic_fetch_add(&g_vk.val_warnings, 1); if (n < 50) VKLOG("validation warning: %s", d->pMessage); }
    return VK_FALSE;
}

void vk_name(uint64_t handle, VkObjectType type, const char *name) {
    if (!g_vk.validation || !vkSetDebugUtilsObjectNameEXT || !handle) return;
    VkDebugUtilsObjectNameInfoEXT n = {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, NULL, type, handle, name};
    vkSetDebugUtilsObjectNameEXT(g_vk.dev, &n);
}

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) { for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1; return 0; }

// ---------- Optional device extensions with a feature structure: queried and enabled together (every feature the device has, minus the costly robustness ones) ----------
typedef struct { const char *ext, *alt; VkStructureType st; size_t size; int *flag; } ext_feat;
typedef struct { VkStructureType sType; void *pNext; VkBool32 b[64]; } feat_any;   // every feature structure is sType, pNext, then VkBool32s

static int count_true(const feat_any *f, size_t size) { int n = 0; size_t k = (size - offsetof(feat_any, b)) / sizeof(VkBool32); for (size_t i = 0; i < k; i++) n += f->b[i] != 0; return n; }

int vk_dev_open(const vk_dev_opts *o, const char *const *inst_exts, uint32_t n_inst_exts) {
    if (g_vk.up) return 0;
    plat_rw_init(&g_vk.qlock);
    if (vkf_load_global()) { VKRED("no Vulkan loader (vulkan-1.dll): the Vulkan renderer cannot start"); return -1; }
    uint32_t iv = VK_API_VERSION_1_0; if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&iv);
    if (iv < VK_API_VERSION_1_3) { VKRED("the Vulkan loader is version %u.%u, 1.3 is needed", VK_API_VERSION_MAJOR(iv), VK_API_VERSION_MINOR(iv)); return -1; }

    // Instance: the window system's extensions, debug utils, surface maintenance; validation when asked and installed
    uint32_t nie = 0; vkEnumerateInstanceExtensionProperties(NULL, &nie, NULL);
    VkExtensionProperties *ie = calloc(nie + 1, sizeof *ie); vkEnumerateInstanceExtensionProperties(NULL, &nie, ie);
    const char *exts[32]; uint32_t ne = 0;
    for (uint32_t i = 0; i < n_inst_exts && ne < 24; i++) exts[ne++] = inst_exts[i];
    if (has_ext(ie, nie, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) { exts[ne++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME; g_vk.has.debug_utils = 1; }
    if (!o->headless && has_ext(ie, nie, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) && has_ext(ie, nie, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME)) {
        exts[ne++] = VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME; exts[ne++] = VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME; g_vk.has.surface_maint1 = 1; }
    free(ie);
    const char *layers[1]; uint32_t nl = 0;
    if (o->validation) {
        uint32_t n = 0; vkEnumerateInstanceLayerProperties(&n, NULL);
        VkLayerProperties *lp = calloc(n + 1, sizeof *lp); vkEnumerateInstanceLayerProperties(&n, lp);
        for (uint32_t i = 0; i < n; i++) if (!strcmp(lp[i].layerName, "VK_LAYER_KHRONOS_validation")) { layers[nl++] = "VK_LAYER_KHRONOS_validation"; g_vk.validation = 1; }
        free(lp);
        if (!g_vk.validation) VKLOG("validation asked for, but VK_LAYER_KHRONOS_validation is not installed");
    }
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, DM_GAME_SLUG "-native", 1, "nxrt", 1, VK_API_VERSION_1_3};
    VkDebugUtilsMessengerCreateInfoEXT dci = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT, NULL, 0,
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT, on_debug, NULL};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, g_vk.validation && g_vk.has.debug_utils ? &dci : NULL, 0, &app, nl, layers, ne, exts};
    VkResult r = vkCreateInstance(&ici, NULL, &g_vk.inst);
    if (r != VK_SUCCESS) { VKRED("vkCreateInstance failed (%d)", r); return -1; }
    if (vkf_load_instance(g_vk.inst)) { VKRED("the Vulkan instance lacks core entry points"); return -1; }
    if (g_vk.validation && vkCreateDebugUtilsMessengerEXT) vkCreateDebugUtilsMessengerEXT(g_vk.inst, &dci, NULL, &g_vk.dbg);

    // Physical device: NXRT_VK_DEVICE=<index or part of the name>, else the first discrete GPU with Vulkan 1.3, else the first 1.3 device
    uint32_t npd = 0; vkEnumeratePhysicalDevices(g_vk.inst, &npd, NULL);
    VkPhysicalDevice *pds = calloc(npd + 1, sizeof *pds); vkEnumeratePhysicalDevices(g_vk.inst, &npd, pds);
    int pick = -1, pick_discrete = 0;
    for (uint32_t i = 0; i < npd; i++) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p);
        VKLOG("GPU %u: %s (%s, Vulkan %u.%u.%u, driver %#x, type %d)", i, p.deviceName, vk_vendor_name(p.vendorID), VK_API_VERSION_MAJOR(p.apiVersion),
              VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion, p.deviceType);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        if (o->device_hint && *o->device_hint) {
            char *end; long k = strtol(o->device_hint, &end, 10);
            if ((!*end && k == (long)i) || (*end && strstr(p.deviceName, o->device_hint))) { pick = (int)i; pick_discrete = 2; }
            continue;
        }
        int disc = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        if (pick < 0 || (disc && !pick_discrete)) { pick = (int)i; pick_discrete = disc; }
    }
    if (pick < 0) { VKRED("no Vulkan 1.3 GPU%s%s", o->device_hint ? " matching NXRT_VK_DEVICE=" : "", o->device_hint ? o->device_hint : ""); free(pds); return -1; }
    g_vk.pd = pds[pick]; free(pds);
    vkGetPhysicalDeviceProperties(g_vk.pd, &g_vk.props);
    vkGetPhysicalDeviceMemoryProperties(g_vk.pd, &g_vk.memp);
    g_vk.api = g_vk.props.apiVersion;

    // Queue: one family with graphics, compute and transfer (and sparse binding when it has it); presentation is checked when the surface exists
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(g_vk.pd, &nq, NULL);
    VkQueueFamilyProperties *qf = calloc(nq + 1, sizeof *qf); vkGetPhysicalDeviceQueueFamilyProperties(g_vk.pd, &nq, qf);
    int fam = -1;
    for (uint32_t i = 0; i < nq; i++) {
        VkQueueFlags need = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        if ((qf[i].queueFlags & need) != need) continue;
        if (fam < 0 || ((qf[i].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) && !(qf[fam].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT))) fam = (int)i;
    }
    if (fam < 0) { VKRED("%s has no graphics + compute queue", g_vk.props.deviceName); free(qf); return -1; }
    g_vk.qfam = (uint32_t)fam;
    int q_sparse = (qf[fam].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) != 0;
    free(qf);

    // Features: core 1.0-1.3, then the optional extensions' structures
    uint32_t nde = 0; vkEnumerateDeviceExtensionProperties(g_vk.pd, NULL, &nde, NULL);
    VkExtensionProperties *de = calloc(nde + 1, sizeof *de); vkEnumerateDeviceExtensionProperties(g_vk.pd, NULL, &nde, de);
    if (!has_ext(de, nde, VK_KHR_SWAPCHAIN_EXTENSION_NAME) && !o->headless) { VKRED("%s has no VK_KHR_swapchain", g_vk.props.deviceName); free(de); return -1; }
    vk_caps *h = &g_vk.has;
    ext_feat ef[] = {
        {VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME, NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, sizeof(VkPhysicalDeviceCustomBorderColorFeaturesEXT), &h->custom_border},
        {VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME, NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, sizeof(VkPhysicalDeviceTransformFeedbackFeaturesEXT), &h->xfb},
        {VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME, NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT, sizeof(VkPhysicalDeviceDepthClipControlFeaturesEXT), &h->depth_clip_control},
        {VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME, VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_KHR, sizeof(VkPhysicalDeviceIndexTypeUint8FeaturesKHR), &h->index_u8},
        {VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME, NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT, sizeof(VkPhysicalDeviceConditionalRenderingFeaturesEXT), &h->cond_render},
        {VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME, NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT, sizeof(VkPhysicalDeviceProvokingVertexFeaturesEXT), &h->provoking_last},
        {VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, sizeof(VkPhysicalDeviceRobustness2FeaturesEXT), &h->null_descriptor},
        {VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME, NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT, sizeof(VkPhysicalDeviceExtendedDynamicState2FeaturesEXT), &h->eds2_logic_op},
        {VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT, sizeof(VkPhysicalDeviceExtendedDynamicState3FeaturesEXT), &h->eds3},
        {VK_KHR_PRESENT_ID_EXTENSION_NAME, NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, sizeof(VkPhysicalDevicePresentIdFeaturesKHR), &h->present_id},
        {VK_KHR_PRESENT_WAIT_EXTENSION_NAME, NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR, sizeof(VkPhysicalDevicePresentWaitFeaturesKHR), &h->present_wait},
        {VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME, NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT, sizeof(VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT), &h->swap_maint1},
        {VK_KHR_LINE_RASTERIZATION_EXTENSION_NAME, VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_KHR, sizeof(VkPhysicalDeviceLineRasterizationFeaturesKHR), &h->line_raster},
        {VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR, sizeof(VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR), &h->vertex_divisor},
        {VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME, NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT, sizeof(VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT), &h->prim_list_restart},
        {VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME, NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT, sizeof(VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT), &h->fs_interlock},
        {VK_EXT_BLEND_OPERATION_ADVANCED_EXTENSION_NAME, NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BLEND_OPERATION_ADVANCED_FEATURES_EXT, sizeof(VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT), &h->blend_adv},
        {VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME, NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT, sizeof(VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT), &h->feedback_loop},
        {VK_EXT_COLOR_WRITE_ENABLE_EXTENSION_NAME, NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COLOR_WRITE_ENABLE_FEATURES_EXT, sizeof(VkPhysicalDeviceColorWriteEnableFeaturesEXT), &h->color_write_enable},
        {VK_EXT_DEPTH_CLAMP_ZERO_ONE_EXTENSION_NAME, NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLAMP_ZERO_ONE_FEATURES_EXT, sizeof(VkPhysicalDeviceDepthClampZeroOneFeaturesEXT), &h->depth_clamp_01},
        {VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME, NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT, sizeof(VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT), &h->gpl},
        {VK_EXT_4444_FORMATS_EXTENSION_NAME, NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT, sizeof(VkPhysicalDevice4444FormatsFeaturesEXT), &h->fmt_4444},
        {VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME, NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT, sizeof(VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT), &h->vertex_input_dynamic},   // the replay (M5-C)
        {VK_EXT_DEVICE_FAULT_EXTENSION_NAME, NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, sizeof(VkPhysicalDeviceFaultFeaturesEXT), &h->device_fault},   // vk_lost's report
    };
    enum { NEF = sizeof ef / sizeof *ef };
    // Extensions without a feature structure
    struct { const char *ext; int *flag; } ex[] = {
        {VK_NV_VIEWPORT_SWIZZLE_EXTENSION_NAME, &h->viewport_swizzle_nv}, {VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, &h->memory_budget},
        {VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME, &h->stencil_export}, {VK_NV_GEOMETRY_SHADER_PASSTHROUGH_EXTENSION_NAME, &h->gs_passthrough_nv},
        {VK_NV_VIEWPORT_ARRAY_2_EXTENSION_NAME, &h->viewport_array2_nv}, {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, &h->push_descriptor},
        {VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME, NULL},   // with the graphics pipeline library only
        {VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME, &h->checkpoints},
        {VK_AMD_BUFFER_MARKER_EXTENSION_NAME, &h->buffer_marker},   // NXRT_VK_MARKERS=1: the watchdog's report names the commands the GPU had started and not finished (RADV has no NV checkpoints)   // vk_lost's report; NXRT_VK_CHECKPOINTS=0 leaves it off
    };
    const char *ck = getenv("NXRT_VK_CHECKPOINTS");
    feat_any *fs[NEF]; const char *dexts[64]; uint32_t nd = 0;
    if (!o->headless) dexts[nd++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    void *chain = NULL;
    for (int i = 0; i < NEF; i++) {
        fs[i] = NULL;
        const char *name = has_ext(de, nde, ef[i].ext) ? ef[i].ext : ef[i].alt && has_ext(de, nde, ef[i].alt) ? ef[i].alt : NULL;
        if (!name) continue;
        if (!strcmp(name, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME) && (!g_vk.has.surface_maint1 || o->headless)) continue;
        if ((!strcmp(name, VK_KHR_PRESENT_ID_EXTENSION_NAME) || !strcmp(name, VK_KHR_PRESENT_WAIT_EXTENSION_NAME)) && o->headless) continue;
        fs[i] = calloc(1, sizeof(feat_any)); fs[i]->sType = ef[i].st; fs[i]->pNext = chain; chain = fs[i];
        dexts[nd++] = name;
    }
    f13.pNext = chain;
    vkGetPhysicalDeviceFeatures2(g_vk.pd, &f2);
    for (unsigned i = 0; i < sizeof ex / sizeof *ex; i++) {
        if (!has_ext(de, nde, ex[i].ext)) continue;
        if (!ex[i].flag && !(has_ext(de, nde, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME))) continue;
        if (ex[i].flag == &h->checkpoints && ck && *ck == '0') continue;
        if (ex[i].flag == &h->buffer_marker && !(getenv("NXRT_VK_MARKERS") && atoi(getenv("NXRT_VK_MARKERS")))) continue;
        dexts[nd++] = ex[i].ext; if (ex[i].flag) *ex[i].flag = 1;
    }
    if (h->checkpoints && ck && !strcmp(ck, "all")) h->checkpoints = 2;
    free(de);
    // Required: what the replay cannot do without (docs/m5-vulkan.md §2)
    const char *miss = NULL;
    if (!f13.dynamicRendering) miss = "dynamicRendering"; else if (!f13.synchronization2) miss = "synchronization2"; else if (!f12.timelineSemaphore) miss = "timelineSemaphore";
    else if (!f12.descriptorIndexing || !f12.runtimeDescriptorArray || !f12.descriptorBindingPartiallyBound || !f12.shaderSampledImageArrayNonUniformIndexing) miss = "descriptor indexing";
    else if (!f2.features.textureCompressionBC) miss = "textureCompressionBC";
    else if (!f13.maintenance4) miss = "maintenance4";
    if (miss) { VKRED("%s lacks %s, which the renderer needs", g_vk.props.deviceName, miss); return -1; }
    // Enabled: every supported feature except robustness that costs performance (robustBufferAccess stays on NVIDIA, as Ryujinx does) and capture/replay addresses
    VkPhysicalDeviceFeatures en = f2.features;
    en.robustBufferAccess = g_vk.props.vendorID == 0x10de && f2.features.robustBufferAccess;
    en.inheritedQueries = 0;
    f13.robustImageAccess = 0; f12.bufferDeviceAddressCaptureReplay = 0; f12.bufferDeviceAddressMultiDevice = 0; f11.protectedMemory = 0;
    for (int i = 0; i < NEF; i++) {
        if (!fs[i]) continue;
        if (ef[i].st == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT) {
            VkPhysicalDeviceRobustness2FeaturesEXT *r2 = (void *)fs[i]; r2->robustBufferAccess2 = 0; r2->robustImageAccess2 = 0; h->null_descriptor = r2->nullDescriptor != 0; continue; }
        if (ef[i].st == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT) {
            VkPhysicalDeviceCustomBorderColorFeaturesEXT *c = (void *)fs[i]; h->custom_border = c->customBorderColors != 0; h->custom_border_noformat = c->customBorderColorWithoutFormat != 0; continue; }
        if (ef[i].st == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT) {
            VkPhysicalDeviceTransformFeedbackFeaturesEXT *x = (void *)fs[i]; h->xfb = x->transformFeedback != 0; h->xfb_streams = x->geometryStreams != 0; continue; }
        if (ef[i].st == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT) {
            VkPhysicalDeviceExtendedDynamicState2FeaturesEXT *x = (void *)fs[i]; h->eds2_logic_op = x->extendedDynamicState2LogicOp != 0; h->eds2_patch_points = x->extendedDynamicState2PatchControlPoints != 0; continue; }
        if (ef[i].st == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT) {
            VkPhysicalDeviceProvokingVertexFeaturesEXT *x = (void *)fs[i]; x->transformFeedbackPreservesProvokingVertex = x->transformFeedbackPreservesProvokingVertex && h->xfb; h->provoking_last = x->provokingVertexLast != 0; continue; }
        if (ef[i].st == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT) {
            VkPhysicalDeviceFaultFeaturesEXT *x = (void *)fs[i]; x->deviceFaultVendorBinary = 0; h->device_fault = x->deviceFault != 0; continue; }
        *ef[i].flag = count_true(fs[i], ef[i].size) > 0;
    }
    const char *sp = getenv("NXRT_VK_SPARSE");   // test switch (#12): 0 backs every memory pool fully at once instead of sparse granules on first use
    h->sparse_buffer = q_sparse && en.sparseBinding && en.sparseResidencyBuffer && !(sp && *sp == '0');
    h->sparse_aliased = h->sparse_buffer && en.sparseResidencyAliased;
    h->bc = en.textureCompressionBC; h->astc_ldr = en.textureCompressionASTC_LDR;
    h->mirror_clamp_edge = f12.samplerMirrorClampToEdge; h->minmax_filter = f12.samplerFilterMinmax;
    g_vk.f10 = en; g_vk.f11 = f11; g_vk.f12 = f12; g_vk.f13 = f13;
    g_vk.f11.pNext = g_vk.f12.pNext = g_vk.f13.pNext = NULL;
    VkPhysicalDeviceFeatures2 enf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11, en};

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, g_vk.qfam, 1, &prio};
    VkDeviceCreateInfo dci2 = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &enf, 0, 1, &qci, 0, NULL, nd, dexts, NULL};
    r = vkCreateDevice(g_vk.pd, &dci2, NULL, &g_vk.dev);
    for (int i = 0; i < NEF; i++) free(fs[i]);
    if (r != VK_SUCCESS) { VKRED("vkCreateDevice failed (%d) on %s", r, g_vk.props.deviceName); return -1; }
    if (vkf_load_device(g_vk.dev)) { VKRED("the Vulkan device lacks core entry points"); return -1; }
    vkGetDeviceQueue(g_vk.dev, g_vk.qfam, 0, &g_vk.q);
    VkSemaphoreTypeCreateInfo tci = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &tci, 0};
    if (vkCreateSemaphore(g_vk.dev, &sci, NULL, &g_vk.tl) != VK_SUCCESS) { VKRED("cannot create the GPU timeline semaphore"); return -1; }
    vk_name((uint64_t)g_vk.tl, VK_OBJECT_TYPE_SEMAPHORE, "gpu timeline");

    VkPhysicalDeviceProperties *p = &g_vk.props;
    VKLOG("device: %s, %s driver %u.%u (raw %#x), Vulkan %u.%u.%u, queue family %u%s, %u device extensions enabled, validation %s",
          p->deviceName, vk_vendor_name(p->vendorID), p->vendorID == 0x10de ? (p->driverVersion >> 22) & 0x3ff : VK_API_VERSION_MAJOR(p->driverVersion),
          p->vendorID == 0x10de ? (p->driverVersion >> 14) & 0xff : VK_API_VERSION_MINOR(p->driverVersion), p->driverVersion,
          VK_API_VERSION_MAJOR(p->apiVersion), VK_API_VERSION_MINOR(p->apiVersion), VK_API_VERSION_PATCH(p->apiVersion), g_vk.qfam,
          q_sparse ? " (with sparse binding)" : "", nd, g_vk.validation ? "on" : "off");
    VKLOG("features: geometry %d tessellation %d multiViewport %d dualSrcBlend %d logicOp %d depthClamp %d samplerAnisotropy %d BC %d ASTC %d sparse buffers %d "
          "custom border %d/%d transform feedback %d depth clip control %d uint8 index %d conditional rendering %d provoking last %d null descriptor %d "
          "eds2 logic op %d eds3 %d NV viewport swizzle %d memory budget %d present wait %d line raster %d vertex divisor %d list restart %d stencil export %d "
          "fs interlock %d GPL %d mirror clamp to edge %d min/max filter %d bufferDeviceAddress %d shaderInt64 %d shaderFloat16 %d",
          en.geometryShader, en.tessellationShader, en.multiViewport, en.dualSrcBlend, en.logicOp, en.depthClamp, en.samplerAnisotropy, h->bc, h->astc_ldr, h->sparse_buffer,
          h->custom_border, h->custom_border_noformat, h->xfb, h->depth_clip_control, h->index_u8, h->cond_render, h->provoking_last, h->null_descriptor,
          h->eds2_logic_op, h->eds3, h->viewport_swizzle_nv, h->memory_budget, h->present_wait, h->line_raster, h->vertex_divisor, h->prim_list_restart, h->stencil_export,
          h->fs_interlock, h->gpl, h->mirror_clamp_edge, h->minmax_filter, f12.bufferDeviceAddress, en.shaderInt64, f12.shaderFloat16);
    VKLOG("if the GPU stops responding: checkpoints %s, the driver's fault report %s", h->checkpoints == 2 ? "before every dispatch and draw" : h->checkpoints ? "before every dispatch" : "off",
          h->device_fault ? "on" : "not available");
    for (uint32_t i = 0; i < g_vk.memp.memoryHeapCount; i++)
        VKLOG("memory heap %u: %.0f MB%s", i, g_vk.memp.memoryHeaps[i].size / 1048576.0, g_vk.memp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? " device local" : "");
    // What the translated corpus declares (docs/m5-record-shaders.md section 7, track A's 3,967 modules): DrawParameters, GroupNonUniformBallot / Vote / Shuffle,
    // StorageImageRead/WriteWithoutFormat, ImageGatherExtended, Geometry, texel buffers (core), descriptor indexing (required above)
    VkPhysicalDeviceSubgroupProperties sg = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 pp2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &sg};
    vkGetPhysicalDeviceProperties2(g_vk.pd, &pp2);
    h->subgroup_stages = sg.supportedStages; h->subgroup_ops = sg.supportedOperations; h->subgroup_size = sg.subgroupSize;
    VkSubgroupFeatureFlags ops = VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_VOTE_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT;
    VkShaderStageFlags st = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    h->corpus_ok = g_vk.f11.shaderDrawParameters && en.shaderStorageImageReadWithoutFormat && en.shaderStorageImageWriteWithoutFormat && en.shaderImageGatherExtended &&
                   en.geometryShader && (sg.supportedOperations & ops) == ops && (sg.supportedStages & st) == st;
    VKLOG("shader corpus needs: draw parameters %d, formatless storage read/write %d/%d, gather extended %d, geometry %d, subgroup ops %#x in stages %#x (size %u): %s",
          g_vk.f11.shaderDrawParameters, en.shaderStorageImageReadWithoutFormat, en.shaderStorageImageWriteWithoutFormat, en.shaderImageGatherExtended, en.geometryShader,
          sg.supportedOperations, sg.supportedStages, sg.subgroupSize, h->corpus_ok ? "all present" : "[RED] something is missing");
    g_vk.up = 1;
    return 0;
}

void vk_dev_close(void) {
    if (!g_vk.dev) return;
    plat_rw_wrlock(&g_vk.qlock); vkDeviceWaitIdle(g_vk.dev); plat_rw_wrunlock(&g_vk.qlock);
    vk_retire_poll();
    void vk_alloc_shutdown(void); vk_alloc_shutdown();
    if (g_vk.tl) vkDestroySemaphore(g_vk.dev, g_vk.tl, NULL);
    vkDestroyDevice(g_vk.dev, NULL);
    if (g_vk.dbg && vkDestroyDebugUtilsMessengerEXT) vkDestroyDebugUtilsMessengerEXT(g_vk.inst, g_vk.dbg, NULL);
    vkDestroyInstance(g_vk.inst, NULL);
    memset(&g_vk, 0, sizeof g_vk);
}

int vk_mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < g_vk.memp.memoryTypeCount; i++) {
            VkMemoryPropertyFlags f = g_vk.memp.memoryTypes[i].propertyFlags;
            if (!(bits & (1u << i)) || (f & want) != want) continue;
            if (!pass && (f & avoid)) continue;
            return (int)i;
        }
    return -1;
}

int vk_budget(uint64_t *budget, uint64_t *usage) {
    *budget = *usage = 0;
    if (!g_vk.has.memory_budget) return 0;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT b = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 m = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &b};
    vkGetPhysicalDeviceMemoryProperties2(g_vk.pd, &m);
    for (uint32_t i = 0; i < m.memoryProperties.memoryHeapCount; i++)
        if (m.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) { *budget += b.heapBudget[i]; *usage += b.heapUsage[i]; }
    return 1;
}

// ---------- Device memory: per memory type, 64 MB blocks with a first-fit free list; requests of 32 MB and more are dedicated ----------
enum { BLOCK = 64u << 20, DEDICATED_MIN = 32u << 20, MAXBLK = 1024, MAXFREE = 512 };
typedef struct { VkDeviceSize off, size; } span_t;
typedef struct { VkDeviceMemory mem; uint32_t type; void *map; span_t fr[MAXFREE]; int nfr; VkDeviceSize used; } block_t;
static block_t *g_blk[MAXBLK]; static int g_nblk;
static plat_rwlock_t g_alk = PLAT_RWLOCK_INIT;
static _Atomic uint64_t g_dedicated_bytes;

static int block_take(block_t *b, VkDeviceSize size, VkDeviceSize align, VkDeviceSize *off) {
    for (int i = 0; i < b->nfr; i++) {
        VkDeviceSize a = (b->fr[i].off + align - 1) / align * align, end = b->fr[i].off + b->fr[i].size;
        if (a + size > end) continue;
        span_t lead = {b->fr[i].off, a - b->fr[i].off}, tail = {a + size, end - (a + size)};
        if (lead.size && tail.size) {
            if (b->nfr >= MAXFREE) continue;
            memmove(&b->fr[i + 2], &b->fr[i + 1], (size_t)(b->nfr - i - 1) * sizeof(span_t)); b->fr[i] = lead; b->fr[i + 1] = tail; b->nfr++;
        } else if (lead.size) b->fr[i] = lead;
        else if (tail.size) b->fr[i] = tail;
        else { memmove(&b->fr[i], &b->fr[i + 1], (size_t)(b->nfr - i - 1) * sizeof(span_t)); b->nfr--; }
        *off = a; b->used += size;
        return 1;
    }
    return 0;
}
static void block_give(block_t *b, VkDeviceSize off, VkDeviceSize size) {
    int i = 0; while (i < b->nfr && b->fr[i].off < off) i++;
    int left = i > 0 && b->fr[i - 1].off + b->fr[i - 1].size == off, right = i < b->nfr && off + size == b->fr[i].off;
    if (left && right) { b->fr[i - 1].size += size + b->fr[i].size; memmove(&b->fr[i], &b->fr[i + 1], (size_t)(b->nfr - i - 1) * sizeof(span_t)); b->nfr--; }
    else if (left) b->fr[i - 1].size += size;
    else if (right) { b->fr[i].off = off; b->fr[i].size += size; }
    else if (b->nfr < MAXFREE) { memmove(&b->fr[i + 1], &b->fr[i], (size_t)(b->nfr - i) * sizeof(span_t)); b->fr[i] = (span_t){off, size}; b->nfr++; }
    b->used -= size;   // a free list that is full leaks the span until the block is freed: logged by vk_alloc_stats' caller when it matters
}

static int vk_alloc_mem_impl(const VkMemoryRequirements *r, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid, vk_alloc *out);
// A census of who holds device memory: per call site of vk_alloc_mem (its return address), the live and the peak bytes and the live count, for the watchdog's memory line
enum { CEN_N = 48 };
static struct { void *site; _Atomic uint64_t live, peak, n; _Atomic int host; } g_cen[CEN_N]; static _Atomic int g_ncen;
static int census_slot(void *site) {
    int n = atomic_load(&g_ncen);
    for (int i = 0; i < n; i++) if (g_cen[i].site == site) return i;
    int i = atomic_fetch_add(&g_ncen, 1); if (i >= CEN_N) { atomic_store(&g_ncen, CEN_N); return CEN_N - 1; }
    g_cen[i].site = site; return i;
}
void vk_alloc_census(char *out, size_t n) {
    size_t o = 0; int cnt = atomic_load(&g_ncen); out[0] = 0;
    for (int i = 0; i < cnt && i < CEN_N && o + 80 < n; i++) {
        uint64_t l = atomic_load(&g_cen[i].live); if (l < (16u << 20)) continue;
        Dl_info di; uintptr_t off = 0; if (dladdr(g_cen[i].site, &di) && di.dli_fbase) off = (uintptr_t)g_cen[i].site - (uintptr_t)di.dli_fbase;
        o += (size_t)snprintf(out + o, n - o, " [%s %#lx live %lluMB peak %lluMB x%llu]", atomic_load(&g_cen[i].host) ? "host" : "dev", (unsigned long)off, (unsigned long long)(l >> 20), (unsigned long long)(atomic_load(&g_cen[i].peak) >> 20), (unsigned long long)atomic_load(&g_cen[i].live) ? (unsigned long long)atomic_load(&g_cen[i].n) : 0ull);
    }
}
static void census_add(vk_alloc *out, void *site, int host) {
    int s = census_slot(site); out->tag = s + 1;
    uint64_t l = atomic_fetch_add(&g_cen[s].live, out->size) + out->size; atomic_fetch_add(&g_cen[s].n, 1); atomic_store(&g_cen[s].host, host);
    uint64_t pk = atomic_load(&g_cen[s].peak); while (l > pk && !atomic_compare_exchange_weak(&g_cen[s].peak, &pk, l)) {}
}
int vk_alloc_mem(const VkMemoryRequirements *r, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid, vk_alloc *out) {
    int rc = vk_alloc_mem_impl(r, want, avoid, out);
    if (!rc) census_add(out, __builtin_return_address(0), (g_vk.memp.memoryTypes[vk_mem_type(r->memoryTypeBits, want, avoid)].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0);
    return rc;
}
static int vk_alloc_mem_impl(const VkMemoryRequirements *r, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid, vk_alloc *out) {
    memset(out, 0, sizeof *out); out->block = -1;
    int type = vk_mem_type(r->memoryTypeBits, want, avoid);
    if (type < 0) return -1;
    int host = (g_vk.memp.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    VkDeviceSize align = r->alignment ? r->alignment : 1;
    if (host && g_vk.props.limits.nonCoherentAtomSize > align) align = g_vk.props.limits.nonCoherentAtomSize;
    if (r->size >= DEDICATED_MIN) {
        VkMemoryAllocateFlagsInfo fl = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, NULL, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};   // pool mirrors are read through device addresses (vk_mem.c gmem_*)
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, g_vk.f12.bufferDeviceAddress ? &fl : NULL, r->size, (uint32_t)type};
        if (vkAllocateMemory(g_vk.dev, &ai, NULL, &out->mem) != VK_SUCCESS) return -1;
        out->size = r->size;
        if (host && vkMapMemory(g_vk.dev, out->mem, 0, VK_WHOLE_SIZE, 0, &out->map) != VK_SUCCESS) { vkFreeMemory(g_vk.dev, out->mem, NULL); return -1; }
        atomic_fetch_add(&g_dedicated_bytes, r->size);
        return 0;
    }
    plat_rw_wrlock(&g_alk);
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < g_nblk; i++) {
            block_t *b = g_blk[i];
            if (!b || b->type != (uint32_t)type) continue;
            VkDeviceSize off;
            if (block_take(b, r->size, align, &off)) {
                out->mem = b->mem; out->off = off; out->size = r->size; out->block = i; out->map = b->map ? (uint8_t *)b->map + off : NULL;
                plat_rw_wrunlock(&g_alk); return 0;
            }
        }
        if (pass || g_nblk >= MAXBLK) break;
        block_t *b = calloc(1, sizeof *b);
        VkMemoryAllocateFlagsInfo fl = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, NULL, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, g_vk.f12.bufferDeviceAddress ? &fl : NULL, BLOCK, (uint32_t)type};
        if (!b || vkAllocateMemory(g_vk.dev, &ai, NULL, &b->mem) != VK_SUCCESS) { free(b); break; }
        if (host && vkMapMemory(g_vk.dev, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map) != VK_SUCCESS) { vkFreeMemory(g_vk.dev, b->mem, NULL); free(b); break; }
        b->type = (uint32_t)type; b->fr[0] = (span_t){0, BLOCK}; b->nfr = 1;
        int slot = -1; for (int i = 0; i < g_nblk; i++) if (!g_blk[i]) { slot = i; break; }
        if (slot < 0) slot = g_nblk++;
        g_blk[slot] = b;
    }
    plat_rw_wrunlock(&g_alk);
    return -1;
}

void vk_free_mem(vk_alloc *a) {
    if (!a->mem) return;
    if (a->tag > 0 && a->tag <= CEN_N) atomic_fetch_sub(&g_cen[a->tag - 1].live, a->size);
    if (a->block < 0) { atomic_fetch_sub(&g_dedicated_bytes, a->size); vkFreeMemory(g_vk.dev, a->mem, NULL); }
    else { plat_rw_wrlock(&g_alk); if (g_blk[a->block]) block_give(g_blk[a->block], a->off, a->size); plat_rw_wrunlock(&g_alk); }
    memset(a, 0, sizeof *a); a->block = -1;
}

void vk_alloc_shutdown(void) {   // the blocks themselves (everything in them is gone by now)
    plat_rw_wrlock(&g_alk);
    for (int i = 0; i < g_nblk; i++) if (g_blk[i]) { vkFreeMemory(g_vk.dev, g_blk[i]->mem, NULL); free(g_blk[i]); g_blk[i] = NULL; }
    g_nblk = 0;
    plat_rw_wrunlock(&g_alk);
}
void vk_alloc_bytes_nolock(uint64_t *block_bytes, uint64_t *dedicated, uint64_t *used, uint64_t *host_blocks) {
    uint64_t u = 0, h = 0, n = 0; int cnt = g_nblk;
    for (int i = 0; i < cnt && i < MAXBLK; i++) { block_t *b = g_blk[i]; if (!b) continue; n++; u += b->used; if (g_vk.memp.memoryTypes[b->type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) h++; }
    *block_bytes = n * BLOCK; *dedicated = atomic_load(&g_dedicated_bytes); *used = u; *host_blocks = h;
}   // for the watchdog: no lock, a racy block count is fine
void vk_alloc_stats(uint64_t *blocks, uint64_t *block_bytes, uint64_t *used, uint64_t *dedicated) {
    plat_rw_rdlock(&g_alk);
    uint64_t n = 0, u = 0; for (int i = 0; i < g_nblk; i++) if (g_blk[i]) { n++; u += g_blk[i]->used; }
    plat_rw_rdunlock(&g_alk);
    *blocks = n; *block_bytes = n * BLOCK; *used = u; *dedicated = atomic_load(&g_dedicated_bytes);
}

uint64_t vk_tl_value(void) { uint64_t v = 0; if (g_vk.tl) vkGetSemaphoreCounterValue(g_vk.dev, g_vk.tl, &v); return v; }
int vk_tl_wait(uint64_t value, uint64_t timeout_ns) {
    if (!value) return 1;
    VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &g_vk.tl, &value};
    VkResult r = vkWaitSemaphores(g_vk.dev, &wi, timeout_ns);
    vk_lost("a wait for the GPU", r);
    return r == VK_SUCCESS;
}

// ---------- A lost device (#12, #17): a checkpoint before each dispatch (and draw) names what the GPU had started and finished when it stopped ----------
enum { CK_N = 4096 };
static struct { uint64_t seq; char kind, indirect, key[17]; uint32_t x, y, z; } g_ck[CK_N];
static _Atomic uint64_t g_ck_seq;

// AMD buffer markers (NXRT_VK_MARKERS=1): at every dispatch and draw two 32-bit words in a host-visible buffer get the sequence number, one by the command processor when it reaches the
// command (top of pipe: "started"), one when everything before it has finished (all commands: "finished before"). After a hang the watchdog reads both without asking the driver:
// the commands from finished-before to started are the ones in flight. Each marker drains the GPU like a barrier, so this is a diagnostic
static struct { VkBuffer buf; vk_alloc mem; volatile uint32_t *map; int ok, tried; } g_mk;
static void marker_init(void) {
    g_mk.tried = 1;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (vkCreateBuffer(g_vk.dev, &bci, NULL, &g_mk.buf) != VK_SUCCESS) return;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, g_mk.buf, &mr);
    if (vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, &g_mk.mem)) { vkDestroyBuffer(g_vk.dev, g_mk.buf, NULL); g_mk.buf = VK_NULL_HANDLE; return; }
    vkBindBufferMemory(g_vk.dev, g_mk.buf, g_mk.mem.mem, g_mk.mem.off);
    g_mk.map = (volatile uint32_t *)g_mk.mem.map; for (int i = 0; i < 16; i++) g_mk.map[i] = 0;
    g_mk.ok = 1;
    VKLOG("AMD buffer markers on: the watchdog names the commands the GPU had started and not finished");
}
static uint64_t marker_note(char kind, const char *key, uint32_t x, uint32_t y, uint32_t z, int indirect);
static void marker_write(VkCommandBuffer cb, uint64_t s) {
    vkCmdWriteBufferMarker2AMD(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, g_mk.buf, 0, (uint32_t)s);
    vkCmdWriteBufferMarker2AMD(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g_mk.buf, 4, (uint32_t)s);
}

void vk_checkpoint(VkCommandBuffer cb, char kind, const char *key, uint32_t x, uint32_t y, uint32_t z, int indirect) {
    if (g_vk.has.buffer_marker && vkCmdWriteBufferMarker2AMD) {
        if (!g_mk.tried) marker_init();
        if (g_mk.ok) marker_write(cb, marker_note(kind, key, x, y, z, indirect));
    }
    if (!vkCmdSetCheckpointNV || g_vk.has.checkpoints < (kind == 'd' ? 2 : 1)) return;
    uint64_t s = atomic_fetch_add(&g_ck_seq, 1) + 1;
    g_ck[s % CK_N].seq = s; g_ck[s % CK_N].kind = kind; g_ck[s % CK_N].indirect = (char)indirect;
    g_ck[s % CK_N].x = x; g_ck[s % CK_N].y = y; g_ck[s % CK_N].z = z;
    snprintf(g_ck[s % CK_N].key, sizeof g_ck[0].key, "%s", key ? key : "?");
    vkCmdSetCheckpointNV(cb, (const void *)(uintptr_t)s);
}
// The marker table: what each sequence number was (kind 'c' dispatch, 'd' draw, 'E' the end of a submission, whose value is in x/y)
static struct { uint64_t seq, frame, sub; char kind, indirect, key[17]; uint32_t x, y, z; } g_mkt[CK_N];
static _Atomic uint64_t g_mk_seq;
uint64_t vks_recording_value(void); uint64_t vk_frame_now(void);
static uint64_t marker_note(char kind, const char *key, uint32_t x, uint32_t y, uint32_t z, int indirect) {
    uint64_t s = atomic_fetch_add(&g_mk_seq, 1) + 1; typeof(g_mkt[0]) *e = &g_mkt[s % CK_N];
    e->seq = 0; e->kind = kind; e->indirect = (char)indirect; e->x = x; e->y = y; e->z = z; e->frame = vk_frame_now(); e->sub = vks_recording_value();
    snprintf(e->key, sizeof e->key, "%s", key ? key : "?"); e->seq = s;
    return s;
}
void vk_marker_end(VkCommandBuffer cb, uint64_t value) {
    if (!g_vk.has.buffer_marker || !vkCmdWriteBufferMarker2AMD) return;
    if (!g_mk.tried) marker_init();
    if (g_mk.ok) marker_write(cb, marker_note('E', "end", (uint32_t)value, 0, 0, 0));
}
static void mk_text(const typeof(g_mkt[0]) *e, char *out, size_t n) {
    if (e->kind == 'E') snprintf(out, n, "#%llu the end of the submission that signals %u", (unsigned long long)e->seq, e->x);
    else snprintf(out, n, "#%llu %s %s (frame %llu, submission %llu%s %u,%u,%u)", (unsigned long long)e->seq, e->kind == 'd' ? "draw, fragment/vertex" : "dispatch", e->key, (unsigned long long)e->frame,
                  (unsigned long long)e->sub, e->indirect ? ", indirect, guest words" : ",", e->x, e->y, e->z);
}
int vk_marker_report(char *out, size_t cap) {
    if (!g_mk.ok) return 0;
    uint32_t top = g_mk.map[0], fin = g_mk.map[1]; uint64_t n = atomic_load(&g_mk_seq), t = n - (uint32_t)(n - top), f = n - (uint32_t)(n - fin);
    int o = snprintf(out, cap, " started #%llu, everything before #%llu finished, %llu recorded:", (unsigned long long)t, (unsigned long long)f, (unsigned long long)n);
    for (uint64_t s = f; s <= t + 1 && s <= f + 10 && o + 200 < (int)cap; s++) {
        if (!s || g_mkt[s % CK_N].seq != s) continue;
        char b[200]; mk_text(&g_mkt[s % CK_N], b, sizeof b); o += snprintf(out + o, cap - o, "\n    %s%s", b, s == f ? "  <- first not finished (the cause is it or what is recorded just before it)" : s > t ? "  (not started)" : "");
    }
    return o;
}

static const char *ck_text(uint64_t s, char *out, size_t n) {
    if (!s || g_ck[s % CK_N].seq != s) { snprintf(out, n, "checkpoint #%llu (no longer kept)", (unsigned long long)s); return out; }
    if (g_ck[s % CK_N].kind == 'd') snprintf(out, n, "#%llu a draw with shader %s", (unsigned long long)s, g_ck[s % CK_N].key);
    else snprintf(out, n, "#%llu a dispatch of shader %s, %ux%ux%u groups%s", (unsigned long long)s, g_ck[s % CK_N].key, g_ck[s % CK_N].x, g_ck[s % CK_N].y,
                  g_ck[s % CK_N].z, g_ck[s % CK_N].indirect ? " (indirect: what guest memory held when it was recorded; the GPU reads its own copy)" : "");
    return out;
}

void vk_lost(const char *where, VkResult r) {
    if (r != VK_ERROR_DEVICE_LOST || !g_vk.dev || atomic_exchange(&g_vk.lost, 1)) return;
    VKRED("the GPU stopped responding (device lost, first seen at %s); what it was doing:", where);
    char t[256];
    if (g_vk.has.checkpoints && vkGetQueueCheckpointData2NV) {
        VkCheckpointData2NV cd[32]; uint32_t n = 32; uint64_t lo = 0, hi = 0;
        for (uint32_t i = 0; i < n; i++) cd[i] = (VkCheckpointData2NV){VK_STRUCTURE_TYPE_CHECKPOINT_DATA_2_NV};
        plat_rw_wrlock(&g_vk.qlock); vkGetQueueCheckpointData2NV(g_vk.q, &n, cd); plat_rw_wrunlock(&g_vk.qlock);
        for (uint32_t i = 0; i < n; i++) {
            uint64_t s = (uint64_t)(uintptr_t)cd[i].pCheckpointMarker;
            const char *at = cd[i].stage == VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT ? "started" : cd[i].stage == VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT ? "finished everything before" : "reached";
            VKRED("  the GPU %s %s (stage %#llx)", at, ck_text(s, t, sizeof t), (unsigned long long)cd[i].stage);
            if (cd[i].stage == VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT) lo = s; else if (s > hi) hi = s;
        }
        if (!n) VKRED("  no checkpoint was reached");
        for (uint64_t s = lo + 1; lo && s <= hi && s <= lo + 8; s++) VKRED("  in flight: %s", ck_text(s, t, sizeof t));
    } else VKRED("  (no checkpoints on this GPU: they need NVIDIA's VK_NV_device_diagnostic_checkpoints)");
    if (g_vk.has.device_fault && vkGetDeviceFaultInfoEXT) {
        VkDeviceFaultCountsEXT c = {VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
        VkDeviceFaultAddressInfoEXT ai[8]; VkDeviceFaultVendorInfoEXT vi[8];
        if (vkGetDeviceFaultInfoEXT(g_vk.dev, &c, NULL) == VK_SUCCESS) {
            if (c.addressInfoCount > 8) c.addressInfoCount = 8;
            if (c.vendorInfoCount > 8) c.vendorInfoCount = 8;
            c.vendorBinarySize = 0;
            VkDeviceFaultInfoEXT fi = {VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT, NULL, {0}, c.addressInfoCount ? ai : NULL, c.vendorInfoCount ? vi : NULL, NULL};
            VkResult fr = vkGetDeviceFaultInfoEXT(g_vk.dev, &c, &fi);
            static const char *const aty[] = {"none", "invalid read", "invalid write", "invalid execute", "unknown instruction pointer", "invalid instruction pointer",
                                              "instruction pointer of the fault"};
            if (fr == VK_SUCCESS || fr == VK_INCOMPLETE) {
                VKRED("  the driver's fault report: %s", fi.description[0] ? fi.description : "(no description)");
                for (uint32_t i = 0; i < c.addressInfoCount; i++)
                    VKRED("  fault address: %s at %#llx (within %#llx)", ai[i].addressType < 7 ? aty[ai[i].addressType] : "?", (unsigned long long)ai[i].reportedAddress,
                          (unsigned long long)ai[i].addressPrecision);
                for (uint32_t i = 0; i < c.vendorInfoCount; i++)
                    VKRED("  vendor fault: %s (code %#llx, data %#llx)", vi[i].description, (unsigned long long)vi[i].vendorFaultCode, (unsigned long long)vi[i].vendorFaultData);
            } else VKRED("  the driver's fault report could not be read (%d)", fr);
        } else VKRED("  the driver has no fault report for this loss");
    } else VKRED("  (this driver gives no fault report: VK_EXT_device_fault)");
}

void vk_image_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                      VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, ss, sa, ds, da, from, to, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img,
                               {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &b};
    vkCmdPipelineBarrier2(cb, &di);
}
void vk_mem_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkMemoryBarrier2 b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, ss, sa, ds, da};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &b, 0, NULL, 0, NULL};
    vkCmdPipelineBarrier2(cb, &di);
}

// ---------- Deferred destruction behind the timeline ----------
typedef struct { uint64_t at, presents; void (*fn)(void *); void *arg; } retire_t;
static retire_t *g_ret; static size_t g_nret, g_capret; static plat_rwlock_t g_retlk = PLAT_RWLOCK_INIT;
void vk_retire(void (*fn)(void *), void *arg) {
    plat_rw_wrlock(&g_retlk);
    if (g_nret == g_capret) { g_capret = g_capret ? g_capret * 2 : 256; g_ret = realloc(g_ret, g_capret * sizeof *g_ret); }
    g_ret[g_nret++] = (retire_t){atomic_load(&g_vk.tl_assigned) + 1, atomic_load(&g_vk.presents), fn, arg};   // the submission being recorded may still use it
    plat_rw_wrunlock(&g_retlk);
}
void vk_retire_poll(void) {
    uint64_t done = g_vk.dev ? vk_tl_value() : UINT64_MAX;
    int vks_recording(void);
    if (g_vk.dev && !vks_recording() && atomic_load(&g_vk.tl_submitted) == atomic_load(&g_vk.tl_assigned) && done >= atomic_load(&g_vk.tl_assigned)) done = UINT64_MAX;   // idle (nothing being recorded either): everything may go
    for (;;) {
        retire_t run = {0};
        plat_rw_wrlock(&g_retlk);
        uint64_t pr = atomic_load(&g_vk.presents); int presenting = atomic_load(&g_vk.presenting);
        for (size_t i = 0; i < g_nret; i++) if (g_ret[i].at <= done && (!presenting || pr >= g_ret[i].presents + 3 || done == UINT64_MAX)) { run = g_ret[i]; g_ret[i] = g_ret[--g_nret]; break; }
        plat_rw_wrunlock(&g_retlk);
        if (!run.fn) break;
        run.fn(run.arg);
    }
}
