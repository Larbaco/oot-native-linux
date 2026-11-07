/* The replay (docs/m5-replay.md): pipeline-state translation (blend states as the game records them, factors, advanced modes, compare and stencil
   functions, topologies, front face against the window origin, vertex formats); descriptor mapping (a translated shader's bindings and their kinds, its
   descriptor set, a view format in the class the shader declares, gl_PerVertex trimmed to the device's clip and cull distance limit); copies through the
   record walk (buffer to buffer, fill, update, buffer to texture, texture to texture, a scaled blit, texture to buffer) landing in guest memory; and the
   screenshot path (format conversion, the PNG written and decoded again, a GPU texture captured cropped and turned over, the files the present-number
   and control-file triggers write). M6 phase B: colour and stencil clears through their masks, a GPU copy read back into a storage-backed texture, and the dual-source
   and alpha-test fragment variants drawn. The GPU part is skipped without a device (77 overall only when nothing could run). */
#include "vk_test.h"
#include "vk_pipe.h"
#include "vk_shader.h"
#include "vk_replay.h"
#include "vk_shot.h"
#include "test_pervertex_vert.h"
#include "test_pervertex_cull_vert.h"
#include "test_bind_frag.h"
#include "test_dual_frag.h"
#include "test_tri_vert.h"
#include "nvn_texio.h"

NXRT_EARLY_RESERVE()

static blend_st blend_of(uint64_t raw) { blend_st b; memcpy(&b, &raw, sizeof b); return b; }

// ---------- PNG read back (what vks_png_write writes: 8-bit RGBA, filter 0, stored deflate blocks) ----------
static uint32_t crc_tab[256];
static uint32_t crc(const uint8_t *p, size_t n) {
    if (!crc_tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1; crc_tab[i] = c; }
    uint32_t c = 0xffffffffu; for (size_t i = 0; i < n; i++) c = crc_tab[(c ^ p[i]) & 0xff] ^ (c >> 8); return c ^ 0xffffffffu;
}
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
// The pixels of a PNG file (caller frees), or NULL with *why set
static uint8_t *png_read(const char *path, int *w, int *h, const char **why) {
    FILE *f = fopen(path, "rb"); if (!f) { *why = "no file"; return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n); size_t got = fread(b, 1, (size_t)n, f); fclose(f);
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    uint8_t *z = malloc((size_t)n), *px = NULL; size_t zn = 0; *why = NULL;
    if (got != (size_t)n || n < 8 || memcmp(b, sig, 8)) { *why = "bad signature"; goto out; }
    for (size_t o = 8; o + 12 <= (size_t)n; ) {
        uint32_t len = be32(b + o); const uint8_t *type = b + o + 4;
        if (o + 12 + len > (size_t)n) { *why = "truncated chunk"; goto out; }
        if (crc(type, len + 4) != be32(b + o + 8 + len)) { *why = "chunk CRC"; goto out; }
        if (!memcmp(type, "IHDR", 4)) { *w = (int)be32(type + 4); *h = (int)be32(type + 8); if (type[12] != 8 || type[13] != 6) { *why = "not 8-bit RGBA"; goto out; } }
        else if (!memcmp(type, "IDAT", 4)) { memcpy(z + zn, type + 4, len); zn += len; }
        else if (!memcmp(type, "IEND", 4)) break;
        o += 12 + len;
    }
    size_t rowb = (size_t)*w * 4 + 1, need = rowb * *h; uint8_t *raw = malloc(need ? need : 1); size_t rn = 0, p = 2;
    if (zn < 6 || (z[0] & 0x0f) != 8) { *why = "not zlib"; free(raw); goto out; }
    for (int last = 0; !last && p + 5 <= zn; ) {   // stored blocks only
        last = z[p] & 1; if ((z[p] >> 1 & 3) != 0) { *why = "a compressed block"; free(raw); goto out; }
        uint16_t len = (uint16_t)(z[p + 1] | z[p + 2] << 8), nlen = (uint16_t)(z[p + 3] | z[p + 4] << 8);
        if ((uint16_t)~nlen != len || rn + len > need) { *why = "bad stored block"; free(raw); goto out; }
        memcpy(raw + rn, z + p + 5, len); rn += len; p += 5 + (size_t)len;
    }
    uint32_t a = 1, b2 = 0; for (size_t i = 0; i < rn; i++) { a = (a + raw[i]) % 65521; b2 = (b2 + a) % 65521; }
    if (rn != need || p + 4 > zn || be32(z + p) != (b2 << 16 | a)) { *why = rn != need ? "short image data" : "adler32"; free(raw); goto out; }
    px = malloc(need);
    for (int y = 0; y < *h; y++) { if (raw[y * rowb] != 0) { *why = "a row filter"; free(px); px = NULL; break; } memcpy(px + (size_t)y * *w * 4, raw + y * rowb + 1, (size_t)*w * 4); }
    free(raw);
out:
    free(b); free(z);
    return px;
}

// ---------- M6 phase B: a fragment variant drawn over a 16x8 RGBA8 target cleared to `clear`, read back (dual: blend ONE + SRC1_COLOR; pc: the alpha test's constants) ----------
enum { VW = 16, VH = 8 };
static int variant_draw(const uint32_t *fsw, size_t fsn, int dual, const uint32_t *pc, float clear, uint8_t out[VW * VH * 4]) {
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {VW, VH, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img; vk_alloc im, bm; VkBuffer buf; VkImageView view; VkShaderModule vs, fs; VkPipelineLayout pl; VkPipeline pipe; int rc = -1;
    if (vkCreateImage(g_vk.dev, &ici, NULL, &img) != VK_SUCCESS) return -1;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_vk.dev, img, &mr); vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &im); vkBindImageMemory(g_vk.dev, img, im.mem, im.off);
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, VW * VH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    vkCreateBuffer(g_vk.dev, &bci, NULL, &buf); vkGetBufferMemoryRequirements(g_vk.dev, buf, &mr);
    vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, &bm); vkBindBufferMemory(g_vk.dev, buf, bm.mem, bm.off);
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCreateImageView(g_vk.dev, &vci, NULL, &view);
    VkShaderModuleCreateInfo m1 = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof test_tri_vert_spv, test_tri_vert_spv}, m2 = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, fsn * 4, fsw};
    vkCreateShaderModule(g_vk.dev, &m1, NULL, &vs);
    if (vkCreateShaderModule(g_vk.dev, &m2, NULL, &fs) != VK_SUCCESS) return -1;
    VkPushConstantRange pr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, VKP_ALPHA_PC_SIZE};
    VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, pc ? 1u : 0u, &pr};
    vkCreatePipelineLayout(g_vk.dev, &lci, NULL, &pl);
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
    VkViewport vpt = {0, 0, VW, VH, 0, 1}; VkRect2D sc = {{0, 0}, {VW, VH}};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &vpt, 1, &sc};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_FALSE, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_FALSE, 0, 0, 0, 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba = {dual ? VK_TRUE : VK_FALSE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC1_COLOR, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, 0xf};
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, 0, 1, &ba};
    VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, 1, &cf, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};
    VkGraphicsPipelineCreateInfo gci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, 0, 2, st, &vi, &ia, NULL, &vp, &rs, &ms, NULL, &cbs, NULL, pl, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, -1};
    if (vkCreateGraphicsPipelines(g_vk.dev, VK_NULL_HANDLE, 1, &gci, NULL, &pipe) == VK_SUCCESS) {
        VkCommandBuffer cb = vk_once_begin();
        vk_image_barrier(cb, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo a = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED,
                                       VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, {{{clear, clear, clear, clear}}}};
        VkRenderingInfo rin = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, sc, 1, 0, 1, &a, NULL, NULL};
        vkCmdBeginRendering(cb, &rin); vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        if (pc) vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, VKP_ALPHA_PC_SIZE, pc);
        vkCmdDraw(cb, 3, 1, 0, 0); vkCmdEndRendering(cb);
        vk_image_barrier(cb, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy r = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VW, VH, 1}};
        vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &r);
        vk_mem_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
        if (!vk_once_end(cb)) { memcpy(out, bm.map, VW * VH * 4); rc = 0; }
        vkDestroyPipeline(g_vk.dev, pipe, NULL);
    }
    vkDestroyPipelineLayout(g_vk.dev, pl, NULL); vkDestroyShaderModule(g_vk.dev, vs, NULL); vkDestroyShaderModule(g_vk.dev, fs, NULL);
    vkDestroyImageView(g_vk.dev, view, NULL); vkDestroyImage(g_vk.dev, img, NULL); vkDestroyBuffer(g_vk.dev, buf, NULL); vk_free_mem(&im); vk_free_mem(&bm);
    return rc;
}
static int px_near(const uint8_t *p, int r, int g, int b, int a) { return abs(p[0] - r) <= 2 && abs(p[1] - g) <= 2 && abs(p[2] - b) <= 2 && abs(p[3] - a) <= 2; }
// The stencil of an image's level 0, layer 0 (a one-shot copy of its stencil aspect)
static int stencil_of(vkt_image *im, uint8_t *out, size_t n) {
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, (n + 3) & ~(size_t)3, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer buf; vk_alloc bm; VkMemoryRequirements mr;
    vkCreateBuffer(g_vk.dev, &bci, NULL, &buf); vkGetBufferMemoryRequirements(g_vk.dev, buf, &mr);
    vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, &bm); vkBindBufferMemory(g_vk.dev, buf, bm.mem, bm.off);
    VkCommandBuffer cb = vk_once_begin();
    vk_image_barrier(cb, im->img, im->aspect, im->rest, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy r = {0, 0, 0, {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1}, {0, 0, 0}, {im->w, im->h, 1}};
    vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &r);
    vk_image_barrier(cb, im->img, im->aspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im->rest, VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    vk_mem_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    int rc = vk_once_end(cb);
    if (!rc) memcpy(out, bm.map, n);
    vkDestroyBuffer(g_vk.dev, buf, NULL); vk_free_mem(&bm);
    return rc;
}

// ---------- records ----------
static uint8_t *rec(uint8_t *p, uint32_t op, const void *payload, uint32_t n) {
    op_hdr h = {op, (8 + n + 7) & ~7u}; memcpy(p, &h, 8); memset(p + 8, 0, h.size - 8); if (n) memcpy(p + 8, payload, n); return p + h.size;
}

static void cpu_part(void) {
    // the blend states the game binds on the title screen (raw words also in the Mac host's pass dump): alpha blending, additive glow, a multiply layer
    blend_st a = blend_of(0x4860a1828ull), g = blend_of(0x4820a0828ull), m = blend_of(0x485020c08ull), ov = blend_of(0x482021410ull);
    CHECK(vkp_blend_factor(a.src_rgb) == VK_BLEND_FACTOR_SRC_ALPHA && vkp_blend_factor(a.dst_rgb) == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA && vkp_blend_op(a.eq_rgb) == VK_BLEND_OP_ADD,
          "alpha blending from its recorded word");
    CHECK(vkp_blend_factor(g.src_rgb) == VK_BLEND_FACTOR_SRC_ALPHA && vkp_blend_factor(g.dst_rgb) == VK_BLEND_FACTOR_ONE, "additive glow");
    CHECK(vkp_blend_factor(m.src_rgb) == VK_BLEND_FACTOR_ZERO && vkp_blend_factor(m.dst_rgb) == VK_BLEND_FACTOR_SRC_COLOR && vkp_blend_factor(m.dst_a) == VK_BLEND_FACTOR_SRC_ALPHA, "multiply layer");
    CHECK(vkp_blend_factor(ov.src_rgb) == VK_BLEND_FACTOR_ONE && vkp_blend_factor(ov.dst_rgb) == VK_BLEND_FACTOR_SRC_ALPHA && vkp_blend_factor(ov.src_a) == VK_BLEND_FACTOR_ZERO &&
          vkp_blend_factor(ov.dst_a) == VK_BLEND_FACTOR_ONE, "overlay composite (transmittance in alpha)");
    static const struct { int n; VkBlendFactor v; } bf[] = {
        {NVN_BLEND_FUNC_ZERO, VK_BLEND_FACTOR_ZERO}, {NVN_BLEND_FUNC_ONE, VK_BLEND_FACTOR_ONE}, {NVN_BLEND_FUNC_SRC_COLOR, VK_BLEND_FACTOR_SRC_COLOR},
        {NVN_BLEND_FUNC_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR}, {NVN_BLEND_FUNC_SRC_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA},
        {NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA}, {NVN_BLEND_FUNC_DST_ALPHA, VK_BLEND_FACTOR_DST_ALPHA},
        {NVN_BLEND_FUNC_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA}, {NVN_BLEND_FUNC_DST_COLOR, VK_BLEND_FACTOR_DST_COLOR},
        {NVN_BLEND_FUNC_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR}, {NVN_BLEND_FUNC_SRC_ALPHA_SATURATE, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE},
        {NVN_BLEND_FUNC_SRC1_COLOR, VK_BLEND_FACTOR_SRC1_COLOR}, {NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA},
        {NVN_BLEND_FUNC_CONSTANT_COLOR, VK_BLEND_FACTOR_CONSTANT_COLOR}, {NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA}};
    int bad = 0; for (size_t i = 0; i < sizeof bf / sizeof *bf; i++) bad += vkp_blend_factor(bf[i].n) != bf[i].v;
    CHECK(!bad, "every blend factor (%d wrong)", bad);
    CHECK(vkp_blend_zero_src1(NVN_BLEND_FUNC_SRC1_ALPHA) == NVN_BLEND_FUNC_ZERO && vkp_blend_zero_src1(NVN_BLEND_FUNC_ONE_MINUS_SRC1_COLOR) == NVN_BLEND_FUNC_ONE &&
          vkp_blend_zero_src1(NVN_BLEND_FUNC_SRC_ALPHA) == NVN_BLEND_FUNC_SRC_ALPHA, "a missing second source counts as zero");
    blend_st adv = a; adv.adv = NVN_BLEND_ADVANCED_MODE_SRC_OVER; blend_st r = vkp_adv_blend(adv);
    CHECK(r.src_rgb == NVN_BLEND_FUNC_ONE && r.dst_rgb == NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA, "advanced SRC_OVER, premultiplied");
    adv.adv_npm = 1; r = vkp_adv_blend(adv);
    CHECK(r.src_rgb == NVN_BLEND_FUNC_SRC_ALPHA && r.dst_rgb == NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA, "advanced SRC_OVER, source not premultiplied");
    CHECK(vkp_compare(NVN_DEPTH_FUNC_NEVER) == VK_COMPARE_OP_NEVER && vkp_compare(NVN_DEPTH_FUNC_LEQUAL) == VK_COMPARE_OP_LESS_OR_EQUAL &&
          vkp_compare(NVN_DEPTH_FUNC_GREATER) == VK_COMPARE_OP_GREATER && vkp_compare(NVN_DEPTH_FUNC_ALWAYS) == VK_COMPARE_OP_ALWAYS, "compare functions");
    CHECK(vkp_stencil_op(NVN_STENCIL_OP_KEEP) == VK_STENCIL_OP_KEEP && vkp_stencil_op(NVN_STENCIL_OP_INVERT) == VK_STENCIL_OP_INVERT &&
          vkp_stencil_op(NVN_STENCIL_OP_INCR_WRAP) == VK_STENCIL_OP_INCREMENT_AND_WRAP, "stencil operations");
    int ex = 0;
    CHECK(vkp_topology(NVN_DRAW_PRIMITIVE_QUADS, &ex) == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST && ex == VKP_EXPAND_QUADS, "quads drawn as triangle lists");
    CHECK(vkp_topology(NVN_DRAW_PRIMITIVE_LINE_LOOP, &ex) == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP && ex == VKP_EXPAND_LINE_LOOP, "a line loop as a closed strip");
    CHECK(vkp_topology(NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP, &ex) == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP && ex == VKP_EXPAND_NONE, "triangle strips as they are");
    // the window origin flips the viewport, and with it the winding (nvn_mtl.m's rule)
    CHECK(vkp_front_face(NVN_FRONT_FACE_CCW, 0) == VK_FRONT_FACE_COUNTER_CLOCKWISE && vkp_front_face(NVN_FRONT_FACE_CCW, 1) == VK_FRONT_FACE_CLOCKWISE &&
          vkp_front_face(NVN_FRONT_FACE_CW, 0) == VK_FRONT_FACE_CLOCKWISE && vkp_front_face(NVN_FRONT_FACE_CW, 1) == VK_FRONT_FACE_COUNTER_CLOCKWISE, "front face against the origin");
    CHECK(vkp_cull(NVN_FACE_BACK) == VK_CULL_MODE_BACK_BIT && vkp_cull(NVN_FACE_NONE) == VK_CULL_MODE_NONE, "cull faces");
    CHECK(vkp_vertex_format(NVN_FORMAT_RGBA8, VKS_CT_FLOAT, &ex) == VK_FORMAT_R8G8B8A8_UNORM && ex, "RGBA8 vertex colour");
    CHECK(vkp_vertex_format(NVN_FORMAT_RGB32F, VKS_CT_FLOAT, &ex) == VK_FORMAT_R32G32B32_SFLOAT && ex, "RGB32F position");
    CHECK(vkp_vertex_format(NVN_FORMAT_R32F, VKS_CT_FLOAT, &ex) == VK_FORMAT_R32_SFLOAT && ex && vkp_vertex_format_size(NVN_FORMAT_RG32F) == 8, "R32F index, RG32F size");
    int ct = -1;
    CHECK(vkp_vertex_format_natural(NVN_FORMAT_RGBA16I, &ct) == VK_FORMAT_R16G16B16A16_SINT && ct == VKS_CT_INT, "RGBA16I read in its own class");
    // the view format in the class the shader declares, bits kept
    CHECK(vkr_fmt_as_ct(NVN_FORMAT_R32UI, VKS_CT_FLOAT) == NVN_FORMAT_R32F && vkr_fmt_as_ct(NVN_FORMAT_RGBA32F, VKS_CT_UINT) == NVN_FORMAT_RGBA32UI &&
          vkr_fmt_as_ct(NVN_FORMAT_RG32I, VKS_CT_FLOAT) == NVN_FORMAT_RG32F, "32-bit components between float, uint and int");
    CHECK(vkr_fmt_as_ct(NVN_FORMAT_RGBA16I, VKS_CT_UINT) == NVN_FORMAT_RGBA16UI && vkr_fmt_as_ct(NVN_FORMAT_RG8UI, VKS_CT_INT) == NVN_FORMAT_RG8I, "integers between uint and int");
    CHECK(vkr_fmt_as_ct(NVN_FORMAT_R32F, VKS_CT_FLOAT) == -1 && vkr_fmt_as_ct(NVN_FORMAT_RGBA8, VKS_CT_UINT) == -1 && vkr_fmt_as_ct(NVN_FORMAT_RGBA16F, VKS_CT_INT) == -1,
          "a matching class, or none that keeps the bits, leaves the format");
    // gl_PerVertex: the unused cull distance goes, a used one stays, a module within the limit is left alone
    size_t n = sizeof test_pervertex_vert_spv / 4; uint32_t *w = malloc(n * 4); memcpy(w, test_pervertex_vert_spv, n * 4);
    CHECK(vkspv_trim_cull(&w, n, 9) == n, "within the limit: unchanged");
    size_t tn = vkspv_trim_cull(&w, n, 8);
    int cull_left = 0, struct_members = -1;
    for (size_t i = 5; tn && i < tn; i += w[i] >> 16) { uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (op == 72 && wc >= 5 && w[i + 3] == 11 && w[i + 4] == 4) cull_left = 1;
        if (op == 30 && wc == 6) struct_members = 4;
        if (op == 30 && wc == 5) struct_members = 3; }
    CHECK(tn && tn < n && !cull_left && struct_members == 3, "the unused cull distance is dropped (%zu -> %zu words)", n, tn);
    free(w);
    n = sizeof test_pervertex_cull_vert_spv / 4; w = malloc(n * 4); memcpy(w, test_pervertex_cull_vert_spv, n * 4);
    CHECK(vkspv_trim_cull(&w, n, 8) == 0, "a written cull distance cannot be dropped");
    free(w);
    // screenshot formats
    uint8_t o[16];
    uint16_t hf[4] = {0x3c00, 0x3800, 0x0000, 0x3c00};   // 1, 0.5, 0, 1
    CHECK(!vks_to_rgba8(VK_FORMAT_R16G16B16A16_SFLOAT, (const uint8_t *)hf, 1, o) && o[0] == 255 && (o[1] == 127 || o[1] == 128) && o[2] == 0 && o[3] == 255, "RGBA16F to 8 bits");
    uint32_t p10 = 1023u | 0u << 10 | 512u << 20 | 3u << 30;
    CHECK(!vks_to_rgba8(VK_FORMAT_A2B10G10R10_UNORM_PACK32, (const uint8_t *)&p10, 1, o) && o[0] == 255 && o[1] == 0 && (o[2] == 127 || o[2] == 128) && o[3] == 255, "A2B10G10R10 to 8 bits");
    uint8_t bgra[4] = {10, 20, 30, 40};
    CHECK(!vks_to_rgba8(VK_FORMAT_B8G8R8A8_UNORM, bgra, 1, o) && o[0] == 30 && o[1] == 20 && o[2] == 10 && o[3] == 40, "BGRA8 swapped");
    CHECK(vks_to_rgba8(VK_FORMAT_BC7_UNORM_BLOCK, bgra, 1, o) != 0, "a format the screenshots do not know is refused");
    // PNG: written, read again bit for bit
    enum { PW = 300, PH = 230 };   // rows of 1201 bytes: stored blocks split across rows
    uint8_t *img = malloc(PW * PH * 4); for (int i = 0; i < PW * PH * 4; i++) img[i] = (uint8_t)vt_rand();
    char path[MAX_PATH]; GetTempPathA(MAX_PATH, path); strcat(path, "nxrt-t-vk-replay.png");
    int pw = 0, ph = 0; const char *why = NULL;
    CHECK(!vks_png_write(path, img, PW, PH), "PNG written");
    uint8_t *back = png_read(path, &pw, &ph, &why);
    CHECK(back && pw == PW && ph == PH && !memcmp(back, img, PW * PH * 4), "the PNG decodes to the same pixels (%s)", why ? why : "ok");
    free(back); free(img); DeleteFileA(path);
}

int main(void) {
    crash_install();
    cpu_part();
    char tmp[MAX_PATH], dir[MAX_PATH], ctl[MAX_PATH], ctl_out[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp); snprintf(dir, sizeof dir, "%snxrt-t-vk-replay-shots", tmp); CreateDirectoryA(dir, NULL);
    snprintf(ctl, sizeof ctl, "%s\\take.txt", dir); snprintf(ctl_out, sizeof ctl_out, "%s\\from-control.png", dir);
    char e1[MAX_PATH + 32], e2[MAX_PATH + 32];
    snprintf(e1, sizeof e1, "NXRT_VK_SHOT_DIR=%s", dir); _putenv(e1); _putenv("NXRT_VK_SHOT=3");
    snprintf(e2, sizeof e2, "NXRT_VK_SHOT_FILE=%s", ctl); _putenv(e2);
    int rc = vt_up(0, 1);
    if (rc == 77) { printf("GPU part skipped (no device); %s: %d checks, %d failed\n", g_fail ? "FAIL" : "ok", g_checks, g_fail); return g_fail ? 1 : 0; }
    if (rc) return rc;
    CHECK(!vkr_init(), "replay started");
    vks_shot_init();

    // ---- descriptor mapping: a fragment shader's bindings ----
    static const char js[] = "{\"key\":\"t_bind\",\"stage\":\"ps\",\"bindings\":["
        "{\"set\":0,\"binding\":0,\"descriptor\":\"uniform_buffer\",\"count\":1,\"nvn\":{\"kind\":\"support\"}},"
        "{\"set\":0,\"binding\":1,\"descriptor\":\"combined_image_sampler\",\"count\":1,\"nvn\":{\"kind\":\"tex\",\"reg\":3,\"sampler\":{\"kind\":\"samp_tex\",\"reg\":3}}},"
        "{\"set\":0,\"binding\":2,\"descriptor\":\"uniform_buffer\",\"count\":1,\"nvn\":{\"kind\":\"cbuf\",\"reg\":3}},"
        "{\"set\":0,\"binding\":3,\"descriptor\":\"uniform_texel_buffer\",\"count\":1,\"nvn\":{\"kind\":\"tbuf\",\"reg\":5}}],"
        "\"inputs\":[],\"outputs\":[{\"location\":0,\"type\":\"float\",\"components\":4}]}";
    const char *why = NULL;
    vks_shader *fs = vksh_make("t_bind", (const uint8_t *)test_bind_frag_spv, sizeof test_bind_frag_spv, js, sizeof js - 1, &why);
    CHECK(fs != NULL, "fragment shader made (%s)", why ? why : "ok");
    if (fs) {
        CHECK(fs->set == vksh_set_of(VKS_FS) && fs->set == 1 && fs->nb == 4 && fs->dsl && fs->mod, "its descriptors in set 1, a layout and a module");
        CHECK(fs->b[0].kind == VKB_SUPPORT && fs->b[2].kind == VKB_CBUF && fs->b[2].reg == 3, "support buffer, constant buffer slot 3");
        CHECK(fs->b[1].kind == VKB_TEX && fs->b[1].reg == 3 && fs->b[1].samp_kind == VKB_SAMP_TEX && fs->b[1].vtype == VK_IMAGE_VIEW_TYPE_2D && fs->b[1].ct == VKS_CT_UINT,
              "texture slot 3 with its own sampler, 2D, read as uint");
        CHECK(fs->b[3].kind == VKB_TBUF && fs->b[3].reg == 5 && fs->b[3].type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER && fs->b[3].ct == VKS_CT_FLOAT, "buffer texture slot 5, float");
        CHECK(fs->out_mask == 1 && fs->out_ct[0] == VKS_CT_FLOAT, "one float output");
        size_t n = sizeof test_bind_frag_spv / 4; uint32_t *w = malloc(n * 4); memcpy(w, test_bind_frag_spv, n * 4);
        int dec = vkspv_set_descriptor_set(w, n, 1), all1 = 1;
        for (size_t i = 5; i < n; i += w[i] >> 16) if ((w[i] & 0xffff) == 71 && (w[i] >> 16) >= 4 && w[i + 2] == 34) all1 &= w[i + 3] == 1;
        CHECK(dec == 4 && all1, "every DescriptorSet decoration moved to the stage's set (%d)", dec);
        vkspv_img im; CHECK(!vkspv_image_of(w, n, 3, &im) && im.is_image && im.dim == 5, "the buffer texture's image is Buffer-dimensioned");
        free(w);
        vksh_free(fs);
    }
    static const char jv[] = "{\"key\":\"t_pv\",\"stage\":\"vs\",\"bindings\":[],\"inputs\":[{\"location\":0,\"type\":\"float\",\"components\":4}],\"outputs\":[]}";
    vks_shader *vs = vksh_make("t_pv", (const uint8_t *)test_pervertex_vert_spv, sizeof test_pervertex_vert_spv, jv, sizeof jv - 1, &why);
    CHECK(vs && vs->mod, "a vertex shader with 8 clip and 1 cull distance is made on this device (%u combined at most)", g_vk.props.limits.maxCombinedClipAndCullDistances);
    if (vs) vksh_free(vs);

    // ---- copies through the record walk ----
    enum { PSZ = 1 << 20, TW = 16, TH = 8 };
    uint8_t *mem = VirtualAlloc(NULL, PSZ, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    for (int i = 0; i < PSZ; i++) mem[i] = (uint8_t)vt_rand();
    uint8_t *src = mem, *b2b = mem + 0x10000, *fill = mem + 0x20000, *upd = mem + 0x30000, *texsrc = mem + 0x40000, *texdst = mem + 0x50000, *blitdst = mem + 0x60000;
    for (int i = 0; i < TW * TH * 4; i++) texsrc[i] = (uint8_t)(i * 7 + 3);
    uint8_t *flat = mem + 0x70000; for (int i = 0; i < TW * TH; i++) { flat[4 * i] = 200; flat[4 * i + 1] = 100; flat[4 * i + 2] = 50; flat[4 * i + 3] = 255; }
    vkm_pool_add(mem, PSZ);
    uint8_t expect[4096]; memcpy(expect, src, sizeof expect);
    tex_t ta = vt_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, TW, TH, 1, 1, 0, 0, mem + 0x80000), tb = vt_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, TW, TH, 1, 1, 0, 0, mem + 0x90000);
    tex_t tc = vt_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, TW / 2, TH / 2, 1, 1, 0, 0, mem + 0xa0000), tf = vt_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, TW, TH, 1, 1, 0, 0, mem + 0xb0000);
    vkt_tex_init(&ta); vkt_tex_init(&tb); vkt_tex_init(&tc); vkt_tex_init(&tf);
    uint8_t *rb = malloc(1 << 16), *p = rb;
    uint64_t c3[3] = {(uint64_t)(uintptr_t)src, (uint64_t)(uintptr_t)b2b, sizeof expect};
    p = rec(p, OP_COPY_B2B, c3, sizeof c3);
    p_copy_b2t cb = {0}; cb.src = (uint64_t)(uintptr_t)texsrc; cb.tex = &ta; cb.r = (NVNcopyRegion){0, 0, 0, TW, TH, 1};
    p = rec(p, OP_COPY_B2T, &cb, sizeof cb);
    cb.src = (uint64_t)(uintptr_t)flat; cb.tex = &tf; p = rec(p, OP_COPY_B2T, &cb, sizeof cb);
    p_copy_t2t tt = {0}; tt.src = &ta; tt.dst = &tb; tt.sr = tt.dr = (NVNcopyRegion){0, 0, 0, TW, TH, 1};
    p = rec(p, OP_COPY_T2T, &tt, sizeof tt);
    tt.src = &tf; tt.dst = &tc; tt.dr = (NVNcopyRegion){0, 0, 0, TW / 2, TH / 2, 1}; tt.flags = NVN_COPY_FLAGS_LINEAR_FILTER_BIT;
    p = rec(p, OP_COPY_T2T, &tt, sizeof tt);
    p_copy_b2t tb2 = {0}; tb2.src = (uint64_t)(uintptr_t)texdst; tb2.tex = &tb; tb2.r = (NVNcopyRegion){0, 0, 0, TW, TH, 1};
    p = rec(p, OP_COPY_T2B, &tb2, sizeof tb2);
    tb2.src = (uint64_t)(uintptr_t)blitdst; tb2.tex = &tc; tb2.r = (NVNcopyRegion){0, 0, 0, TW / 2, TH / 2, 1};
    p = rec(p, OP_COPY_T2B, &tb2, sizeof tb2);
    p = rec(p, OP_END, NULL, 0);
    vks_begin(); vkr_sub_begin();
    vkr_exec(rb);
    vkr_fill((uint64_t)(uintptr_t)fill, 256, 0xdeadbeefu);
    uint8_t data[64]; for (int i = 0; i < 64; i++) data[i] = (uint8_t)(i * 5 + 1);
    vkr_update((uint64_t)(uintptr_t)upd, 64, data);
    vkr_sub_end(); vks_wait(vks_commit(), UINT64_MAX);
    CHECK(!memcmp(b2b, expect, sizeof expect), "a buffer copy lands in guest memory");
    uint32_t f0, f63; memcpy(&f0, fill, 4); memcpy(&f63, fill + 252, 4);
    CHECK(f0 == 0xdeadbeefu && f63 == 0xdeadbeefu, "a fill lands in guest memory (%08x %08x)", f0, f63);
    CHECK(!memcmp(upd, data, 64), "an inline update lands in guest memory");
    CHECK(!memcmp(texdst, texsrc, TW * TH * 4), "buffer -> texture -> texture -> buffer keeps every texel");
    int blit_ok = 1; for (int i = 0; i < TW * TH / 4; i++) blit_ok &= blitdst[4 * i] == 200 && blitdst[4 * i + 1] == 100 && blitdst[4 * i + 2] == 50 && blitdst[4 * i + 3] == 255;
    CHECK(blit_ok, "a scaled, filtered copy of a flat image keeps its colour (%u,%u,%u,%u)", blitdst[0], blitdst[1], blitdst[2], blitdst[3]);
    vkr_stats_t rs; vkr_stats(&rs);
    CHECK(rs.tex_copies >= 5 && rs.bad == 0, "five texture copies, no bad records (%llu, %llu)", (unsigned long long)rs.tex_copies, (unsigned long long)rs.bad);

    // ---- M6 phase B: masked clears drawn through the mask, a GPU copy read back into a storage-backed texture ----
    {
        // tf holds (200, 100, 50, 255) everywhere: R and B cleared to 255 and 128 in the left half only (the scissor)
        uint8_t *rb2 = malloc(1 << 16), *q = rb2;
        p_set_rt rt; memset(&rt, 0, sizeof rt); rt.n = 1; rt.color[0] = &tf;
        q = rec(q, OP_SET_RT, &rt, sizeof rt);
        p_scissor sc = {0, 0, TW / 2, TH}; q = rec(q, OP_SCISSOR, &sc, sizeof sc);
        p_clear_color cc = {0, 0x5, {1.0f, 0.0f, 0.5f, 0.0f}}; q = rec(q, OP_CLEAR_COLOR, &cc, sizeof cc);
        // a depth-stencil target: cleared whole to stencil 0x0f, then only its top four bits to those of 0xa5
        tex_t tds = vt_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_DEPTH24_STENCIL8, TW, TH, 1, 1, 0, 0, mem + 0xd0000); vkt_tex_init(&tds);
        p_set_rt rd; memset(&rd, 0, sizeof rd); rd.depth = &tds; q = rec(q, OP_SET_RT, &rd, sizeof rd);
        p_scissor full = {0, 0, TW, TH}; q = rec(q, OP_SCISSOR, &full, sizeof full);
        p_clear_ds c1 = {1.0f, 1, 0x0f, 0xff}; q = rec(q, OP_CLEAR_DS, &c1, sizeof c1);
        p_clear_ds c2 = {0.0f, 0, 0xa5, 0xf0}; q = rec(q, OP_CLEAR_DS, &c2, sizeof c2);
        // ta (texsrc's texels, its content the GPU's) copied into a texture whose content is its storage
        memset(mem + 0xe0000, 0, 0x10000);
        tex_t tst = vt_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, TW, TH, 1, 1, 0, 0, mem + 0xe0000); vkt_tex_init(&tst); vkt_tex_cpu_written(&tst);
        p_copy_t2t t3 = {0}; t3.src = &ta; t3.dst = &tst; t3.sr = t3.dr = (NVNcopyRegion){0, 0, 0, TW, TH, 1}; q = rec(q, OP_COPY_T2T, &t3, sizeof t3);
        p_copy_b2t rbk = {0}; rbk.src = (uint64_t)(uintptr_t)(mem + 0xf0000); rbk.tex = &tf; rbk.r = (NVNcopyRegion){0, 0, 0, TW, TH, 1}; q = rec(q, OP_COPY_T2B, &rbk, sizeof rbk);
        q = rec(q, OP_END, NULL, 0);
        vks_begin(); vkr_sub_begin(); vkr_exec(rb2); vkr_sub_end(); vks_wait(vks_commit(), UINT64_MAX);
        const uint8_t *o = mem + 0xf0000; int left = 1, right = 1;
        for (int y = 0; y < TH; y++) for (int x = 0; x < TW; x++) { const uint8_t *c = o + 4 * (y * TW + x);
            if (x < TW / 2) left &= px_near(c, 255, 100, 128, 255); else right &= px_near(c, 200, 100, 50, 255); }
        CHECK(left && right, "a colour clear of R and B within the scissor keeps G, A and the rest (%u,%u,%u,%u | %u,%u,%u,%u)", o[0], o[1], o[2], o[3],
              o[4 * (TW - 1)], o[4 * (TW - 1) + 1], o[4 * (TW - 1) + 2], o[4 * (TW - 1) + 3]);
        vkt_image *dsi = vkt_by_serial(tds.serial); uint8_t sb[TW * TH] = {0}; int s_ok = dsi && !stencil_of(dsi, sb, sizeof sb), all = s_ok;
        for (int i = 0; i < TW * TH && s_ok; i++) all &= sb[i] == 0xaf;
        CHECK(all, "a stencil clear of its top four bits keeps the others (0x%02x, expected 0xaf)", sb[0]);
        uint8_t back[TW * TH * 4]; NVNcopyRegion whole = {0, 0, 0, TW, TH, 1};
        int rd_ok = !nvn_texio_read(&tst, 0, 0, &whole, back, TW * 4, TW * TH * 4);
        CHECK(rd_ok && !memcmp(back, texsrc, sizeof back), "a GPU copy into a storage-backed texture lands in its storage (read back at completion)");
        vkt_tex_release(&tds); vkt_tex_release(&tst); free(rb2);
    }
    // ---- M6 phase B: the fragment variants drawn (dual-source blending, the alpha test, both) ----
    {
        size_t n = sizeof test_dual_frag_spv / 4, nd = 0, na = 0, nda = 0, nb = 0;
        uint32_t *dw = vkspv_dual_source(test_dual_frag_spv, n, &nd), *aw = vkspv_alpha_test(test_dual_frag_spv, n, &na);
        uint32_t *daw = dw ? vkspv_alpha_test(dw, nd, &nda) : NULL, *nod = vkspv_dual_source(test_bind_frag_spv, sizeof test_bind_frag_spv / 4, &nb);
        CHECK(dw && aw && daw && !nod, "dual-source and alpha-test variants made (no dual-source one for a shader with one output)");
        uint8_t px[VW * VH * 4];
        int ok = dw && !variant_draw(dw, nd, 1, NULL, 1.0f, px);
        CHECK(ok && px_near(px, 191, 191, 255, 153) && px_near(px + 4 * 8, 191, 191, 255, 102), "dual-source: target 0 blends ONE + SRC1_COLOR with output 1 (%u,%u,%u,%u)", px[0], px[1], px[2], px[3]);
        uint32_t gt[2] = {NVN_ALPHA_FUNC_GREATER, 0}, lt[2] = {NVN_ALPHA_FUNC_LESS, 0}; float half = 0.5f; memcpy(&gt[1], &half, 4); memcpy(&lt[1], &half, 4);
        ok = aw && !variant_draw(aw, na, 0, gt, 0.0f, px);
        CHECK(ok && px_near(px, 64, 128, 191, 153) && px_near(px + 4 * 8, 0, 0, 0, 0), "alpha test GREATER 0.5: alpha 0.6 drawn, 0.4 discarded (%u / %u)", px[3], px[4 * 8 + 3]);
        ok = aw && !variant_draw(aw, na, 0, lt, 0.0f, px);
        CHECK(ok && px_near(px, 0, 0, 0, 0) && px_near(px + 4 * 8, 64, 128, 191, 102), "alpha test LESS 0.5: the reverse, the early return tested too (%u / %u)", px[3], px[4 * 8 + 3]);
        ok = daw && !variant_draw(daw, nda, 1, gt, 1.0f, px);
        CHECK(ok && px_near(px, 191, 191, 255, 153) && px_near(px + 4 * 8, 255, 255, 255, 255), "dual-source and the alpha test together (%u,%u,%u,%u)", px[0], px[1], px[2], px[3]);
        free(dw); free(aw); free(daw); free(nod);
    }

    // ---- screenshots of a GPU texture: cropped, turned over; the present-number and control-file triggers ----
    uint8_t *cap = NULL; int cw = 0, ch = 0; int crop[4] = {2, 1, 10, 6};
    CHECK(!vks_shot_capture(&ta, crop, 1, &cap, &cw, &ch) && cw == 10 && ch == 6, "capture of the copied texture");
    int same = cap != NULL;
    for (int y = 0; same && y < ch; y++) same &= !memcmp(cap + (size_t)y * cw * 4, texsrc + ((size_t)(crop[1] + ch - 1 - y) * TW + crop[0]) * 4, (size_t)cw * 4);
    CHECK(same, "the capture is the cropped region, rows turned over");
    free(cap);
    vks_shot_present(&ta, 3, NULL);
    char shot3[MAX_PATH + 32]; snprintf(shot3, sizeof shot3, "%s\\shot-000003.png", dir);
    int pw = 0, ph = 0; uint8_t *px = png_read(shot3, &pw, &ph, &why);
    CHECK(px && pw == TW && ph == TH, "present 3 written as %s (%s)", shot3, why ? why : "ok");
    free(px); DeleteFileA(shot3);
    FILE *cf = fopen(ctl, "w"); fputs(ctl_out, cf); fclose(cf);
    vks_shot_present(&ta, 4, NULL);
    px = png_read(ctl_out, &pw, &ph, &why);
    CHECK(px && pw == TW && ph == TH && GetFileAttributesA(ctl) == INVALID_FILE_ATTRIBUTES, "the control file's path written, the file removed (%s)", why ? why : "ok");
    free(px); DeleteFileA(ctl_out); RemoveDirectoryA(dir);

    vkt_tex_release(&ta); vkt_tex_release(&tb); vkt_tex_release(&tc); vkt_tex_release(&tf); vkt_drop_pool(ta.pool);
    vkm_pool_del(mem);
    free(rb);
    uint32_t verr = atomic_load(&g_vk.val_errors);
    vkr_shutdown();
    vt_down(0);
    CHECK(verr == 0, "no validation errors (%u)", verr);
    VirtualFree(mem, 0, MEM_RELEASE);
    printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "ok", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
