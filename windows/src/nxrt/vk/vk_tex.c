// Textures: images, views, uploads through s2tex, sharing, the CPU-written rule, samplers, registrations (vk_tex.h; docs/m5-vulkan.md §4).
// Behaviour follows nxnative's Metal backend (nvn_mtl.m: tex_mtl_, the g_alias sharing table and alias_take_over, mtl_alias_drop_pool, tex_from_storage,
// tex_cpu_content, mtl_tex_cpu_written, the packaged-texture dirty check); the Vulkan side and the IMAGE rule for block-compressed textures are ours.
// Storage addressing is nvn_tex.c's (the recording layer's), the byte order inside GOBs s2tex's (Switch 2), through gfx/nvn_texio.c.
#include "vk_tex.h"
#include "vk_mem.h"
#include "vk_submit.h"
#include "vk_astc.h"
#include "vk_hmap.h"
#include "nvn_dirty.h"
#include "nvn_texio.h"
#include "s2tex.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t pool; int64_t off; int32_t target, format, samples, cls, storage_img, pkg_bh; uint32_t w, h, d, levels, stride, layout_flags; } skey;
static vk_hmap g_share = {sizeof(skey)}, g_serial = {8}, g_live = {8}, g_cpu = {8}, g_samp = {sizeof(samp_par)};
static plat_rwlock_t g_lk = PLAT_RWLOCK_INIT;   // every table here; the replay and the present thread take it
static vkt_reg *g_reg; static int g_nreg; static samp_par *g_sreg; static uint8_t *g_sreg_ok; static int g_nsreg;
static int g_pool_n;   // the texture pool's capacity (IDs past it read as empty: the replay's descriptor lookups)
int vkt_pool_n(void) { return g_pool_n; }
// Bumped when what a pool slot's descriptor would hold may have changed: a registration, a texture released, an image or a view destroyed (the bindless pool set's sync, vk_replay.c)
static _Atomic uint64_t g_reg_epoch;
uint64_t vkt_reg_epoch(void) { return atomic_load(&g_reg_epoch); }
static uint64_t g_frame;
static _Atomic uint64_t s_images, s_bytes, s_uploads, s_reuploads, s_up_bytes, s_up_ns, s_shared, s_astc_fast, s_astc_high, s_cpuw, s_evicted, s_samplers;
// ASTC levels / layers still at the fast pass: where their high-quality data goes
typedef struct { vka_key key; vkt_image *im; uint32_t level, layer, w, h, slices; } upgrade_t;
static upgrade_t *g_upg; static int g_nupg, g_cupg;
static vka_key *g_ready_key; static uint8_t **g_ready_data; static size_t *g_ready_size; static int g_nready, g_cready;

static uint32_t mip(uint32_t s, unsigned l) { s >>= l; return s ? s : 1; }

// ---------- Which content an NVN texture has ----------
static int pool_cpu_writable(const pool_t *p) { return p && !(p->flags & (NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_VIRTUAL)); }
static int cpu_written(uint64_t serial) { return serial && vk_hm_get(&g_cpu, &serial) != NULL; }   // caller holds g_lk
// From storage: a packaged layout; textures the CPU wrote (flushes, WriteTexels); and every block-compressed texture of a CPU-writable pool, IMAGE flag or
// not. A BC or ASTC texture cannot be a render target or a storage image, so its storage is the only place its content can be: Pokopia's icons are BC1 /
// BC7 textures flagged IMAGE whose storage the CPU rewrites, and treating IMAGE as "GPU-owned" left them stale (research/01 lesson 17)
static int from_storage(const tex_t *t) {
    if (!t->mem || t->d.target == NVN_TEXTURE_TARGET_BUFFER) return 0;
    if (t->has_layout || cpu_written(t->serial)) return 1;
    // A linear texture in a CPU-writable pool is pool memory the CPU writes in place (nxnative makes it a view of the pool's buffer: zero-copy). Pokopia's 2x1
    // exposure texture is one: the game writes it on the CPU each frame and a draw copies it into a render target (M5-C)
    if ((t->d.flags & NVN_TEXTURE_FLAGS_LINEAR) && !(t->d.flags & NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET) && pool_cpu_writable(t->pool) && t->d.samples <= 1) return 1;
    const vkf_info *fi = vkf_get(t->d.format);
    return fi && (fi->flags & VKF_COMPRESSED) && pool_cpu_writable(t->pool) && t->d.samples <= 1 &&
           !(t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET | NVN_TEXTURE_FLAGS_SPARSE));
}
static void key_of(const tex_t *t, int cls, skey *k) {
    memset(k, 0, sizeof *k);
    k->pool = (uint64_t)(uintptr_t)t->pool; k->off = t->pool ? t->off : (int64_t)(uintptr_t)t->mem;
    k->target = t->d.target; k->format = t->d.format; k->samples = t->d.samples > 1 ? t->d.samples : 1; k->cls = cls;
    k->storage_img = (t->d.flags & NVN_TEXTURE_FLAGS_IMAGE) && !vkf_is_compressed(t->d.format);   // IMAGE adds storage use; it changes nothing for BC / ASTC
    k->pkg_bh = t->has_layout ? t->d.pkg_block_h : 0;
    k->w = t->d.width; k->h = t->d.height; k->d = t->d.depth; k->levels = t->d.levels ? t->d.levels : 1; k->stride = t->d.stride;
    k->layout_flags = t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET | NVN_TEXTURE_FLAGS_SPARSE);
}

// ---------- Image creation ----------
static void shape(const nvn_texdesc *d, VkImageType *type, VkImageViewType *vt, uint32_t *w, uint32_t *h, uint32_t *depth, uint32_t *layers, int *cube) {
    *w = d->width ? d->width : 1; *h = d->height ? d->height : 1; *depth = 1; *layers = 1; *cube = 0; *type = VK_IMAGE_TYPE_2D; *vt = VK_IMAGE_VIEW_TYPE_2D;
    switch (d->target) {
    case NVN_TEXTURE_TARGET_1D: *type = VK_IMAGE_TYPE_1D; *vt = VK_IMAGE_VIEW_TYPE_1D; *h = 1; break;
    case NVN_TEXTURE_TARGET_1D_ARRAY: *type = VK_IMAGE_TYPE_1D; *vt = VK_IMAGE_VIEW_TYPE_1D_ARRAY; *layers = *h; *h = 1; break;   // the layer count is in height
    case NVN_TEXTURE_TARGET_3D: *type = VK_IMAGE_TYPE_3D; *vt = VK_IMAGE_VIEW_TYPE_3D; *depth = d->depth ? d->depth : 1; break;
    case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: *vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY; *layers = d->depth ? d->depth : 1; break;
    case NVN_TEXTURE_TARGET_CUBEMAP: *vt = VK_IMAGE_VIEW_TYPE_CUBE; *layers = 6; *cube = 1; break;
    case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: *vt = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY; *layers = d->depth ? d->depth : 6; *cube = 1; break;   // layer-faces, as in GL
    default: break;   // 2D, rectangle, multisample
    }
}

static VkImageView make_view(vkt_image *im, VkFormat f, VkImageViewType vt, VkComponentMapping swz, uint32_t base_level, uint32_t nlevels, uint32_t base_layer, uint32_t nlayers, VkImageAspectFlags aspect) {
    VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, im->img, vt, f, swz, {aspect, base_level, nlevels, base_layer, nlayers}};
    // A view in a format the image's storage use does not allow (an sRGB view of an RGBA8 image that compute shaders write: VUID-VkImageViewCreateInfo-usage-02275,
    // seen in gameplay) leaves storage out of the view's usage
    VkImageViewUsageCreateInfo uci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, NULL, im->usage & ~(VkImageUsageFlags)VK_IMAGE_USAGE_STORAGE_BIT};
    if (im->usage & VK_IMAGE_USAGE_STORAGE_BIT) {
        VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_vk.pd, f, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && uci.usage) ci.pNext = &uci;
    }
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(g_vk.dev, &ci, NULL, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    return v;
}

static void view_free_cb(void *v) { vkDestroyImageView(g_vk.dev, (VkImageView)v, NULL); }
static void image_free(vkt_image *im) {
    for (int i = 0; i < im->nviews; i++) vkDestroyImageView(g_vk.dev, im->views[i].v, NULL);
    if (im->present_view && im->present_view != im->view) vkDestroyImageView(g_vk.dev, im->present_view, NULL);
    if (im->view) vkDestroyImageView(g_vk.dev, im->view, NULL);
    if (im->img) vkDestroyImage(g_vk.dev, im->img, NULL);
    vk_free_mem(&im->alloc);
    atomic_fetch_sub(&s_images, 1); atomic_fetch_sub(&s_bytes, im->alloc.size);
    free(im);
}
static void image_free_cb(void *a) { image_free(a); }

static vkt_image *image_create(const tex_t *t, int cls) {
    const vkf_info *fi = vkf_get(t->d.format);
    if (!fi) { static int n; if (n++ < 16) VKRED("texture format %#x has no Vulkan counterpart on this device: not created", t->d.format); return NULL; }
    vkt_image *im = calloc(1, sizeof *im);
    im->pool = t->pool; im->off = t->off; im->mem = t->mem; im->d = t->d; im->cls = cls; im->has_layout = t->has_layout; im->storage = t->L.size; im->fi = fi; im->serial = t->serial;
    VkImageType type; int cube;
    shape(&t->d, &type, &im->vtype, &im->w, &im->h, &im->depth, &im->layers, &cube);
    im->levels = t->d.levels ? t->d.levels : 1;
    uint32_t maxl = 1; for (uint32_t s = im->w > im->h ? im->w : im->h; s > 1; s >>= 1) maxl++;
    if (im->levels > maxl) im->levels = maxl;
    im->samples = t->d.samples > 1 ? (VkSampleCountFlagBits)t->d.samples : VK_SAMPLE_COUNT_1_BIT;
    im->aspect = vkf_aspect(fi->vk);
    VkImageUsageFlags u = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    int depth = (fi->flags & (VKF_DEPTH | VKF_STENCIL)) != 0;
    if (cls == VKT_GPU && !(fi->flags & VKF_COMPRESSED)) {   // NVN has no flag for "render target": every uncompressed GPU texture may be one
        if (depth && (fi->feat & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) u |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if (!depth && (fi->feat & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) u |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if ((t->d.flags & NVN_TEXTURE_FLAGS_IMAGE) && (fi->feat & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) u |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    VkImageCreateFlags cf = (cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0) | (!depth ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0);
    // E5B9G9R9 has no storage use in Vulkan, but OoT's NDSR light pass (agl_ndsr_vr_light.pck) writes its scene colour into one with raw 32-bit stores (the packing
    // done in the shader): storage through an R32_SFLOAT view (vk_replay.c img_view), allowed on the image by EXTENDED_USAGE. Without it the writes were lost and
    // the world stayed black
    // (NVN2 also lets a shader bind any texture as an image without NVN_TEXTURE_FLAGS_IMAGE, so no flag is required here)
    int e5_storage = cls == VKT_GPU && fi->vk == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    if (e5_storage) { u |= VK_IMAGE_USAGE_STORAGE_BIT; cf |= VK_IMAGE_CREATE_EXTENDED_USAGE_BIT; }
    // The same holds for every uncompressed colour image the GPU owns, whatever its flags: the 2026-11-05 hardware run bound texture 4271 (480x270, NVN format 0x14 = R16G16_UINT,
    // flags COMPRESSIBLE, no IMAGE) as the output image of an NDSR compute pass: "no storage use: writes are lost". Storage (when the format has it: the properties query below
    // drops it where it cannot be) costs a little compression on some GPUs; NXRT_VK_STORAGE_ALL=0 gives back the old rule (IMAGE flag only)
    static int storage_all = -1; if (storage_all < 0) { const char *e = getenv("NXRT_VK_STORAGE_ALL"); storage_all = !(e && e[0] == '0'); }
    if (storage_all && cls == VKT_GPU && !depth && !(fi->flags & VKF_COMPRESSED) && (fi->feat & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) u |= VK_IMAGE_USAGE_STORAGE_BIT;
    // Then, for the formats that cannot have it natively: the same for an sRGB colour image that a shader writes as a storage image: Vulkan has no storage use for the sRGB formats, the UNORM view (vk_replay.c img_view,
    // vkf_unorm_of) does. OoT's movie chain ends in one (an RGBA8_SRGB 2560x1440 the NDSR programs write as raw bytes and the HUD pass then samples): without storage on
    // the image the writes were lost. Only for big 2D single-sample images (or flagged IMAGE), where such a write target is plausible: storage use costs some drivers
    // their colour compression, so it is not given to every sRGB render target
    if (cls == VKT_GPU && !depth && !e5_storage && !(fi->flags & VKF_COMPRESSED) && !(u & VK_IMAGE_USAGE_STORAGE_BIT) && im->samples == VK_SAMPLE_COUNT_1_BIT && type == VK_IMAGE_TYPE_2D) {
        VkFormat un = vkf_unorm_of(fi->vk);
        if (un != fi->vk && ((t->d.flags & NVN_TEXTURE_FLAGS_IMAGE) || (im->w >= 1280 && im->h >= 720))) {
            VkFormatProperties up; vkGetPhysicalDeviceFormatProperties(g_vk.pd, un, &up);
            if (up.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) { u |= VK_IMAGE_USAGE_STORAGE_BIT; cf |= VK_IMAGE_CREATE_EXTENDED_USAGE_BIT; }
        }
    }
    if (type == VK_IMAGE_TYPE_3D && !(fi->flags & VKF_COMPRESSED)) cf |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    // drop what this format cannot have in this combination (storage on an sRGB format, a sample count)
    for (int pass = 0; pass < 3; pass++) {
        VkImageFormatProperties ip;
        if (vkGetPhysicalDeviceImageFormatProperties(g_vk.pd, fi->vk, type, VK_IMAGE_TILING_OPTIMAL, u, cf, &ip) == VK_SUCCESS && (ip.sampleCounts & im->samples)) break;
        if (pass == 0) u &= ~VK_IMAGE_USAGE_STORAGE_BIT; else if (pass == 1) u &= ~(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    }
    im->usage = u;
    im->rest = (u & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT)) ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, cf, type, fi->vk, {im->w, im->h, im->depth}, im->levels, im->layers, im->samples,
                            VK_IMAGE_TILING_OPTIMAL, u, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(g_vk.dev, &ci, NULL, &im->img) != VK_SUCCESS) { VKRED("vkCreateImage %ux%ux%u format %s (%d) levels %u layers %u failed", im->w, im->h, im->depth, fi->name, fi->vk, im->levels, im->layers); free(im); return NULL; }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_vk.dev, im->img, &mr);
    if (vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &im->alloc)) { VKRED("no device memory for a %ux%u %s texture (%llu bytes)", im->w, im->h, fi->name, (unsigned long long)mr.size); vkDestroyImage(g_vk.dev, im->img, NULL); free(im); return NULL; }
    vkBindImageMemory(g_vk.dev, im->img, im->alloc.mem, im->alloc.off);
    im->view = make_view(im, fi->vk, im->vtype, fi->swz, 0, im->levels, 0, im->layers, im->aspect & ~VK_IMAGE_ASPECT_STENCIL_BIT ? im->aspect & ~VK_IMAGE_ASPECT_STENCIL_BIT : im->aspect);
    VkFormat pf = vkf_unorm_of(fi->vk);
    im->present_view = pf != fi->vk ? make_view(im, pf, VK_IMAGE_VIEW_TYPE_2D, (VkComponentMapping){0}, 0, 1, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT) : VK_NULL_HANDLE;
    if (!im->present_view && im->vtype == VK_IMAGE_VIEW_TYPE_2D && !depth) im->present_view = im->view;
    atomic_fetch_add(&s_images, 1); atomic_fetch_add(&s_bytes, im->alloc.size);
    return im;
}

// ---------- Uploads from storage ----------
typedef struct { const s2tex_surface *s; const uint8_t *bl; size_t bl_size; uint8_t *lin; size_t row, slice; int rc; } deswizzle_args;
static void deswizzle_run(void *p) { deswizzle_args *a = p; a->rc = s2tex_deswizzle(a->s, a->bl, a->bl_size, a->lin, a->row, a->slice); }
static int guarded_deswizzle(const s2tex_surface *s, const uint8_t *bl, size_t bl_size, uint8_t *lin, size_t row, size_t slice) {
    deswizzle_args a = {s, bl, bl_size, lin, row, slice, 0};
    return plat_guarded(deswizzle_run, &a) ? -100 : a.rc;
}
typedef struct { uint8_t *d; const uint8_t *s; size_t n; } tcopy_args;
static void tcopy_run(void *p) { tcopy_args *a = p; memcpy(a->d, a->s, a->n); }
static int guarded_copy(uint8_t *dst, const uint8_t *src, size_t n) {
    tcopy_args a = {dst, src, n};
    if (!plat_guarded(tcopy_run, &a)) return 0;
    memset(dst, 0, n); return -1;
}
// The "one" of a component type, for the alpha that 3-component formats gain
static void expand_rgb(const uint8_t *src, uint8_t *dst, size_t texels, uint32_t bpb, int nvn_format) {
    uint32_t comp = bpb / 3, flags = vkf_get(nvn_format)->flags;
    VkFormat f = vkf_get(nvn_format)->vk;
    uint8_t one[4] = {0};
    if (comp == 1) one[0] = (flags & VKF_INT) ? 1 : (f == VK_FORMAT_R8G8B8A8_SNORM ? 0x7f : 0xff);
    else if (comp == 2) { uint16_t o = (flags & VKF_INT) ? 1 : f == VK_FORMAT_R16G16B16A16_SFLOAT ? 0x3c00 : f == VK_FORMAT_R16G16B16A16_SNORM ? 0x7fff : 0xffff; memcpy(one, &o, 2); }
    else { uint32_t o = (flags & VKF_INT) ? 1 : 0x3f800000u; memcpy(one, &o, 4); }
    for (size_t i = 0; i < texels; i++) { memcpy(dst + i * comp * 4, src + i * bpb, bpb); memcpy(dst + i * comp * 4 + bpb, one, comp); }
}
static void unpack_1010102s(const uint8_t *src, uint8_t *dst, size_t texels, int snorm) {
    for (size_t i = 0; i < texels; i++) {
        uint32_t v; memcpy(&v, src + i * 4, 4);
        int32_t c[4] = {(int32_t)(v << 22) >> 22, (int32_t)(v << 12) >> 22, (int32_t)(v << 2) >> 22, (int32_t)v >> 30};
        int16_t o[4];
        for (int k = 0; k < 4; k++) {
            if (!snorm) o[k] = (int16_t)c[k];
            else { float m = k < 3 ? 511.0f : 1.0f, x = c[k] / m; if (x < -1) x = -1; o[k] = (int16_t)lrintf(x * 32767.0f); }
        }
        memcpy(dst + i * 8, o, 8);
    }
}

typedef struct { VkBufferImageCopy r[64]; int n; VkBuffer src; } img_batch;
static void ib_flush(img_batch *b, vkt_image *im, VkCommandBuffer cb) { if (b->n) vkCmdCopyBufferToImage(cb, b->src, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)b->n, b->r); b->n = 0; }
static void ib_add(img_batch *b, vkt_image *im, VkCommandBuffer cb, VkBuffer src, VkBufferImageCopy r) { if (b->n == 64 || (b->n && b->src != src)) ib_flush(b, im, cb); b->src = src; b->r[b->n++] = r; }

static void upgrade_add(vka_key k, vkt_image *im, uint32_t level, uint32_t layer, uint32_t w, uint32_t h, uint32_t slices) {   // caller holds g_lk
    if (g_nupg == g_cupg) { g_cupg = g_cupg ? 2 * g_cupg : 64; g_upg = realloc(g_upg, (size_t)g_cupg * sizeof *g_upg); }
    g_upg[g_nupg++] = (upgrade_t){k, im, level, layer, w, h, slices};
}

// Every level and layer of t's storage into im, recorded into cb (whose submission owns the staging). Returns bytes uploaded, or -1
static int64_t upload(vkt_image *im, const tex_t *t, VkCommandBuffer cb) {
    const vkf_info *fi = im->fi; uint64_t t0 = now_ns(); int64_t total = 0;
    VkImageAspectFlags all = im->aspect;
    vk_image_barrier(cb, im->img, all, im->content ? im->rest : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    img_batch b; b.n = 0; b.src = VK_NULL_HANDLE;
    int is3d = t->d.target == NVN_TEXTURE_TARGET_3D;
    for (uint32_t level = 0; level < im->levels; level++)
        for (uint32_t layer = 0; layer < im->layers; layer++) {
            s2tex_surface s; uint64_t base; size_t row = 0;
            int k = nvn_texio_surface(t, level, layer, &s, &base, &row);
            if (k < 0) { if (!level) { static int n; if (n++ < 8) VKRED("texture %ux%u format %s: level %u layer %u is not addressable in its storage", im->w, im->h, fi->name, level, layer); } continue; }
            uint32_t lw = mip(im->w, level), lh = im->vtype == VK_IMAGE_VIEW_TYPE_1D || im->vtype == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? 1 : mip(im->h, level), ld = is3d ? mip(im->depth, level) : 1;
            size_t lrow = (size_t)s.width * s.bytes_per_block, lsize = lrow * s.height * s.depth;
            uint8_t *lin = malloc(lsize ? lsize : 1);
            int rc;
            if (k == 1) {   // linear: rows at the texture's stride
                rc = 0; for (uint32_t y = 0; y < s.height && !rc; y++) rc = guarded_copy(lin + y * lrow, t->mem + base + y * row, lrow);
            } else rc = guarded_deswizzle(&s, t->mem + base, (size_t)(t->L.size - base), lin, lrow, lrow * s.height);
            if (rc) { static int n; if (n++ < 8) VKRED("texture %ux%u format %s: level %u layer %u storage could not be read (%d)", im->w, im->h, fi->name, level, layer, rc); free(lin); continue; }
            // what goes to the staging buffer, and how Vulkan reads it
            const uint8_t *out = lin; size_t out_size = lsize; uint8_t *conv = NULL; uint32_t row_texels = s.width * fi->bw, img_rows = s.height * fi->bh;
            if (fi->conv == VKF_RGB_TO_RGBA) { conv = malloc((size_t)s.width * s.height * s.depth * fi->vbpb); expand_rgb(lin, conv, (size_t)s.width * s.height * s.depth, fi->bpb, t->d.format); out = conv; out_size = (size_t)s.width * s.height * s.depth * fi->vbpb; }
            else if (fi->conv == VKF_RGB10A2S_RGBA16) { conv = malloc(lsize * 2); unpack_1010102s(lin, conv, lsize / 4, fi->vk == VK_FORMAT_R16G16B16A16_SNORM); out = conv; out_size = lsize * 2; }
            else if (fi->conv == VKF_ASTC_BC7 || fi->conv == VKF_ASTC_RGBA8) {
                int srgb = (fi->flags & VKF_SRGB) != 0;
                if (fi->conv == VKF_ASTC_BC7) {
                    out_size = vka_bc7_size(lw, lh, ld); conv = malloc(out_size); vka_key key;
                    int q = vka_get_bc7(lin, lw, lh, ld, fi->bw, fi->bh, srgb, conv, &key);
                    if (q < 0) { free(conv); free(lin); continue; }
                    if (q == VKA_FAST) { upgrade_add(key, im, level, layer, lw, lh, ld); im->astc_fast++; atomic_fetch_add(&s_astc_fast, 1); }
                    else atomic_fetch_add(&s_astc_high, 1);
                    row_texels = (lw + 3) / 4 * 4; img_rows = (lh + 3) / 4 * 4;
                } else {
                    out_size = (size_t)lw * lh * ld * 4; conv = malloc(out_size);
                    if (vka_decode_rgba8(lin, lw, lh, ld, fi->bw, fi->bh, srgb, conv)) { free(conv); free(lin); continue; }
                    row_texels = lw; img_rows = lh;
                }
                out = conv;
            }
            else if (fi->conv == VKF_D24_D32F || fi->conv == VKF_D24S8_D32FS8) {   // 24-bit depth in the low bits (unverified on real data), to float; stencil from the top byte
                size_t n = lsize / 4; float *dz = malloc(n * 4); uint8_t *st = malloc(n);
                for (size_t i = 0; i < n; i++) { uint32_t v; memcpy(&v, lin + i * 4, 4); dz[i] = (float)(v & 0xffffff) / 16777215.0f; st[i] = (uint8_t)(v >> 24); }
                VkBuffer sb; VkDeviceSize so; uint8_t *p = vkm_staging(n * 4, 16, &sb, &so);
                if (p) { memcpy(p, dz, n * 4); ib_add(&b, im, cb, sb, (VkBufferImageCopy){so, row_texels, img_rows, {VK_IMAGE_ASPECT_DEPTH_BIT, level, layer, 1}, {0, 0, 0}, {lw, lh, ld}}); total += (int64_t)n * 4; }
                if (fi->conv == VKF_D24S8_D32FS8 && (p = vkm_staging(n, 16, &sb, &so))) { memcpy(p, st, n); ib_add(&b, im, cb, sb, (VkBufferImageCopy){so, row_texels, img_rows, {VK_IMAGE_ASPECT_STENCIL_BIT, level, layer, 1}, {0, 0, 0}, {lw, lh, ld}}); total += (int64_t)n; }
                free(dz); free(st); free(lin); continue;
            }
            else if (im->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {   // exact depth-stencil storage: one aspect at a time
                VkBuffer sb; VkDeviceSize so; size_t n = (size_t)s.width * s.height * s.depth; uint8_t *p;
                if (fi->vk == VK_FORMAT_D24_UNORM_S8_UINT && (p = vkm_staging(n * 5, 16, &sb, &so))) {
                    for (size_t i = 0; i < n; i++) { uint32_t v; memcpy(&v, lin + i * 4, 4); uint32_t d = v & 0xffffff; memcpy(p + i * 4, &d, 4); p[n * 4 + i] = (uint8_t)(v >> 24); }
                    ib_add(&b, im, cb, sb, (VkBufferImageCopy){so, row_texels, img_rows, {VK_IMAGE_ASPECT_DEPTH_BIT, level, layer, 1}, {0, 0, 0}, {lw, lh, ld}});
                    ib_add(&b, im, cb, sb, (VkBufferImageCopy){so + n * 4, row_texels, img_rows, {VK_IMAGE_ASPECT_STENCIL_BIT, level, layer, 1}, {0, 0, 0}, {lw, lh, ld}});
                } else if (fi->vk == VK_FORMAT_D32_SFLOAT_S8_UINT && (p = vkm_staging(n * 5, 16, &sb, &so))) {   // NVN: 32-bit float depth, then a 32-bit word with the stencil in its low byte
                    for (size_t i = 0; i < n; i++) { memcpy(p + i * 4, lin + i * 8, 4); p[n * 4 + i] = lin[i * 8 + 4]; }
                    ib_add(&b, im, cb, sb, (VkBufferImageCopy){so, row_texels, img_rows, {VK_IMAGE_ASPECT_DEPTH_BIT, level, layer, 1}, {0, 0, 0}, {lw, lh, ld}});
                    ib_add(&b, im, cb, sb, (VkBufferImageCopy){so + n * 4, row_texels, img_rows, {VK_IMAGE_ASPECT_STENCIL_BIT, level, layer, 1}, {0, 0, 0}, {lw, lh, ld}});
                }
                total += (int64_t)n * 5; free(lin); continue;
            }
            VkBuffer sb; VkDeviceSize so;
            uint8_t *p = vkm_staging(out_size, fi->vbpb >= 4 ? fi->vbpb : 4, &sb, &so);
            if (p) {
                memcpy(p, out, out_size);
                VkImageAspectFlags asp = fi->conv == VKF_S8_DS ? VK_IMAGE_ASPECT_STENCIL_BIT : (im->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) && !(im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_STENCIL_BIT : im->aspect & ~VK_IMAGE_ASPECT_STENCIL_BIT ? im->aspect & ~VK_IMAGE_ASPECT_STENCIL_BIT : im->aspect;
                ib_add(&b, im, cb, sb, (VkBufferImageCopy){so, row_texels, img_rows, {asp, level, is3d ? 0 : layer, 1}, {0, 0, 0}, {lw, lh, ld}});
                total += (int64_t)out_size;
            }
            free(conv); free(lin);
        }
    ib_flush(&b, im, cb);
    vk_image_barrier(cb, im->img, all, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, im->rest, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
    im->content = 1; im->uploads++;
    atomic_fetch_add(&s_up_bytes, (uint64_t)total); atomic_fetch_add(&s_up_ns, now_ns() - t0);
    return total;
}
static void clear_new(vkt_image *im, VkCommandBuffer cb) {   // a GPU texture's first content: zeros (its storage is not read; nxnative fills it once from a CPU pool, M5-C)
    vk_image_barrier(cb, im->img, im->aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageSubresourceRange rr = {im->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    if (im->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        VkClearDepthStencilValue z = {1.0f, 0};
        vkCmdClearDepthStencilImage(cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &rr);
    } else if (!(im->fi->flags & VKF_COMPRESSED)) { VkClearColorValue z = {{0}}; vkCmdClearColorImage(cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &rr); }
    vk_image_barrier(cb, im->img, im->aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, im->rest, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    im->content = 1;
}

// nxnative's tex_init_fill: a GPU texture whose storage already holds what the game put there before its first use takes that as its initial content (the
// uncompressed textures NVN cannot tell from render targets): in a pool the CPU cannot access (filled when the pool was made), an uncached one, or storage the
// CPU flushed. Not depth, multisample or compressible ones. Pokopia fills its 32x32x32 colour-grading LUT this way (an IMAGE-flagged RGBA8 3D texture)
int nvn_flushed(const void *p, uint64_t n);   // hle_nvn.c
typedef struct { const uint8_t *p; uint64_t n; int any; } any_args;
static void any_run(void *p_) { any_args *a = p_; const uint8_t *p = a->p; uint64_t n = a->n, i = 0, v; for (; i + 8 <= n; i += 8) { memcpy(&v, p + i, 8); if (v) { a->any = 1; return; } } for (; i < n; i++) if (p[i]) { a->any = 1; return; } }
static int mem_any(const uint8_t *p, uint64_t n) { any_args a = {p, n, 0}; return plat_guarded(any_run, &a) ? 0 : a.any; }
static int init_fill(const tex_t *t, const vkt_image *im) {
    if (!t->mem || !t->pool || (t->pool->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) || t->has_layout || t->d.samples > 1 || (t->d.flags & NVN_TEXTURE_FLAGS_COMPRESSIBLE)) return 0;
    if ((im->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) || (im->fi->flags & VKF_COMPRESSED)) return 0;
    int no_cpu = (t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) != 0, uncached = (t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED) != 0;
    if (!no_cpu && !uncached && !nvn_flushed(t->mem, t->L.size)) return 0;
    if (!mem_any(t->mem, t->L.size)) return 0;
    int why = no_cpu ? 0 : uncached ? 1 : 2;
    static uint64_t n[3]; if (n[why]++ < 4) VKLOG("initial content: ser=%llu %ux%ux%u format %#x filled from its storage, %s (%llu so far)", (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format,
                                                 (const char *[]){"a CPU_NO_ACCESS pool's", "a CPU_UNCACHED pool's", "flushed by the CPU before its first use"}[why], (unsigned long long)n[why]);
    return 1;
}

// Records into the submission being recorded, or into a one-shot when there is none (submitted and waited for at the end)
static VkCommandBuffer cb_get(int *once) { VkCommandBuffer cb = vks_upload_cb(); *once = 0; if (!cb && vks_owner() < 0) { cb = vk_once_begin(); *once = 1; } return cb; }
static void cb_put(VkCommandBuffer cb, int once) { if (once) vk_once_end(cb); }

// Uploads a storage-backed image when its pages were written since its last upload (nxnative's order: stamp, protect, then read). Caller holds g_lk
static void refresh(vkt_image *im, const tex_t *t) {
    if (!im->span) im->span = nvn_dirty_span(t->mem, (size_t)t->L.size);
    if (im->content && im->span && !nvn_dirty_since_span(im->span, t->mem, (size_t)t->L.size, im->stamp)) return;
    int once; VkCommandBuffer cb = cb_get(&once);
    if (!cb) return;
    uint64_t st = nvn_dirty_now();
    if (im->span) nvn_dirty_protect(t->mem, (size_t)t->L.size);
    int first = !im->content;
    if (upload(im, t, cb) >= 0) { im->stamp = st; if (first) atomic_fetch_add(&s_uploads, 1); else atomic_fetch_add(&s_reuploads, 1); }
    cb_put(cb, once);
}

vkt_image *vkt_resolve(const tex_t *t) {
    if (!t || !t->serial || !g_vk.up) return NULL;
    if (t->d.target == NVN_TEXTURE_TARGET_BUFFER) return NULL;   // texel buffers: a VkBufferView over the pool mirror (M5-C)
    plat_rw_wrlock(&g_lk);
    int cls = from_storage(t) ? VKT_STORAGE : VKT_GPU;
    skey k; key_of(t, cls, &k);
    vkt_image *im = vk_hm_get(&g_share, &k);
    if (!im) {
        // a new texture over storage: entries of the same pool overlapping it under another description go, unless used in the last 16 frames (nxnative's alias_take_over)
        if (t->pool) for (uint32_t i = 0; i < g_share.cap; i++) {
            vkt_image *o = g_share.vals[i]; if (!o || o->pool != t->pool) continue;
            int64_t lo = t->off, hi = t->off + (int64_t)t->L.size;
            if (o->off >= hi || o->off + (int64_t)o->storage <= lo || g_frame - o->last_frame < 16) continue;
            skey ok; memcpy(&ok, g_share.keys + (size_t)i * g_share.klen, sizeof ok);
            vk_hm_del(&g_share, &ok); i--;   // the deletion shifted entries: look at this slot again
            for (uint32_t j = 0; j < g_serial.cap; j++) if (g_serial.vals[j] == o) { uint64_t sk; memcpy(&sk, g_serial.keys + (size_t)j * 8, 8); vk_hm_del(&g_serial, &sk); j--; }
            vk_retire(image_free_cb, o); atomic_fetch_add(&s_evicted, 1); atomic_fetch_add(&g_reg_epoch, 1);
        }
        if (!(im = image_create(t, cls))) { plat_rw_wrunlock(&g_lk); return NULL; }
        vk_hm_put(&g_share, &k, im);
        if (cls == VKT_GPU) { int once; VkCommandBuffer cb = cb_get(&once); if (cb) { if (!init_fill(t, im) || upload(im, t, cb) < 0) clear_new(im, cb); cb_put(cb, once); } }
    } else if (cls == VKT_GPU && !im->content) {   // M6 phase B: made where nothing could record its first content (and layout): recorded now, before its first use
        int once; VkCommandBuffer cb = cb_get(&once);
        if (cb) { static int n; if (n++ < 4) VKLOG("texture ser=%llu %ux%ux%u: its initial content recorded at a later use", (unsigned long long)t->serial, im->w, im->h, im->layers);
                  if (!init_fill(t, im) || upload(im, t, cb) < 0) clear_new(im, cb); cb_put(cb, once); }
        atomic_fetch_add(&s_shared, 1);
    } else atomic_fetch_add(&s_shared, 1);
    vk_hm_put(&g_serial, &t->serial, im);
    im->last_frame = g_frame; im->serial = t->serial;
    if (cls == VKT_STORAGE) refresh(im, t);
    plat_rw_wrunlock(&g_lk);
    return im;
}

// ---------- Views: the image's fallback applied to every view (research/01 lesson 8) ----------
static VkComponentSwizzle nvn_swz(unsigned s) { static const VkComponentSwizzle m[6] = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A}; return s < 6 ? m[s] : VK_COMPONENT_SWIZZLE_IDENTITY; }
static VkComponentSwizzle compose(VkComponentSwizzle user, const VkComponentMapping *base) {   // the user's channel read through the format's base mapping
    switch (user) {
    case VK_COMPONENT_SWIZZLE_R: return base->r == VK_COMPONENT_SWIZZLE_IDENTITY ? VK_COMPONENT_SWIZZLE_R : base->r;
    case VK_COMPONENT_SWIZZLE_G: return base->g == VK_COMPONENT_SWIZZLE_IDENTITY ? VK_COMPONENT_SWIZZLE_G : base->g;
    case VK_COMPONENT_SWIZZLE_B: return base->b == VK_COMPONENT_SWIZZLE_IDENTITY ? VK_COMPONENT_SWIZZLE_B : base->b;
    case VK_COMPONENT_SWIZZLE_A: return base->a == VK_COMPONENT_SWIZZLE_IDENTITY ? VK_COMPONENT_SWIZZLE_A : base->a;
    default: return user;
    }
}
VkImageView vkt_view_raw_(vkt_image *im, VkFormat f, VkImageViewType vt, uint32_t bl, uint32_t nl, uint32_t b0, uint32_t nly, VkImageAspectFlags asp, const VkComponentMapping *mp);
VkImageView vkt_view(vkt_image *im, const view_t *v, int set_swz, uint16_t swz) {
    if (!im) return VK_NULL_HANDLE;
    if (!v && !set_swz) return im->view;
    int fmt = v && v->set_format ? v->format : im->d.format;
    uint32_t bl = v && v->set_levels ? v->base_level : 0, nl = v && v->set_levels ? v->nlevels : im->levels;
    uint32_t b0 = v && v->set_layers ? v->min_layer : 0, nly = v && v->set_layers ? v->nlayers : im->layers;
    if (bl >= im->levels) bl = im->levels - 1; if (!nl || bl + nl > im->levels) nl = im->levels - bl;
    if (b0 >= im->layers) b0 = im->layers - 1; if (!nly || b0 + nly > im->layers) nly = im->layers - b0;
    VkFormat f = vkf_view_format(im->d.format, fmt);   // the image's own fallback, whatever the view asks for
    const vkf_info *vi = vkf_get(fmt); const VkComponentMapping *base = vi && f == vi->vk ? &vi->swz : &im->fi->swz;
    VkComponentMapping m = *base;
    if (set_swz) m = (VkComponentMapping){compose(nvn_swz(swz & 7), base), compose(nvn_swz(swz >> 3 & 7), base), compose(nvn_swz(swz >> 6 & 7), base), compose(nvn_swz(swz >> 9 & 7), base)};
    if (v && v->set_swizzle) m = (VkComponentMapping){compose(nvn_swz(v->swz[0]), &m), compose(nvn_swz(v->swz[1]), &m), compose(nvn_swz(v->swz[2]), &m), compose(nvn_swz(v->swz[3]), &m)};
    VkImageViewType vt = im->vtype;
    if (v && v->set_target) {
        switch (v->target) {
        case NVN_TEXTURE_TARGET_2D: case NVN_TEXTURE_TARGET_RECTANGLE: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE: vt = VK_IMAGE_VIEW_TYPE_2D; nly = 1; break;
        case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
        case NVN_TEXTURE_TARGET_CUBEMAP: vt = VK_IMAGE_VIEW_TYPE_CUBE; nly = 6; break;
        case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: vt = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY; break;
        case NVN_TEXTURE_TARGET_1D: vt = VK_IMAGE_VIEW_TYPE_1D; nly = 1; break;
        case NVN_TEXTURE_TARGET_1D_ARRAY: vt = VK_IMAGE_VIEW_TYPE_1D_ARRAY; break;
        case NVN_TEXTURE_TARGET_3D: vt = VK_IMAGE_VIEW_TYPE_3D; break;
        }
    }
    VkImageAspectFlags asp = vkt_sample_aspect(im->aspect, v && v->set_dsmode, v ? v->dsmode : 0);
    if ((vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) && !(im->vtype == VK_IMAGE_VIEW_TYPE_CUBE || im->vtype == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)) {
        static int n; if (n++ < 8) VKRED("a cube view of a %ux%u texture that is not a cube map: viewed as a 2D array", im->w, im->h);
        vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    }
    if (vt == VK_IMAGE_VIEW_TYPE_CUBE && nly != 6) nly = b0 + 6 <= im->layers ? 6 : nly;
    return vkt_view_raw_(im, f, vt, bl, nl, b0, nly, asp, &m);
}
// A view of exact parameters (attachments, storage images, views the shader's declaration asks for), cached with the image's views
VkImageView vkt_view_raw_(vkt_image *im, VkFormat f, VkImageViewType vt, uint32_t bl, uint32_t nl, uint32_t b0, uint32_t nly, VkImageAspectFlags asp, const VkComponentMapping *mp) {
    VkComponentMapping m = mp ? *mp : (VkComponentMapping){0};
    if (bl >= im->levels) bl = im->levels - 1; if (!nl || bl + nl > im->levels) nl = im->levels - bl;
    uint32_t all = im->vtype == VK_IMAGE_VIEW_TYPE_3D && vt != VK_IMAGE_VIEW_TYPE_3D ? mip(im->depth, bl) : im->layers;   // a 3D level viewed as a 2D array: its depth slices
    if (b0 >= all) b0 = all - 1; if (!nly || b0 + nly > all) nly = all - b0;
    uint64_t key = (uint64_t)f ^ (uint64_t)vt << 20 ^ (uint64_t)bl << 24 ^ (uint64_t)nl << 28 ^ (uint64_t)b0 << 34 ^ (uint64_t)nly << 46 ^ (uint64_t)asp << 58 ^
                   ((uint64_t)m.r | (uint64_t)m.g << 3 | (uint64_t)m.b << 6 | (uint64_t)m.a << 9) * 0x9E3779B97F4A7C15ull;
    plat_rw_wrlock(&g_lk);
    for (int i = 0; i < im->nviews; i++) if (im->views[i].key == key) { VkImageView r = im->views[i].v; plat_rw_wrunlock(&g_lk); return r; }
    VkImageView nv = make_view(im, f, vt, m, bl, nl, b0, nly, asp);
    if (nv) {   // a full cache drops its oldest view behind the GPU
        if (im->nviews == VKT_NVIEWS) { vk_retire(view_free_cb, (void *)im->views[0].v); atomic_fetch_add(&g_reg_epoch, 1); memmove(im->views, im->views + 1, (VKT_NVIEWS - 1) * sizeof im->views[0]); im->nviews--; }
        im->views[im->nviews].key = key; im->views[im->nviews].v = nv; im->nviews++;
    }
    plat_rw_wrunlock(&g_lk);
    return nv ? nv : im->view;
}

// ---------- Samplers ----------
static VkSamplerAddressMode wrap(uint8_t w) {
    switch (w) {
    case NVN_WRAP_MODE_REPEAT: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case NVN_WRAP_MODE_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case NVN_WRAP_MODE_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case NVN_WRAP_MODE_MIRROR_CLAMP: case NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE: case NVN_WRAP_MODE_MIRROR_CLAMP_TO_BORDER:   // GL's mirror-clamp modes: the edge variant (border: M5-C)
        return g_vk.has.mirror_clamp_edge ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;   // CLAMP (GL's half-border clamp) and CLAMP_TO_EDGE
    }
}
// The compare function as Vulkan numbers it: NVN's (NEVER = 1 .. ALWAYS = 8, the GL order) shifted by one; an unset one is NEVER. Dref OP texel in both APIs
VkCompareOp vkt_cmp_op(uint8_t f) { return (VkCompareOp)(f >= 1 && f <= 8 ? f - 1 : 0); }
// The sampler's cache key: the whole parameter block (no padding: 8 bytes and 8 floats), with what the sampler ignores zeroed: the compare function while the compare mode is off
samp_par vkt_samp_key(const samp_par *p) {
    _Static_assert(sizeof(samp_par) == 40, "samp_par is the sampler cache's key: bytes, no padding");
    samp_par k = *p; if (k.cmp_mode != NVN_COMPARE_MODE_COMPARE_R_TO_TEXTURE) k.cmp_func = 0;
    return k;
}
// P3 / N1: the sampler a texture of the given aspects needs. A sampler that asks for depth comparison on an image with no depth aspect is undefined in Vulkan (VUID-vkCmdDraw-None-06479; a
// 1x1 black / white colour texture under a shadow sampler: the game's default for lights without shadows): the same sampler with the comparison off. 1 when it changed *out, else *out = *p
int vkt_samp_for_aspect(const samp_par *p, VkImageAspectFlags aspect, samp_par *out) {
    *out = *p;
    if (p->cmp_mode != NVN_COMPARE_MODE_COMPARE_R_TO_TEXTURE || (aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) return 0;
    out->cmp_mode = NVN_COMPARE_MODE_NONE; out->cmp_func = 0;
    return 1;
}
int vkt_compare_fix_parse(const char *e) { return !(e && *e == '0'); }   // NXRT_VK_COMPARE_FIX: on unless "0..." (the owner's A/B switch)
int vkt_compare_fix_enabled(void) { static int on = -1; if (on < 0) on = vkt_compare_fix_parse(getenv("NXRT_VK_COMPARE_FIX")); return on; }
// A depth-comparing sample of a texture (the shader declares a 2D depth-compare image, "t2ds") reads a depth view; a texture with no depth aspect is stood in for by the replay's 1x1 depth twin
int vkt_cmp_needs_twin(int shader_depth_compare, VkImageAspectFlags image_aspect) { return shader_depth_compare && !(image_aspect & VK_IMAGE_ASPECT_DEPTH_BIT); }
// The aspect a sampled view of an image shows: depth, or stencil when the view asks for stencil reads of a depth-stencil image (NVN TextureView DepthStencilMode)
VkImageAspectFlags vkt_sample_aspect(VkImageAspectFlags a, int set_dsmode, int dsmode) {
    return a == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) ? (set_dsmode && dsmode == 1 ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT) : a;
}
static vk_hmap g_cmpfix_seen = {sizeof(samp_par) + 4};   // (sampler, NVN format) → logged
static _Atomic uint64_t s_cmpfix;
// Whether (sampler, format) is new: the first sight of each pair logs once
int vkt_cmpfix_first(const samp_par *p, int nvn_format) {
    uint8_t k[sizeof(samp_par) + 4]; samp_par q = vkt_samp_key(p); memcpy(k, &q, sizeof q); memcpy(k + sizeof q, &nvn_format, 4);
    plat_rw_wrlock(&g_lk); int fresh = !vk_hm_get(&g_cmpfix_seen, k); if (fresh) vk_hm_put(&g_cmpfix_seen, k, (void *)1); plat_rw_wrunlock(&g_lk);
    return fresh;
}
uint64_t vkt_cmpfix_count(void) { return atomic_load(&s_cmpfix); }
VkSampler vkt_sampler_for(const samp_par *p, const vkt_image *im) {
    samp_par q;
    if (im && vkt_compare_fix_enabled() && vkt_samp_for_aspect(p, im->aspect, &q)) {
        atomic_fetch_add(&s_cmpfix, 1);
        if (vkt_cmpfix_first(p, (int)im->d.format))
            VKLOG("compare-fix: a sampler with depth comparison (func %u) meets a colour image (%s, ser%llu %ux%u, NVN format %#x): bound with the comparison off (NXRT_VK_COMPARE_FIX=0 turns this off)",
                  (unsigned)p->cmp_func, im->fi ? im->fi->name : "?", (unsigned long long)im->serial, im->w, im->h, (unsigned)im->d.format);
        return vkt_sampler(&q);
    }
    return vkt_sampler(p);
}
VkSampler vkt_sampler(const samp_par *pp) {
    samp_par key = vkt_samp_key(pp); const samp_par *p = &key;   // the cache and the sampler made from it see the key's form
    plat_rw_wrlock(&g_lk);
    VkSampler s = (VkSampler)vk_hm_get(&g_samp, p);
    if (s) { plat_rw_wrunlock(&g_lk); return s; }
    int mipf = p->minf >= NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST;
    VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0,
        p->magf == NVN_MAG_FILTER_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        (p->minf == NVN_MIN_FILTER_LINEAR || p->minf == NVN_MIN_FILTER_LINEAR_MIPMAP_NEAREST || p->minf == NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        (p->minf == NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR || p->minf == NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        wrap(p->ws), wrap(p->wt), wrap(p->wr), p->lod_bias, p->aniso > 1.0f && g_vk.f10.samplerAnisotropy, p->aniso > 1.0f ? fminf(p->aniso, g_vk.props.limits.maxSamplerAnisotropy) : 1.0f,
        p->cmp_mode == NVN_COMPARE_MODE_COMPARE_R_TO_TEXTURE, vkt_cmp_op(p->cmp_func),
        mipf ? p->lod_min : 0.0f, mipf ? p->lod_max : 0.25f, VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, VK_FALSE};
    VkSamplerCustomBorderColorCreateInfoEXT cb = {VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT, NULL, {{p->border[0], p->border[1], p->border[2], p->border[3]}}, VK_FORMAT_UNDEFINED};
    VkSamplerReductionModeCreateInfo rm = {VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO, NULL,
        p->reduction == NVN_SAMPLER_REDUCTION_MIN ? VK_SAMPLER_REDUCTION_MODE_MIN : p->reduction == NVN_SAMPLER_REDUCTION_MAX ? VK_SAMPLER_REDUCTION_MODE_MAX : VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE};
    const void **tail = &ci.pNext;
    int uses_border = ci.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER || ci.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER || ci.addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    if (uses_border) {
        const float *b = p->border; int std = b[0] == 0 && b[1] == 0 && b[2] == 0 ? (b[3] == 0 ? 1 : b[3] == 1 ? 2 : 0) : b[0] == 1 && b[1] == 1 && b[2] == 1 && b[3] == 1 ? 3 : 0;
        if (std) ci.borderColor = std == 1 ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK : std == 2 ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        else if (g_vk.has.custom_border && g_vk.has.custom_border_noformat) { ci.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT; *tail = &cb; tail = (const void **)&cb.pNext; }
    }
    if (rm.reductionMode != VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE && g_vk.has.minmax_filter) { *tail = &rm; tail = (const void **)&rm.pNext; }
    if (vkCreateSampler(g_vk.dev, &ci, NULL, &s) == VK_SUCCESS) { vk_hm_put(&g_samp, p, (void *)s); atomic_fetch_add(&s_samplers, 1); }
    else s = VK_NULL_HANDLE;
    plat_rw_wrunlock(&g_lk);
    return s;
}

// ---------- Object lifetimes and registrations ----------
void vkt_tex_init(const tex_t *t) { if (!t->serial) return; plat_rw_wrlock(&g_lk); vk_hm_put(&g_live, &t->serial, (void *)1); plat_rw_wrunlock(&g_lk); }
void vkt_tex_release(const tex_t *t) {   // the serial goes; its image stays shared by storage until its pool is finalized or another texture takes the storage
    if (!t->serial) return;
    plat_rw_wrlock(&g_lk); vk_hm_del(&g_live, &t->serial); vk_hm_del(&g_serial, &t->serial); atomic_fetch_add(&g_reg_epoch, 1); plat_rw_wrunlock(&g_lk);
}
void vkt_tex_cpu_written(const tex_t *t) {
    if (!t->serial) return;
    plat_rw_wrlock(&g_lk);
    if (!vk_hm_get(&g_cpu, &t->serial)) {
        vk_hm_put(&g_cpu, &t->serial, t->pool ? (void *)t->pool : (void *)1); atomic_fetch_add(&s_cpuw, 1);
        static int n; if (n++ < 16) VKLOG("CPU-written texture ser=%llu %ux%ux%u format %#x flags %#x: uploaded from its storage, again when its pages change", (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format, t->d.flags);
    }
    plat_rw_wrunlock(&g_lk);
}
int vkt_tex_gpu_owned(uint64_t serial) {
    plat_rw_rdlock(&g_lk);
    vkt_image *im = vk_hm_get(&g_serial, &serial);
    int r = im && im->cls == VKT_GPU && (im->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT));
    plat_rw_rdunlock(&g_lk);
    return r;
}
void vkt_drop_pool(const void *pool) {
    if (!pool) return;
    plat_rw_wrlock(&g_lk);
    for (uint32_t i = 0; i < g_share.cap; i++) {
        vkt_image *o = g_share.vals[i]; if (!o || o->pool != pool) continue;
        skey ok; memcpy(&ok, g_share.keys + (size_t)i * g_share.klen, sizeof ok);
        vk_hm_del(&g_share, &ok); i--;
        for (uint32_t j = 0; j < g_serial.cap; j++) if (g_serial.vals[j] == o) { uint64_t sk; memcpy(&sk, g_serial.keys + (size_t)j * 8, 8); vk_hm_del(&g_serial, &sk); j--; }
        for (int u = 0; u < g_nupg; ) if (g_upg[u].im == o) g_upg[u] = g_upg[--g_nupg]; else u++;
        vk_retire(image_free_cb, o); atomic_fetch_add(&s_evicted, 1); atomic_fetch_add(&g_reg_epoch, 1);
    }
    for (uint32_t i = 0; i < g_cpu.cap; i++) if (g_cpu.vals[i] == pool) { uint64_t sk; memcpy(&sk, g_cpu.keys + (size_t)i * 8, 8); vk_hm_del(&g_cpu, &sk); i--; }
    plat_rw_wrunlock(&g_lk);
}
void vkt_reg_tex(int id, const tex_t *t, const view_t *v, int image) {
    if (id < 0 || id > (1 << 20)) return;
    plat_rw_wrlock(&g_lk);
    if (id >= g_nreg) { int n = id + 1024; g_reg = realloc(g_reg, (size_t)n * sizeof *g_reg); memset(g_reg + g_nreg, 0, (size_t)(n - g_nreg) * sizeof *g_reg); g_nreg = n; }
    vkt_reg *r = &g_reg[id]; r->t = *t; r->has_view = v != NULL; if (v) r->v = *v; r->image = image; r->live = 1; r->gone = 0;
    atomic_fetch_add(&g_reg_epoch, 1);
    plat_rw_wrunlock(&g_lk);
}
void vkt_reg_tex_gone(int id) { plat_rw_wrlock(&g_lk); if (id >= 0 && id < g_nreg) g_reg[id].gone = 1; atomic_fetch_add(&g_reg_epoch, 1); plat_rw_wrunlock(&g_lk); }
const vkt_reg *vkt_reg_get(int id) { return id >= 0 && id < g_nreg && g_reg[id].live ? &g_reg[id] : NULL; }
void vkt_reg_samp(int id, const samp_par *p) {
    if (id < 0 || id > (1 << 20)) return;
    plat_rw_wrlock(&g_lk);
    if (id >= g_nsreg) { int n = id + 256; g_sreg = realloc(g_sreg, (size_t)n * sizeof *g_sreg); g_sreg_ok = realloc(g_sreg_ok, (size_t)n); memset(g_sreg_ok + g_nsreg, 0, (size_t)(n - g_nsreg)); g_nsreg = n; }
    g_sreg[id] = *p; g_sreg_ok[id] = 1; atomic_fetch_add(&g_reg_epoch, 1);
    plat_rw_wrunlock(&g_lk);
}
const samp_par *vkt_samp_get(int id) { return id >= 0 && id < g_nsreg && g_sreg_ok[id] ? &g_sreg[id] : NULL; }
void vkt_set_pool_n(int n) { g_pool_n = n; }

void vkt_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    // The storage is the ground truth for everything the CPU or a copy wrote; content the GPU rendered or wrote is read back from the image (the caller
    // has waited for the GPU as NVN asks; the replay's submissions are flushed and waited for here as well, as nxnative's mtl_read_texels does)
    plat_rw_rdlock(&g_lk);
    vkt_image *im = t->serial ? vk_hm_get(&g_serial, &t->serial) : NULL;
    int gpu = im && im->cls == VKT_GPU && im->content && !(im->fi->flags & VKF_COMPRESSED) && im->fi->conv == VKF_COPY;
    plat_rw_rdunlock(&g_lk);
    if (gpu && g_vk.up) {
        int nz = r->depth ? r->depth : 1, is3d = t->d.target == NVN_TEXTURE_TARGET_3D;
        size_t wb = (size_t)r->width * im->fi->vbpb, hb = (size_t)r->height;
        uint8_t *tmp = malloc(wb * hb * nz ? wb * hb * nz : 1);
        vks_finish();
        int ok = 1;
        for (int z = 0; z < nz && ok; z++) {
            NVNcopyRegion rr = *r; rr.depth = 1; if (is3d) rr.zoffset = r->zoffset + z;
            ok = !vkt_readback_region(im, level, is3d ? 0 : layer0 + r->zoffset + z, &rr, tmp + (size_t)z * wb * hb, wb * hb);
        }
        if (ok) { for (int z = 0; z < nz; z++) for (size_t y = 0; y < hb; y++) memcpy(dst + z * img + y * row, tmp + (size_t)z * wb * hb + y * wb, wb); free(tmp); return; }
        free(tmp);
        static int n; if (n++ < 4) VKRED("ReadTexels of GPU texture ser=%llu: the readback failed, the storage is read instead", (unsigned long long)t->serial);
    }
    if (!nvn_texio_read(t, level, layer0, r, dst, row, img)) return;
    static int n; if (n++ < 4) VKRED("ReadTexels %dx%d of level %u, format %#x: the storage cannot be addressed", r->width, r->height, level, t->d.format);
}

vkt_image *vkt_by_serial(uint64_t serial) { plat_rw_rdlock(&g_lk); vkt_image *im = vk_hm_get(&g_serial, &serial); plat_rw_rdunlock(&g_lk); return im; }
int vkt_reg_copy(int id, vkt_reg *out) {
    plat_rw_rdlock(&g_lk);
    int ok = id > 0 && id < g_nreg && g_reg[id].live;
    if (ok) *out = g_reg[id];
    plat_rw_rdunlock(&g_lk);
    return ok;
}
int vkt_samp_copy(int id, samp_par *out) {
    plat_rw_rdlock(&g_lk);
    int ok = id >= 0 && id < g_nsreg && g_sreg_ok[id];
    if (ok) *out = g_sreg[id];
    plat_rw_rdunlock(&g_lk);
    return ok;
}

int vkt_readback_region(vkt_image *im, unsigned level, unsigned layer, const NVNcopyRegion *r, uint8_t *dst, size_t size) {
    if (!im || level >= im->levels || r->width <= 0 || r->height <= 0) return -1;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer b; vk_alloc m;
    if (vkCreateBuffer(g_vk.dev, &bci, NULL, &b) != VK_SUCCESS) return -1;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, b, &mr);
    if (vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0, &m) &&
        vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, &m)) { vkDestroyBuffer(g_vk.dev, b, NULL); return -1; }
    vkBindBufferMemory(g_vk.dev, b, m.mem, m.off);
    VkCommandBuffer cb = vk_once_begin();
    VkImageAspectFlags asp = im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : im->aspect;
    vk_image_barrier(cb, im->img, im->aspect, im->rest, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    int is3d = im->vtype == VK_IMAGE_VIEW_TYPE_3D;
    VkBufferImageCopy c = {0, 0, 0, {asp, level, is3d ? 0 : layer, 1}, {r->xoffset, r->yoffset, is3d ? r->zoffset : 0}, {(uint32_t)r->width, (uint32_t)r->height, 1}};
    vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b, 1, &c);
    vk_image_barrier(cb, im->img, im->aspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im->rest, VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    vk_mem_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    int rc = vk_once_end(cb);
    if (!rc) memcpy(dst, m.map, size);
    vkDestroyBuffer(g_vk.dev, b, NULL); vk_free_mem(&m);
    return rc;
}

int vkt_readback(vkt_image *im, unsigned level, unsigned layer, uint8_t *dst, size_t size) {
    if (!im || level >= im->levels || layer >= im->layers) return -1;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer b; vk_alloc m;
    if (vkCreateBuffer(g_vk.dev, &bci, NULL, &b) != VK_SUCCESS) return -1;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, b, &mr);
    if (vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0, &m) &&
        vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, &m)) { vkDestroyBuffer(g_vk.dev, b, NULL); return -1; }
    vkBindBufferMemory(g_vk.dev, b, m.mem, m.off);
    VkCommandBuffer cb = vk_once_begin();
    VkImageAspectFlags asp = im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : im->aspect;
    vk_image_barrier(cb, im->img, im->aspect, im->rest, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    uint32_t lw = mip(im->w, level), lh = mip(im->h, level), ld = im->vtype == VK_IMAGE_VIEW_TYPE_3D ? mip(im->depth, level) : 1;
    VkBufferImageCopy c = {0, 0, 0, {asp, level, layer, 1}, {0, 0, 0}, {lw, lh, ld}};
    vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b, 1, &c);
    vk_image_barrier(cb, im->img, im->aspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im->rest, VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    vk_mem_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    int rc = vk_once_end(cb);
    if (!rc) memcpy(dst, m.map, size);
    vkDestroyBuffer(g_vk.dev, b, NULL); vk_free_mem(&m);
    return rc;
}

// ---------- ASTC high-quality passes, presentation, frames ----------
static void on_ready(vka_key k, const uint8_t *bc7, size_t size, void *user) {
    (void)user;
    if (g_nready == g_cready) { g_cready = g_cready ? 2 * g_cready : 16; g_ready_key = realloc(g_ready_key, (size_t)g_cready * sizeof *g_ready_key); g_ready_data = realloc(g_ready_data, (size_t)g_cready * sizeof *g_ready_data); g_ready_size = realloc(g_ready_size, (size_t)g_cready * sizeof *g_ready_size); }
    g_ready_key[g_nready] = k; g_ready_data[g_nready] = malloc(size); memcpy(g_ready_data[g_nready], bc7, size); g_ready_size[g_nready] = size; g_nready++;
}
void vkt_in_submission(void) {
    vka_poll(on_ready, NULL, 16);
    if (!g_nready) return;
    VkCommandBuffer cb = vks_upload_cb();
    if (!cb) return;
    plat_rw_wrlock(&g_lk);
    for (int r = 0; r < g_nready && vkm_budget_left(); ) {
        int used = 0;
        for (int u = 0; u < g_nupg; ) {
            upgrade_t *g = &g_upg[u];
            if (g->key.lo != g_ready_key[r].lo || g->key.hi != g_ready_key[r].hi || g_ready_size[r] != vka_bc7_size(g->w, g->h, g->slices)) { u++; continue; }
            VkBuffer sb; VkDeviceSize so; uint8_t *p = vkm_staging(g_ready_size[r], 16, &sb, &so);
            if (!p) break;
            memcpy(p, g_ready_data[r], g_ready_size[r]);
            vkt_image *im = g->im;
            VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, g->level, 1, im->vtype == VK_IMAGE_VIEW_TYPE_3D ? 0 : g->layer, 1};
            VkImageMemoryBarrier2 b0 = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                        im->rest, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, im->img, rr};
            VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &b0}; vkCmdPipelineBarrier2(cb, &di);
            VkBufferImageCopy c = {so, (g->w + 3) / 4 * 4, (g->h + 3) / 4 * 4, {VK_IMAGE_ASPECT_COLOR_BIT, g->level, rr.baseArrayLayer, 1}, {0, 0, 0}, {g->w, g->h, g->slices}};
            vkCmdCopyBufferToImage(cb, sb, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            b0.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; b0.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT; b0.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; b0.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
            b0.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b0.newLayout = im->rest; vkCmdPipelineBarrier2(cb, &di);
            vkm_budget_take(g_ready_size[r]); im->astc_fast--; atomic_fetch_add(&s_astc_high, 1);
            g_upg[u] = g_upg[--g_nupg]; used = 1;
        }
        (void)used;
        free(g_ready_data[r]); g_ready_key[r] = g_ready_key[g_nready - 1]; g_ready_data[r] = g_ready_data[g_nready - 1]; g_ready_size[r] = g_ready_size[g_nready - 1]; g_nready--;
    }
    plat_rw_wrunlock(&g_lk);
}

int vkt_present_source(uint64_t serial, vkp_source *out) {
    plat_rw_rdlock(&g_lk);
    vkt_image *im = vk_hm_get(&g_serial, &serial);
    int ok = im && im->content && im->present_view;
    if (ok) { out->view = im->present_view; out->layout = im->rest; out->w = im->w; out->h = im->h; }
    plat_rw_rdunlock(&g_lk);
    return ok;
}

void vkt_frame(uint64_t frame) {
    g_frame = frame;
    if (frame % 300) return;
    vkt_stats_t s; vkt_stats(&s); vka_stats_t a; vka_stats(&a);
    VKLOG("textures up to frame %llu: %llu images %.1f MB, uploads %llu + %llu again (%.1f MB, %.1f ms), shared %llu, CPU-written %llu, evicted %llu, samplers %llu; "
          "ASTC: %llu fast passes (%.1f Mtexel/s), %llu high-quality done, cache hits %llu/%llu",
          (unsigned long long)frame, (unsigned long long)s.images, s.image_bytes / 1048576.0, (unsigned long long)s.uploads, (unsigned long long)s.reuploads,
          s.upload_bytes / 1048576.0, s.upload_ns / 1e6, (unsigned long long)s.shared_hits, (unsigned long long)s.cpu_written, (unsigned long long)s.evicted,
          (unsigned long long)s.samplers, (unsigned long long)a.misses, a.fast_ns ? a.fast_texels / (a.fast_ns / 1e3) : 0.0, (unsigned long long)a.high_done,
          (unsigned long long)a.hits_high, (unsigned long long)a.hits_fast);
}

uint64_t vkt_image_bytes(void) { return atomic_load(&s_bytes); }
void vkt_stats(vkt_stats_t *s) {
    s->images = atomic_load(&s_images); s->image_bytes = atomic_load(&s_bytes); s->uploads = atomic_load(&s_uploads); s->reuploads = atomic_load(&s_reuploads);
    s->upload_bytes = atomic_load(&s_up_bytes); s->upload_ns = atomic_load(&s_up_ns); s->shared_hits = atomic_load(&s_shared); s->astc_fast = atomic_load(&s_astc_fast);
    s->astc_high = atomic_load(&s_astc_high); s->cpu_written = atomic_load(&s_cpuw); s->evicted = atomic_load(&s_evicted); s->samplers = atomic_load(&s_samplers);
}

int vkt_init(void) { vkp_set_resolver(vkt_present_source); return 0; }
void vkt_shutdown(void) {
    plat_rw_wrlock(&g_lk);
    for (uint32_t i = 0; i < g_share.cap; i++) if (g_share.vals[i]) image_free(g_share.vals[i]);
    vk_hm_free(&g_share); vk_hm_free(&g_serial); vk_hm_free(&g_live); vk_hm_free(&g_cpu);
    for (uint32_t i = 0; i < g_samp.cap; i++) if (g_samp.vals[i]) vkDestroySampler(g_vk.dev, (VkSampler)g_samp.vals[i], NULL);
    vk_hm_free(&g_samp);
    free(g_reg); g_reg = NULL; g_nreg = 0; free(g_sreg); free(g_sreg_ok); g_sreg = NULL; g_sreg_ok = NULL; g_nsreg = 0;
    free(g_upg); g_upg = NULL; g_nupg = g_cupg = 0;
    plat_rw_wrunlock(&g_lk);
}
