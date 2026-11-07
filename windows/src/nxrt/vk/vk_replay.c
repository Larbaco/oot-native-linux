// The replay of NVN command streams on Vulkan (vk_replay.h; docs/m5-replay.md). Ported behaviour from nxnative's dm/gfx/nvn/nvn_mtl.m (MIT): the record
// walk (mtl_exec), the binding state and its defaults (state_op, state_reset, per-queue state), what each shader binding takes (encode_args_: uniform
// slots, the driver segments c0 / c2 and their tables, the support buffer, c1, textures through the registered handles), the viewport, scissor and
// winding rules (draw_), clears with their scissor and channel mask (OP_CLEAR_COLOR / OP_CLEAR_DS), copies (copy_b2t / copy_t2b / copy_t2t) with the
// storage mirror of storage-backed textures, counters (occlusion queries summed from reset to report). The Vulkan side is ours: dynamic rendering, one
// descriptor set per stage, extended dynamic state, the pool mirrors of vk_mem.c and the images of vk_tex.c, GENERAL as the resting layout of GPU images.
#include "vk_replay.h"
#include "vk_submit.h"
#include "vk_mem.h"
#include "vk_tex.h"
#include "vk_thandle.h"
#include "vk_fmt.h"
#include "vk_shader.h"
#include "vk_pipe.h"
#include "vk_hmap.h"
#include "vk_shot.h"
static void pk_note(uint64_t a, uint32_t n);
#include "nvn_timestamp.h"
#include "clear_vert.h"
#include "clear_f_frag.h"
#include "clear_u_frag.h"
#include "clear_i_frag.h"
#include "movie_yuv_comp.h"
#include "ndsr_blit_comp.h"
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void dm_guest_write_prepare(void *addr, size_t len);
int nvn_dpool_base(const void *pool, int tex);   // hle_nvn.c: where a descriptor pool's IDs start in the host table
void vk_note_fence_sync(sync_t *s);              // gfx_vk.c: the sync gets the submission's value at the commit
uint64_t guest_ns(void);
void vkpc_note_created(int n);

#define RED_ONCE(id, ...) do { static int once_##id; if (!once_##id++) VKRED(__VA_ARGS__); } while (0)
enum { UBANK_SIZE = 65536, SUPPORT_SIZE = 1392, ZERO_SIZE = 65536, NTEX = 256, MAXB = 192, NQ = 4096 };

uint64_t vks_scan_cur_frame(void); static vkr_stats_t g_st; static uint64_t g_scan_frame;   // g_scan_frame: the frame number (NXRT_VK_NANSCAN tags)
static uint64_t g_img_nostorage;   // images img_view found without storage use (a dispatch that binds one is skipped)
static uint64_t g_pd_frame, g_pd_last; static int g_pd;   // NXRT_VK_PASSDUMP=<present>[-<present>]: the frames after those presents, pass by pass and draw by draw, in the log
#define PD(...) do { if (g_pd) nxrt_logf("[pd] " __VA_ARGS__); } while (0)
static vkt_image *g_td[1024]; static int g_ntd, g_td_on; static uint64_t g_td_frame;   // NXRT_VK_TEXDUMP=<present>: the images that frame drew into or wrote, as PNG at the next present
static void td_note(vkt_image *im) { if (!g_td_on || !im) return; for (int i = 0; i < g_ntd; i++) if (g_td[i] == im) return; if (g_ntd < 1024) g_td[g_ntd++] = im; }

// Experiment control without restarts: NXRT_AB_FILE=<path> is re-read every 30 frames; its lines "draw=<keys>" and "compute=<keys>" (hex shader keys, commas / spaces / colons; "all" or "*" for
// compute) replace the NXRT_SKIP_DRAW / NXRT_SKIP_COMPUTE lists from then on. Touch the file while the game runs to A/B a shader (screenshots: NXRT_VK_SHOT_FILE).
static char *g_ab[2];   // 0: draw skip list, 1: compute skip list, each ",key,key," normalized; swapped atomically, old strings leak (tiny)
static char *ab_norm(const char *e) {
    if (!e || !*e) return NULL;
    size_t n = strlen(e); char *l = malloc(n + 3); size_t o = 0; l[o++] = ',';
    for (size_t i = 0; i < n; i++) { char c = e[i]; if (c == ' ' || c == ';' || c == ':' || c == '\t' || c == '\r' || c == '\n') c = ','; l[o++] = (char)(c >= 'A' && c <= 'F' ? c + 32 : c); }
    l[o++] = ','; l[o] = 0; return l;
}
static char *ab_get(int which) { static int init; if (!init) { init = 1; g_ab[0] = ab_norm(getenv("NXRT_SKIP_DRAW")); g_ab[1] = ab_norm(getenv("NXRT_SKIP_COMPUTE")); } return __atomic_load_n(&g_ab[which], __ATOMIC_ACQUIRE); }
static void ab_poll(uint64_t frame) {
    static const char *path; static int init; if (!init) { init = 1; path = getenv("NXRT_AB_FILE"); ab_get(0); } if (!path || !*path || frame % 30) return;
    FILE *f = fopen(path, "rb"); if (!f) return; char buf[4096]; size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
    char *nd = NULL, *nc = NULL; for (char *ln = strtok(buf, "\n"); ln; ln = strtok(NULL, "\n")) { if (!strncmp(ln, "draw=", 5)) nd = ab_norm(ln + 5); else if (!strncmp(ln, "compute=", 8)) nc = ab_norm(ln + 8);
        else if (!strncmp(ln, "dump=", 5)) { static long last; long v = atol(ln + 5); if (v > 0 && v != last) { last = v; vkr_passdump_next(1); vkr_texdump_next(); } } }   // "dump=<n>": a new n starts a pass dump + texture dump of the next frame (no frame number to guess)
    char *od = g_ab[0], *oc = g_ab[1];
    if (!!od != !!nd || (od && nd && strcmp(od, nd)) || !!oc != !!nc || (oc && nc && strcmp(oc, nc))) {
        __atomic_store_n(&g_ab[0], nd, __ATOMIC_RELEASE); __atomic_store_n(&g_ab[1], nc, __ATOMIC_RELEASE);
        VKLOG("NXRT_AB_FILE (frame %llu): skip draws %s, skip compute %s", (unsigned long long)frame, nd ? nd : "-", nc ? nc : "-");
    } else { free(nd); free(nc); }
}const char *nxrt_log_path(void);
static int g_origin_ll = 1, g_queue_origin_ll = 1, g_depth_zero = 0;
static VkPipeline g_last_pipe;   // the graphics pipeline bound last in this submission

// ---------- Resources that live with a submission: arena chunks, descriptor pools, buffer views (returned at completion) ----------
typedef struct rchunk { VkBuffer buf; vk_alloc mem; VkDeviceSize size, used; struct rchunk *next; } rchunk;
typedef struct rdpool { VkDescriptorPool pool; struct rdpool *next; } rdpool;
struct bl_set;
typedef struct { rchunk *chunks; rdpool *pools; VkBufferView *views; int nviews, cviews; int tq; struct bl_set *bl; } subres;   // tq: timestamp pair + 1; bl: the bindless pool set the submission uses (busy until it completes)
static subres g_sub;
// GPU time: a timestamp before and after each submission's replay, read at its completion
enum { NTQ = 256 };
static VkQueryPool g_tqp; static uint32_t g_tq_next; static _Atomic uint64_t g_gpu_ns, g_gpu_subs;
static rchunk *g_free_chunks; static rdpool *g_free_pools; static plat_rwlock_t g_free_lk = PLAT_RWLOCK_INIT;

static void bl_release(struct bl_set *b);
static void subres_done(void *arg) {
    subres *r = arg;
    uint64_t t[2];
    if (r->tq && vkGetQueryPoolResults(g_vk.dev, g_tqp, 2 * (uint32_t)(r->tq - 1), 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] >= t[0]) {
        atomic_fetch_add(&g_gpu_ns, (uint64_t)((double)(t[1] - t[0]) * g_vk.props.limits.timestampPeriod)); atomic_fetch_add(&g_gpu_subs, 1);
    }
    plat_rw_wrlock(&g_free_lk);
    while (r->chunks) { rchunk *c = r->chunks; r->chunks = c->next; c->next = g_free_chunks; g_free_chunks = c; }
    while (r->pools) { rdpool *p = r->pools; r->pools = p->next; p->next = g_free_pools; g_free_pools = p; }
    plat_rw_wrunlock(&g_free_lk);
    for (int i = 0; i < r->nviews; i++) vkDestroyBufferView(g_vk.dev, r->views[i], NULL);
    bl_release(r->bl);
    free(r->views); free(r);
}
static rchunk *chunk_take(VkDeviceSize need) {
    plat_rw_wrlock(&g_free_lk);
    rchunk **pp = &g_free_chunks; while (*pp && (*pp)->size < need) pp = &(*pp)->next;
    rchunk *c = *pp; if (c) *pp = c->next;
    plat_rw_wrunlock(&g_free_lk);
    if (c) { c->used = 0; return c; }
    c = calloc(1, sizeof *c); c->size = need > (4u << 20) ? need : (4u << 20);
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, c->size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (vkCreateBuffer(g_vk.dev, &bci, NULL, &c->buf) != VK_SUCCESS) { free(c); return NULL; }
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, c->buf, &mr);
    if (vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, &c->mem)) { vkDestroyBuffer(g_vk.dev, c->buf, NULL); free(c); return NULL; }
    vkBindBufferMemory(g_vk.dev, c->buf, c->mem.mem, c->mem.off);
    vk_name((uint64_t)c->buf, VK_OBJECT_TYPE_BUFFER, "replay arena");
    return c;
}
// Host-visible memory the GPU reads in this submission (driver segments, expanded indices, copies of unaligned ranges)
static void *arena(VkDeviceSize n, VkDeviceSize align, VkBuffer *buf, VkDeviceSize *off) {
    if (!align) align = 16;
    rchunk *c = g_sub.chunks;
    if (!c || (c->used + align - 1) / align * align + n > c->size) {
        if (!(c = chunk_take(n))) { RED_ONCE(1, "replay arena: no host memory"); return NULL; }
        c->next = g_sub.chunks; g_sub.chunks = c;
    }
    VkDeviceSize o = (c->used + align - 1) / align * align;
    c->used = o + n; *buf = c->buf; *off = o;
    return (uint8_t *)c->mem.map + o;
}
static VkDescriptorPool dpool_take(void) {
    plat_rw_wrlock(&g_free_lk);
    rdpool *p = g_free_pools; if (p) g_free_pools = p->next;
    plat_rw_wrunlock(&g_free_lk);
    if (p) { vkResetDescriptorPool(g_vk.dev, p->pool, 0); }
    else {
        p = calloc(1, sizeof *p);
        VkDescriptorPoolSize ps[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16384}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8192}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2048},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1024}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 512},
                                     {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1024}};
        VkDescriptorPoolCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 2048, sizeof ps / sizeof *ps, ps};
        if (vkCreateDescriptorPool(g_vk.dev, &ci, NULL, &p->pool) != VK_SUCCESS) { free(p); return VK_NULL_HANDLE; }
    }
    p->next = g_sub.pools; g_sub.pools = p;
    return p->pool;
}
static VkDescriptorSet dset_alloc(VkDescriptorSetLayout l) {
    VkDescriptorSet s = VK_NULL_HANDLE;
    for (int attempt = 0; attempt < 2; attempt++) {
        VkDescriptorPool p = g_sub.pools && !attempt ? g_sub.pools->pool : dpool_take();
        if (!p) break;
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, p, 1, &l};
        if (vkAllocateDescriptorSets(g_vk.dev, &ai, &s) == VK_SUCCESS) { g_st.desc_sets++; return s; }
    }
    RED_ONCE(2, "descriptor sets exhausted"); return VK_NULL_HANDLE;
}
static void view_keep(VkBufferView v) {
    if (g_sub.nviews == g_sub.cviews) { g_sub.cviews = g_sub.cviews ? 2 * g_sub.cviews : 32; g_sub.views = realloc(g_sub.views, (size_t)g_sub.cviews * sizeof *g_sub.views); }
    g_sub.views[g_sub.nviews++] = v;
}

// ---------- Fixed resources ----------
static struct { VkBuffer zero, sink, support, quads, bounce; vk_alloc zero_m, sink_m, support_m, quads_m, bounce_m; VkSampler samp; VkQueryPool qp; } R0;
// A storage buffer at an offset Vulkan cannot bind (minStorageBufferOffsetAlignment; Metal binds at any 4-byte offset): for a dispatch its bytes are copied into
// an aligned place of R0.bounce, bound there and, when the shader writes it, copied back after the dispatch (M6: a 4-byte counter of a gameplay compute shader)
enum { BOUNCE_SIZE = 1 << 20, BOUNCE_MAX = 64 << 10 };
static struct { VkBuffer pool; VkDeviceSize poff, boff, n; } g_bnc[16]; static int g_nbnc; static VkDeviceSize g_bnc_next;
static int mkbuf(VkDeviceSize n, VkBufferUsageFlags u, VkMemoryPropertyFlags want, VkBuffer *b, vk_alloc *m, const char *name) {
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, n, u, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (vkCreateBuffer(g_vk.dev, &bci, NULL, b) != VK_SUCCESS) return -1;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, *b, &mr);
    if (vk_alloc_mem(&mr, want, 0, m)) return -1;
    vkBindBufferMemory(g_vk.dev, *b, m->mem, m->off);
    vk_name((uint64_t)*b, VK_OBJECT_TYPE_BUFFER, name);
    return 0;
}

// ---------- Binding state (nvn_mtl.m's S and R: per queue, kept across submissions) ----------
typedef struct rs {
    uint64_t serial;
    const prog_t *bst[5]; const prog_t *cprog;
    blend_st blend[8]; color_st color; chmask_st chmask; ms_st ms; poly_st poly; ds_st ds;
    va_st attr[16]; int nattr; vs_st stream[16]; int nstream; NVNbufferRange vbuf[16];
    p_bind_buf ubo[6][16], ssbo[6][16];
    uint64_t tex[6][NTEX], samp[6][128], img[6][32];
    uint32_t st_vmask[2], st_wmask[2], st_ref[2];
    p_poly_offset po; float vp[16][4]; float dr[2]; int has_vp, depth_clamp; uint8_t vp_sw[16][4];
    float blend_color[4], alpha_ref; int64_t copy_stride, copy_img_stride;
    int tf_on, rast_discard, tex_base, samp_base, render_skip, vis_on;
    p_prim_restart restart;
    // render targets (the snapshots copied: the record's command memory may be reused after the submission)
    int nrt; tex_t ct[8]; uint8_t has_ct[8]; view_t cview[8]; uint8_t has_cview[8]; tex_t dt; uint8_t has_dt; view_t dview; uint8_t has_dview;
    p_scissor sc; int has_sc;
    struct rs *next;
} rs;
static rs *g_states, *S;
static const p_bind_buf g_nobind;
// the binding behind a uniform or storage descriptor of shader stage st (a storage view of a uniform-table descriptor reads the uniform bindings); an empty one for a slot the replay does not keep
static const p_bind_buf *bind_of(const vks_bind *b, int st) { int r = b->reg; if (r < 0 || r >= 16) return &g_nobind; return b->kind == VKB_CBUF || b->from_ubo ? &S->ubo[st][r] : &S->ssbo[st][r]; }
static void rs_reset(rs *s) {
    uint64_t serial = s->serial; struct rs *next = s->next;
    memset(s, 0, sizeof *s); s->serial = serial; s->next = next;
    for (int i = 0; i < 8; i++) s->blend[i] = (blend_st){(uint64_t)i, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD};
    s->color = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; s->chmask.mask = 0xffffffffu;
    s->poly = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
    s->ds = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, 1, 1, 1, 1, 1, 1};
    s->st_vmask[0] = s->st_vmask[1] = s->st_wmask[0] = s->st_wmask[1] = 0xff; s->dr[1] = 1;
}
static rs *rs_get(uint64_t serial) {
    for (rs *s = g_states; s; s = s->next) if (s->serial == serial) return s;
    rs *s = calloc(1, sizeof *s); s->serial = serial; s->next = g_states; g_states = s; rs_reset(s);
    return s;
}

// ---------- Programs: the shader of each NVN stage, cached by program serial ----------
typedef struct { const vks_shader *sh[6]; } progc;
static vk_hmap g_progc = {8};
static const vks_shader *prog_stage(const prog_t *p, int stage) {
    if (!p || !p->serial) return NULL;
    progc *c = vk_hm_get(&g_progc, &p->serial);
    if (c && c->sh[stage]) return c->sh[stage];
    if (!c) { c = calloc(1, sizeof *c); vk_hm_put(&g_progc, &p->serial, c); }
    for (unsigned i = 0; i < p->nstage && i < 6; i++) { const vks_shader *s = vksh_get(p, i); if (s && s->stage >= 0 && s->stage < 6) c->sh[s->stage] = s; }
    return c->sh[stage];
}

// ---------- Barriers ----------
static void barrier_all(VkCommandBuffer cb) {
    vk_mem_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    g_st.barriers++;
}

// ---------- GPU writes into pools: copied back to guest memory at completion (vkm_gpu_wrote; transfers cannot be recorded inside a pass) ----------
static struct { uint64_t a, n; } g_wb[256]; static int g_nwb;
// NXRT_VK_WRITEBACK: 0 no compute write-back at all (the CPU reads the values the game wrote itself: for A/B), 1 storage buffers a compute shader writes (the default), 2 also the memory that
// shaders with untracked global stores (gmem_bda) write through: the first NXRT_VK_WB_GMEM bytes (default 64 KiB) of each pointer's mapping. NXRT_VK_WB_MAX caps one storage buffer's write-back (0 = none)
static const vks_shader *g_cur_sh;
static int wb_mode(void) { static int m = -1; if (m < 0) { const char *e = getenv("NXRT_VK_WRITEBACK"); m = e ? atoi(e) : 1; } return m; }
static uint64_t wb_max(void) { static uint64_t v = ~0ull; if (v == ~0ull) { const char *e = getenv("NXRT_VK_WB_MAX"); v = e ? strtoull(e, NULL, 0) : 0; } return v; }
static void wb_add(uint64_t a, uint64_t n) {
    if (wb_mode() < 1 || !n) return;
    if (vkm_rdwatch_gpu()) vkm_rdwatch_add(a, n, g_cur_sh ? g_cur_sh->key : "?");
    if (wb_max() && n > wb_max()) n = wb_max() & ~3ull;
    for (int k = 0; k < g_nwb; k++) if (g_wb[k].a == a && g_wb[k].n == n) return;
    if (g_nwb < 256) { g_wb[g_nwb].a = a; g_wb[g_nwb].n = n; g_nwb++; }
}
static void wb_flush(void) { for (int i = 0; i < g_nwb; i++) { vkm_gpu_wrote(g_wb[i].a, g_wb[i].n); g_st.writebacks++; } g_nwb = 0; }

// ---------- Occlusion queries: samples counted from a reset to each report, summed on the CPU when the GPU is done (in submission order) ----------
static _Atomic uint8_t g_qbusy[NQ]; static uint32_t g_qnext; static uint64_t g_vis_acc;
typedef struct { int kind; uint32_t q; uint64_t addr; } qpost;   // kind 0 add the result of q, 1 reset, 2 report into addr
static void q_post_run(void *arg) {
    qpost *p = arg;
    if (p->kind == 0) {
        uint64_t v = 0;
        if (vkGetQueryPoolResults(g_vk.dev, R0.qp, p->q, 1, sizeof v, &v, sizeof v, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) g_vis_acc += v;
        atomic_store(&g_qbusy[p->q], 0);
    } else if (p->kind == 1) g_vis_acc = 0;
    else {
        uint64_t rec[2] = {g_vis_acc, nvn_timestamp_from_ns(guest_ns())};
        dm_guest_write_prepare((void *)(uintptr_t)p->addr, sizeof rec);
        memcpy((void *)(uintptr_t)p->addr, rec, sizeof rec);
    }
    free(p);
}
static void q_post(int kind, uint32_t q, uint64_t addr) { qpost *p = malloc(sizeof *p); p->kind = kind; p->q = q; p->addr = addr; vks_post_fn(q_post_run, p); }
static int q_alloc(void) {
    for (int round = 0; round < 2; round++) {
        for (uint32_t k = 0; k < NQ; k++) { uint32_t q = (g_qnext + k) % NQ; if (!atomic_load(&g_qbusy[q])) { g_qnext = q + 1; atomic_store(&g_qbusy[q], 1); vkResetQueryPool(g_vk.dev, R0.qp, q, 1); return (int)q; } }
        RED_ONCE(3, "all %d occlusion queries in flight", NQ); return -1;
    }
    return -1;
}

// ---------- Render passes (dynamic rendering) ----------
typedef struct {
    int valid, open, ncolor, any;
    VkImageView cv[8]; vkt_image *ci[8]; VkFormat cf[8]; uint32_t cw[8], ch[8], cnl[8], clv[8], cl0[8];   // clv / cl0: the level and first layer (partial clears)
    VkImageView dv; vkt_image *di; VkFormat df; int hd, hs; uint32_t dw, dh, dnl, dlv, dl0;
    uint32_t w, h; uint8_t samples;
    int query;
} pass_t;
static pass_t P = {.query = -1};
static uint32_t mipd(uint32_t s, uint32_t l) { s >>= l; return s ? s : 1; }

static void pass_end(void) {
    if (!P.open) return;
    VkCommandBuffer cb = vks_cb();
    if (P.query >= 0) { vkCmdEndQuery(cb, R0.qp, (uint32_t)P.query); q_post(0, (uint32_t)P.query, 0); P.query = -1; }
    vkCmdEndRendering(cb);
    P.open = 0;
    wb_flush();
}
// An attachment's view: one level, the layers the NVN view names (else all from its first), the view's format through the image's fallback, identity swizzle
static VkImageView att_view(vkt_image *im, const tex_t *t, const view_t *v, int has_v, int depth, uint32_t *w, uint32_t *h, uint32_t *nl, VkFormat *fmt, uint32_t *lv, uint32_t *l0) {
    uint32_t level = has_v && v->set_levels ? v->base_level : 0, b0 = has_v && v->set_layers ? v->min_layer : 0;
    if (level >= im->levels) level = im->levels - 1;
    int is3d = im->vtype == VK_IMAGE_VIEW_TYPE_3D;
    uint32_t all = is3d ? mipd(im->depth, level) : im->layers, n = has_v && v->set_layers && v->nlayers ? v->nlayers : all > b0 ? all - b0 : 1;
    if (b0 >= all) b0 = all - 1; if (b0 + n > all) n = all - b0;
    VkFormat f = depth ? im->fi->vk : vkf_view_format(t->d.format, has_v && v->set_format ? v->format : t->d.format);
    *w = mipd(im->w, level); *h = mipd(im->h, level); *nl = n; *fmt = f; *lv = level; *l0 = b0;
    VkImageViewType vt = n > 1 || is3d || im->layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    return vkt_view_raw_(im, f, vt, level, 1, b0, n, depth ? im->aspect : VK_IMAGE_ASPECT_COLOR_BIT, NULL);
}
static void pass_targets(void) {
    memset(P.cv, 0, sizeof P.cv); memset(P.ci, 0, sizeof P.ci); memset(P.cf, 0, sizeof P.cf);
    P.dv = VK_NULL_HANDLE; P.di = NULL; P.df = VK_FORMAT_UNDEFINED; P.hd = P.hs = 0; P.ncolor = 0; P.any = 0;
    P.w = P.h = 16384; P.samples = 1;
    for (int i = 0; i < S->nrt && i < 8; i++) {
        if (!S->has_ct[i]) continue;
        const tex_t *t = &S->ct[i];
        int dup = 0; for (int j = 0; j < i; j++) dup |= S->has_ct[j] && S->ct[j].serial == t->serial &&
            (S->has_cview[j] && S->cview[j].set_levels ? S->cview[j].base_level : 0) == (S->has_cview[i] && S->cview[i].set_levels ? S->cview[i].base_level : 0) &&
            (S->has_cview[j] && S->cview[j].set_layers ? S->cview[j].min_layer : 0) == (S->has_cview[i] && S->cview[i].set_layers ? S->cview[i].min_layer : 0);
        if (dup) { RED_ONCE(10, "the same texture level is attached at two colour slots: only the first is attached (nvn_mtl.m rt_dup_slot)"); continue; }
        vkt_image *im = vkt_resolve(t);
        if (!im) continue;
        if (!(im->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
            static int n; if (n++ < 8) VKRED("render target ser=%llu %ux%u format %#x (%s) cannot be a colour attachment here (%s): draws to it are lost",
                                             (unsigned long long)t->serial, t->d.width, t->d.height, t->d.format, im->fi->name, im->cls == VKT_STORAGE ? "its content comes from its storage" : "the format cannot");
            continue;
        }
        P.cv[i] = att_view(im, t, &S->cview[i], S->has_cview[i], 0, &P.cw[i], &P.ch[i], &P.cnl[i], &P.cf[i], &P.clv[i], &P.cl0[i]);
        if (!P.cv[i]) continue;
        P.ci[i] = im; P.ncolor = i + 1; P.any = 1; td_note(im);
        if (P.cw[i] < P.w) P.w = P.cw[i]; if (P.ch[i] < P.h) P.h = P.ch[i];
        P.samples = (uint8_t)im->samples;
    }
    if (S->has_dt) {
        const tex_t *t = &S->dt; vkt_image *im = vkt_resolve(t);
        if (im && (im->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
            P.dv = att_view(im, t, &S->dview, S->has_dview, 1, &P.dw, &P.dh, &P.dnl, &P.df, &P.dlv, &P.dl0);
            if (P.dv) {
                P.di = im; P.hd = (im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0; P.hs = (im->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0; P.any = 1;
                if (P.dw < P.w) P.w = P.dw; if (P.dh < P.h) P.h = P.dh;
                P.samples = (uint8_t)im->samples;
            }
        } else if (im) RED_ONCE(11, "depth target format %#x cannot be a depth attachment: not attached", t->d.format);
    }
    if (!P.any) { P.w = P.h = 0; }
    P.valid = 1;
}
// Opens the pass over the current targets (loading what they hold). An attachment-less pass covers the viewport (nvn_mtl.m bare_dims)
static int pass_begin(void) {
    if (P.open) return 1;
    if (!P.valid) pass_targets();
    uint32_t w = P.w, h = P.h;
    if (!P.any) {
        if (S->nrt || S->has_dt) return 0;   // targets set but none could be attached
        w = S->has_vp ? (uint32_t)(S->vp[0][0] + S->vp[0][2]) : 0; h = S->has_vp ? (uint32_t)(S->vp[0][1] + S->vp[0][3]) : 0;
        if (!w || !h) return 0;
        RED_ONCE(12, "attachment-less pass at the viewport's size %ux%u (nvn_mtl.m bare_dims)", w, h);
    }
    VkCommandBuffer cb = vks_cb();
    barrier_all(cb);
    VkRenderingAttachmentInfo ca[8], da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, sa;
    for (int i = 0; i < P.ncolor; i++)
        ca[i] = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, P.cv[i], VK_IMAGE_LAYOUT_GENERAL, 0, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED,
                                            VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, {{{0}}}};
    if (P.dv) da = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, P.dv, VK_IMAGE_LAYOUT_GENERAL, 0, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED,
                                               VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, {{{0}}}};
    sa = da;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {w, h}}, 1, 0, (uint32_t)P.ncolor, ca, P.hd ? &da : NULL, P.hs ? &sa : NULL};
    vkCmdBeginRendering(cb, &ri);
    P.open = 1; g_st.passes++;
    if (g_pd) {
        char b[600]; int o = 0;
        for (int i = 0; i < P.ncolor; i++) if (P.cv[i]) o += snprintf(b + o, sizeof b - o, " c%d=ser%llu/%s/%ux%u", i, (unsigned long long)P.ci[i]->serial, P.ci[i]->fi->name, P.cw[i], P.ch[i]);
        if (P.dv) o += snprintf(b + o, sizeof b - o, " d=ser%llu/%s/%ux%u", (unsigned long long)P.di->serial, P.di->fi->name, P.dw, P.dh);
        PD("pass %llu %ux%u%s", (unsigned long long)g_st.passes, w, h, b);
        for (int i = 0; i < S->nrt; i++) if (S->has_ct[i]) PD("  c%d storage pool %p +%#llx mem %p flags %#x size %#llx image %p", i, (const void *)S->ct[i].pool, (unsigned long long)S->ct[i].off, (const void *)S->ct[i].mem, S->ct[i].d.flags, (unsigned long long)S->ct[i].L.size, (void *)P.ci[i]);
    }
    return 1;
}

// ---------- Clears (nvn_mtl.m OP_CLEAR_COLOR / OP_CLEAR_DS): NVN clears the whole target level (every layer of its view) within the scissor ----------
static int fmt_int_kind(VkFormat f) {   // 0 float / normalized, 1 unsigned integer, 2 signed integer
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_S8_UINT: return 1;
    case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32A32_SINT: case VK_FORMAT_A2B10G10R10_SINT_PACK32: return 2;
    default: return 0;
    }
}
static unsigned fmt_channels(VkFormat f) {   // the colour channels a format has, as an NVN channel mask (nvn_mtl.m mtl_pf_chmask)
    switch (f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SNORM: case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R8_SRGB: return 0x1;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_SNORM:
    case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: return 0x3;
    case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_B5G6R5_UNORM_PACK16: case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: return 0x7;
    default: return 0xf;
    }
}
static VkClearValue clear_color_value(VkFormat f, const float *rgba, const uint32_t *u, int is_uint) {
    VkClearValue v; int k = fmt_int_kind(f);
    for (int i = 0; i < 4; i++) {
        if (is_uint) { if (k == 0) v.color.float32[i] = (float)u[i]; else v.color.uint32[i] = u[i]; }
        else if (k == 1) v.color.uint32[i] = rgba[i] <= 0 ? 0 : (uint32_t)rgba[i];
        else if (k == 2) v.color.int32[i] = (int32_t)rgba[i];
        else v.color.float32[i] = rgba[i];
    }
    return v;
}
// One attachment cleared within rect: a pass of its own whose load operation clears (the view's format converts the value, sRGB included)
static void clear_pass(VkImageView v, VkFormat f, int ds, int cd, int cs, VkRect2D rect, uint32_t layers, VkClearValue val) {
    pass_end();
    VkCommandBuffer cb = vks_cb();
    barrier_all(cb);
    VkRenderingAttachmentInfo a = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, v, VK_IMAGE_LAYOUT_GENERAL, 0, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, val};
    VkRenderingAttachmentInfo d = a, s = a;
    d.loadOp = cd ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD; s.loadOp = cs ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    VkImageAspectFlags asp = vkf_aspect(f);
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, rect, layers ? layers : 1, 0, ds ? 0u : 1u, ds ? NULL : &a,
                          ds && (asp & VK_IMAGE_ASPECT_DEPTH_BIT) ? &d : NULL, ds && (asp & VK_IMAGE_ASPECT_STENCIL_BIT) ? &s : NULL};
    vkCmdBeginRendering(cb, &ri); vkCmdEndRendering(cb);
    g_st.clears++;
}
static int scissor_rect(uint32_t w, uint32_t h, VkRect2D *r) {   // the scissor within a w × h target; 0 when empty
    int64_t x0 = 0, y0 = 0, x1 = w, y1 = h;
    if (S->has_sc) { x0 = S->sc.x > 0 ? S->sc.x : 0; y0 = S->sc.y > 0 ? S->sc.y : 0; x1 = (int64_t)S->sc.x + S->sc.w; y1 = (int64_t)S->sc.y + S->sc.h; if (x1 > w) x1 = w; if (y1 > h) y1 = h; }
    if (x1 <= x0 || y1 <= y0) return 0;
    *r = (VkRect2D){{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
    return 1;
}
// Partial clears (M6 phase B; nvn_mtl.m clear_color_tri / clear_ds_tri): a colour clear that keeps some channels, or a stencil clear that keeps some bits, is a triangle
// over the target level within the scissor: the clear value (converted for integer targets) through the channel mask, or the masked stencil bits replaced, and depth
// written when it is cleared too (the viewport's depth range is the value). One pass per layer of the attachment
typedef struct { uint32_t fmt; uint8_t ds, kind, samples, cmask, dw, sm, pad[2]; } ckey;
static vk_hmap g_cpipe = {sizeof(ckey)};
static VkPipelineLayout g_cpl; static VkShaderModule g_cmod[4];   // vertex; fragment float / uint / int
static VkPipeline clear_pipe(const ckey *k) {
    VkPipeline p = (VkPipeline)vk_hm_get(&g_cpipe, k);
    if (p) return p;
    if (!g_cpl) {
        VkPushConstantRange pr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
        VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 1, &pr};
        if (vkCreatePipelineLayout(g_vk.dev, &lci, NULL, &g_cpl) != VK_SUCCESS) return VK_NULL_HANDLE;
        const uint32_t *src[4] = {clear_vert_spv, clear_f_frag_spv, clear_u_frag_spv, clear_i_frag_spv};
        size_t sz[4] = {sizeof clear_vert_spv, sizeof clear_f_frag_spv, sizeof clear_u_frag_spv, sizeof clear_i_frag_spv};
        for (int i = 0; i < 4; i++) {
            VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sz[i], src[i]};
            if (vkCreateShaderModule(g_vk.dev, &mci, NULL, &g_cmod[i]) != VK_SUCCESS) return VK_NULL_HANDLE;
        }
    }
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, g_cmod[0], "main", NULL},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, g_cmod[1 + (k->kind < 3 ? k->kind : 0)], "main", NULL}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_FALSE, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE,
                                                 VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_FALSE, 0, 0, 0, 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, (VkSampleCountFlagBits)(k->samples ? k->samples : 1)};
    VkStencilOpState so = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_COMPARE_OP_ALWAYS, 0xff, k->sm, 0};
    VkPipelineDepthStencilStateCreateInfo dss = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, NULL, 0, k->dw, k->dw, VK_COMPARE_OP_ALWAYS, VK_FALSE, k->sm != 0, so, so, 0, 1};
    VkPipelineColorBlendAttachmentState ba = {VK_FALSE, 0, 0, 0, 0, 0, 0, k->cmask};
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, 0, k->ds ? 0u : 1u, &ba};
    VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 3, dyn};
    VkFormat cf = (VkFormat)k->fmt; VkImageAspectFlags asp = k->ds ? vkf_aspect(cf) : 0;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, k->ds ? 0u : 1u, k->ds ? NULL : &cf,
                                        asp & VK_IMAGE_ASPECT_DEPTH_BIT ? cf : VK_FORMAT_UNDEFINED, asp & VK_IMAGE_ASPECT_STENCIL_BIT ? cf : VK_FORMAT_UNDEFINED};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, 0, k->ds ? 1u : 2u, st, &vi, &ia, NULL, &vp, &rs, &ms, &dss, &cbs, &dy, g_cpl,
                                       VK_NULL_HANDLE, 0, VK_NULL_HANDLE, -1};
    if (vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &ci, NULL, &p) != VK_SUCCESS) { RED_ONCE(23, "the clear-draw pipeline for format %u could not be made: masked clears are lost", k->fmt); return VK_NULL_HANDLE; }
    vkpc_note_created(1); vk_hm_put(&g_cpipe, k, (void *)p);
    return p;
}
static void clear_draw(vkt_image *im, VkFormat f, uint32_t level, uint32_t l0, uint32_t nl, int ds, int kind, unsigned cmask, int dw, unsigned sm, VkRect2D rect,
                       const uint32_t v[4], float depth, uint32_t stencil) {
    ckey k; memset(&k, 0, sizeof k); k.fmt = (uint32_t)f; k.ds = (uint8_t)ds; k.kind = (uint8_t)kind; k.samples = (uint8_t)im->samples; k.cmask = (uint8_t)cmask; k.dw = (uint8_t)dw; k.sm = (uint8_t)sm;
    VkPipeline p = clear_pipe(&k); if (!p) return;
    pass_end();
    VkCommandBuffer cb = vks_cb();
    barrier_all(cb);
    uint32_t w = mipd(im->w, level), h = mipd(im->h, level);
    VkImageAspectFlags asp = ds ? vkf_aspect(f) : VK_IMAGE_ASPECT_COLOR_BIT;
    for (uint32_t layer = 0; layer < (nl ? nl : 1); layer++) {
        VkImageView view = vkt_view_raw_(im, f, VK_IMAGE_VIEW_TYPE_2D, level, 1, l0 + layer, 1, ds ? im->aspect : VK_IMAGE_ASPECT_COLOR_BIT, NULL);
        if (!view) continue;
        VkRenderingAttachmentInfo a = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, view, VK_IMAGE_LAYOUT_GENERAL, 0, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED,
                                       VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, {{{0}}}};
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, rect, 1, 0, ds ? 0u : 1u, ds ? NULL : &a, ds && (asp & VK_IMAGE_ASPECT_DEPTH_BIT) ? &a : NULL,
                              ds && (asp & VK_IMAGE_ASPECT_STENCIL_BIT) ? &a : NULL};
        vkCmdBeginRendering(cb, &ri);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p); g_last_pipe = VK_NULL_HANDLE;
        VkViewport vp = {0, 0, (float)w, (float)h, ds ? depth : 0, ds ? depth : 1};
        vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &rect);
        vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, stencil & 0xff);
        uint32_t pc[4] = {0}; if (v) memcpy(pc, v, 16);
        vkCmdPushConstants(cb, g_cpl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, pc);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRendering(cb);
    }
    g_st.clears++;
}
static void clear_color(const p_clear_color *c) {
    if (S->render_skip) return;
    if (!P.valid) pass_targets();
    int i = c->index;
    if (i < 0 || i >= 8 || !P.cv[i]) { if (i < 0 || i >= S->nrt || !S->has_ct[i]) RED_ONCE(20, "colour clear of target %d, which is not set", i); return; }
    unsigned ch = fmt_channels(P.cf[i]);
    if ((c->mask & ch) == 0) return;
    VkRect2D r; if (!scissor_rect(P.cw[i], P.ch[i], &r)) return;
    if ((c->mask & ch) != ch) {   // some channels kept: a clear draw through the channel mask (nvn_mtl.m clear_color_tri)
        int kind = fmt_int_kind(P.cf[i]); uint32_t v[4];
        for (int k = 0; k < 4; k++) { float f = c->rgba[k]; int32_t si;   // integer targets take the value converted, as a clear does
            if (kind == 1) v[k] = f > 0 ? (f < 4294967040.f ? (uint32_t)f : 0xffffffffu) : 0;
            else if (kind == 2) { si = !(f == f) ? 0 : f >= 2147483520.f ? INT32_MAX : f <= -2147483648.f ? INT32_MIN : (int32_t)f; memcpy(&v[k], &si, 4); }
            else memcpy(&v[k], &f, 4); }
        { static int n; if (!n++) VKLOG("colour clear with channel mask %#x: drawn through the mask (the first of its kind)", c->mask); }
        clear_draw(P.ci[i], P.cf[i], P.clv[i], P.cl0[i], P.cnl[i], 0, kind, c->mask & ch, 0, 0, r, v, 0, 0);
    } else clear_pass(P.cv[i], P.cf[i], 0, 0, 0, r, P.cnl[i], clear_color_value(P.cf[i], c->rgba, NULL, 0));
    PD("clear c%d ser%llu to %g,%g,%g,%g mask %#x rect %d,%d %ux%u", i, (unsigned long long)P.ci[i]->serial, c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3], c->mask, r.offset.x, r.offset.y, r.extent.width, r.extent.height);
    if (g_pd) { static int dw = -1; if (dw < 0) { const char *e = getenv("NXRT_VK_DRAWSTATS"); dw = e ? atoi(e) : 0; }
        if (dw > 0 && (int)P.ci[i]->w == dw && P.ci[i]->fi->vk == VK_FORMAT_B10G11R11_UFLOAT_PACK32) { char tag[96]; snprintf(tag, sizeof tag, "after clear c%d ser%llu", i, (unsigned long long)P.ci[i]->serial); vks_stat_record(vks_cb(), P.ci[i], tag); } }
}
// NXRT_VK_KEEP_COPIED_DEPTH=1 (diagnostic): a depth clear of a texture that a texture copy filled earlier in the same frame is skipped (the depth copy the OoT materials test EQUAL against)
static struct { uint64_t serial, frame; } g_cpd[16]; static int g_cpd_i;
static void note_depth_copy(uint64_t serial) { g_cpd[g_cpd_i].serial = serial; g_cpd[g_cpd_i].frame = g_scan_frame; g_cpd_i = (g_cpd_i + 1) % 16; }
static int depth_copied_this_frame(uint64_t serial) { for (int i = 0; i < 16; i++) if (g_cpd[i].serial == serial && g_cpd[i].frame == g_scan_frame) return 1; return 0; }
static void clear_ds(const p_clear_ds *c) {
    if (S->render_skip) return;
    if (!P.valid) pass_targets();
    if (!P.dv) return;
    { static int keep = -1; if (keep < 0) { const char *e = getenv("NXRT_VK_KEEP_COPIED_DEPTH"); keep = e && *e == '1'; }
      if (keep && P.di && depth_copied_this_frame(P.di->serial)) { PD("clear depth of ser%llu skipped (filled by a copy this frame)", (unsigned long long)P.di->serial); return; } }
    int cd = c->depth_mask && P.hd; unsigned sm = (unsigned)c->stencil_mask & 0xff; int cs = sm && P.hs;
    if (!cd && !cs) return;
    VkRect2D r; if (!scissor_rect(P.dw, P.dh, &r)) return;
    VkClearValue v; v.depthStencil.depth = c->depth < 0 ? 0 : c->depth > 1 ? 1 : c->depth; v.depthStencil.stencil = (uint32_t)c->stencil & 0xff;
    if (cs && sm != 0xff) {   // some stencil bits kept: a clear draw replacing the masked bits, and depth when it is cleared too (nvn_mtl.m clear_ds_tri)
        { static int n; if (!n++) VKLOG("stencil clear with mask %#x: drawn through the mask (the first of its kind)", sm); }
        clear_draw(P.di, P.df, P.dlv, P.dl0, P.dnl, 1, 0, 0, cd, sm, r, NULL, v.depthStencil.depth, v.depthStencil.stencil);
    } else clear_pass(P.dv, P.df, 1, cd, cs, r, P.dnl, v);
    PD("clear depth %d (%g) stencil %d (%u) from %#llx d=ser%llu %ux%u layers %u+%u/%u level %u rect %d,%d %ux%u", cd, c->depth, cs, (unsigned)c->stencil, (unsigned long long)c->caller, P.di ? (unsigned long long)P.di->serial : 0ull, P.dw, P.dh, P.dl0, P.dnl, P.di ? P.di->layers : 0, P.dlv, r.offset.x, r.offset.y, r.extent.width, r.extent.height);
}

// ---------- Descriptors (nvn_mtl.m encode_args_) ----------
static inline uint64_t tex_slot_h(int st, unsigned reg) {   // nvn_mtl.m tex_slot_h: combined slots past 31 share their c2 word with an image slot
    return reg >= 32 && reg - 32 < DMSH_C2_IMG_SLOTS && S->img[st][reg - 32] ? S->img[st][reg - 32] : reg < NTEX ? S->tex[st][reg] : 0;
}
static VkSampler samp_get(int sid) {
    samp_par p;
    if (sid <= 0 || !vkt_samp_copy(sid, &p)) return R0.samp;
    VkSampler s = vkt_sampler(&p);
    return s ? s : R0.samp;
}
static int nvn_target_of(VkImageViewType vt, int ms) {
    switch (vt) {
    case VK_IMAGE_VIEW_TYPE_1D: return NVN_TEXTURE_TARGET_1D; case VK_IMAGE_VIEW_TYPE_1D_ARRAY: return NVN_TEXTURE_TARGET_1D_ARRAY;
    case VK_IMAGE_VIEW_TYPE_3D: return NVN_TEXTURE_TARGET_3D; case VK_IMAGE_VIEW_TYPE_CUBE: return NVN_TEXTURE_TARGET_CUBEMAP; case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: return NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
    case VK_IMAGE_VIEW_TYPE_2D_ARRAY: return ms ? NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY : NVN_TEXTURE_TARGET_2D_ARRAY;
    default: return ms ? NVN_TEXTURE_TARGET_2D_MULTISAMPLE : NVN_TEXTURE_TARGET_2D;
    }
}
static VkImageViewType view_type_of_target(int target, VkImageViewType dflt) {
    switch (target) {
    case NVN_TEXTURE_TARGET_1D: return VK_IMAGE_VIEW_TYPE_1D; case NVN_TEXTURE_TARGET_1D_ARRAY: return VK_IMAGE_VIEW_TYPE_1D_ARRAY; case NVN_TEXTURE_TARGET_3D: return VK_IMAGE_VIEW_TYPE_3D;
    case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case NVN_TEXTURE_TARGET_CUBEMAP: return VK_IMAGE_VIEW_TYPE_CUBE; case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    case NVN_TEXTURE_TARGET_2D: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE: case NVN_TEXTURE_TARGET_RECTANGLE: return VK_IMAGE_VIEW_TYPE_2D;
    }
    return dflt;
}
// The sampled view of texture ID tid as binding b declares it (a 2D texture read as a 2D array and so on: the view's type is the shader's, nvn_mtl.m tex_fit)
// A format of the shader's numeric class (VKS_CT_*) with the same bits: 32-bit components between float, uint and int, 8- and 16-bit integers between uint and
// int (Maxwell hands the shader the register bits; Vulkan wants the view's class to match the declared one). -1 when it already matches or no format keeps the bits
int vkr_fmt_as_ct(int f, int ct) {
    static const int base[4] = {NVN_FORMAT_R8UI, NVN_FORMAT_RG8UI, NVN_FORMAT_RGB8UI, NVN_FORMAT_RGBA8UI};   // 8UI 8I 16F 16 16SN 16UI 16I 32F 32UI 32I
    for (int g = 0; g < 4; g++) {
        int i = f - base[g]; if (i < 0 || i > 9) continue;
        int cls = i == 0 || i == 5 || i == 8 ? VKS_CT_UINT : i == 1 || i == 6 || i == 9 ? VKS_CT_INT : VKS_CT_FLOAT;
        if (cls == ct) return -1;
        if (i >= 7) return base[g] + (ct == VKS_CT_FLOAT ? 7 : ct == VKS_CT_UINT ? 8 : 9);
        if (ct != VKS_CT_FLOAT && cls != VKS_CT_FLOAT) return base[g] + (i <= 1 ? 0 : 5) + (ct == VKS_CT_INT);
        return -1;
    }
    return -1;
}
static const vks_shader *g_cur_sh;   // the shader whose descriptor set write_set is filling (for the messages below)
// Depth comparison on a colour texture: the game binds its default 1x1 / 8x8 black and white textures to the shadow samplers of lights without shadows. Vulkan
// leaves that undefined (VUID-vkCmdDrawIndexed-None-06479, in gameplay); the GPU the game was made for compares with the texel's red channel. A 1x1 depth image
// holding the red value of the texture's first texel stands in, one per value (M6), viewed as the shader declares it (M6 phase B: a 2D view under an arrayed or cube
// declaration was VUID-vkCmdDrawIndexed-viewType-07752): six cube-compatible layers, views 2D, 2D array, cube, cube array
typedef struct { float v; VkImage img; vk_alloc mem; VkImageView view[4]; } dcmp_twin;
static dcmp_twin g_dct[8]; static int g_ndct;
static vk_hmap g_dct_of = {8};   // texture serial → its value + 1 (0: not read yet)
static int read_u32(uint64_t a, uint32_t *v);
static VkImageView dcmp_twin_view(float v, int vi);
static VkImageView dcmp_view(vkt_image *im, VkImageViewType vt) {
    int vi = vt == VK_IMAGE_VIEW_TYPE_2D ? 0 : vt == VK_IMAGE_VIEW_TYPE_2D_ARRAY ? 1 : vt == VK_IMAGE_VIEW_TYPE_CUBE ? 2 : vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY && g_vk.f10.imageCubeArray ? 3 : -1;
    if (vi < 0) return VK_NULL_HANDLE;
    uint64_t k = im->serial; uintptr_t got = (uintptr_t)vk_hm_get(&g_dct_of, &k);
    float v = 1.0f;
    if (got) memcpy(&v, &(uint32_t){(uint32_t)(got - 1)}, 4);
    else {
        // the first texel: from storage for a texture read from guest memory (its upload may still be in this submission's command buffers: a readback now
        // would see the image before it), else read back; texel (0, 0) is the first bytes in the block-linear layout too
        uint8_t px[16] = {0}; NVNcopyRegion rr = {0, 0, 0, 1, 1, 1}; uint8_t rgba[4] = {255, 0, 0, 0}; int got = -1;
        if (!(im->fi->flags & VKF_COMPRESSED) && im->fi->vbpb <= 16) {
            if (im->cls == VKT_STORAGE && im->mem) { uint32_t w[4]; got = 0; for (unsigned q = 0; q < (im->fi->vbpb + 3) / 4 && !got; q++) got = !read_u32((uint64_t)(uintptr_t)im->mem + 4 * q, &w[q]); if (!got) memcpy(px, w, im->fi->vbpb); got = -got; }
            else got = vkt_readback_region(im, 0, 0, &rr, px, im->fi->vbpb);
        }
        if (!got && !vks_to_rgba8(im->fi->vk, px, 1, rgba)) v = rgba[0] / 255.0f;
        uint32_t bits; memcpy(&bits, &v, 4); vk_hm_put(&g_dct_of, &k, (void *)(uintptr_t)((uintptr_t)bits + 1));
        VKLOG("depth comparison on a colour texture (ser%llu %s %ux%u; shader %s): compared with its red channel, %.3f", (unsigned long long)im->serial, im->fi->name, im->w, im->h,
              g_cur_sh ? g_cur_sh->key : "?", v);
    }
    return dcmp_twin_view(v, vi);
}
static VkImageView dcmp_twin_view(float v, int vi) {   // the 1x1 depth image holding v, viewed as vi says (2D, 2D array, cube, cube array)
    for (int i = 0; i < g_ndct; i++) if (g_dct[i].v == v) return g_dct[i].view[vi];
    if (g_ndct == 8) return VK_NULL_HANDLE;
    dcmp_twin *t = &g_dct[g_ndct];
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, VK_IMAGE_TYPE_2D, VK_FORMAT_D32_SFLOAT, {1, 1, 1}, 1, 6, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(g_vk.dev, &ci, NULL, &t->img) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_vk.dev, t->img, &mr);
    if (vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &t->mem)) { vkDestroyImage(g_vk.dev, t->img, NULL); return VK_NULL_HANDLE; }
    vkBindImageMemory(g_vk.dev, t->img, t->mem.mem, t->mem.off);
    static const VkImageViewType vts[4] = {VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY};
    for (int i = 0; i < 4; i++) {
        if (i == 3 && !g_vk.f10.imageCubeArray) continue;
        VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, t->img, vts[i], VK_FORMAT_D32_SFLOAT, {0}, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, i < 2 ? 1u : 6u}};
        if (vkCreateImageView(g_vk.dev, &vci, NULL, &t->view[i]) != VK_SUCCESS) return VK_NULL_HANDLE;
    }
    VkCommandBuffer cb = vk_once_begin(); VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 6};
    vk_image_barrier(cb, t->img, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdClearDepthStencilImage(cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &(VkClearDepthStencilValue){v, 0}, 1, &sr);
    vk_image_barrier(cb, t->img, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    if (vk_once_end(cb)) return VK_NULL_HANDLE;
    t->v = v; g_ndct++;
    return t->view[vi];
}
static VkImageView tex_view(int tid, const vks_bind *b, VkImageLayout *layout, vkt_image **out_im) {
    vkt_reg r; *out_im = NULL;
    int pool_n = vkt_pool_n();
    if (tid <= 0 || (pool_n && tid >= pool_n) || !vkt_reg_copy(tid, &r) || r.gone) return VK_NULL_HANDLE;
    if (r.t.d.target == NVN_TEXTURE_TARGET_BUFFER) { RED_ONCE(30, "a buffer texture in a slot a shader samples as an image: it reads 0 (nvn_mtl.m: dummy)"); return VK_NULL_HANDLE; }
    vkt_image *im = vkt_resolve(&r.t);
    if (!im) return VK_NULL_HANDLE;
    view_t v = r.has_view ? r.v : (view_t){0}; int use = r.has_view;
    VkImageViewType have = v.set_target ? view_type_of_target(v.target, im->vtype) : im->vtype;
    if (have != b->vtype) { v.set_target = 1; v.target = (uint8_t)nvn_target_of(b->vtype, b->ms); use = 1; }
    if ((have == VK_IMAGE_VIEW_TYPE_3D) != (b->vtype == VK_IMAGE_VIEW_TYPE_3D)) {
        RED_ONCE(31, "a 3D texture read as another type, or the reverse (texture ID %d, ser%llu %ux%ux%u view type %d; shader %s binding %u declares view type %d): it reads 0", tid,
                 (unsigned long long)im->serial, im->w, im->h, im->depth, (int)have, g_cur_sh ? g_cur_sh->key : "?", b->binding, (int)b->vtype);
        return VK_NULL_HANDLE; }
    if (b->depth && !(im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) {
        VkImageView dv = dcmp_view(im, b->vtype);
        if (dv) { *layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; return dv; }
    }
    int cf = vkr_fmt_as_ct(v.set_format ? v.format : r.t.d.format, b->ct);
    if (cf >= 0 && !(im->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) { v.set_format = 1; v.format = (uint16_t)cf; use = 1; }
    *layout = im->rest; *out_im = im; td_note(im);
    return vkt_view(im, use ? &v : NULL, r.t.set_swz, r.t.swz);
}
static VkImageView img_view(int tid, const vks_bind *b) {   // a storage image: one level, identity swizzle, a format storage allows
    vkt_reg r;
    if (tid <= 0 || !vkt_reg_copy(tid, &r) || r.gone || r.t.d.target == NVN_TEXTURE_TARGET_BUFFER) return VK_NULL_HANDLE;
    vkt_image *im = vkt_resolve(&r.t);
    if (!im) return VK_NULL_HANDLE;
    if (!(im->usage & VK_IMAGE_USAGE_STORAGE_BIT)) { g_img_nostorage++; RED_ONCE(32, "image binding of texture ID %d (format %#x) whose image has no storage use: writes are lost (ser%llu %ux%u cls %d flags %#x usage %#x vkfmt %d)", tid, r.t.d.format, (unsigned long long)im->serial, im->w, im->h, im->cls, r.t.d.flags, (unsigned)im->usage, (int)im->fi->vk); return VK_NULL_HANDLE; }
    uint32_t level = r.has_view && r.v.set_levels ? r.v.base_level : 0, b0 = r.has_view && r.v.set_layers ? r.v.min_layer : 0;
    VkFormat f = vkf_unorm_of(vkf_view_format(r.t.d.format, r.has_view && r.v.set_format ? r.v.format : r.t.d.format));
    if (f == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32) f = VK_FORMAT_R32_SFLOAT;   // raw 32-bit stores (vk_tex.c image_create)
    VkImageViewType vt = b->vtype;
    if (vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    im->content = 1; td_note(im);
    return vkt_view_raw_(im, f, vt, level, 1, b0, vt == VK_IMAGE_VIEW_TYPE_2D || vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_3D ? 1 : 0, VK_IMAGE_ASPECT_COLOR_BIT, NULL);
}
static VkBufferView tbuf_view(int tid, int ct) {   // a buffer texture: a texel buffer view over the pool mirror, in the shader's numeric class when the bits allow
    vkt_reg r;
    if (tid <= 0 || !vkt_reg_copy(tid, &r) || r.gone || r.t.d.target != NVN_TEXTURE_TARGET_BUFFER || !r.t.mem) return VK_NULL_HANDLE;
    int cf = vkr_fmt_as_ct(r.t.d.format, ct);
    const vkf_info *fi = vkf_get(cf >= 0 ? cf : r.t.d.format);
    if (!fi || !fi->vk || fi->conv != VKF_COPY) { RED_ONCE(33, "buffer texture format %#x has no texel buffer format", r.t.d.format); return VK_NULL_HANDLE; }
    uint64_t n = (uint64_t)r.t.d.width * fi->bpb; if (!n) return VK_NULL_HANDLE;
    if (n / fi->bpb > g_vk.props.limits.maxTexelBufferElements) n = (uint64_t)g_vk.props.limits.maxTexelBufferElements * fi->bpb;
    VkBuffer b; VkDeviceSize o;
    if (!vkm_use((uint64_t)(uintptr_t)r.t.mem, n, &b, &o)) return VK_NULL_HANDLE;
    if (o % g_vk.props.limits.minTexelBufferOffsetAlignment) { RED_ONCE(34, "buffer texture at an offset Vulkan cannot view (%#llx)", (unsigned long long)o); return VK_NULL_HANDLE; }
    VkBufferViewCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, NULL, 0, b, fi->vk, o, n};
    VkBufferView v = VK_NULL_HANDLE;
    if (vkCreateBufferView(g_vk.dev, &ci, NULL, &v) == VK_SUCCESS) view_keep(v);
    return v;
}
static int c0_ssbo_base(int st) { return st == 0 ? DMSH_C0_SSBO_VS : st == 1 ? DMSH_C0_SSBO_PS : st == 2 ? DMSH_C0_SSBO_GS : st == 5 ? DMSH_C0_SSBO_CS : -1; }
static uint32_t g_draw_binst, g_dispatch_dim[3];
static void put_range(uint8_t *d, unsigned off, uint64_t a, uint64_t n) { if (off + 12 <= DMSH_DRV_SIZE) { uint32_t s = (uint32_t)n; memcpy(d + off, &a, 8); memcpy(d + off + 8, &s, 4); } }
// A driver segment as the shader reads it (dmsh_abi.h): c0 the buffer tables and per-draw words, c2 the handle tables
static void drv_fill(uint8_t *d, const vks_shader *sh, int st, int bank) {
    memset(d, 0, DMSH_DRV_SIZE);
    if (bank == 0) {
        if (sh->s2cs) {   // the Switch 2 compute bank, packed per program: [0x10][uniforms 16 B each][storage 16 B each][textures 8 B each][images 8 B each]
            // The first 16 bytes: the dispatch's workgroup counts (c0[0][1] = x, [0][2] = y, [0][3] = z; word 0 is read by no program of the corpus: 6 of 1500 compute modules read words 1 and 2).
            // The auto-exposure pair (2cfd3d8d, 5a6f9e2a), the SH projection of the sky (a225f731) and the NDSR / bloom downsamplers take (c0[1] << 4 or << 5) - 1 as the number of threads across and
            // divide the thread's id by it: with the zeros the bank carried the divisor was -1 (4.3e9) or 0, so every thread of the exposure pair read one texel and the SH sums were 1/0 = inf, -inf and NaN
            // (the SSBO words 12..26 at 0x744a757f00, the NaN the deferred light volumes multiplied by 0 into the HDR target, and the NaN the CPU reads from 0x744fdfe00c)
            { static int on = -1; if (on < 0) { const char *e = getenv("NXRT_VK_S2_C0_DISPATCH"); on = !(e && *e == '0'); }   // NXRT_VK_S2_C0_DISPATCH=0: the zero header of before (to compare)
              uint32_t g[4] = {0, g_dispatch_dim[0], g_dispatch_dim[1], g_dispatch_dim[2]}; if (on) memcpy(d, g, 16); }
            for (int r = 0; r < 16 && 0x10u + 16u * r + 16 <= sh->s2_ssbo_base; r++) put_range(d, 0x10 + 16 * r, S->ubo[5][r].addr, S->ubo[5][r].size);
            for (int r = 0; r < 16 && sh->s2_ssbo_base + 16u * r + 16 <= sh->s2_tex_base; r++) put_range(d, sh->s2_ssbo_base + 16 * r, S->ssbo[5][r].addr, S->ssbo[5][r].size);
            for (uint32_t r = 0; r < sh->s2_ntex && sh->s2_tex_base + 8 * r + 4 <= DMSH_DRV_SIZE; r++) { uint32_t h = (uint32_t)S->tex[5][r]; memcpy(d + sh->s2_tex_base + 8 * r, &h, 4); }
            for (uint32_t r = 0; r < 8 && sh->s2_tex_base + 8 * (sh->s2_ntex + r) + 4 <= DMSH_DRV_SIZE; r++) { uint32_t h = (uint32_t)S->img[5][r]; memcpy(d + sh->s2_tex_base + 8 * (sh->s2_ntex + r), &h, 4); }
            return;
        }
        int base = c0_ssbo_base(st);
        if (base >= 0) for (int r = 0; r < DMSH_C0_SSBO_SLOTS; r++) put_range(d, base + DMSH_C0_ROW * r, S->ssbo[st][r].addr, S->ssbo[st][r].size);
        if (st != 5) {
            const float max_point = 2047.0f; memcpy(d + DMSH_C0_POINT_SIZE, &max_point, 4);   // Maxwell's point size limit (nvn_mtl.m)
            memcpy(d + DMSH_C0_BASE_INSTANCE, &g_draw_binst, 4);                              // the draw's base instance, which GLSLC adds to the instance ID
        }
        if (st == 1) { d[DMSH_C0_SAMPLE_TABLE] = 0x88; }                                     // one sample at the pixel's centre (nvn_mtl.m c0_sample_rows)
        if (st == 5) {
            memcpy(d + DMSH_C0_DISPATCH, g_dispatch_dim, 12);
            for (int r = 0; r < DMSH_C0_CS_UBO_SLOTS; r++) put_range(d, DMSH_C0_CS_UBO + DMSH_C0_ROW * r, S->ubo[5][r].addr, S->ubo[5][r].size);
        }
    } else {
        const uint64_t *tx = S->tex[st], *im = S->img[st];
        for (int r = 0; r < DMSH_C2_IMG_SLOTS; r++) { uint32_t v = (uint32_t)im[r]; memcpy(d + DMSH_C2_IMG + DMSH_C2_ROW * r, &v, 4); }
        for (int r = 0; r < DMSH_C2_TEX_SLOTS; r++) {
            if (r >= 32 && (!tx[r] || (r - 32 < DMSH_C2_IMG_SLOTS && im[r - 32]))) continue;
            uint32_t v = (uint32_t)tx[r]; memcpy(d + DMSH_C2_TEX + DMSH_C2_ROW * r, &v, 4);
        }
        for (int r = 0; r < DMSH_C2_SEP_SLOTS; r++) { uint32_t v = (uint32_t)tx[NVN_SEPTEX_SLOT0 + r]; memcpy(d + DMSH_C2_SEPTEX + DMSH_C2_ROW * r, &v, 4); }
        for (int r = 0; r < DMSH_C2_SEP_SLOTS; r++) { uint32_t v = (uint32_t)S->samp[st][r]; memcpy(d + DMSH_C2_SAMP + DMSH_C2_ROW * r, &v, 4); }
    }
}
// A uniform range: the pool mirror at its offset (uploaded as needed), else a copy (offsets Vulkan cannot bind), else zeros
static VkDescriptorBufferInfo ubo_info(const p_bind_buf *u) {
    VkDescriptorBufferInfo zero = {R0.zero, 0, ZERO_SIZE};
    if (!u->addr || !u->size) return zero;
    VkDeviceSize n = u->size > g_vk.props.limits.maxUniformBufferRange ? g_vk.props.limits.maxUniformBufferRange : u->size;
    VkBuffer b; VkDeviceSize o;
    if (u->size >= UBANK_SIZE) vkm_recopy_once(u->addr, UBANK_SIZE);   // a native program's constant bank: copied again once per frame
    if (!vkm_use(u->addr, n, &b, &o)) { RED_ONCE(40, "a uniform buffer at %#llx is in no memory pool: it reads zeros", (unsigned long long)u->addr); return zero; }
    if (o % g_vk.props.limits.minUniformBufferOffsetAlignment) {
        RED_ONCE(41, "a uniform buffer at an address Vulkan cannot bind (%#llx): read from a copy of guest memory", (unsigned long long)u->addr);
        uint8_t *p = arena(n, g_vk.props.limits.minUniformBufferOffsetAlignment, &b, &o);
        if (!p) return zero;
        memcpy(p, (const void *)(uintptr_t)u->addr, (size_t)n);
    }
    return (VkDescriptorBufferInfo){b, o, n};
}
// The texture ID a handle bound to a slot names, checked against the texture pool: an offset-style Switch 2 handle (index * 32) is converted, an ID past the pool is an empty
// descriptor (reads 0) and is logged, instead of looking past the pool (Eglantine 049c9633, Lichii b34b24cd)
static int handle_id(uint64_t h, int st, const vks_bind *b, const char *what) {
    int kind; int id = vkt_handle_index(h, vkt_pool_n(), &kind);
    if (kind == VKH_OFFSET || kind == VKH_RANGE) {
        static _Atomic int n; int k = atomic_fetch_add(&n, 1);
        if (k < 16) VKRED("%s handle %#llx (stage %d, slot %d, shader %s binding %u): %s", what, (unsigned long long)h, st, b ? b->reg : -1, g_cur_sh ? g_cur_sh->key : "?", b ? b->binding : 0,
                          kind == VKH_OFFSET ? "its ID is a descriptor header byte offset (index * 32): taken as index" : "its ID is outside the texture pool: an empty descriptor");
        if (kind == VKH_RANGE) g_st.handles_bad++;
    }
    return id;
}
static uint64_t g_ind_seen[64]; static int g_ind_nseen;   // the indirect-argument addresses dispatches have used (NXRT_VK_WATCH_IND)
static void gmem_wb_add(uint64_t ptr) {   // NXRT_VK_WRITEBACK=2: the start of the memory a global-store shader writes through, copied back to guest memory at completion
    if (wb_mode() < 2 || !ptr) return;
    static uint64_t cap; if (!cap) { const char *e = getenv("NXRT_VK_WB_GMEM"); cap = e ? strtoull(e, NULL, 0) & ~3ull : 64u << 10; if (!cap) cap = 4; }
    uint64_t n = cap; while (n >= 16 && !vkm_addr(ptr, n, NULL, NULL)) n >>= 1;
    if (n >= 16) wb_add(ptr, n);
}
static uint64_t g_gm_ptr[16]; static int g_gm_np;   // the pointers the gmem shader being bound goes through (diagnostics)
// Descriptor set of stage st's shader, filled from the current bindings
static VkDescriptorSet write_set(const vks_shader *sh, int st, int *swrites) {
    if (sh->big_array) { g_st.skip_bigarray++; RED_ONCE(46, "shader %s declares a descriptor array of %u elements (the bindless texture / sampler pool, binding %u): not replayed yet (a set that large cannot come from the per-draw pools), its draws are skipped",
                                                    sh->key, sh->big_array, sh->big_binding); return VK_NULL_HANDLE; }
    VkDescriptorSet ds = dset_alloc(sh->dsl);
    if (!ds) return VK_NULL_HANDLE;
    g_cur_sh = sh; g_gm_np = 0;
    if (sh->nb > MAXB) { RED_ONCE(42, "shader %s has %d bindings (more than %d)", sh->key, sh->nb, MAXB); return VK_NULL_HANDLE; }
    VkWriteDescriptorSet w[MAXB]; VkDescriptorBufferInfo bi[MAXB]; VkDescriptorImageInfo ii[MAXB]; VkBufferView bv[MAXB];
    int nw = 0;
    for (int i = 0; i < sh->nb; i++) {
        const vks_bind *b = &sh->b[i];
        VkWriteDescriptorSet *x = &w[nw];
        *x = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, b->binding, 0, 1, b->type, NULL, NULL, NULL};
        if (b->kind == VKB_TEXPOOL || b->kind == VKB_SAMPPOOL) continue;   // in the persistent pool set (bl_get)
        if (b->count != 1) { RED_ONCE(43, "shader %s: an array binding (%u elements) is not replayed: left unwritten", sh->key, b->count); continue; }
        switch (b->type) {
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: {
            VkDescriptorBufferInfo *o = &bi[nw]; *o = (VkDescriptorBufferInfo){R0.zero, 0, ZERO_SIZE};
            if (b->kind == VKB_SUPPORT) *o = (VkDescriptorBufferInfo){R0.support, 0, SUPPORT_SIZE};
            else if (b->kind == VKB_CBUF) { if (b->reg >= 0 && b->reg < 16) *o = ubo_info(&S->ubo[st][b->reg]); }
            else if (b->kind == VKB_C1) { if (sh->c1buf) *o = (VkDescriptorBufferInfo){sh->c1buf, sh->c1off, (sh->c1_len + 15) & ~15u}; }
            else if (b->kind == VKB_DRV && (b->bank == 0 || b->bank == 2) && sh->drv_read[b->bank]) {
                VkBuffer ab; VkDeviceSize ao; uint8_t *d = arena(DMSH_DRV_SIZE, g_vk.props.limits.minUniformBufferOffsetAlignment, &ab, &ao);
                if (d) { drv_fill(d, sh, st, b->bank); *o = (VkDescriptorBufferInfo){ab, ao, DMSH_DRV_SIZE}; }
            }
            x->pBufferInfo = o; break; }
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: {
            VkDescriptorBufferInfo *o = &bi[nw]; *o = (VkDescriptorBufferInfo){R0.sink, 0, ZERO_SIZE};
            if (b->kind == VKB_GMEM) {   // the page table of untracked global memory (nxs_spv.py gmem_bda): without it the shader's look-ups have nothing to read, so nothing runs
                VkDeviceSize gsize; VkBuffer gb = vkm_gmem_table(&gsize);
                if (!gb) { RED_ONCE(46, "shader %s reads global memory through device addresses, which this device cannot do: its draws are skipped", sh->key); return VK_NULL_HANDLE; }
                *o = (VkDescriptorBufferInfo){gb, 0, gsize}; x->pBufferInfo = o; vkm_gmem_used(sh->key, b->rw, sh->gmem_open);
                // the pointers it goes through, read here from guest memory as the game wrote them: what they point at is brought up to date for this submission
                g_gm_np = 0;
                for (int r = 0; r < sh->gmem_nroots; r++) {
                    const p_bind_buf *u = sh->gmem_root[r].ubo ? &S->ubo[st][sh->gmem_root[r].reg] : &S->ssbo[st][sh->gmem_root[r].reg]; uint32_t lo, hi; uint64_t at = u->addr + 4ull * sh->gmem_root[r].word;
                    if (u->addr && 4ull * sh->gmem_root[r].word + 8 <= u->size && read_u32(at, &lo) && read_u32(at + 4, &hi)) { vkm_gmem_touch((uint64_t)hi << 32 | lo); if (b->rw) { vkm_gmem_own((uint64_t)hi << 32 | lo); gmem_wb_add((uint64_t)hi << 32 | lo); } if (g_gm_np < 16) g_gm_ptr[g_gm_np++] = (uint64_t)hi << 32 | lo; }
                }
                break; }
            const p_bind_buf *u = b->kind == VKB_SBUF && b->reg >= 0 && b->reg < 16 ? (b->from_ubo ? &S->ubo[st][b->reg] : &S->ssbo[st][b->reg]) : NULL;
            VkBuffer bb; VkDeviceSize oo;
            if (u && u->addr && u->size && vkm_use(u->addr, u->size, &bb, &oo)) {
                VkDeviceSize al = g_vk.props.limits.minStorageBufferOffsetAlignment, need = (u->size + 255) & ~(VkDeviceSize)255;
                if (oo % al && st == 5 && R0.bounce && u->size <= BOUNCE_MAX && g_nbnc < 16) {   // a dispatch (no pass open): through the bounce buffer
                    if (g_bnc_next + need > BOUNCE_SIZE) g_bnc_next = 0;
                    VkDeviceSize bo = g_bnc_next; g_bnc_next += need < al ? al : need;
                    VkCommandBuffer cb = vks_cb(); VkBufferCopy c = {oo, bo, u->size}; barrier_all(cb);   // earlier GPU writes of the pool first
                    vkCmdCopyBuffer(cb, bb, R0.bounce, 1, &c);   // dispatch's barrier_all orders it before the shader
                    *o = (VkDescriptorBufferInfo){R0.bounce, bo, u->size};
                    if (b->rw) { g_bnc[g_nbnc].pool = bb; g_bnc[g_nbnc].poff = oo; g_bnc[g_nbnc].boff = bo; g_bnc[g_nbnc].n = u->size; g_nbnc++;
                        if (swrites) { *swrites = 1; wb_add(u->addr, u->size); } }
                    RED_ONCE(45, "a storage buffer at an address Vulkan cannot bind (%#llx, %#llx bytes, %s; shader %s binding %u): bound through a copy (later ones are not logged)",
                             (unsigned long long)u->addr, (unsigned long long)u->size, b->rw ? "written" : "read only", sh->key, b->binding);
                }
                else if (oo % al) RED_ONCE(44, "a storage buffer at an address Vulkan cannot bind (%#llx, %#llx bytes, %s; shader %s binding %u): not bound",
                                                                                   (unsigned long long)u->addr, (unsigned long long)u->size, b->rw ? "written" : "read only", sh->key, b->binding);
                else {
                    *o = (VkDescriptorBufferInfo){bb, oo, u->size};
                    if (b->rw && swrites) { *swrites = 1; wb_add(u->addr, u->size); }
                }
            }
            x->pBufferInfo = o; break; }
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: {
            VkDescriptorImageInfo *o = &ii[nw]; *o = (VkDescriptorImageInfo){R0.samp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            if (b->kind == VKB_TEX) {
                uint64_t h = tex_slot_h(st, (unsigned)b->reg);
                int tid = handle_id(h, st, b, "texture"), sid = b->samp_kind == VKB_SAMP_SLOT ? (b->samp_reg >= 0 && b->samp_reg < 128 ? (int)((S->samp[st][b->samp_reg] >> 20) & 0xfff) : 0)
                                                  : b->samp_kind == VKB_SAMP_ID ? b->samp_reg : (int)((h >> 20) & 0xfff);
                vkt_image *im;
                o->imageView = tex_view(tid, b, &o->imageLayout, &im);
                o->sampler = samp_get(sid);
                if (!o->imageView) o->imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
            x->pImageInfo = o; break; }
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: {
            VkDescriptorImageInfo *o = &ii[nw]; *o = (VkDescriptorImageInfo){VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL};
            if (b->kind == VKB_IMG && b->reg >= 0 && b->reg < 32) { o->imageView = img_view(handle_id(S->img[st][b->reg], st, b, "image"), b); if (swrites) *swrites = 1; }
            x->pImageInfo = o; break; }
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
            bv[nw] = b->kind == VKB_TBUF ? tbuf_view(handle_id(tex_slot_h(st, (unsigned)b->reg), st, b, "texel buffer"), b->ct) : VK_NULL_HANDLE;
            x->pTexelBufferView = &bv[nw]; break; }
        default: continue;
        }
        nw++;
    }
    vkUpdateDescriptorSets(g_vk.dev, (uint32_t)nw, w, 0, NULL);
    return ds;
}

// ---------- The bindless pool set ----------
// OoT's fragment shaders read a texture handle from a constant buffer and index tex_pool (a SAMPLED_IMAGE array, 1,048,576 long as translated) and samp_pool (4,096 samplers) with
// its two fields: image = handle & 0xfffff, sampler = handle >> 20, both indices of the pools the game bound (SetTexturePool / SetSamplerPool: host ids S->tex_base + i,
// S->samp_base + i). Those cannot be per-draw descriptors: one persistent set (vksh_pool_dsl: UPDATE_AFTER_BIND, PARTIALLY_BOUND, the image array of a variable count) holds a
// descriptor for every pool slot and is bound at set VKS_POOL_SET for the shaders that read them.
//  - The wanted content is rebuilt from the registrations (vkt_reg_epoch moved, or the bound pools changed): sampler slot i = the sampler registered at samp_base + i (the default
//    sampler where none); image slot i = the texture registered at tex_base + i as the shader declares it (a 2D depth-compare image, "t2ds"): a depth texture as its depth view,
//    a small colour texture (the game's 1x1 / 8x8 defaults for lights without shadows) as dcmp_view's depth twin, everything else a 1x1 depth image of 1.0 (lit) so no slot reads
//    garbage. Slots past the pool that are a multiple of 32 alias slot index / 32 (the Switch 2 offset-style handle, index * 32: handle_id)
//  - A ring of sets (a set must not be rewritten while a submission that uses it is pending): the submission takes a free one at its first pool draw, brings it up to date by
//    comparing the wanted descriptors with what that set holds (only the differing slots are written, in runs), and the set is busy until the submission completes (subres_done)
//  - One set per submission: a registration made after the submission's first pool draw is seen by the next submission
typedef struct bl_set { VkDescriptorPool pool; VkDescriptorSet set; VkImageView *cv; VkSampler *cs; uint32_t hi; int fresh; _Atomic int busy; int retired; } bl_set;   // hi: past it every image slot holds the dummy
#define BL_MAXSETS 6
static struct {
    int state;   // 0 not tried, 1 usable, -1 unavailable
    uint32_t ntex_max, nsamp, n_alloc;
    bl_set *sets[BL_MAXSETS]; int nsets; int last;
    VkImageView *wv; VkImageLayout *wl; VkSampler *ws; uint32_t w_hi; int w_valid, w_tbase, w_sbase; uint64_t w_epoch; VkImageView dummy;
    bl_set *cur;
    uint64_t writes, syncs, rebuilds;
} BL;
static const vks_bind g_pool_bind = {.vtype = VK_IMAGE_VIEW_TYPE_2D, .ct = VKS_CT_FLOAT, .depth = 1};
static void bl_release(bl_set *b) { if (b) atomic_store(&b->busy, 0); }
// The image slots a set holds. The module clamps its index to 1048575 (UMin), so every index it can compute must lie inside the array (an out-of-range descriptor read can fault
// the GPU): all of what the device takes, unless NXRT_VK_POOL_SLOTS asks for fewer (memory: 32 bytes a slot on RADV)
static uint32_t bl_need(void) {
    uint32_t n = BL.ntex_max; const char *e = getenv("NXRT_VK_POOL_SLOTS");
    if (e && atoi(e) >= 4096) { uint32_t q = ((uint32_t)atoi(e) + 4095) & ~4095u; if (q < n) n = q; }
    return n;
}
static void bl_free_set(bl_set *b) {
    if (b->pool) vkDestroyDescriptorPool(g_vk.dev, b->pool, NULL);
    free(b->cv); free(b->cs); free(b);
}
static bl_set *bl_new_set(void) {
    bl_set *b = calloc(1, sizeof *b);
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_SAMPLER, BL.nsamp}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, BL.n_alloc}};
    VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT, 1, 2, ps};
    VkDescriptorSetLayout l = vksh_pool_dsl(NULL, NULL);
    VkDescriptorSetVariableDescriptorCountAllocateInfo vc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO, NULL, 1, &BL.n_alloc};
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, &vc, VK_NULL_HANDLE, 1, &l};
    if (vkCreateDescriptorPool(g_vk.dev, &pi, NULL, &b->pool) != VK_SUCCESS) { b->pool = VK_NULL_HANDLE; bl_free_set(b); return NULL; }
    ai.descriptorPool = b->pool;
    if (vkAllocateDescriptorSets(g_vk.dev, &ai, &b->set) != VK_SUCCESS) { bl_free_set(b); return NULL; }
    b->cv = malloc((size_t)BL.n_alloc * sizeof *b->cv); b->cs = malloc((size_t)BL.nsamp * sizeof *b->cs);
    b->fresh = 1; b->hi = BL.n_alloc;   // holds nothing yet: every slot is written once
    for (uint32_t i = 0; i < BL.n_alloc; i++) b->cv[i] = (VkImageView)(uintptr_t)~0ull;
    for (uint32_t i = 0; i < BL.nsamp; i++) b->cs[i] = (VkSampler)(uintptr_t)~0ull;
    return b;
}
// Frees sets of an earlier size (or all of them) that no submission uses
static void bl_sweep(int all) {
    int k = 0;
    for (int i = 0; i < BL.nsets; i++) {
        bl_set *b = BL.sets[i];
        if ((all || b->retired) && !atomic_load(&b->busy)) bl_free_set(b); else BL.sets[k++] = b;
    }
    BL.nsets = k; if (BL.last >= k) BL.last = 0;
}
static int bl_init(void) {
    if (BL.state) return BL.state > 0;
    BL.state = -1;
    if (!vksh_pool_dsl(&BL.ntex_max, &BL.nsamp)) return 0;
    BL.state = 1; return 1;
}
// What every slot should hold now
static void bl_want_build(void) {
    uint64_t ep = vkt_reg_epoch();
    if (BL.w_valid && ep == BL.w_epoch && BL.w_tbase == S->tex_base && BL.w_sbase == S->samp_base) return;
    uint32_t n = BL.n_alloc;
    int first = !BL.wv;
    if (first) { BL.wv = malloc((size_t)n * sizeof *BL.wv); BL.wl = malloc((size_t)n * sizeof *BL.wl); BL.ws = malloc((size_t)BL.nsamp * sizeof *BL.ws); BL.w_hi = n; }
    uint64_t t0 = now_ns();
    BL.w_epoch = ep; BL.w_tbase = S->tex_base; BL.w_sbase = S->samp_base; BL.w_valid = 1;
    if (!BL.dummy) { BL.dummy = dcmp_twin_view(1.0f, 0); if (!BL.dummy) RED_ONCE(47, "bindless pools: no 1x1 depth image for the unused slots: they hold a null descriptor (%s)", g_vk.has.null_descriptor ? "robustness2" : "left unwritten"); }
    int pool_n = vkt_pool_n(); uint32_t limit = pool_n > BL.w_tbase ? (uint32_t)(pool_n - BL.w_tbase) : 0; if (limit > n) limit = n;
    uint32_t ndepth = 0, nsmall = 0;
    uint32_t reset = BL.w_hi + 1 < n ? BL.w_hi + 1 : n;   // everything past the last build's highest slot already holds the dummy
    for (uint32_t i = 0; i < reset; i++) { BL.wv[i] = BL.dummy; BL.wl[i] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }
    BL.w_hi = 0;
    for (uint32_t i = 1; i < limit; i++) {
        int id = BL.w_tbase + (int)i; vkt_reg r;
        if (!vkt_reg_copy(id, &r) || r.gone || r.t.d.target == NVN_TEXTURE_TARGET_BUFFER) continue;
        const vkf_info *fi = vkf_get(r.t.d.format);
        int depth = fi && (fi->flags & VKF_DEPTH), small = r.t.d.width <= 16 && r.t.d.height <= 16 && r.t.d.depth <= 1 && !r.image;
        if (!depth && !small) continue;
        VkImageLayout lay = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; vkt_image *im = NULL;
        g_cur_sh = NULL;
        VkImageView v = tex_view(id, &g_pool_bind, &lay, &im);
        if (!v) continue;
        BL.wv[i] = v; BL.wl[i] = lay; if (depth) ndepth++; else nsmall++;
        if (i > BL.w_hi) BL.w_hi = i;
    }
    for (uint32_t i = limit ? limit : 1; i < n; i++) {   // the index * 32 form of the handle (handle_id's VKH_OFFSET): slot j * 32 holds slot j
        if (i % 32 == 0 && i / 32 < limit && i / 32 > 0 && BL.wv[i / 32] != BL.dummy) { BL.wv[i] = BL.wv[i / 32]; BL.wl[i] = BL.wl[i / 32]; if (i > BL.w_hi) BL.w_hi = i; }
    }
    for (uint32_t i = 0; i < BL.nsamp; i++) BL.ws[i] = samp_get(BL.w_sbase + (int)i);
    BL.rebuilds++;
    static int nl; if (nl++ < 8 || !(nl & 255)) VKLOG("bindless pools: wanted state rebuilt (#%llu) for texture ids %d.. / sampler ids %d..: %u depth, %u small colour (depth-compare stand-ins) of %u slots, %.2f ms",
                                                      (unsigned long long)BL.rebuilds, BL.w_tbase, BL.w_sbase, ndepth, nsmall, limit, (double)(now_ns() - t0) / 1e6);
}
// Writes what differs between the wanted state and b's contents
static void bl_sync(bl_set *b) {
    enum { RUN = 256, BATCH = 32 };
    static VkDescriptorImageInfo ii[RUN * BATCH]; static VkWriteDescriptorSet w[BATCH]; int nw = 0, ni = 0; uint64_t nwrote = 0;   // the replay is one thread
    for (int pass = 0; pass < 2; pass++) {
        uint32_t hi = b->hi > BL.w_hi ? b->hi : BL.w_hi, n = pass ? BL.nsamp : b->fresh || hi + 1 > BL.n_alloc ? BL.n_alloc : hi + 1;   // past both highest slots they all hold the dummy
        for (uint32_t i = 0; i < n; ) {
            int diff = pass ? b->cs[i] != BL.ws[i] : b->cv[i] != BL.wv[i] && (BL.wv[i] || g_vk.has.null_descriptor);
            if (!diff) { i++; continue; }
            uint32_t j = i; int first = ni;
            while (j < n && j - i < RUN) {
                int d = pass ? b->cs[j] != BL.ws[j] : b->cv[j] != BL.wv[j] && (BL.wv[j] || g_vk.has.null_descriptor);
                if (!d) break;
                ii[ni++] = pass ? (VkDescriptorImageInfo){BL.ws[j], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED} : (VkDescriptorImageInfo){VK_NULL_HANDLE, BL.wv[j], BL.wl[j]};
                if (pass) b->cs[j] = BL.ws[j]; else b->cv[j] = BL.wv[j];
                j++;
            }
            w[nw++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, b->set, pass ? 0u : 1u, i, j - i, pass ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ii[first], NULL, NULL};
            nwrote += j - i; i = j;
            if (nw == BATCH || ni > RUN * BATCH - RUN) { vkUpdateDescriptorSets(g_vk.dev, (uint32_t)nw, w, 0, NULL); nw = 0; ni = 0; }
        }
        if (nw) { vkUpdateDescriptorSets(g_vk.dev, (uint32_t)nw, w, 0, NULL); nw = 0; ni = 0; }
    }
    b->hi = BL.w_hi < BL.n_alloc ? BL.w_hi : BL.n_alloc; b->fresh = 0;
    BL.writes += nwrote; BL.syncs++;
    static int nl; if (nl++ < 12 || !(nl & 511)) VKLOG("bindless pools: set %p brought up to date: %llu descriptors written (sync #%llu, %llu in all)", (void *)b->set, (unsigned long long)nwrote, (unsigned long long)BL.syncs, (unsigned long long)BL.writes);
}
// The set the submission being recorded uses (taken, brought up to date and marked busy at its first pool draw); NULL when none can be had (the draw is skipped)
static bl_set *bl_get(void) {
    if (BL.cur) return BL.cur;
    if (!bl_init()) return NULL;
    uint32_t need = bl_need();
    if (BL.n_alloc && need > BL.n_alloc) { for (int i = 0; i < BL.nsets; i++) BL.sets[i]->retired = 1; BL.n_alloc = 0; free(BL.wv); free(BL.wl); free(BL.ws); BL.wv = NULL; BL.wl = NULL; BL.ws = NULL; BL.w_valid = 0; }
    if (!BL.n_alloc) { BL.n_alloc = need; VKLOG("bindless pools: sets of %u images and %u samplers (the texture table is %d ids; the device takes up to %u)", BL.n_alloc, BL.nsamp, vkt_pool_n(), BL.ntex_max); }
    bl_sweep(0);
    bl_want_build();
    bl_set *b = NULL;
    for (int k = 1; k <= BL.nsets && !b; k++) { bl_set *c = BL.sets[(BL.last + k) % BL.nsets]; if (!c->retired && !atomic_load(&c->busy)) { b = c; BL.last = (BL.last + k) % BL.nsets; } }
    if (!b && BL.nsets < BL_MAXSETS) {
        b = bl_new_set();
        if (b) { BL.sets[BL.nsets] = b; BL.last = BL.nsets++; }
    }
    if (!b) { RED_ONCE(48, "bindless pools: every set of the ring is in use by a pending submission (or none could be made): the draws that read the pools are skipped"); return NULL; }
    bl_sync(b);
    atomic_store(&b->busy, 1); BL.cur = b; g_sub.bl = b;
    return b;
}
static void bl_shutdown(void) {
    bl_sweep(1); free(BL.wv); free(BL.wl); free(BL.ws); memset(&BL, 0, sizeof BL);
}

// ---------- Draws (nvn_mtl.m draw_) ----------
static VkFormatFeatureFlags fmt_features(VkFormat f, int buffer) {
    static vk_hmap m = {8};
    uint64_t k = (uint64_t)f | (uint64_t)buffer << 32;
    void *v = vk_hm_get(&m, &k);
    if (v) return (VkFormatFeatureFlags)((uintptr_t)v >> 1);
    VkFormatProperties p; vkGetPhysicalDeviceFormatProperties(g_vk.pd, f, &p);
    VkFormatFeatureFlags r = buffer ? p.bufferFeatures : p.optimalTilingFeatures;
    vk_hm_put(&m, &k, (void *)(((uintptr_t)r << 1) | 1));
    return r;
}
// The inputs whose format's numeric class differs from the declared one in a way Vulkan cannot fetch bit-exactly: the vertex shader variant that declares them
// so (vksh_vin_variant). 32-bit components need none (an integer format read into a float input keeps the bits, and the reverse)
static const vks_shader *vin_shader(const vks_shader *vs) {
    uint32_t mask = 0; uint8_t ct[32] = {0};
    for (int i = 0; i < vs->nin; i++) {
        int a = vs->in[i].loc, nct; if (a >= 16 || a >= S->nattr || !S->attr[a].fmt) continue;
        int exact; VkFormat f = vkp_vertex_format(S->attr[a].fmt, vs->in[i].ct, &exact);
        if (f && exact) continue;
        if (vkp_vertex_format_natural(S->attr[a].fmt, &nct) && nct != vs->in[i].ct) { mask |= 1u << a; ct[a] = (uint8_t)nct; }
    }
    return mask ? vksh_vin_variant(vs, mask, ct) : vs;
}
static void set_vertex_input(VkCommandBuffer cb, const vks_shader *vs, vkp_gkey *k, uint32_t *used, int *use_zero) {
    VkVertexInputBindingDescription2EXT vb[17]; VkVertexInputAttributeDescription2EXT va[32];
    uint32_t na = 0, nb = 0; *used = 0; *use_zero = 0;
    for (int i = 0; i < vs->nin && na < 32; i++) {
        int a = vs->in[i].loc, ct = vs->in[i].ct;
        VkFormat f = VK_FORMAT_UNDEFINED; int exact = 1, st = 0; uint32_t off = 0;
        if (a < 16 && a < S->nattr && S->attr[a].fmt) {
            f = vkp_vertex_format(S->attr[a].fmt, ct, &exact); st = S->attr[a].stream; off = S->attr[a].offset;
            if (st >= 16) f = VK_FORMAT_UNDEFINED;
            if (f && !(fmt_features(f, 1) & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) { RED_ONCE(50, "vertex format %#x (Vulkan %d) is not a vertex buffer format on this device: the attribute reads zeros", S->attr[a].fmt, f); f = VK_FORMAT_UNDEFINED; }
            if (f && !exact) { g_st.inexact_vertex++; static int n; if (n++ < 8) VKRED("vertex attribute %d: format %#x into a %s input of shader %s cannot keep Maxwell's bits (nvn_mtl.m converts these on the CPU): read as %d",
                                                                                          a, S->attr[a].fmt, ct == VKS_CT_UINT ? "uint" : ct == VKS_CT_INT ? "int" : "float", vs->key, f); }
        }
        if (!f) {   // no attribute: the zero buffer (nvn_mtl.m ZERO_ATTR)
            f = ct == VKS_CT_UINT ? VK_FORMAT_R32G32B32A32_UINT : ct == VKS_CT_INT ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_SFLOAT;
            va[na++] = (VkVertexInputAttributeDescription2EXT){VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, NULL, (uint32_t)a, 16, f, 0};
            *use_zero = 1; continue;
        }
        va[na++] = (VkVertexInputAttributeDescription2EXT){VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, NULL, (uint32_t)a, (uint32_t)st, f, off};
        *used |= 1u << st;
    }
    for (int st = 0; st < 16; st++) if (*used >> st & 1) {
        uint32_t div = S->stream[st].divisor;
        vb[nb++] = (VkVertexInputBindingDescription2EXT){VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, NULL, (uint32_t)st, S->stream[st].stride,
                                                         div ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX, div ? div : 1};
    }
    if (*use_zero) vb[nb++] = (VkVertexInputBindingDescription2EXT){VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, NULL, 16, 0, VK_VERTEX_INPUT_RATE_VERTEX, 1};
    if (g_vkp_dyn.vertex_input) vkCmdSetVertexInputEXT(cb, nb, vb, na, va);
    else {   // baked into the pipeline
        k->nattr = (uint8_t)(na > 16 ? 16 : na); k->nbind = 0;
        for (uint32_t i = 0; i < k->nattr; i++) { k->attr[i].loc = (uint8_t)va[i].location; k->attr[i].bind = (uint8_t)va[i].binding; k->attr[i].fmt = va[i].format; k->attr[i].off = va[i].offset; }
        for (uint32_t i = 0; i < nb; i++) { uint32_t bi = vb[i].binding; k->bind[bi].stride = vb[i].stride; k->bind[bi].rate = (uint8_t)vb[i].inputRate; k->bind[bi].divisor = vb[i].divisor; if (bi + 1 > k->nbind) k->nbind = (uint8_t)(bi + 1); }
    }
}
static void bind_vertex_buffers(VkCommandBuffer cb, uint32_t used, int use_zero) {
    for (int st = 0; st < 16; st++) if (used >> st & 1) {
        NVNbufferRange r = S->vbuf[st]; VkBuffer b = R0.zero; VkDeviceSize o = 0, n = ZERO_SIZE;
        if (r.address && r.size && vkm_use(r.address, r.size, &b, &o)) n = r.size;
        else { b = R0.zero; o = 0; n = ZERO_SIZE; if (r.address) RED_ONCE(51, "vertex stream %d at %#llx is in no memory pool: it reads zeros", st, (unsigned long long)r.address); }
        vkCmdBindVertexBuffers2(cb, (uint32_t)st, 1, &b, &o, &n, NULL);
    }
    if (use_zero) { VkDeviceSize o = 0, n = ZERO_SIZE; vkCmdBindVertexBuffers2(cb, 16, 1, &R0.zero, &o, &n, NULL); }
}
static int read_u32(uint64_t a, uint32_t *v) { return !plat_read(a, v, 4); }   // guest memory, guarded
static void draw(const p_draw *dw, const p_draw_idx *di) {
    if (S->render_skip) { g_st.draws_skipped++; return; }
    int prim = dw ? dw->prim : di->prim, expand;
    uint64_t ind = dw ? dw->ind : di->ind, mdi = dw ? dw->mdi_cnt : di->mdi_cnt; int mdi_i = dw ? dw->mdi_i : di->mdi_i;
    if (mdi) { uint32_t cnt; if (read_u32(mdi, &cnt) && (uint32_t)mdi_i >= cnt) return;
        RED_ONCE(52, "an indirect-count multi-draw: its count is read from guest memory at replay (a count the GPU writes in the same submission is missed)"); }
    VkPrimitiveTopology topo = vkp_topology(prim, &expand);
    if (expand == VKP_EXPAND_UNSUPPORTED || expand == VKP_EXPAND_LINE_LOOP) { static int n; if (n++ < 4) VKRED("primitive type %d is not drawn", prim); g_st.draws_skipped++; return; }
    if (S->poly.cull == NVN_FACE_FRONT_AND_BACK && !S->rast_discard) return;
    const vks_shader *sh[5] = {0};
    for (int s = 0; s < 5; s++) if (S->bst[s]) sh[s] = prog_stage(S->bst[s], s);
    if (!sh[0] || (S->bst[1] && !sh[1] && !S->rast_discard)) { g_st.skip_no_shader++; g_st.draws_skipped++; return; }
    { char *list = ab_get(0);   // NXRT_SKIP_DRAW / the NXRT_AB_FILE "draw=" line: vertex or fragment shader keys (hex, 16 digits): draws that use one are never made, to bisect a GPU stall or an image
        if (list) for (int q = 0; q < 2; q++) { const vks_shader *x = q ? sh[1] : sh[0]; char pat[24]; if (!x) continue; snprintf(pat, sizeof pat, ",%s,", x->key); if (strstr(list, pat)) { g_st.draws_skipped++; return; } } }
    {   // NXRT_OOT_LOG_EXPOSURE=<every n draws>: the tone map's (log-bloom final, 42cd3b20a2171413) constant buffer rows 0..3 as the game wrote them, to see whether the exposure (row 3, z) adapts
        static int every = -1, cnt; if (every < 0) { const char *e = getenv("NXRT_OOT_LOG_EXPOSURE"); every = e ? (atoi(e) > 0 ? atoi(e) : 1) : 0; }
        if (every && sh[1] && !strcmp(sh[1]->key, "42cd3b20a2171413") && S->ubo[1][0].addr && S->ubo[1][0].size >= 64 && (cnt++ % every) == 0) {
            float f[16]; memcpy(f, (const void *)(uintptr_t)S->ubo[1][0].addr, 64);
            VKLOG("OoT tone map cbuf (tone-map draw %d): r0 %g %g %g %g | r1 %g %g %g %g | r2 %g %g %g %g | r3 %g %g(K) %g(exposure) %g", cnt - 1, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]); }
    }
    sh[0] = vin_shader(sh[0]);
    if (S->rast_discard) sh[1] = NULL;
    // the render pass, its scissor and viewport (nvn_mtl.m draw_: viewport flipped for the lower-left origin there; Vulkan's y axis runs the other way)
    // indirect draw arguments, written by the GPU: logged and copied through a clamp pass (a wild count draws as nothing; a compute pass cannot run inside a render pass, so it ends the open one)
    VkBuffer ind_b = VK_NULL_HANDLE; VkDeviceSize ind_o = 0;
    if (ind && expand != VKP_EXPAND_QUADS) {
        VkBuffer b0; VkDeviceSize o0; int kind = dw ? 1 : 2;
        if (vkm_use(ind, dw ? 16 : 20, &b0, &o0)) { pass_end(); VkCommandBuffer cb0 = vks_cb(); vkm_ind_record(cb0, sh[0]->key, ind, b0, o0, kind); vkm_ind_clamp(cb0, b0, o0, kind, &ind_b, &ind_o); }
    }
    if (!P.open && !pass_begin()) { g_st.skip_no_target++; g_st.draws_skipped++; return; }
    uint32_t tw = P.any ? P.w : (uint32_t)(S->vp[0][0] + S->vp[0][2]), th = P.any ? P.h : (uint32_t)(S->vp[0][1] + S->vp[0][3]);
    VkRect2D sc; if (!scissor_rect(tw, th, &sc)) { if (!S->rast_discard) return; sc = (VkRect2D){{0, 0}, {tw, th}}; }
    VkViewport vp = S->has_vp ? (VkViewport){S->vp[0][0], S->vp[0][1], S->vp[0][2], S->vp[0][3], S->dr[0], S->dr[1]} : (VkViewport){0, 0, (float)tw, (float)th, S->dr[0], S->dr[1]};
    if (vp.width <= 0 || vp.height == 0) return;
    int flip = !g_origin_ll;
    { uint32_t sw; memcpy(&sw, S->vp_sw[0], 4);
      if (sw == 0x100) flip = !flip;   // NEGATIVE_Y: clip y negated, the viewport flipped about its centre
      else if (sw) RED_ONCE(53, "viewport swizzle %d,%d,%d,%d not implemented (drawn unswizzled)", S->vp_sw[0][0], S->vp_sw[0][1] ^ 2, S->vp_sw[0][2] ^ 4, S->vp_sw[0][3] ^ 6); }
    if (flip) { vp.y += vp.height; vp.height = -vp.height; }
    vp.minDepth = vp.minDepth < 0 ? 0 : vp.minDepth > 1 ? 1 : vp.minDepth; vp.maxDepth = vp.maxDepth < 0 ? 0 : vp.maxDepth > 1 ? 1 : vp.maxDepth;
    VkCommandBuffer cb = vks_cb();
    // M6 phase B: target 0 blending with output 1 as its second source takes the dual-source variant (nvn_mtl.m pso_reads_src1 / fs_dual), an alpha function other than
    // ALWAYS the alpha test variant (nvn_mtl.m atest_key: no test when colour 0 goes to an integer target), with the function and reference as push constants (at_bind)
    int fsv = 0, atest = 0;
    if (sh[1] && P.ncolor && P.cv[0] && (S->chmask.mask & 0xf) && (S->color.blend_en & 1) && (sh[1]->out_mask & 2) && g_vk.f10.dualSrcBlend) {
        blend_st b0 = vkp_adv_blend(S->blend[0]);
        if (vkp_blend_is_src1(b0.src_rgb) || vkp_blend_is_src1(b0.dst_rgb) || vkp_blend_is_src1(b0.src_a) || vkp_blend_is_src1(b0.dst_a)) fsv |= VKSH_FS_DUAL;
    }
    if (sh[1] && S->color.alpha_func && S->color.alpha_func != NVN_ALPHA_FUNC_ALWAYS && (sh[1]->out_mask & 1) && !(P.ncolor && P.cv[0] && fmt_int_kind(P.cf[0]))) fsv |= VKSH_FS_ALPHA;
    if (fsv) { sh[1] = vksh_fs_variant(sh[1], fsv); atest = sh[1]->alpha; }
    // pipeline: what is not dynamic here
    vkp_gkey k; memset(&k, 0, sizeof k);
    for (int s = 0; s < 5; s++) k.sh[s] = sh[s];
    k.ncolor = (uint8_t)P.ncolor; for (int i = 0; i < P.ncolor; i++) k.cfmt[i] = P.cv[i] ? (uint32_t)P.cf[i] : 0;
    k.dfmt = P.dv ? (uint32_t)P.df : 0; k.has_depth = (uint8_t)P.hd; k.has_stencil = (uint8_t)P.hs; k.samples = P.samples;
    k.topo_class = g_vkp_dyn.topo_unrestricted ? VKP_TOPO_ANY : (uint8_t)vkp_topology_class(topo);
    k.depth_m1 = !g_depth_zero;
    VkBool32 ben[8]; VkColorBlendEquationEXT beq[8]; VkColorComponentFlags wm[8];
    const vks_shader *fs = sh[1];
    for (int i = 0; i < P.ncolor; i++) {
        uint32_t m = (S->chmask.mask >> (4 * i)) & 0xf;
        if (!P.cv[i] || !fs || !(fs->out_mask >> i & 1)) m = 0;   // an output the shader does not write: Maxwell leaves the target as it was (nvn_mtl.m pso_desc)
        blend_st bl = vkp_adv_blend(S->blend[i]);
        int en = m && (S->color.blend_en >> i & 1) && (fmt_features(P.cf[i], 0) & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT);
        if (en && (vkp_blend_is_src1(bl.src_rgb) || vkp_blend_is_src1(bl.dst_rgb) || vkp_blend_is_src1(bl.src_a) || vkp_blend_is_src1(bl.dst_a))) {
            if (i == 0 && fs && fs->dual) { static int n; if (!n++) VKLOG("dual-source blending: shader %s's output 1 is target 0's second source", fs->key); }
            else {   // an unwritten second source counts as zero, as nvn_mtl.m blend_zero_src1
                RED_ONCE(54, "dual-source blending (shader %s, target %d): the second source counts as zero (%s)", fs ? fs->key : "-", i, !fs || !(fs->out_mask & 2) ? "the shader does not write output 1" :
                         !g_vk.f10.dualSrcBlend ? "the device has no dual-source blending" : i ? "only target 0 blends with a second source" : "no dual-source variant");
                bl.src_rgb = vkp_blend_zero_src1(bl.src_rgb); bl.dst_rgb = vkp_blend_zero_src1(bl.dst_rgb); bl.src_a = vkp_blend_zero_src1(bl.src_a); bl.dst_a = vkp_blend_zero_src1(bl.dst_a);
            }
        }
        if (m && fs && fs->out_ct[i] == VKS_CT_FLOAT && fmt_int_kind(P.cf[i])) RED_ONCE(55, "shader %s writes float colour %d into an integer target (nvn_mtl.m fs_uint): the values are undefined", fs->key, i);
        ben[i] = en; wm[i] = m;
        beq[i] = (VkColorBlendEquationEXT){vkp_blend_factor(bl.src_rgb), vkp_blend_factor(bl.dst_rgb), vkp_blend_op(bl.eq_rgb), vkp_blend_factor(bl.src_a), vkp_blend_factor(bl.dst_a), vkp_blend_op(bl.eq_a)};
        k.att[i] = (vkp_att){(uint8_t)(g_vkp_dyn.blend_enable ? 0 : en), (uint8_t)(g_vkp_dyn.blend_eq ? 0 : beq[i].srcColorBlendFactor), (uint8_t)(g_vkp_dyn.blend_eq ? 0 : beq[i].dstColorBlendFactor),
                             (uint8_t)(g_vkp_dyn.blend_eq ? 0 : beq[i].srcAlphaBlendFactor), (uint8_t)(g_vkp_dyn.blend_eq ? 0 : beq[i].dstAlphaBlendFactor),
                             (uint8_t)(g_vkp_dyn.blend_eq ? 0 : beq[i].colorBlendOp), (uint8_t)(g_vkp_dyn.blend_eq ? 0 : beq[i].alphaBlendOp), (uint8_t)(g_vkp_dyn.write_mask ? 0 : m)};
    }
    int logic = S->color.logic_op != NVN_LOGIC_OP_COPY && g_vk.f10.logicOp;
    if ((fsv & VKSH_FS_ALPHA) && atest) { static int n; if (!n++) VKLOG("alpha test: function %u, reference %g (shader %s's alpha test variant)", S->color.alpha_func, S->alpha_ref, fs->key); }
    if (!g_vkp_dyn.logic_op_enable) k.logic_en = (uint8_t)logic;
    if (!g_vkp_dyn.logic_op) k.logic_op = (uint8_t)(logic ? vkp_logic_op(S->color.logic_op) : VK_LOGIC_OP_COPY);
    if (!g_vkp_dyn.a2c) k.a2c = S->ms.a2c != 0;
    if (!g_vkp_dyn.polygon_mode) k.poly = (uint8_t)S->poly.mode;
    if (!g_vkp_dyn.depth_clamp) k.depth_clamp = S->depth_clamp != 0 && g_vk.f10.depthClamp;
    uint32_t used; int use_zero;
    set_vertex_input(cb, sh[0], &k, &used, &use_zero);
    VkPipelineLayout layout;
    VkPipeline pipe = vkp_graphics(&k, &layout);
    if (!pipe) { g_st.skip_no_pipeline++; g_st.draws_skipped++; return; }
    if (pipe != g_last_pipe) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); g_last_pipe = pipe;
        { char nb[24]; snprintf(nb, sizeof nb, "v%.8s/f%.8s", sh[0] ? sh[0]->key : "-", fs ? fs->key : "-"); vks_note(nb); } }   // the watchdog's report names the last draws' shaders too
    if (atest) { uint32_t pc[2] = {S->color.alpha_func, 0}; memcpy(&pc[1], &S->alpha_ref, 4); vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, VKP_ALPHA_PC_SIZE, pc); }
    // dynamic state
    vkCmdSetViewportWithCount(cb, 1, &vp); vkCmdSetScissorWithCount(cb, 1, &sc);
    vkCmdSetLineWidth(cb, 1.0f);
    vkCmdSetCullMode(cb, vkp_cull(S->poly.cull)); vkCmdSetFrontFace(cb, vkp_front_face(S->poly.front, g_queue_origin_ll));
    vkCmdSetPrimitiveTopology(cb, topo);
    int cls = vkp_topology_class(topo), pm = S->poly.mode;
    int bias = (S->poly.offset_en & (cls == 0 || pm == NVN_POLYGON_MODE_POINT ? 1 : cls == 1 || pm == NVN_POLYGON_MODE_LINE ? 2 : 4)) != 0;
    vkCmdSetDepthBiasEnable(cb, bias); vkCmdSetDepthBias(cb, S->po.units, g_vk.f10.depthBiasClamp ? S->po.clamp : 0.0f, S->po.factor);
    vkCmdSetBlendConstants(cb, S->blend_color);
    vkCmdSetDepthTestEnable(cb, P.hd && S->ds.dt); vkCmdSetDepthWriteEnable(cb, P.hd && S->ds.dw); vkCmdSetDepthCompareOp(cb, vkp_depth_compare(S->ds.df));
    vkCmdSetDepthBoundsTestEnable(cb, VK_FALSE);
    vkCmdSetStencilTestEnable(cb, P.hs && S->ds.st);
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_BIT, vkp_stencil_op(S->ds.fail_f), vkp_stencil_op(S->ds.zpass_f), vkp_stencil_op(S->ds.zfail_f), vkp_compare(S->ds.sf_f));
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_BACK_BIT, vkp_stencil_op(S->ds.fail_b), vkp_stencil_op(S->ds.zpass_b), vkp_stencil_op(S->ds.zfail_b), vkp_compare(S->ds.sf_b));
    for (int f = 0; f < 2; f++) {
        VkStencilFaceFlags fl = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
        vkCmdSetStencilCompareMask(cb, fl, S->st_vmask[f] & 0xff); vkCmdSetStencilWriteMask(cb, fl, S->st_wmask[f] & 0xff); vkCmdSetStencilReference(cb, fl, S->st_ref[f] & 0xff);
    }
    vkCmdSetRasterizerDiscardEnable(cb, S->rast_discard != 0);
    int restart = S->restart.on && di;
    if (restart && ((di->itype == NVN_INDEX_TYPE_UNSIGNED_BYTE && S->restart.index != 0xff) || (di->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT && S->restart.index != 0xffff) || (di->itype == NVN_INDEX_TYPE_UNSIGNED_INT && S->restart.index != 0xffffffffu)))
        RED_ONCE(57, "primitive restart index %#x is not the index type's largest value: Vulkan restarts only there", S->restart.index);
    if (restart && (cls != 1 && cls != 2 ? 1 : topo == VK_PRIMITIVE_TOPOLOGY_LINE_LIST || topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST || topo == VK_PRIMITIVE_TOPOLOGY_POINT_LIST) && !g_vk.has.prim_list_restart) restart = 0;
    vkCmdSetPrimitiveRestartEnable(cb, restart);
    if (g_vkp_dyn.blend_enable && P.ncolor) vkCmdSetColorBlendEnableEXT(cb, 0, (uint32_t)P.ncolor, ben);
    if (g_vkp_dyn.blend_eq && P.ncolor) vkCmdSetColorBlendEquationEXT(cb, 0, (uint32_t)P.ncolor, beq);
    if (g_vkp_dyn.write_mask && P.ncolor) vkCmdSetColorWriteMaskEXT(cb, 0, (uint32_t)P.ncolor, wm);
    if (g_vkp_dyn.logic_op_enable) vkCmdSetLogicOpEnableEXT(cb, logic);
    if (g_vkp_dyn.logic_op) vkCmdSetLogicOpEXT(cb, logic ? vkp_logic_op(S->color.logic_op) : VK_LOGIC_OP_COPY);
    if (g_vkp_dyn.a2c) vkCmdSetAlphaToCoverageEnableEXT(cb, S->ms.a2c != 0);
    if (g_vkp_dyn.polygon_mode) vkCmdSetPolygonModeEXT(cb, vkp_polygon_mode(S->poly.mode));
    if (g_vkp_dyn.depth_clamp) vkCmdSetDepthClampEnableEXT(cb, S->depth_clamp != 0 && g_vk.f10.depthClamp);
    bind_vertex_buffers(cb, used, use_zero);
    // the draw's base instance for c0 + 0x644 (an indirect draw's from its record in guest memory)
    g_draw_binst = (uint32_t)(dw ? dw->base_instance : di->base_instance);
    if (ind) { uint32_t bi; if (read_u32(ind + (dw ? 12 : 16), &bi)) g_draw_binst = bi; }
    int swrites = 0;
    for (int s = 0; s < 5; s++) if (sh[s]) {
        VkDescriptorSet ds = write_set(sh[s], s, &swrites);
        if (!ds) { g_st.draws_skipped++; return; }
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, (uint32_t)sh[s]->set, 1, &ds, 0, NULL);
    }
    for (int s = 0; s < 5; s++) if (sh[s] && sh[s]->pool) {   // the bindless pools (once: the layout has the pool set once)
        bl_set *bs = bl_get();
        if (!bs) { g_st.draws_skipped++; g_st.skip_bigarray++; return; }
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, VKS_POOL_SET, 1, &bs->set, 0, NULL);
        g_st.pool_draws++; break;
    }
    if (g_pd) {
        char b[400]; int o = 0;
        for (int i = 0; i < P.ncolor; i++) o += snprintf(b + o, sizeof b - o, " a%d{en%d wm%x %d,%d,%d/%d,%d,%d raw%#llx}", i, (int)ben[i], (unsigned)wm[i], beq[i].srcColorBlendFactor, beq[i].dstColorBlendFactor, beq[i].colorBlendOp,
                                                         beq[i].srcAlphaBlendFactor, beq[i].dstAlphaBlendFactor, beq[i].alphaBlendOp, (unsigned long long)*(const uint64_t *)&S->blend[i]);
        if (P.ncolor && ben[0] && (beq[0].dstColorBlendFactor == VK_BLEND_FACTOR_CONSTANT_COLOR || beq[0].srcColorBlendFactor == VK_BLEND_FACTOR_CONSTANT_COLOR || beq[0].dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR || beq[0].dstColorBlendFactor == VK_BLEND_FACTOR_CONSTANT_ALPHA)) o += snprintf(b + o, sizeof b - o, " blendconst %g,%g,%g,%g", S->blend_color[0], S->blend_color[1], S->blend_color[2], S->blend_color[3]);
        PD("    depth state: range %g..%g clamp %d bias en %#x units %g factor %g clamp %g poly mode %d", vp.minDepth, vp.maxDepth, S->depth_clamp, (unsigned)S->poly.offset_en, S->po.units, S->po.factor, S->po.clamp, (int)S->poly.mode);
        PD("draw vs=%s fs=%s prim=%d %s count=%d inst=%u first=%d base_v=%d ind=%#llx | depth t%d w%d f%d stencil %d cull %d front %d | vp %g,%g %gx%g sc %d,%d %ux%u |%s",
           sh[0]->key, sh[1] ? sh[1]->key : "-", prim, di ? (di->itype == 0 ? "u8" : di->itype == 1 ? "u16" : "u32") : "-", dw ? dw->count : di->count,
           (unsigned)((dw ? dw->instances : di->instances) > 0 ? (dw ? dw->instances : di->instances) : 1), dw ? dw->first : 0, di ? di->base_vertex : 0, (unsigned long long)ind,
           P.hd && S->ds.dt, P.hd && S->ds.dw, (int)S->ds.df, P.hs && S->ds.st, (int)S->poly.cull, (int)S->poly.front, vp.x, vp.y, vp.width, vp.height, sc.offset.x, sc.offset.y, sc.extent.width, sc.extent.height, b);
        for (int s = 0; s < 5; s++) if (sh[s]) for (int i = 0; i < sh[s]->nb; i++) {
            const vks_bind *bb = &sh[s]->b[i];
            if (bb->kind == VKB_TEX || bb->kind == VKB_TBUF || bb->kind == VKB_IMG) { uint64_t h = bb->kind == VKB_IMG ? S->img[s][bb->reg] : tex_slot_h(s, (unsigned)bb->reg); vkt_reg r; int tid = (int)(h & 0xfffff);
                int ok = tid > 0 && vkt_reg_copy(tid, &r); vkt_image *im = ok ? vkt_by_serial(r.t.serial) : NULL;
                samp_par sp = {0}; int sok = vkt_samp_copy((int)((h >> 20) & 0xfff), &sp);
                if (sok) PD("    sampler min%d mag%d wrap%d,%d,%d lod %g..%g bias %g aniso %g cmp %d/%d red %d", sp.minf, sp.magf, sp.ws, sp.wt, sp.wr, sp.lod_min, sp.lod_max, sp.lod_bias, sp.aniso, sp.cmp_mode, sp.cmp_func, sp.reduction);
                PD("  st%d b%u %s slot%d tid=%d sid=%d %s ser%llu fmt%#x %ux%ux%u tgt%d lv%u view%d/%d %s", s, bb->binding, bb->kind == VKB_TEX ? "tex" : bb->kind == VKB_TBUF ? "tbuf" : "img", bb->reg, tid, (int)((h >> 20) & 0xfff),
                   ok ? "" : "(none)", ok ? (unsigned long long)r.t.serial : 0ull, ok ? r.t.d.format : 0, ok ? r.t.d.width : 0, ok ? r.t.d.height : 0, ok ? r.t.d.depth : 0, ok ? r.t.d.target : 0, ok ? r.t.d.levels : 0,
                   (int)bb->vtype, ok && r.has_view ? (int)r.v.target : -1, im ? (im->cls == VKT_STORAGE ? "storage" : "gpu") : "-");
                if (ok) PD("    storage pool %p +%#llx mem %p flags %#x layout %d size %#llx image %p", (const void *)r.t.pool, (unsigned long long)r.t.off, (const void *)r.t.mem, r.t.d.flags, r.t.has_layout, (unsigned long long)r.t.L.size, (void *)im); }
            else if (bb->kind == VKB_CBUF || bb->kind == VKB_SBUF) { const p_bind_buf *u = bind_of(bb, s);
                float f[8] = {0}; if (u->addr && u->size >= 32) memcpy(f, (const void *)(uintptr_t)u->addr, 32); else if (u->addr && u->size >= 16) memcpy(f, (const void *)(uintptr_t)u->addr, 16);
                PD("  st%d b%u %s slot%d addr=%#llx size=%llu first %g,%g,%g,%g %g,%g,%g,%g", s, bb->binding, bb->kind == VKB_CBUF ? "cbuf" : "sbuf", bb->reg, (unsigned long long)u->addr, (unsigned long long)u->size, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
                static int nfull = -1; if (nfull < 0) { const char *e = getenv("NXRT_VK_PASSDUMP_CBUF"); nfull = e ? atoi(e) : 0; }   // NXRT_VK_PASSDUMP_CBUF=<floats>: the first n floats of every uniform / storage buffer, 4 to a group
                if (nfull > 0 && u->addr && u->size >= 16) { char t[1400]; int o = 0; uint64_t nf = u->size / 4 < (uint64_t)nfull ? u->size / 4 : (uint64_t)nfull;
                    for (uint64_t k = 0; k < nf && o < (int)sizeof t - 40; k++) { float v; memcpy(&v, (const uint8_t *)(uintptr_t)u->addr + 4 * k, 4); o += snprintf(t + o, sizeof t - o, "%s%g", k % 4 ? "," : " |", v); }
                    PD("    full%s", t); }
                if (bb->kind == VKB_SBUF && u->addr && u->size >= 16 && u->size <= 4096) pk_note(u->addr, (uint32_t)u->size);   // small storage buffers: the mirror's view is logged at the end of the dump frame
            }
        }
        if (di && di->count > 0 && di->count <= 12) { char t[200]; int o = 0; int isz = di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4;
            for (int k = 0; k < di->count; k++) { uint32_t x = 0; memcpy(&x, (const uint8_t *)(uintptr_t)di->ibuf + (size_t)k * isz, (size_t)isz); o += snprintf(t + o, sizeof t - o, " %u", x); }
            PD("  indices at %#llx:%s", (unsigned long long)di->ibuf, t); }
        if (sh[0] && S->nattr && S->attr[0].fmt == 0x22) { int st = S->attr[0].stream; const uint8_t *vb = (const uint8_t *)(uintptr_t)S->vbuf[st].address; char t[300]; int o = 0;
            for (int k = 0; k < 4 && vb && (uint64_t)(k * S->stream[st].stride + 12) <= S->vbuf[st].size; k++) { float f[3]; memcpy(f, vb + k * S->stream[st].stride + S->attr[0].offset, 12); o += snprintf(t + o, sizeof t - o, " (%g,%g,%g)", f[0], f[1], f[2]); }
            PD("  positions:%s", t); }
        if (sh[0]) for (int i = 0; i < sh[0]->nin; i++) { int a = sh[0]->in[i].loc; if (a >= S->nattr) continue;
            int st = S->attr[a].stream & 15; uint32_t v0 = 0; char hx[40] = "-";   // the first vertex's bytes of the attribute
            if (di && di->ibuf && !di->ind && di->count > 0) { int isz = di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4; memcpy(&v0, (const uint8_t *)(uintptr_t)di->ibuf, (size_t)isz); v0 += (uint32_t)di->base_vertex; }
            else if (dw) v0 = (uint32_t)dw->first;
            uint64_t at = (uint64_t)v0 * S->stream[st].stride + S->attr[a].offset;
            if (S->vbuf[st].address && at + 16 <= S->vbuf[st].size) { const uint8_t *p = (const uint8_t *)(uintptr_t)S->vbuf[st].address + at; for (int k = 0; k < 16; k++) snprintf(hx + 2 * k, 3, "%02x", p[k]); }
            PD("  in%d fmt%#x off%u stream%u stride%u div%u vb=%#llx/%llu ct%d v%u %s", a, S->attr[a].fmt, S->attr[a].offset, S->attr[a].stream,
            S->stream[st].stride, S->stream[st].divisor, (unsigned long long)S->vbuf[st].address, (unsigned long long)S->vbuf[st].size, sh[0]->in[i].ct, v0, hx); }
    }
    if (S->vis_on && P.query < 0) { int q = q_alloc(); if (q >= 0) { vkCmdBeginQuery(cb, R0.qp, (uint32_t)q, 0); P.query = q; g_st.queries++; } }
    // the draw
    uint32_t inst = (uint32_t)((dw ? dw->instances : di->instances) > 0 ? (dw ? dw->instances : di->instances) : 1);
    vk_checkpoint(cb, 'd', sh[1] ? sh[1]->key : sh[0]->key, inst, 0, 0, ind != 0);
    if (dw) {
        if (ind && expand == VKP_EXPAND_QUADS) {   // no indirect form of the expansion: the arguments as guest memory holds them now (as the indexed count below; M6, gameplay)
            uint32_t a[4] = {0, 1, 0, 0}; for (int k = 0; k < 4; k++) if (!read_u32(ind + 4 * (uint64_t)k, &a[k])) { RED_ONCE(58, "indirect quads whose arguments cannot be read: not drawn"); return; }
            RED_ONCE(57, "indirect quads: drawn with the arguments guest memory holds when they are recorded (%u vertices, %u instances)", a[0], a[1]);
            if (a[1] > 65536) { RED_ONCE(56, "indirect quads with %u instances in guest memory (a count the GPU had not written yet?): not drawn", a[1]); return; }   // #17: a stale count must not keep the GPU busy past Windows' timeout
            if (a[0] >= 4 && a[1]) { vkCmdBindIndexBuffer(cb, R0.quads, 0, VK_INDEX_TYPE_UINT32); vkCmdDrawIndexed(cb, (a[0] / 4 < 65536 ? a[0] / 4 : 65536) * 6, a[1], 0, (int32_t)a[2], a[3]); }
        }
        else if (ind) { VkBuffer b; VkDeviceSize o;
            if (!vkm_use(ind, 16, &b, &o)) { RED_ONCE(59, "indirect draw arguments at %#llx are in no memory pool", (unsigned long long)ind); return; }
            vkCmdDrawIndirect(cb, ind_b ? ind_b : b, ind_b ? ind_o : o, 1, 16); }
        else if (expand == VKP_EXPAND_QUADS) { if (dw->count >= 4) { vkCmdBindIndexBuffer(cb, R0.quads, 0, VK_INDEX_TYPE_UINT32); vkCmdDrawIndexed(cb, (uint32_t)(dw->count / 4) * 6, inst, 0, dw->first, (uint32_t)dw->base_instance); } }
        else if (dw->count > 0) vkCmdDraw(cb, (uint32_t)dw->count, inst, (uint32_t)dw->first, (uint32_t)dw->base_instance);
    } else {
        int isz = di->itype == NVN_INDEX_TYPE_UNSIGNED_BYTE ? 1 : di->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? 2 : 4;
        VkIndexType it = isz == 1 ? VK_INDEX_TYPE_UINT8_EXT : isz == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        if (isz == 1 && !g_vk.has.index_u8) { RED_ONCE(60, "8-bit indices without VK_EXT_index_type_uint8: not drawn"); return; }
        VkBuffer b; VkDeviceSize o;
        if (expand == VKP_EXPAND_QUADS) {   // indexed quads: expanded from guest memory into a triangle list (nvn_mtl.m nvn_expand_primitive)
            if (ind || di->count < 4) { if (ind) RED_ONCE(61, "indirect indexed quads are not drawn"); return; }
            uint32_t nq = (uint32_t)di->count / 4; uint32_t *d = arena((VkDeviceSize)nq * 24, 16, &b, &o); if (!d) return;
            const uint8_t *src = (const uint8_t *)(uintptr_t)di->ibuf;
            for (uint32_t q = 0; q < nq; q++) { uint32_t v[4]; for (int j = 0; j < 4; j++) { uint32_t x = 0; memcpy(&x, src + (size_t)(4 * q + j) * isz, (size_t)isz); v[j] = x; }
                d[6 * q] = v[0]; d[6 * q + 1] = v[1]; d[6 * q + 2] = v[2]; d[6 * q + 3] = v[0]; d[6 * q + 4] = v[2]; d[6 * q + 5] = v[3]; }
            vkCmdBindIndexBuffer(cb, b, o, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cb, nq * 6, inst, 0, di->base_vertex, (uint32_t)di->base_instance);
        } else {
            uint32_t icount = (uint32_t)(di->count > 0 ? di->count : 1);
            if (ind && !read_u32(ind, &icount)) icount = 4096;   // an indirect draw's index count, as guest memory holds it now
            uint64_t n = (uint64_t)(icount ? icount : 1) * isz;
            if (!vkm_use(di->ibuf, n, &b, &o) && !vkm_use(di->ibuf, (uint64_t)isz, &b, &o)) { RED_ONCE(62, "index buffer at %#llx is in no memory pool", (unsigned long long)di->ibuf); return; }
            if (o % isz) { RED_ONCE(63, "index buffer at an address not aligned to its index size: not drawn"); return; }
            vkCmdBindIndexBuffer(cb, b, o, it);
            if (ind) { VkBuffer ab; VkDeviceSize ao; if (!vkm_use(ind, 20, &ab, &ao)) { RED_ONCE(59, "indirect draw arguments are in no memory pool"); return; } vkCmdDrawIndexedIndirect(cb, ind_b ? ind_b : ab, ind_b ? ind_o : ao, 1, 20); }
            else if (di->count > 0) vkCmdDrawIndexed(cb, (uint32_t)di->count, inst, 0, di->base_vertex, (uint32_t)di->base_instance);
        }
    }
    g_st.draws++;
    (void)swrites;
    if (g_pd) {   // NXRT_VK_WATCH: after each draw that binds the watched storage buffer, what the mirror holds there (changes only; the draw's fragment shader names who read it)
        static uint64_t wa; static uint32_t wn; static int wi; if (!wi) { wi = 1; const char *e = getenv("NXRT_VK_WATCH"); if (e) { char *end; wa = strtoull(e, &end, 16); wn = *end == ',' ? (uint32_t)strtoul(end + 1, NULL, 0) : 192; if (wn > 1024) wn = 1024; } }
        static uint8_t prev[1024]; static uint64_t pf; static int nlog;
        int uses = 0; if (wa) for (int s = 0; s < 5 && !uses; s++) if (sh[s]) for (int i = 0; i < sh[s]->nb; i++) { const vks_bind *bb = &sh[s]->b[i]; if (bb->kind == VKB_SBUF && bind_of(bb, s)->addr == wa) { uses = 1; break; } }
        if (uses) { pass_end(); vks_finish(); uint8_t cur[1024]; if (!vkm_peek(wa, cur, wn)) { if (pf != g_pd_frame) { pf = g_pd_frame; nlog = 0; }
                int nd = 0; for (uint32_t k = 0; k < wn / 4; k++) if (memcmp(cur + 4 * k, prev + 4 * k, 4)) nd++;
                if (nd || nlog < 3) { nlog++; float f[48] = {0}; memcpy(f, cur, wn < sizeof f ? wn : sizeof f);
                    PD("watch %#llx at draw vs=%s fs=%s: %d words changed; w4..11 %g %g %g %g %g %g %g %g | w12..14 %g %g %g | w16..18 %g %g %g | w20..22 %g %g %g | w24..26 %g %g %g | w32..35 %g %g %g %g", (unsigned long long)wa, sh[0]->key, sh[1] ? sh[1]->key : "-", nd,
                       f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[16], f[17], f[18], f[20], f[21], f[22], f[24], f[25], f[26], f[32], f[33], f[34], f[35]); }
                memcpy(prev, cur, wn); } }
    }
    {   // NXRT_VK_NANSCAN=<text in an fs key> (see vks_scan_record): a full copy of the colour target after each such draw into an R11G11B10F target
        static const char *ns = (const char *)-1; if (ns == (const char *)-1) ns = getenv("NXRT_VK_NANSCAN");
        if (ns && *ns && sh[1] && strstr(sh[1]->key, ns) && P.ci[0] && P.ci[0]->fi->vk == VK_FORMAT_B10G11R11_UFLOAT_PACK32 && P.ci[0]->w >= 1280) {
            vkt_image *ti = P.ci[0]; pass_end();
            char tag[160]; snprintf(tag, sizeof tag, "frame %llu #%llu ser%llu vp%gx%g vs=%s fs=%s", (unsigned long long)g_scan_frame, (unsigned long long)g_st.draws, (unsigned long long)ti->serial, S->has_vp ? S->vp[0][2] : 0.f, S->has_vp ? S->vp[0][3] : 0.f, sh[0] ? sh[0]->key : "-", sh[1]->key);
            vks_scan_record(vks_cb(), ti, tag); }
    }
    if (g_pd) {   // NXRT_VK_DRAWSTATS=<width>: after each draw into a colour target of that width (R11G11B10F), the target's luma statistics (waits for the GPU; the pass is reopened by the next draw)
        static int dw = -1; if (dw < 0) { const char *e = getenv("NXRT_VK_DRAWSTATS"); dw = e ? atoi(e) : 0; }
        static int nst, dc; if (dw > 0 && nst < 890 && P.ci[0] && (int)P.ci[0]->w == dw && P.ci[0]->fi->vk == VK_FORMAT_B10G11R11_UFLOAT_PACK32) {
            nst++; vkt_image *ti = P.ci[0]; pass_end();
            char tag[200]; snprintf(tag, sizeof tag, "#%llu ser%llu vp%gx%g vs=%s fs=%s", (unsigned long long)g_st.draws, (unsigned long long)ti->serial, S->has_vp ? S->vp[0][2] : 0.f, S->has_vp ? S->vp[0][3] : 0.f, sh[0] ? sh[0]->key : "-", sh[1] ? sh[1]->key : "-"); vks_stat_record(vks_cb(), ti, tag); }
        no_stat:;
    }
    if (g_nwb >= 240) pass_end();   // the write-back list is nearly full: record the copies now
}

// ---------- Compute (nvn_mtl.m dispatch) ----------
// OoT's final pass is NDSR (agl_ndsr_vr.pck): compute programs the lowering rejects (texture gather, packed half predicates, F2I modes) that upscale the
// 1920x1080 scene into the 2560x1440 target the HUD is drawn over and that is presented. Skipped, that target is never written: the world is black under the
// HUD. Stand-in (as Lichii's f1f5909e / Eglantine dbf34170): the skipped dispatch whose texture slot 0 is E5B9G9R9 (the NDSR input, written by the NDSR light
// pass) becomes a linear blit of that input (NXRT_NDSR_SRC=3: the R11G11B10F texture of its size bound beside it) into the colour textures of the same aspect
// at 1x..4x its size. No temporal or sharpening work: the image is a plain bilinear upscale
static VkImageLayout xfer_in(VkCommandBuffer cb, vkt_image *im, int dst);
static void xfer_out(VkCommandBuffer cb, vkt_image *im, VkImageLayout l);
static int ndsr_standin(void) {   // experimental: NXRT_NDSR_STANDIN=1
    static int on = -1; if (on < 0) { const char *e = getenv("NXRT_NDSR_STANDIN"); on = e && atoi(e) > 0; }
    if (!on) return 0;
    vkt_reg in; uint64_t h0 = tex_slot_h(5, 0); int id0 = (int)(h0 & 0xfffff);
    if (id0 <= 0 || !vkt_reg_copy(id0, &in) || in.t.d.format != 0x40) return 0;
    static int force_in = -1; if (force_in < 0) { const char *e = getenv("NXRT_NDSR_SRC"); force_in = !(e && *e == '3'); }
    vkt_reg src = in;
    for (unsigned sl = 1; sl < 32 && !force_in; sl++) { vkt_reg r; int id = (int)(tex_slot_h(5, sl) & 0xfffff);
        if (id > 0 && vkt_reg_copy(id, &r) && r.t.d.format == 0x3f && r.t.d.width == in.t.d.width && r.t.d.height == in.t.d.height) { src = r; break; } }
    // the targets: every live colour texture of the input's aspect at 1x..4x its width (OoT keeps two 2560x1440 R11G11B10F, ping-pong): all written, the
    // HUD pass draws over whichever is current. The list is rebuilt when one of them goes away
    static int dst_id[4], ndst; static uint64_t dst_ser[4]; vkt_reg dst[4]; int nd = 0;
    for (int i = 0; i < ndst; i++) if (vkt_reg_copy(dst_id[i], &dst[nd]) && dst[nd].live && dst[nd].t.serial == dst_ser[i]) nd++;
    if (nd != ndst || !ndst) {
        nd = ndst = 0;
        for (int id = 1, n = vkt_pool_n(); id < n && ndst < 4; id++) {
            vkt_reg r; if (!vkt_reg_copy(id, &r) || !r.live || r.image) continue;
            uint32_t w = r.t.d.width, hh = r.t.d.height; int dup = 0;
            if ((r.t.d.format != 0x3f && r.t.d.format != 0x40 && r.t.d.format != 0x2e) || w <= in.t.d.width || w > 4 * in.t.d.width || (uint64_t)w * in.t.d.height != (uint64_t)hh * in.t.d.width) continue;
            for (int i = 0; i < ndst; i++) dup |= dst_ser[i] == r.t.serial;
            if (dup) continue;
            dst[ndst] = r; dst_id[ndst] = id; dst_ser[ndst] = r.t.serial; ndst++;
        }
        nd = ndst;
    }
    if (!nd) { RED_ONCE(130, "NDSR stand-in: no %ux%u-aspect target up to 4x for the upscaler's output", in.t.d.width, in.t.d.height); return 0; }
    vkt_image *s = vkt_resolve(&src.t); if (!s) return 0;
    pass_end();
    VkCommandBuffer cb = vks_cb(); barrier_all(cb);
    int filt = (fmt_features(s->fi->vk, 0) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    VkImageLayout ls = xfer_in(cb, s, 0);
    vkt_image *d = NULL;
    for (int i = 0; i < nd; i++) {
        d = vkt_resolve(&dst[i].t); if (!d || d == s) continue;
        VkImageLayout ld = xfer_in(cb, d, 1);
        VkImageBlit r = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int)s->w, (int)s->h, 1}}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int)d->w, (int)d->h, 1}}};
        vkCmdBlitImage(cb, s->img, ls, d->img, ld, 1, &r, filt ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        xfer_out(cb, d, ld); d->content = 1;
    }
    xfer_out(cb, s, ls);
    if (!d) return 0;
    static int nlog; if (nlog++ < 4) VKLOG("NDSR stand-in: ser%llu %s %ux%u -> %d target%s, ser%llu first, %s %ux%u (bilinear blit in place of the upscaler)", (unsigned long long)src.t.serial, s->fi->name, s->w, s->h,
                                          nd, nd > 1 ? "s" : "", (unsigned long long)dst[0].t.serial, d->fi->name, d->w, d->h);
    PD("NDSR stand-in ser%llu -> %d targets", (unsigned long long)src.t.serial, nd);
    return 1;
}
// OoT's movies are drawn through the NDSR chain too: the game copies the decoder's NV12 frame into an R8 (Y, 1920x1080) and an RG8 (UV, half size) texture and runs
// fourteen compute programs (all rejected by the lowering) that turn it into RGB, upscale it into a 2560x1440 RGBA8_SRGB target, and the HUD passes draw that target.
// The first program is the YUV conversion: tex slot 1 = Y, slot 2 = UV, image slot 1 = the final target. Stand-in (NXRT_MOVIE_STANDIN=0 turns it off): that dispatch
// runs shaders/movie_yuv.comp (BT.709 limited range, bilinear to the target's size) straight into the target; the other thirteen stay skipped
static const vks_shader *movie_yuv_cs(void) {
    static vks_shader *s; static int tried;
    if (tried) return s;
    tried = 1;
    static const char js[] = "{\"key\":\"movie_yuv\",\"stage\":\"cs\",\"bindings\":["
        "{\"set\":0,\"binding\":0,\"descriptor\":\"combined_image_sampler\",\"count\":1,\"nvn\":{\"kind\":\"tex\",\"reg\":1,\"sampler\":{\"kind\":\"samp_tex\",\"reg\":1}}},"
        "{\"set\":0,\"binding\":1,\"descriptor\":\"combined_image_sampler\",\"count\":1,\"nvn\":{\"kind\":\"tex\",\"reg\":2,\"sampler\":{\"kind\":\"samp_tex\",\"reg\":2}}},"
        "{\"set\":0,\"binding\":2,\"descriptor\":\"storage_image\",\"count\":1,\"nvn\":{\"kind\":\"img\",\"reg\":1,\"rw\":true}}],"
        "\"inputs\":[],\"outputs\":[],\"local_size\":[8,8,1]}";
    const char *why = NULL;
    s = vksh_make("movie_yuv", (const uint8_t *)movie_yuv_comp_spv, sizeof movie_yuv_comp_spv, js, sizeof js - 1, &why);
    if (!s) VKRED("movie stand-in: its compute shader could not be made (%s): movies stay black", why ? why : "?");
    return s;
}
static uint64_t g_movie_dst;   // the serial of the movie target the NV12 stand-in last wrote: later chain dispatches that write it are skipped (see dispatch_run)
static int movie_yuv_match(uint32_t *tw, uint32_t *th, uint64_t *dser) {
    static int on = -1; if (on < 0) { const char *e = getenv("NXRT_MOVIE_STANDIN"); on = !(e && *e && !atoi(e)); }
    if (!on) return 0;
    vkt_reg y, uv, dst;
    if (!vkt_reg_copy((int)(tex_slot_h(5, 1) & 0xfffff), &y) || !vkt_reg_copy((int)(tex_slot_h(5, 2) & 0xfffff), &uv) || !vkt_reg_copy((int)(S->img[5][1] & 0xfffff), &dst)) return 0;
    if (y.t.d.format != 0x01 || uv.t.d.format != 0x0d || dst.t.d.width < 64 || y.t.d.width < 64) return 0;
    if (uv.t.d.width != (y.t.d.width + 1) / 2 || uv.t.d.height != (y.t.d.height + 1) / 2 || y.t.d.target == NVN_TEXTURE_TARGET_BUFFER) return 0;
    *tw = dst.t.d.width; *th = dst.t.d.height; *dser = dst.t.serial;
    return 1;
}
// NXRT_SKIP_COMPUTE=<key>,<key>... (hex shader keys, separated by commas, spaces or colons; "all" or "*" skips every compute dispatch): the listed compute shaders are never
// dispatched. A safety net for a shader that hangs the GPU (a hung dispatch resets the whole GPU: amdgpu "ring gfx_0.0.0 timeout"), found from the GPU watchdog's report
// (vk_submit.c) or the log's last "(compute)" line. Read once
static int skip_compute(const char *key) {
    static int logged; char *list = ab_get(1);
    if (!list) return 0;
    if (strstr(list, ",all,") || strstr(list, ",*,")) { if (!logged++) VKRED("NXRT_SKIP_COMPUTE=all: every compute dispatch is skipped"); return 1; }
    char pat[24]; snprintf(pat, sizeof pat, ",%s,", key);
    return strstr(list, pat) != NULL;
}
// ---------- The real NDSR final dispatch (e5addddb...): a blit stand-in ----------
// The four NDSR dispatches of the 3D scene exist in the shader cache and run (the skipped-dispatch stand-in above never fires for them); the last one (40x45 groups of 128,
// e5addddb60498099) is the network's final step: it reads the scene colour (E5B9G9R9, written by 36ce52b7) and stores the 2560x1440 image the HUD is drawn over. Whatever the
// translated chain produces there, this replaces that dispatch with shaders/ndsr_blit.comp: the same two images, bilinear, as Eglantine's UpscaleInsteadOfNativeProgram (dbf34170)
// does for a rejected network. The images are the ones the program itself reads and stores to (its own texture / image bindings, then c0+0x80..0x11c as the fallback); a pair
// must keep the aspect ratio and scale by more than 1x and at most 4x (anything else is a lookup table or a luminance image); the largest input wins.
//   NXRT_NDSR_STANDIN=0|1     the stand-in on (default) / off; the skipped-dispatch stand-in above is still opt-in with =1
//   NXRT_NDSR_FINAL=<keys>    the shaders replaced, hex keys separated by commas / spaces / colons (default e5addddb60498099,6824e44c7bc45ddf; see ndsr_auto_final)
//   NXRT_NDSR_IN=tm|auto      tm (default): the R11G11B10F texture the chain's first dispatch read (the tone-mapped colour), else the program's own bindings; auto: only the program's own texture bindings (the E5B9G9R9
//                             scene colour, which the translated 36ce52b7 left black in the validation runs)
//   NXRT_NDSR_CHAIN=<keys>    the earlier chain dispatches whose R11G11B10F inputs "tm" remembers (default b9932240635ba8c4)
static const vks_shader *ndsr_blit_cs(void) {
    static vks_shader *s; static int tried;
    if (tried) return s;
    tried = 1;
    static const char js[] = "{\"key\":\"ndsr_blit\",\"stage\":\"cs\",\"bindings\":["
        "{\"set\":0,\"binding\":0,\"descriptor\":\"combined_image_sampler\",\"count\":1,\"nvn\":{\"kind\":\"tex\",\"reg\":0,\"sampler\":{\"kind\":\"samp_tex\",\"reg\":0}}},"
        "{\"set\":0,\"binding\":1,\"descriptor\":\"storage_image\",\"count\":1,\"nvn\":{\"kind\":\"img\",\"reg\":1,\"rw\":true}}],"
        "\"inputs\":[],\"outputs\":[],\"local_size\":[8,8,1]}";
    const char *why = NULL;
    s = vksh_make("ndsr_blit", (const uint8_t *)ndsr_blit_comp_spv, sizeof ndsr_blit_comp_spv, js, sizeof js - 1, &why);
    if (!s) VKRED("NDSR final stand-in: its compute shader could not be made (%s)", why ? why : "?");
    return s;
}
typedef struct { char *list; int init; } keylist;
static int key_listed(keylist *kl, const char *env, const char *def, const char *key) {
    if (!kl->init) {
        kl->init = 1; const char *e = getenv(env); if (!e || !*e) e = def;
        size_t n = strlen(e); kl->list = malloc(n + 3); kl->list[0] = ','; size_t o = 1;
        for (size_t i = 0; i < n; i++) { char c = e[i]; if (c == ' ' || c == ';' || c == ':' || c == '\t') c = ','; kl->list[o++] = (char)(c >= 'A' && c <= 'F' ? c + 32 : c); }
        kl->list[o++] = ','; kl->list[o] = 0;
    }
    char pat[24]; snprintf(pat, sizeof pat, ",%s,", key);
    return strstr(kl->list, pat) != NULL;
}
static int ndsr_final_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("NXRT_NDSR_STANDIN"); on = !(e && *e && !atoi(e)); } return on; }   // default on (validated, r7); NXRT_NDSR_STANDIN=0 runs the real dispatch
static int ndsr_in_tm(void) { static int m = -1; if (m < 0) { const char *e = getenv("NXRT_NDSR_IN"); m = !(e && (!strcmp(e, "auto") || !strcmp(e, "e5"))); } return m; }   // default tm: the E5B9G9R9 colour the translated 36ce52b7 writes was black (r3)
#define NDSR_HIST 4
static uint64_t g_ndsr_hist[NDSR_HIST]; static int g_ndsr_nhist;   // tone-mapped colour handles the chain's first dispatch read ("tm")
static int ndsr_pair_ok(const vkt_reg *f, const vkt_reg *t) {   // Eglantine upscale_pair: the scale in (1, 4], the aspect ratio kept (within 1/32)
    uint64_t fw = f->t.d.width, fh = f->t.d.height, tw = t->t.d.width, th = t->t.d.height;
    if (f->t.d.target == NVN_TEXTURE_TARGET_BUFFER || t->t.d.target == NVN_TEXTURE_TARGET_BUFFER || !f->live || !t->live || f->t.serial == t->t.serial) return 0;
    uint64_t a = tw * fh, b = th * fw;
    return fw >= 64 && tw > fw && tw <= 4 * fw && (a > b ? a - b : b - a) * 32 <= a;
}
static int ndsr_reg(uint64_t h, vkt_reg *r) { int id = (int)(h & 0xfffff); return id > 0 && vkt_reg_copy(id, r) && !r->gone; }
// the (input handle, output handle) pair for the final dispatch; 0 without one
static int ndsr_pick(const vks_shader *cs, uint64_t *hin, uint64_t *hout, vkt_reg *rin, vkt_reg *rout, const char **how) {
    uint64_t reads[64], stores[24]; int nr = 0, ns = 0;
    int tm = ndsr_in_tm() && g_ndsr_nhist;
    if (tm) { for (int i = 0; i < g_ndsr_nhist && nr < 64; i++) reads[nr++] = g_ndsr_hist[i]; }
    else for (int i = 0; i < cs->nb; i++) if (cs->b[i].kind == VKB_TEX && cs->b[i].reg >= 0 && nr < 64) reads[nr++] = tex_slot_h(5, (unsigned)cs->b[i].reg);
    for (int i = 0; i < cs->nb; i++) if (cs->b[i].kind == VKB_IMG && cs->b[i].reg >= 0 && cs->b[i].reg < 32 && ns < 16) stores[ns++] = S->img[5][cs->b[i].reg];
    int own_r = nr, own_s = ns;
    const p_bind_buf *u = &S->ubo[5][0];
    for (int pass = 0; pass < 2; pass++) {   // the program's own handles first, then cbuf0+0x80..0x11c
        if (pass) {
            if (!u->addr) break;
            for (unsigned o = 0x80; o < 0x120 && o + 4 <= u->size; o += 4) { uint32_t lo = 0; if (!read_u32(u->addr + o, &lo) || !lo) continue;
                if (!tm && nr < 64) reads[nr++] = lo;
                if (ns < 24) stores[ns++] = lo; }
        }
        int best = 0; uint64_t bestarea = 0;
        for (int si = pass ? own_s : 0; si < ns && !best; si++) { vkt_reg to; if (!ndsr_reg(stores[si], &to)) continue;
            for (int same = 1; same >= 0 && !best; same--)
                for (int ri = pass ? own_r : 0; ri < nr; ri++) { vkt_reg from; if (!ndsr_reg(reads[ri], &from)) continue;
                    if (same && from.t.d.format != to.t.d.format) continue;
                    if (!ndsr_pair_ok(&from, &to)) continue;
                    uint64_t area = (uint64_t)from.t.d.width * from.t.d.height;
                    if (!best || area > bestarea) { best = 1; bestarea = area; *rin = from; *rout = to; *hin = reads[ri]; *hout = stores[si]; } } }
        if (best) { *how = pass ? "c0 words" : tm ? "chain history" : "bindings"; return 1; }
    }
    return 0;
}
static void ndsr_note_chain(const vks_shader *cs) {   // "tm": remember the R11G11B10F textures a chain dispatch reads
    static keylist kl; if (!ndsr_in_tm() || !key_listed(&kl, "NXRT_NDSR_CHAIN", "b9932240635ba8c4", cs->key)) return;
    g_ndsr_nhist = 0;
    for (int i = 0; i < cs->nb && g_ndsr_nhist < NDSR_HIST; i++) { vkt_reg r; if (cs->b[i].kind != VKB_TEX || cs->b[i].reg < 0) continue;
        uint64_t h = tex_slot_h(5, (unsigned)cs->b[i].reg); if (ndsr_reg(h, &r) && r.t.d.format == 0x3f && r.t.d.width >= 64) g_ndsr_hist[g_ndsr_nhist++] = h; }
}
// The network's final step is a different program for each input size: e5addddb60498099 takes the 1920x1080 frame of the cutscenes and the house, 6824e44c7bc45ddf
// (54x30 groups of 128) the 1280x720 frame of gameplay outdoors (Kokiri Forest). Left to run, the translated 6824e44c stores black into the 2560x1440 target and only
// the HUD is seen over it. Besides the listed keys, NXRT_NDSR_AUTO (default 1) takes as a final step any dispatch whose own bindings read an E5B9G9R9 texture and store
// to an R11G11B10F image larger by (1, 4]x with the aspect kept (Lichii f069f561 pairs the same two formats for the gameplay network); each such key is logged once.
static int ndsr_auto_final(const vks_shader *cs) {
    static int on = -1; if (on < 0) { const char *e = getenv("NXRT_NDSR_AUTO"); on = !(e && *e && !atoi(e)); }
    if (!on) return 0;
    for (int i = 0; i < cs->nb; i++) { vkt_reg from; if (cs->b[i].kind != VKB_TEX || cs->b[i].reg < 0 || !ndsr_reg(tex_slot_h(5, (unsigned)cs->b[i].reg), &from) || from.t.d.format != 0x40) continue;
        for (int j = 0; j < cs->nb; j++) { vkt_reg to; if (cs->b[j].kind != VKB_IMG || cs->b[j].reg < 0 || cs->b[j].reg >= 32 || !ndsr_reg(S->img[5][cs->b[j].reg], &to) || to.t.d.format != 0x3f) continue;
            if (ndsr_pair_ok(&from, &to)) {
                static char seen[8][17]; static int nseen; int k;
                for (k = 0; k < nseen && strcmp(seen[k], cs->key); k++) ;
                if (k == nseen && nseen < 8) { snprintf(seen[nseen++], sizeof seen[0], "%s", cs->key);
                    VKLOG("NDSR final stand-in: %s taken as the network's final step (reads E5B9G9R9 ser%llu %ux%u, stores R11G11B10F ser%llu %ux%u; NXRT_NDSR_AUTO=0 runs it)", cs->key,
                          (unsigned long long)from.t.serial, from.t.d.width, from.t.d.height, (unsigned long long)to.t.serial, to.t.d.width, to.t.d.height); }
                return 1; } } }
    return 0;
}
static void dispatch_run(const p_dispatch *dp, const vks_shader *force_cs);
static void dispatch(const p_dispatch *dp) {
    const vks_shader *cs = S->cprog ? prog_stage(S->cprog, VKS_CS) : NULL;
    if (cs && ndsr_final_on()) {
        static keylist kl; ndsr_note_chain(cs);
        if (key_listed(&kl, "NXRT_NDSR_FINAL", "e5addddb60498099,6824e44c7bc45ddf", cs->key) || ndsr_auto_final(cs)) {
            const vks_shader *bl = ndsr_blit_cs(); uint64_t hin = 0, hout = 0; vkt_reg ri, ro; const char *how = "";
            if (bl && ndsr_pick(cs, &hin, &hout, &ri, &ro, &how)) {
                static int nlog; if (nlog++ < 6) VKLOG("NDSR final stand-in: %s %dx%dx%d replaced, ser%llu %#x %ux%u -> ser%llu %#x %ux%u (%s; bilinear blit)", cs->key, (int)dp->x, (int)dp->y, (int)dp->z,
                                                       (unsigned long long)ri.t.serial, ri.t.d.format, ri.t.d.width, ri.t.d.height, (unsigned long long)ro.t.serial, ro.t.d.format, ro.t.d.width, ro.t.d.height, how);
                PD("NDSR final stand-in %s: ser%llu %ux%u -> ser%llu %ux%u (%s)", cs->key, (unsigned long long)ri.t.serial, ri.t.d.width, ri.t.d.height, (unsigned long long)ro.t.serial, ro.t.d.width, ro.t.d.height, how);
                uint64_t t0 = S->tex[5][0], i1 = S->img[5][1]; p_dispatch md = *dp;
                S->tex[5][0] = hin; S->img[5][1] = hout;
                md.x = (ro.t.d.width + 7) / 8; md.y = (ro.t.d.height + 7) / 8; md.z = 1; md.ind = 0;
                dispatch_run(&md, bl);
                S->tex[5][0] = t0; S->img[5][1] = i1;
                return;
            }
            RED_ONCE(131, "NDSR final stand-in: shader %s has no input / output pair (aspect kept, scale up to 4x) among its own bindings or c0+0x80..0x11c: the real dispatch runs", cs->key);
        }
    }
    dispatch_run(dp, NULL);
}
static void dispatch_run(const p_dispatch *dp, const vks_shader *force_cs) {
    const vks_shader *cs = force_cs ? force_cs : S->cprog ? prog_stage(S->cprog, VKS_CS) : NULL;
    p_dispatch md;
    // the movie chain's first program (fdbba6b7fcecc235, NXRT_MOVIE_YUV) is in the shader cache since the 0x14 control-block dumps are translated: it still gets the NV12 stand-in
    // (the translated one fills an intermediate the stand-in does not use), and the later dispatches that translate now and store to the movie target are skipped, as they were
    // when they had no shader: they would overwrite the stand-in's picture with the output of a chain that never got its input
    static keylist myuv;
    if (!force_cs && S->cprog && (!cs || key_listed(&myuv, "NXRT_MOVIE_YUV", "fdbba6b7fcecc235", cs->key))) { uint32_t w, h; uint64_t ser;
        if (movie_yuv_match(&w, &h, &ser) && (cs = movie_yuv_cs())) {
            md = *dp; md.x = (w + 7) / 8; md.y = (h + 7) / 8; md.z = 1; md.ind = 0; dp = &md; g_movie_dst = ser;
            static int nlog; if (nlog++ < 3) VKLOG("movie stand-in: NV12 -> RGB into the %ux%u target in place of the game's YUV pass (BT.709 limited, bilinear)", w, h); } }
    else if (!force_cs && cs && g_movie_dst) {
        for (int i = 0; i < cs->nb; i++) { vkt_reg r; if (cs->b[i].kind != VKB_IMG || cs->b[i].reg < 0 || cs->b[i].reg >= 32) continue;
            if (ndsr_reg(S->img[5][cs->b[i].reg], &r) && r.t.serial == g_movie_dst) { g_st.skip_env++; static int n; if (n++ < 4) VKLOG("movie stand-in: compute shader %s skipped: it stores to the movie target the stand-in wrote", cs->key); return; } } }
    if (!cs) {
        if (S->cprog && ndsr_standin()) return;
        g_st.skip_no_shader++;
        if (g_pd && S->cprog) {   // what a missing compute program reads: its key, c0 (bank 0 of the compute stage) +0x80..+0x11f, the textures / images those words name
            uint8_t k[8] = {0}; prog_key(S->cprog, 0, k); const p_bind_buf *u = &S->ubo[5][0];
            PD("dispatch SKIPPED (no shader) key=%02x%02x%02x%02x%02x%02x%02x%02x %dx%dx%d ind=%#llx c0 addr=%#llx size=%llu", k[0], k[1], k[2], k[3], k[4], k[5], k[6], k[7],
               (int)dp->x, (int)dp->y, (int)dp->z, (unsigned long long)dp->ind, (unsigned long long)u->addr, (unsigned long long)u->size);
            for (unsigned o = 0; u->addr && o + 4 <= u->size && o < 0x100; o += 4) {
                uint32_t lo = 0; read_u32(u->addr + o, &lo); vkt_reg r; int tid = (int)(lo & 0xfffff);
                if (lo && tid > 0 && vkt_reg_copy(tid, &r)) PD("  c0+%#x %08x -> id %d ser%llu fmt%#x %ux%u%s", o, lo, tid, (unsigned long long)r.t.serial, r.t.d.format, r.t.d.width, r.t.d.height, r.image ? " image" : "");
            }
            for (int img = 0; img < 2; img++) for (unsigned sl = 0; sl < 32; sl++) {
                uint64_t h = img ? (sl < sizeof S->img[5] / sizeof S->img[5][0] ? S->img[5][sl] : 0) : tex_slot_h(5, sl); int tid = (int)(h & 0xfffff); vkt_reg r;
                if (tid > 0 && vkt_reg_copy(tid, &r)) PD("  %s slot%u id %d ser%llu fmt%#x %ux%u", img ? "img" : "tex", sl, tid, (unsigned long long)r.t.serial, r.t.d.format, r.t.d.width, r.t.d.height);
            }
        }
        return;
    }
    if (skip_compute(cs->key)) { g_st.skip_env++; static int n; if (n++ < 16 || !(n & 1023)) VKLOG("compute shader %s: dispatch %dx%dx%d skipped (NXRT_SKIP_COMPUTE; %d so far)", cs->key, (int)dp->x, (int)dp->y, (int)dp->z, n); return; }
    VkPipelineLayout layout; VkPipeline pipe = vkp_compute(cs, &layout);
    if (!pipe) return;
    pass_end();
    VkCommandBuffer cb = vks_cb();
    g_dispatch_dim[0] = (uint32_t)dp->x; g_dispatch_dim[1] = (uint32_t)dp->y; g_dispatch_dim[2] = (uint32_t)dp->z;
    if (dp->ind) for (int i = 0; i < 3; i++) read_u32(dp->ind + 4 * i, &g_dispatch_dim[i]);   // for c0's dispatch words: the record as guest memory holds it now
    int swrites = 0; uint64_t nos0 = g_img_nostorage;
    VkDescriptorSet ds = write_set(cs, 5, &swrites);
    if (!ds) return;
    if (g_img_nostorage != nos0) {   // an image this shader writes has no storage use: its writes would be lost (a null descriptor), so the dispatch does no useful work; skip it
        g_st.skip_nostorage++; static int n; if (n++ < 8) VKRED("compute shader %s: dispatch %dx%dx%d skipped: a storage image binding has no storage use (see the image message above)", cs->key, (int)dp->x, (int)dp->y, (int)dp->z);
        return;
    }
    vks_note(cs->key);
    barrier_all(cb);
    if (vkm_ind_is_wanted(cs->key)) for (int i = 0; i < g_gm_np; i++) vkm_snap(cb, cs->key, "pre", g_gm_ptr[i]);
    VkBuffer ind_buf = VK_NULL_HANDLE; VkDeviceSize ind_off = 0;
    if (dp->ind) { static int ni; int seen = 0; for (int i = 0; i < ni && i < (int)(sizeof g_ind_seen / sizeof *g_ind_seen); i++) seen |= g_ind_seen[i] == dp->ind; if (!seen && ni < (int)(sizeof g_ind_seen / sizeof *g_ind_seen)) g_ind_seen[ni++] = dp->ind; g_ind_nseen = ni; }
    if (dp->ind) {   // the arguments as the mirror holds them, copied for the log and clamped before the dispatch reads them (the clamp pass binds its own pipeline, so it goes first)
        VkBuffer b; VkDeviceSize o; if (!vkm_use(dp->ind, 12, &b, &o)) { RED_ONCE(70, "indirect dispatch arguments are in no memory pool"); return; }
        vkm_ind_record(cb, cs->key, dp->ind, b, o, 0); vkm_ind_clamp(cb, b, o, 0, &ind_buf, &ind_off);
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe); g_last_pipe = VK_NULL_HANDLE;
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &ds, 0, NULL);
    if (cs->pool) {
        bl_set *bs = bl_get();
        if (!bs) { g_st.skip_bigarray++; return; }
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, VKS_POOL_SET, 1, &bs->set, 0, NULL);
        g_st.pool_draws++;
    }
    if (g_pd) {
        PD("dispatch cs=%s %ux%ux%u ind=%#llx local %ux%ux%u", cs->key, g_dispatch_dim[0], g_dispatch_dim[1], g_dispatch_dim[2], (unsigned long long)dp->ind, cs->local[0], cs->local[1], cs->local[2]);
        for (int i = 0; i < cs->nb; i++) { const vks_bind *bb = &cs->b[i];
            if (bb->kind == VKB_SBUF || bb->kind == VKB_CBUF) { const p_bind_buf *u = bind_of(bb, 5); PD("  b%u %s slot%d addr=%#llx size=%llu rw%d", bb->binding, bb->kind == VKB_CBUF ? "cbuf" : "sbuf", bb->reg, (unsigned long long)u->addr, (unsigned long long)u->size, bb->rw);
                static int nfull = -1; if (nfull < 0) { const char *e = getenv("NXRT_VK_PASSDUMP_CBUF"); nfull = e ? atoi(e) : 0; }   // the first n floats (guest memory's view) of the dispatch's buffers, 4 to a group
                if (nfull > 0 && u->addr && u->size >= 16) { char t[1400]; int o = 0; uint64_t nf = u->size / 4 < (uint64_t)nfull ? u->size / 4 : (uint64_t)nfull;
                    for (uint64_t k = 0; k < nf && o < (int)sizeof t - 40; k++) { float v; memcpy(&v, (const uint8_t *)(uintptr_t)u->addr + 4 * k, 4); o += snprintf(t + o, sizeof t - o, "%s%g", k % 4 ? "," : " |", v); }
                    PD("    full%s", t); } }
            else if (bb->kind == VKB_TEX || bb->kind == VKB_IMG || bb->kind == VKB_TBUF) { uint64_t h = bb->kind == VKB_IMG ? S->img[5][bb->reg] : tex_slot_h(5, (unsigned)bb->reg); vkt_reg r; int tid = (int)(h & 0xfffff); int ok = tid > 0 && vkt_reg_copy(tid, &r);
                PD("  b%u %s slot%d tid=%d ser%llu fmt%#x %ux%u tgt%d", bb->binding, bb->kind == VKB_TEX ? "tex" : bb->kind == VKB_IMG ? "img" : "tbuf", bb->reg, tid, ok ? (unsigned long long)r.t.serial : 0ull, ok ? r.t.d.format : 0, ok ? r.t.d.width : 0, ok ? r.t.d.height : 0, ok ? r.t.d.target : 0); }
            else if (bb->kind == VKB_DRV) PD("  b%u drv c%d read %d (s2 %d ssbo %u tex %u n %u)", bb->binding, bb->bank, cs->drv_read[bb->bank], cs->s2cs, cs->s2_ssbo_base, cs->s2_tex_base, cs->s2_ntex);
        }
    }
    {   // NXRT_VK_ZERO_BEFORE=<compute key>:<hex address>:<bytes> (an experiment): the device copy of that range is cleared right before each dispatch of that program, whatever the CPU did to guest memory
        static char zk[20]; static uint64_t za, zn; static int zon = -1;
        if (zon < 0) { zon = 0; const char *e = getenv("NXRT_VK_ZERO_BEFORE"); if (e && sscanf(e, "%16[0-9a-fA-F]:%llx:%llu", zk, (unsigned long long *)&za, (unsigned long long *)&zn) == 3 && zn) zon = 1; }
        VkBuffer zb; VkDeviceSize zo; if (zon && !strcmp(cs->key, zk) && vkm_addr(za, zn, &zb, &zo)) { barrier_all(cb); vkCmdFillBuffer(cb, zb, zo, zn, 0); barrier_all(cb); }
    }
    vk_checkpoint(cb, 'c', cs->key, g_dispatch_dim[0], g_dispatch_dim[1], g_dispatch_dim[2], dp->ind != 0);
    if (dp->ind) { vkCmdDispatchIndirect(cb, ind_buf, ind_off); }
    else if (vkm_ind_direct(cs->key, dp->x, dp->y, dp->z), dp->x > 0 && dp->y > 0 && dp->z > 0) { vkCmdDispatch(cb, (uint32_t)dp->x, (uint32_t)dp->y, (uint32_t)dp->z); }
    g_st.dispatches++;
    if (vkm_ind_is_wanted(cs->key) && g_gm_np) { barrier_all(cb); for (int i = 0; i < g_gm_np; i++) vkm_snap(cb, cs->key, "post", g_gm_ptr[i]); }
    if (g_nbnc) {   // written bounced storage buffers back into their pools, before the write-back to guest memory below reads them
        barrier_all(cb);
        for (int i = 0; i < g_nbnc; i++) { VkBufferCopy c = {g_bnc[i].boff, g_bnc[i].poff, g_bnc[i].n}; vkCmdCopyBuffer(cb, R0.bounce, g_bnc[i].pool, 1, &c); }
        g_nbnc = 0;
        if (!g_nwb) barrier_all(cb);
    }
    if (g_nwb) { barrier_all(cb); wb_flush(); }
    if (g_pd) {   // NXRT_VK_WATCH=<hex address>[,<bytes>]: in a pass-dump frame, what the mirror holds there after each dispatch (waits for the GPU; logged when it changed)
        static uint64_t wa; static uint32_t wn; static int wi; if (!wi) { wi = 1; const char *e = getenv("NXRT_VK_WATCH"); if (e) { char *end; wa = strtoull(e, &end, 16); wn = *end == ',' ? (uint32_t)strtoul(end + 1, NULL, 0) : 192; if (wn > 1024) wn = 1024; } }
        static uint8_t prev[1024]; static uint64_t pf;
        if (wa) { vks_finish(); uint8_t cur[1024]; if (!vkm_peek(wa, cur, wn)) { if (pf != g_pd_frame) { pf = g_pd_frame; memset(prev, 0, sizeof prev); }
                char t[1500]; int o = 0, nd = 0; for (uint32_t k = 0; k < wn / 4; k++) { uint32_t x, y; memcpy(&x, cur + 4 * k, 4); memcpy(&y, prev + 4 * k, 4); if (x != y) { float f; memcpy(&f, &x, 4); if (o < (int)sizeof t - 40) o += snprintf(t + o, sizeof t - o, " w%u=%g", k, f); nd++; } }
                if (nd) PD("watch %#llx after dispatch %s %ux%ux%u: %d words changed:%s", (unsigned long long)wa, cs->key, g_dispatch_dim[0], g_dispatch_dim[1], g_dispatch_dim[2], nd, t);
                memcpy(prev, cur, wn); } }
        static int wind = -1; if (wind < 0) { const char *e = getenv("NXRT_VK_WATCH_IND"); wind = e && atoi(e); }
        if (wind && g_ind_nseen) {   // NXRT_VK_WATCH_IND=1: after each dispatch of a pass-dump frame, the 12 argument bytes of every indirect dispatch seen so far (logged when they changed)
            static uint32_t pv[64][3]; static uint64_t pfr = ~0ull; vks_finish();
            if (pfr != g_pd_frame) { pfr = g_pd_frame; memset(pv, 0xff, sizeof pv); }
            for (int i = 0; i < g_ind_nseen; i++) { uint32_t cur[3]; if (vkm_peek(g_ind_seen[i], cur, 12)) continue;
                if (memcmp(cur, pv[i], 12)) { PD("watch-ind %#llx after dispatch %s: %x %x %x (global-memory table: %s)", (unsigned long long)g_ind_seen[i], cs->key, cur[0], cur[1], cur[2], vkm_gmem_lookup(g_ind_seen[i]) ? "mapped" : "NOT MAPPED"); memcpy(pv[i], cur, 12); } }
        }
    }
}

// ---------- Buffer operations in GPU order (the null backend's and track B's effects, now inside the replay's ordering) ----------
// NXRT_VK_COPYLOG=1: every distinct buffer copy (source, destination, size) logged once with the frame, counted; the table is listed every 600 presents
static struct { uint64_t src, dst, n, cnt, first; } g_cl[128]; static int g_ncl; static uint64_t g_cl_frame;
static void copylog_note(uint64_t src, uint64_t dst, uint64_t n) {
    static int on = -1; if (on < 0) { const char *e = getenv("NXRT_VK_COPYLOG"); on = e && atoi(e); } if (!on) return;
    for (int i = 0; i < g_ncl; i++) if (g_cl[i].src == src && g_cl[i].dst == dst && g_cl[i].n == n) { g_cl[i].cnt++; return; }
    if (g_ncl < 128) { g_cl[g_ncl].src = src; g_cl[g_ncl].dst = dst; g_cl[g_ncl].n = n; g_cl[g_ncl].cnt = 1; g_cl[g_ncl].first = g_cl_frame; g_ncl++;
        VKLOG("copylog: NEW buffer copy %#llx -> %#llx, %llu bytes (frame %llu)", (unsigned long long)src, (unsigned long long)dst, (unsigned long long)n, (unsigned long long)g_cl_frame); }
}
static void copylog_report(uint64_t frame) {
    g_cl_frame = frame; if (!g_ncl || frame % 600) return;
    for (int i = 0; i < g_ncl; i++) VKLOG("copylog: frame %llu: %#llx -> %#llx %llu B x%llu (first frame %llu)", (unsigned long long)frame, (unsigned long long)g_cl[i].src, (unsigned long long)g_cl[i].dst, (unsigned long long)g_cl[i].n, (unsigned long long)g_cl[i].cnt, (unsigned long long)g_cl[i].first);
}
void vkr_copy(uint64_t src, uint64_t dst, uint64_t n) {
    g_st.copies++; copylog_note(src, dst, n);
    VkBuffer sb, db; VkDeviceSize so, dof;
    VkCommandBuffer cb = vks_cb();
    if (!cb || !n || !vkm_use(src, n, &sb, &so) || !vkm_use(dst, n, &db, &dof)) {   // not two pool ranges: on the CPU, as the null backend does it
        if (vkm_addr(src, n, NULL, NULL) && vkm_addr(dst, n, NULL, NULL)) { dm_guest_write_prepare((void *)(uintptr_t)dst, (size_t)n); memmove((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, (size_t)n); }
        return;
    }
    pass_end(); barrier_all(cb);
    VkBufferCopy c = {so, dof, n};
    vkCmdCopyBuffer(cb, sb, db, 1, &c);
    vkm_gpu_wrote(dst, n);
}
void vkr_fill(uint64_t dst, uint64_t n, uint32_t value) {
    g_st.fills++;
    VkBuffer db; VkDeviceSize dof; VkCommandBuffer cb = vks_cb();
    if (!n) return;
    if (!cb || (dst & 3) || (n & 3) || !vkm_use(dst, n, &db, &dof)) {
        if (!vkm_addr(dst, n, NULL, NULL)) return;
        dm_guest_write_prepare((void *)(uintptr_t)dst, (size_t)n);
        uint8_t *p = (uint8_t *)(uintptr_t)dst; for (uint64_t i = 0; i < n; i++) p[i] = (uint8_t)(value >> 8 * (i & 3));
        return;
    }
    pass_end(); barrier_all(cb);
    vkCmdFillBuffer(cb, db, dof, n, value);
    vkm_cpu_also(dst, n, NULL, 1, value);
}
void vkr_update(uint64_t d, uint64_t n, const uint8_t *data) {
    g_st.updates++;
    VkBuffer db; VkDeviceSize dof; VkCommandBuffer cb = vks_cb();
    if (!n) return;
    if (!cb || !vkm_use(d, n, &db, &dof)) {
        if (vkm_addr(d, n, NULL, NULL)) { dm_guest_write_prepare((void *)(uintptr_t)d, (size_t)n); memcpy((void *)(uintptr_t)d, data, (size_t)n); }
        return;
    }
    pass_end(); barrier_all(cb);
    if (!(dof & 3) && !(n & 3) && n <= 65536) vkCmdUpdateBuffer(cb, db, dof, n, data);   // inline in the command buffer
    else { VkBuffer sb; VkDeviceSize so; uint8_t *p = vkm_staging(n, 16, &sb, &so); if (p) { memcpy(p, data, (size_t)n); VkBufferCopy c = {so, dof, n}; vkCmdCopyBuffer(cb, sb, db, 1, &c); } }
    vkm_cpu_also(d, n, data, 0, 0);
}

// ---------- Texture copies and clears (nvn_mtl.m copy_b2t, copy_t2b, copy_t2t, clear_tex) ----------
static const tex_t *snap(const tex_t *t) { return t && t->magic == MAGIC('T', 'E', 'X', 0) && t->serial ? t : NULL; }
// A transfer on an image: GENERAL images stay so; a sampled-only image goes to the transfer layout and back to its resting one
static VkImageLayout xfer_in(VkCommandBuffer cb, vkt_image *im, int dst) {
    if (im->rest == VK_IMAGE_LAYOUT_GENERAL) return VK_IMAGE_LAYOUT_GENERAL;
    VkImageLayout l = dst ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vk_image_barrier(cb, im->img, im->aspect, im->rest, l, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                     dst ? VK_ACCESS_2_TRANSFER_WRITE_BIT : VK_ACCESS_2_TRANSFER_READ_BIT);
    return l;
}
static void xfer_out(VkCommandBuffer cb, vkt_image *im, VkImageLayout l) {
    if (l == im->rest) return;
    vk_image_barrier(cb, im->img, im->aspect, l, im->rest, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
}
static VkImageAspectFlags copy_aspect(vkt_image *im) { return im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : im->aspect; }
// The buffer side of a copy: SetCopyRowStride / SetCopyImageStride, else tightly packed (nvn_mtl.m copy_b2t)
static int copy_layout(const vkt_image *im, const NVNcopyRegion *r, size_t *row, size_t *img, uint32_t *row_texels, uint32_t *img_rows) {
    const vkf_info *fi = im->fi;
    size_t rows = (size_t)(r->height + fi->bh - 1) / fi->bh;
    *row = S->copy_stride ? (size_t)S->copy_stride : (size_t)((r->width + fi->bw - 1) / fi->bw) * fi->bpb;
    *img = S->copy_img_stride ? (size_t)S->copy_img_stride : *row * rows;
    if (*row % fi->bpb || (*row && *img % *row)) return 0;
    *row_texels = (uint32_t)(*row / fi->bpb * fi->bw); *img_rows = (uint32_t)(*row ? *img / *row * fi->bh : 0);
    return 1;
}
static void copy_b2t(const p_copy_b2t *c) {
    const tex_t *t = snap(c->tex); if (!t) return;
    PD("copy buffer %#llx -> ser%llu fmt%#x region %d,%d,%d %dx%dx%d", (unsigned long long)c->src, (unsigned long long)t->serial, t->d.format, c->r.xoffset, c->r.yoffset, c->r.zoffset, c->r.width, c->r.height, c->r.depth);
    vkt_image *im = vkt_resolve(t); if (!im) return;
    g_st.tex_copies++;
    if (im->fi->conv != VKF_COPY) { RED_ONCE(80, "buffer→texture copy into a format the device stores converted (%s): not copied", im->fi->name); return; }
    unsigned level = c->has_view && c->view.set_levels ? c->view.base_level : 0, layer0 = c->has_view && c->view.set_layers ? c->view.min_layer : 0;
    if (level >= im->levels) return;
    size_t row, img; uint32_t rt, ir;
    if (!copy_layout(im, &c->r, &row, &img, &rt, &ir)) { RED_ONCE(81, "buffer→texture copy with strides Vulkan cannot express"); return; }
    int is3d = t->d.target == NVN_TEXTURE_TARGET_3D, nz = c->r.depth > 0 ? c->r.depth : 1;
    VkBuffer b; VkDeviceSize o;
    if (!vkm_use(c->src, img * nz, &b, &o)) { RED_ONCE(82, "buffer→texture copy source is in no memory pool"); return; }
    // a storage-backed texture gets the bytes in its storage too (hle_nvn.c nvn_tex_mirror_b2t), or the next upload from there would drop them
    if (im->cls == VKT_STORAGE && !is3d && nvn_tex_mirror_b2t(t, level, layer0, &c->r, (const uint8_t *)(uintptr_t)c->src, row, img)) RED_ONCE(83, "a buffer→texture copy into a storage-backed texture could not be written into its storage");
    pass_end();
    VkCommandBuffer cb = vks_cb(); barrier_all(cb);
    VkImageLayout l = xfer_in(cb, im, 1);
    VkBufferImageCopy rg[64]; int n = 0;
    for (int z = 0; z < (is3d ? 1 : nz) && n < 64; z++)
        rg[n++] = (VkBufferImageCopy){o + (VkDeviceSize)z * img, rt, ir, {copy_aspect(im), level, is3d ? 0 : layer0 + c->r.zoffset + z, 1},
                                      {c->r.xoffset, c->r.yoffset, is3d ? c->r.zoffset : 0}, {(uint32_t)c->r.width, (uint32_t)c->r.height, is3d ? (uint32_t)nz : 1}};
    vkCmdCopyBufferToImage(cb, b, im->img, l, (uint32_t)n, rg);
    xfer_out(cb, im, l);
    im->content = 1;
}
static void copy_t2b(const p_copy_b2t *c) {
    const tex_t *t = snap(c->tex); if (!t) return;
    vkt_image *im = vkt_resolve(t); if (!im) return;
    g_st.tex_copies++;
    if (im->fi->conv != VKF_COPY) { RED_ONCE(84, "texture→buffer copy from a format the device stores converted (%s): not copied", im->fi->name); return; }
    unsigned level = c->has_view && c->view.set_levels ? c->view.base_level : 0, layer0 = c->has_view && c->view.set_layers ? c->view.min_layer : 0;
    if (level >= im->levels) return;
    size_t row, img; uint32_t rt, ir;
    if (!copy_layout(im, &c->r, &row, &img, &rt, &ir)) { RED_ONCE(85, "texture→buffer copy with strides Vulkan cannot express"); return; }
    int is3d = t->d.target == NVN_TEXTURE_TARGET_3D, nz = c->r.depth > 0 ? c->r.depth : 1;
    VkBuffer b; VkDeviceSize o;
    if (!vkm_use(c->src, img * nz, &b, &o)) { RED_ONCE(86, "texture→buffer copy destination is in no memory pool"); return; }
    pass_end();
    VkCommandBuffer cb = vks_cb(); barrier_all(cb);
    VkImageLayout l = xfer_in(cb, im, 0);
    VkBufferImageCopy rg[64]; int n = 0;
    for (int z = 0; z < (is3d ? 1 : nz) && n < 64; z++)
        rg[n++] = (VkBufferImageCopy){o + (VkDeviceSize)z * img, rt, ir, {copy_aspect(im), level, is3d ? 0 : layer0 + c->r.zoffset + z, 1},
                                      {c->r.xoffset, c->r.yoffset, is3d ? c->r.zoffset : 0}, {(uint32_t)c->r.width, (uint32_t)c->r.height, is3d ? (uint32_t)nz : 1}};
    vkCmdCopyImageToBuffer(cb, im->img, l, b, (uint32_t)n, rg);
    xfer_out(cb, im, l);
    vkm_gpu_wrote(c->src, img * nz);   // NVN pools are coherent with the CPU: the bytes reach guest memory when the submission completes
}
// M6 phase B: a GPU copy into a texture whose content comes from its storage (VKT_STORAGE), from a source whose content is the GPU's: the destination region is read back
// after the copy into the submission's arena and written into the storage when the submission completes (nvn_mtl.m mirror_readback / mirror_rb_land), so the next upload
// from there keeps the copy. The texture record is copied (the game may finalize it once it has waited)
typedef struct { tex_t t; unsigned level, layer0; NVNcopyRegion r; size_t row, img; const uint8_t *buf; } mirror_rb;
static void mirror_land(void *p) {
    mirror_rb *m = p;
    if (nvn_tex_mirror_b2t(&m->t, m->level, m->layer0, &m->r, m->buf, m->row, m->img)) RED_ONCE(96, "a GPU copy read back into the storage of texture ser=%llu could not be written there", (unsigned long long)m->t.serial);
    else { static int n; if (!n++) VKLOG("a GPU copy into a storage-backed texture (ser=%llu %dx%d): read back into its storage", (unsigned long long)m->t.serial, m->r.width, m->r.height); }
    free(m);
}
static void mirror_readback(VkCommandBuffer cb, vkt_image *im, const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r) {
    const vkf_info *fi = im->fi; int nz = r->depth > 0 ? r->depth : 1;
    if (fi->conv != VKF_COPY || im->samples > 1 || (im->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) || t->d.target == NVN_TEXTURE_TARGET_3D || r->width <= 0 || r->height <= 0) {
        RED_ONCE(95, "a GPU copy into a storage-backed texture (format %s) cannot be read back into its storage: the next upload from there drops it", fi->name); return; }
    size_t row = (size_t)((r->width + fi->bw - 1) / fi->bw) * fi->bpb, img = row * (size_t)((r->height + fi->bh - 1) / fi->bh);
    VkBuffer b; VkDeviceSize o; uint8_t *p = arena(img * nz, 16, &b, &o);
    if (!p) return;
    barrier_all(cb);   // the copy's writes first
    VkImageLayout l = xfer_in(cb, im, 0);
    VkBufferImageCopy rg[64]; int n = 0;
    for (int z = 0; z < nz && n < 64; z++)
        rg[n++] = (VkBufferImageCopy){o + (VkDeviceSize)z * img, 0, 0, {copy_aspect(im), level, layer0 + r->zoffset + z, 1}, {r->xoffset, r->yoffset, 0}, {(uint32_t)r->width, (uint32_t)r->height, 1}};
    vkCmdCopyImageToBuffer(cb, im->img, l, b, (uint32_t)n, rg);
    xfer_out(cb, im, l);
    vk_mem_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    mirror_rb *m = malloc(sizeof *m); *m = (mirror_rb){*t, level, layer0, *r, row, img, p};
    vks_post_fn(mirror_land, m);   // before the arena chunk is returned (subres_done is posted at the end of the submission)
    g_st.tex_copies++;
}
static void copy_t2t(const p_copy_t2t *c) {
    const tex_t *st = snap(c->src), *dt = snap(c->dst); if (!st || !dt) return;
    PD("copy ser%llu %dx%d -> ser%llu %dx%d flags %#x (src view %d layer %d level %d, dst view %d layer %d level %d, z %d -> %d)", (unsigned long long)st->serial, c->sr.width, c->sr.height, (unsigned long long)dt->serial, c->dr.width, c->dr.height, c->flags,
       c->has_sv, c->has_sv && c->sv.set_layers ? c->sv.min_layer : -1, c->has_sv && c->sv.set_levels ? c->sv.base_level : -1, c->has_dv, c->has_dv && c->dv.set_layers ? c->dv.min_layer : -1, c->has_dv && c->dv.set_levels ? c->dv.base_level : -1, c->sr.zoffset, c->dr.zoffset);
    vkt_image *s = vkt_resolve(st), *d = vkt_resolve(dt); if (!s || !d) return;
    g_st.tex_copies++;
    unsigned sl = c->has_sv && c->sv.set_levels ? c->sv.base_level : 0, dl = c->has_dv && c->dv.set_levels ? c->dv.base_level : 0;
    unsigned sz0 = c->has_sv && c->sv.set_layers ? c->sv.min_layer : 0, dz0 = c->has_dv && c->dv.set_layers ? c->dv.min_layer : 0;
    if (sl >= s->levels || dl >= d->levels) return;
    int s3 = st->d.target == NVN_TEXTURE_TARGET_3D, d3 = dt->d.target == NVN_TEXTURE_TARGET_3D, nz = c->sr.depth > 0 ? c->sr.depth : 1;
    int mirror = (c->flags & (NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT)) != 0, linear = (c->flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT) != 0;
    int same = c->sr.width == c->dr.width && c->sr.height == c->dr.height && !mirror && s->fi->vk == d->fi->vk, rb = 0;
    // the copy's bytes in the destination's storage when it is storage-backed (hle_nvn.c nvn_tex_mirror_t2t, from the source's storage when that holds its content)
    if (d->cls == VKT_STORAGE && !d3 && same && s->cls == VKT_STORAGE && !s3 && nvn_tex_mirror_t2t(st, sl, sz0, &c->sr, dt, dl, dz0, &c->dr))
        RED_ONCE(90, "a texture copy into a storage-backed texture could not be written into its storage");
    else if (d->cls == VKT_STORAGE) rb = 1;   // the source's content is the GPU's: read back after the copy (nvn_mtl.m mirror_readback)
    pass_end();
    VkCommandBuffer cb = vks_cb(); barrier_all(cb);
    VkImageLayout ls = xfer_in(cb, s, 0), ld = s == d ? ls : xfer_in(cb, d, 1);
    if (s == d && ls != VK_IMAGE_LAYOUT_GENERAL) { RED_ONCE(92, "a texture copied into itself outside the GENERAL layout: not copied"); xfer_out(cb, s, ls); return; }
    VkImageAspectFlags sa = copy_aspect(s), da = copy_aspect(d);
    if (da & VK_IMAGE_ASPECT_DEPTH_BIT) note_depth_copy(dt->serial);
    if ((c->flags & DM_COPY_RESOLVE) && s->samples > 1 && d->samples == 1 && s->fi->vk == d->fi->vk && !(s->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
        VkImageResolve r = {{sa, sl, sz0, 1}, {0, 0, 0}, {da, dl, dz0, 1}, {0, 0, 0}, {mipd(s->w, sl), mipd(s->h, sl), 1}};
        vkCmdResolveImage(cb, s->img, ls, d->img, ld, 1, &r);
    } else if (same && s->samples == d->samples) {
        VkImageCopy r[64]; int n = 0;
        if (s3 || d3) r[n++] = (VkImageCopy){{sa, sl, s3 ? 0 : sz0 + c->sr.zoffset, s3 ? 1 : (uint32_t)nz}, {c->sr.xoffset, c->sr.yoffset, s3 ? c->sr.zoffset : 0},
                                             {da, dl, d3 ? 0 : dz0 + c->dr.zoffset, d3 ? 1 : (uint32_t)nz}, {c->dr.xoffset, c->dr.yoffset, d3 ? c->dr.zoffset : 0}, {(uint32_t)c->sr.width, (uint32_t)c->sr.height, s3 && d3 ? (uint32_t)nz : 1}};
        else r[n++] = (VkImageCopy){{sa, sl, sz0 + c->sr.zoffset, (uint32_t)nz}, {c->sr.xoffset, c->sr.yoffset, 0}, {da, dl, dz0 + c->dr.zoffset, (uint32_t)nz}, {c->dr.xoffset, c->dr.yoffset, 0},
                                    {(uint32_t)c->sr.width, (uint32_t)c->sr.height, 1}};
        vkCmdCopyImage(cb, s->img, ls, d->img, ld, (uint32_t)n, r);
    } else if (!(s->fi->flags & VKF_COMPRESSED) && !(d->fi->flags & VKF_COMPRESSED) && s->samples == 1 && d->samples == 1 && !s3 && !d3 && (sa == da)) {
        // scaling, mirroring or a format change: a blit (nvn_mtl.m draws these: copy_t2t_draw)
        int x0 = c->dr.xoffset, y0 = c->dr.yoffset, x1 = c->dr.xoffset + c->dr.width, y1 = c->dr.yoffset + c->dr.height;
        if (c->flags & NVN_COPY_FLAGS_MIRROR_X_BIT) { int t = x0; x0 = x1; x1 = t; }
        if (c->flags & NVN_COPY_FLAGS_MIRROR_Y_BIT) { int t = y0; y0 = y1; y1 = t; }
        int depth = (sa & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0;
        int filt = linear && !depth && (fmt_features(s->fi->vk, 0) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
        VkImageBlit r[64]; int n = 0;
        for (int z = 0; z < nz && n < 64; z++)
            r[n++] = (VkImageBlit){{sa, sl, sz0 + c->sr.zoffset + z, 1}, {{c->sr.xoffset, c->sr.yoffset, 0}, {c->sr.xoffset + c->sr.width, c->sr.yoffset + c->sr.height, 1}},
                                   {da, dl, dz0 + c->dr.zoffset + z, 1}, {{x0, y0, 0}, {x1, y1, 1}}};
        if (depth && s->fi->vk != d->fi->vk) RED_ONCE(93, "a depth copy between different formats: not copied");
        else vkCmdBlitImage(cb, s->img, ls, d->img, ld, (uint32_t)n, r, filt ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    } else RED_ONCE(94, "texture→texture copy case not replayed: %dx%d→%dx%d flags %#x %s→%s samples %u→%u 3D %d→%d", c->sr.width, c->sr.height, c->dr.width, c->dr.height, c->flags,
                    s->fi->name, d->fi->name, (unsigned)s->samples, (unsigned)d->samples, s3, d3);
    if (s != d) xfer_out(cb, d, ld);
    xfer_out(cb, s, ls);
    d->content = 1;
    if (rb) mirror_readback(cb, d, dt, dl, dz0, &c->dr);
}
static void clear_tex(const p_clear_tex *c) {
    const tex_t *t = snap(c->tex); if (!t) return;
    vkt_image *im = vkt_resolve(t); if (!im) return;
    if (im->fi->flags & VKF_COMPRESSED) { RED_ONCE(100, "a clear of a compressed texture: not cleared"); return; }
    unsigned lv = c->has_view && c->view.set_levels ? c->view.base_level : 0, l0 = c->has_view && c->view.set_layers ? c->view.min_layer : 0;
    if (lv >= im->levels) return;
    uint32_t w = mipd(im->w, lv), h = mipd(im->h, lv);
    int is3d = im->vtype == VK_IMAGE_VIEW_TYPE_3D, nz = c->r.depth > 0 ? c->r.depth : 1;
    VkFormat vf = vkf_view_format(t->d.format, c->has_view && c->view.set_format ? c->view.format : t->d.format);
    int depth = (im->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0;
    unsigned ch = depth ? 0xf : fmt_channels(vf);
    if ((c->mask & ch) != ch && !depth) RED_ONCE(101, "texture clear with channel mask %#x: cleared whole (a partial mask needs a clear draw)", c->mask);
    int whole = !c->r.xoffset && !c->r.yoffset && (uint32_t)c->r.width >= w && (uint32_t)c->r.height >= h;
    g_st.clears++;
    if (!whole && (im->usage & (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) && !is3d) {   // a region: a pass with that render area
        VkImageView v = vkt_view_raw_(im, depth ? im->fi->vk : vf, VK_IMAGE_VIEW_TYPE_2D_ARRAY, lv, 1, l0 + c->r.zoffset, (uint32_t)nz, depth ? im->aspect : VK_IMAGE_ASPECT_COLOR_BIT, NULL);
        VkRect2D r = {{c->r.xoffset, c->r.yoffset}, {(uint32_t)c->r.width, (uint32_t)c->r.height}};
        VkClearValue val; if (depth) { val.depthStencil.depth = c->c.f[0]; val.depthStencil.stencil = c->c.u[1]; } else val = clear_color_value(vf, c->c.f, c->c.u, c->is_uint);
        if (v) clear_pass(v, depth ? im->fi->vk : vf, depth, 1, 1, r, (uint32_t)nz, val);
        im->content = 1;
        return;
    }
    if (!whole) RED_ONCE(102, "a region clear of a texture that cannot be an attachment: the whole level is cleared");
    pass_end();
    VkCommandBuffer cb = vks_cb(); barrier_all(cb);
    VkImageLayout l = xfer_in(cb, im, 1);
    VkImageSubresourceRange rr = {im->aspect, lv, 1, is3d ? 0 : l0 + c->r.zoffset, is3d ? 1 : (uint32_t)nz};
    if (depth) { VkClearDepthStencilValue v = {c->c.f[0], c->c.u[1]}; vkCmdClearDepthStencilImage(cb, im->img, l, &v, 1, &rr); }
    else { VkClearValue v = clear_color_value(im->fi->vk, c->c.f, c->c.u, c->is_uint); vkCmdClearColorImage(cb, im->img, l, &v.color, 1, &rr); }
    xfer_out(cb, im, l);
    im->content = 1;
}

// ---------- Counters ----------
// Report records are written to guest memory by the completion thread, after the GPU has finished the submission that holds them. A conditional render
// (OP_RENDER_COND) reads those records while the next submission is replayed, which can happen before the previous one has completed: it then read the
// previous "visible" value (Eglantine beaf6932: the OoT predicate macro read a stale record, fog was drawn over Link and parts flickered). The replay notes
// where each recorded report will land and the timeline value that lands it; render_cond waits for that value before it reads
enum { NREP = 64 };
static struct { uint64_t addr, value; } g_rep[NREP]; static unsigned g_rep_n;
static void rep_note(uint64_t addr) { uint64_t v = vks_recording_value(); if (!v) return; g_rep[g_rep_n].addr = addr; g_rep[g_rep_n].value = v; g_rep_n = (g_rep_n + 1) % NREP; }
// The newest recorded report whose 16 bytes overlap [addr, addr + 16): the value its write lands with (0 = none)
static uint64_t rep_pending(uint64_t addr) {
    uint64_t best = 0;
    for (unsigned i = 0; i < NREP; i++) if (g_rep[i].value && g_rep[i].addr + 16 > addr && addr + 16 > g_rep[i].addr && g_rep[i].value > best) best = g_rep[i].value;
    return best;
}
// Makes the reports recorded at addr and addr + 16 visible in guest memory: flushes the queue and waits for the submissions that hold them. A report of the
// submission being replayed cannot land before it is committed; that case is logged once and the record is read as it stands
static void rep_make_visible(uint64_t addr) {
    uint64_t need = rep_pending(addr), n2 = rep_pending(addr + 16); if (n2 > need) need = n2;
    if (!need || need <= atomic_load(&g_vk.tl_done)) return;
    if (need >= vks_recording_value()) { RED_ONCE(60, "a conditional render reads a counter reported earlier in the same submission (%#llx): it reads the record as the last completed submission left it", (unsigned long long)addr); return; }
    g_st.rep_waits++;
    if (!vks_wait(need, 2000000000ull)) VKRED("a conditional render waited 2 s for the counter reports at %#llx (submission %llu): the records are read as they stand", (unsigned long long)addr, (unsigned long long)need);
}
static void report_counter(uint64_t type, uint64_t addr) {
    rep_note(addr);
    if (type == NVN_COUNTER_TYPE_SAMPLES_PASSED) {
        if (P.query >= 0) { vkCmdEndQuery(vks_cb(), R0.qp, (uint32_t)P.query); q_post(0, (uint32_t)P.query, 0); P.query = -1; }
        q_post(2, 0, addr); return;
    }
    if (type != NVN_COUNTER_TYPE_TIMESTAMP && type != NVN_COUNTER_TYPE_TIMESTAMP_TOP) { static uint32_t seen; if (!(seen >> (type & 31) & 1)) { seen |= 1u << (type & 31); VKRED("counter type %llu is reported as 0 (only timestamps and samples passed are counted)", (unsigned long long)type); } }
    vks_post_counter(addr, (uint32_t)type);
}
static void reset_counter(uint64_t type) {
    if (type != NVN_COUNTER_TYPE_SAMPLES_PASSED) return;
    if (P.query >= 0) { vkCmdEndQuery(vks_cb(), R0.qp, (uint32_t)P.query); q_post(0, (uint32_t)P.query, 0); P.query = -1; }
    q_post(1, 0, 0); S->vis_on = 1;
}

// ---------- State records (nvn_mtl.m state_op) ----------
static void bind_handles_fresh(const p_bind_handles *b) {   // BindTexturesDeferred: the handles at src as they are now (nvn_mtl.m bh_fresh)
    if (b->defer && b->src && b->n > 0 && b->n <= 32) memcpy((void *)b->h, (const void *)(uintptr_t)b->src, 8 * (size_t)b->n);
}
static void state_op(uint32_t op, const void *a) {
    switch (op) {
    case OP_SCISSOR: S->sc = *(const p_scissor *)a; S->has_sc = 1; break;
    case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: { int tex = op == OP_SET_TEX_POOL, b = nvn_dpool_base(((const p_set_pool *)a)->pool, tex); if (tex) S->tex_base = b; else S->samp_base = b; break; }
    case OP_BIND_PROGRAM: { const p_bind_program *bp = a;
        if (bp->stages & 0x20) S->cprog = bp->prog;
        for (int st = 0; st < 5; st++) if (bp->stages >> st & 1) S->bst[st] = bp->prog;
        break; }
    case OP_BIND_STATE: {
        const p_bind_state *b = a; blend_st bs;
        switch (b->kind) {
        case 0: memcpy(&bs, b->bytes, sizeof bs); if (bs.target < 8) S->blend[bs.target] = bs; break;
        case 1: memcpy(&S->color, b->bytes, sizeof S->color); break;
        case 2: memcpy(&S->chmask, b->bytes, sizeof S->chmask); break;
        case 3: memcpy(&S->ms, b->bytes, sizeof S->ms); break;
        case 4: memcpy(&S->poly, b->bytes, sizeof S->poly); break;
        case 5: memcpy(&S->ds, b->bytes, sizeof S->ds); break;
        }
        break; }
    case OP_BIND_VATTR: { const p_bind_vattr *b = a; if (b->n >= 0 && b->n <= 16) { memcpy(S->attr, b->st, 4 * (size_t)b->n); S->nattr = b->n; } break; }
    case OP_BIND_VSTREAM: { const p_bind_vstream *b = a; if (b->n >= 0 && b->n <= 16) { memcpy(S->stream, b->st, 8 * (size_t)b->n); S->nstream = b->n; } break; }
    case OP_BIND_VBUF: case OP_BIND_VBUF1: { p_bind_vbuf tmp; const p_bind_vbuf *b = vbuf_rec(op, a, &tmp); if (b->first >= 0 && b->first + b->n <= 16) memcpy(&S->vbuf[b->first], b->r, sizeof *b->r * (size_t)b->n); break; }
    case OP_BIND_UBO: case OP_BIND_UBO_SMALL: { p_bind_buf tmp; const p_bind_buf *b = bind_buf_rec(op, a, &tmp); if (b->stage >= 0 && b->stage < 6 && b->index >= 0 && b->index < 16) { S->ubo[b->stage][b->index] = *b; if (g_pd && b->stage == 5) PD("bind ubo st5 idx%d addr=%#llx size=%llu", b->index, (unsigned long long)b->addr, (unsigned long long)b->size); } break; }
    case OP_BIND_SSBO: { const p_bind_buf *b = a; if (b->stage >= 0 && b->stage < 6 && b->index >= 0 && b->index < 16) { S->ssbo[b->stage][b->index] = *b; if (g_pd && b->stage == 5) PD("bind ssbo st5 idx%d addr=%#llx size=%llu", b->index, (unsigned long long)b->addr, (unsigned long long)b->size); } else { RED_ONCE(47, "a storage buffer bound at slot %d of stage %d: the replay keeps 16 slots", b->index, b->stage);
        static int nlost; if (b->stage == 5 && nlost++ < 60) VKLOG("storage buffer bind LOST (slot %d of stage %d, addr %#llx size %llu): the replay keeps 16 slots (%d so far)", b->index, b->stage, (unsigned long long)b->addr, (unsigned long long)b->size, nlost); } break; }
    case OP_BIND_TEX: case OP_BIND_TEX1: { p_bind_handles t1; const p_bind_handles *b = bind_rec(op, a, &t1);
        if (b->n < 0 || b->n > 32) { RED_ONCE(110, "a bind record holding count %d: overwritten after it was recorded", b->n); break; }
        bind_handles_fresh(b);
        if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= NTEX) memcpy(&S->tex[b->stage][b->first], b->h, 8 * (size_t)b->n);
        break; }
    case OP_BIND_SAMP: { const p_bind_handles *b = a; bind_handles_fresh(b); if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= 128) memcpy(&S->samp[b->stage][b->first], b->h, 8 * (size_t)b->n); break; }
    case OP_BIND_IMG: { const p_bind_handles *b = a; bind_handles_fresh(b); if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= 32) memcpy(&S->img[b->stage][b->first], b->h, 8 * (size_t)b->n); break; }
    case OP_STENCIL: { const p_stencil *b = a; for (int f = 0; f < 2; f++) if (b->face & (1 << f)) { if (b->which == 0) S->st_vmask[f] = (uint32_t)b->value_mask; else if (b->which == 1) S->st_wmask[f] = (uint32_t)b->mask; else S->st_ref[f] = (uint32_t)b->ref; } break; }
    case OP_POLY_OFFSET: S->po = *(const p_poly_offset *)a; break;
    case OP_VIEWPORTS: { const p_viewports *b = a; for (int i = 0; i < b->n && b->first + i < 16; i++) memcpy(S->vp[b->first + i], b->v[i], 16); S->has_vp = 1; break; }
    case OP_DEPTH_RANGE: { const p_depth_range *b = a; S->dr[0] = b->n; S->dr[1] = b->f; break; }
    case OP_DEPTH_CLAMP: S->depth_clamp = ((const p_depth_clamp *)a)->on; break;
    case OP_VP_SWIZZLE: { const p_vp_swizzle *b = a; if (b->first >= 0 && b->first + b->n <= 16) memcpy(S->vp_sw[b->first], b->sw, 4 * (size_t)b->n); break; }
    case OP_BLEND_COLOR: memcpy(S->blend_color, ((const p_blend_color *)a)->rgba, 16); break;
    case OP_ALPHA_REF: S->alpha_ref = ((const p_alpha_ref *)a)->ref; break;
    case OP_COPY_STRIDE: { const p_copy_stride *cs = a; if (cs->image) S->copy_img_stride = cs->stride; else S->copy_stride = cs->stride; break; }
    case OP_TF_STATE: { const p_tf_state *b = a; S->tf_on = b->what == 0 || b->what == 3; if (S->tf_on) RED_ONCE(111, "transform feedback is not captured yet"); break; }
    case OP_PRIM_RESTART: S->restart = *(const p_prim_restart *)a; break;
    case OP_RAST_DISCARD: S->rast_discard = ((const p_rast_discard *)a)->on; break;
    default: break;
    }
}
static void set_rt(const p_set_rt *r) {
    pass_end(); P.valid = 0;
    S->nrt = r->n > 8 ? 8 : r->n < 0 ? 0 : r->n;
    memset(S->has_ct, 0, sizeof S->has_ct);
    for (int i = 0; i < S->nrt; i++) {
        const tex_t *t = snap(r->color[i]);
        if (t) { S->ct[i] = *t; S->has_ct[i] = 1; }
        S->cview[i] = r->cview[i]; S->has_cview[i] = r->has_cview[i];
    }
    const tex_t *d = snap(r->depth);
    S->has_dt = d != NULL; if (d) S->dt = *d;
    S->dview = r->dview; S->has_dview = r->has_dview;
}
static void render_cond(const p_render_cond *rc) {   // nvn_mtl.m OP_RENDER_COND: the counters as guest memory holds them at replay
    if (rc->mode >= 2) { S->render_skip = rc->mode == 3; return; }
    rep_make_visible(rc->addr);
    uint64_t x = 0, y = 0; uint32_t lo = 0, hi = 0; int known = 1;
    known &= read_u32(rc->addr, &lo) && read_u32(rc->addr + 4, &hi); x = (uint64_t)hi << 32 | lo;
    known &= read_u32(rc->addr + 16, &lo) && read_u32(rc->addr + 20, &hi); y = (uint64_t)hi << 32 | lo;
    S->render_skip = known && (rc->mode ? x == y : x != y);
}

// ---------- The record walk (nvn_mtl.m mtl_exec; the framing checks of track B's walk) ----------
static void bad_record(const uint8_t *p, op_hdr h, const char *why) {
    if (g_st.bad++ < 8) VKRED("%s at %p (op %u, size %u): the rest of this command list is not replayed", why, (const void *)p, h.op, h.size);
}
static int walk(const uint8_t *p, int depth, uint64_t *budget) {
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        if (!(*budget)--) { bad_record(p, h, "more than 16M records in one list (a JUMP loop?)"); return -1; }
        if (h.op >= OP_COUNT_ || h.size < 8 || (h.size & 7) || (h.op != OP_END && h.size > (64u << 20))) { bad_record(p, h, "a malformed record"); return -1; }
        const uint8_t *a = p + 8;
        g_st.records++;
        switch (h.op) {
        case OP_END: return 0;
        case OP_BEGIN: break;
        case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); if (!t) { bad_record(p, h, "a JUMP to 0"); return -1; } p = t; continue; }
        case OP_CALL: { uint64_t t; memcpy(&t, a, 8); if (depth >= 8 || !t) { bad_record(p, h, depth >= 8 ? "CALLs nested deeper than 8" : "a CALL of handle 0"); return -1; }
            if (walk((const uint8_t *)(uintptr_t)t, depth + 1, budget)) return -1; break; }
        case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, a + 8, 4); memcpy(&v, a + 12, 4); if (val) vks_post_event(val, m == NVN_EVENT_SIGNAL_MODE_ADD ? 1 : 0, v); break; }
        case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, a + 8, 8); report_counter(t, ad); break; }
        case OP_RESET_COUNTER: { uint64_t t; memcpy(&t, a, 8); reset_counter(t); break; }
        case OP_FENCE_SYNC: { sync_t *s; memcpy(&s, a, 8); if (s) vk_note_fence_sync(s); break; }
        case OP_COPY_B2B: { uint64_t s_, d, n; memcpy(&s_, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8); vkr_copy(s_, d, n); break; }
        case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8);
            if (24 + n > h.size) { bad_record(p, h, "a uniform update longer than its record"); return -1; }
            vkr_update(d, n, a + 16); break; }
        case OP_CLEAR_BUF: { const p_clear_buf *cb = (const p_clear_buf *)a; vkr_fill(cb->addr, cb->size, cb->value); break; }
        case OP_SET_RT: set_rt((const p_set_rt *)a); break;
        case OP_CLEAR_COLOR: clear_color((const p_clear_color *)a); break;
        case OP_CLEAR_DS: clear_ds((const p_clear_ds *)a); break;
        case OP_DRAW: { p_draw t; draw(draw_rec(a, h.size, &t), NULL); break; }
        case OP_DRAW_IDX: { p_draw_idx t; draw(NULL, draw_idx_rec(a, h.size, &t)); break; }
        case OP_DISPATCH: dispatch((const p_dispatch *)a); break;
        case OP_BARRIER: pass_end(); g_st.barriers++; break;
        case OP_COPY_B2T: copy_b2t((const p_copy_b2t *)a); break;
        case OP_COPY_T2B: copy_t2b((const p_copy_b2t *)a); break;
        case OP_COPY_T2T: copy_t2t((const p_copy_t2t *)a); break;
        case OP_CLEAR_TEX: clear_tex((const p_clear_tex *)a); break;
        case OP_DRAW_TEX: RED_ONCE(120, "DrawTexture is not replayed yet"); break;
        case OP_RENDER_COND: render_cond((const p_render_cond *)a); break;
        case OP_TF_BIND: break;
        default: state_op(h.op, a); break;
        }
        p += h.size;
    }
}
void vkr_exec(const uint8_t *handle) {
    g_st.handles++;
    if (!handle) { op_hdr z = {0, 0}; bad_record(handle, z, "a null command handle"); return; }
    if (!S) S = rs_get(0);
    uint64_t budget = 1u << 24;
    walk(handle, 0, &budget);
}

// ---------- Submissions, queues ----------
static void guest_poke(void);
void vkr_sub_begin(void) {
    guest_poke();
    memset(&g_sub, 0, sizeof g_sub); BL.cur = NULL;
    P.valid = 0; P.open = 0; P.query = -1; g_last_pipe = VK_NULL_HANDLE; g_nwb = 0;
    if (!S) S = rs_get(0);
    // the slot's reset is recorded (VUID-vkResetQueryPool-firstQuery-02741: a host reset may meet the slot's last use still running while loading floods the queue)
    if (g_tqp) { uint32_t q = g_tq_next++ % NTQ; vkCmdResetQueryPool(vks_cb(), g_tqp, 2 * q, 2); vkCmdWriteTimestamp2(vks_cb(), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g_tqp, 2 * q); g_sub.tq = (int)q + 1; }
}
void vkr_sub_end(void) {
    pass_end();
    if (g_nwb) { barrier_all(vks_cb()); wb_flush(); }
    if (g_sub.tq) vkCmdWriteTimestamp2(vks_cb(), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g_tqp, 2 * (uint32_t)(g_sub.tq - 1) + 1);
    subres *r = malloc(sizeof *r); *r = g_sub; memset(&g_sub, 0, sizeof g_sub);
    vks_post_fn(subres_done, r);
    P.valid = 0;
}
void vkr_queue_activate(uint64_t serial) { S = rs_get(serial); P.valid = 0; }
void vkr_queue_forget(uint64_t serial) {
    for (rs **pp = &g_states; *pp; pp = &(*pp)->next) if ((*pp)->serial == serial) { rs *s = *pp; *pp = s->next; if (S == s) S = NULL; free(s); break; }
}
void vkr_set_origin_lower_left(int on) { if (g_origin_ll != on) VKLOG("window origin: %s", on ? "lower left (the viewport as given)" : "upper left (the viewport flipped)"); g_origin_ll = on; }
void vkr_set_queue_origin_lower_left(int on) { g_queue_origin_ll = on; }
void vkr_set_depth_zero(int on) { g_depth_zero = on; }

void vkr_stats(vkr_stats_t *s) { *s = g_st; }
// Small storage buffers bound in a pass-dump frame (up to 64 distinct): at the frame's end the mirror's words are logged beside guest memory's (NXRT_VK_PASSDUMP_CBUF floats)
static struct { uint64_t a; uint32_t n; } g_pk[64]; static int g_npk;
static void pk_note(uint64_t a, uint32_t n) { for (int i = 0; i < g_npk; i++) if (g_pk[i].a == a && g_pk[i].n == n) return; if (g_npk < 64) { g_pk[g_npk].a = a; g_pk[g_npk].n = n; g_npk++; } }
static void pk_flush(void) {
    const char *e = getenv("NXRT_VK_PASSDUMP_CBUF"); int nfull = e ? atoi(e) : 0; if (nfull > 0) for (int q = 0; q < g_npk; q++) { uint8_t pk[4096]; uint64_t a = g_pk[q].a; uint32_t n = g_pk[q].n;
        if (vkm_peek(a, pk, n)) continue; uint64_t nf = n / 4 < (uint64_t)nfull ? n / 4 : (uint64_t)nfull; int diff = 0; char t[1400]; int o = 0;
        for (uint64_t k = 0; k < nf; k++) { uint32_t x, y; memcpy(&x, pk + 4 * k, 4); memcpy(&y, (const uint8_t *)(uintptr_t)a + 4 * k, 4); diff += x != y; float v; memcpy(&v, pk + 4 * k, 4); if (o < (int)sizeof t - 40) o += snprintf(t + o, sizeof t - o, "%s%g", k % 4 ? "," : " |", v); }
        VKLOG("sbuf mirror %#llx size %u: %d of %d words differ from guest memory;%s", (unsigned long long)a, n, diff, (int)nf, t); }
    g_npk = 0;
}
static _Atomic long g_pd_req; static _Atomic int g_td_req;
void vkr_passdump_next(long n) { atomic_store(&g_pd_req, n > 0 ? n : 1); }
void vkr_texdump_next(void) { atomic_store(&g_td_req, 1); }
// NXRT_VK_GWATCH=<hex address>[,<bytes>][;<address>[,<bytes>]...] (up to 4, default 64 bytes, at most 256 each): at every present, what guest memory holds there (the CPU's view, read directly), logged
// when a word changed; with NXRT_VK_GWATCH_MIRROR=1 also what the GPU's mirror holds (waits for the GPU: a diagnostic only). NXRT_VK_GWATCH_N limits the lines per address (default 4000)
// NXRT_GUEST_POKE="<hex address>:<value>;..." (up to 16, diagnostic only): rewrites guest memory at every present and at the start of every submission's replay (and every NXRT_GUEST_POKE_US microseconds
// from a thread, when set) so an experiment can pin a game parameter without changing the game. <address> is "744821f928" or "[744821f508]+0x190" (the 64-bit word there, plus an offset: skipped while it is 0).
// <value> is a float ("0.000061", "2^-14" = 2 to the power), "u0x1000" a 32-bit word, "|0x1000" ORs bits into the word, "&~0x1000" clears bits. The first change of each entry is logged.
static struct { uint64_t a, off; int ind, kind; uint32_t u; float f; int logged; } g_pk_w[16]; static int g_pk_n;
static void guest_poke(void) {
    for (int i = 0; i < g_pk_n; i++) { uint64_t a = g_pk_w[i].a; if (g_pk_w[i].ind) { a = __atomic_load_n((uint64_t *)(uintptr_t)a, __ATOMIC_RELAXED); if (!a) continue; a += g_pk_w[i].off; }
        uint32_t *p = (uint32_t *)(uintptr_t)a; uint32_t old = __atomic_load_n(p, __ATOMIC_RELAXED), nv;
        switch (g_pk_w[i].kind) { case 1: nv = g_pk_w[i].u; break; case 2: nv = old | g_pk_w[i].u; break; case 3: nv = old & ~g_pk_w[i].u; break; default: memcpy(&nv, &g_pk_w[i].f, 4); }
        if (nv != old) __atomic_store_n(p, nv, __ATOMIC_RELAXED);
        if (!g_pk_w[i].logged) { g_pk_w[i].logged = 1; float fo, fn; memcpy(&fo, &old, 4); memcpy(&fn, &nv, 4); VKLOG("guest poke %#llx: %#x(%g) -> %#x(%g)", (unsigned long long)a, old, fo, nv, fn); } }
}
static void *guest_poke_thread(void *arg) { useconds_t us = (useconds_t)(uintptr_t)arg; for (;;) { usleep(us); guest_poke(); } return NULL; }
static void guest_poke_init(void) {
    const char *e = getenv("NXRT_GUEST_POKE");
    while (e && *e && g_pk_n < 16) { char *end; int ind = 0; uint64_t a, off = 0;
        if (*e == '[') { ind = 1; a = strtoull(e + 1, &end, 16); if (*end == ']') end++; if (*end == '+') off = strtoull(end + 1, &end, 0); } else a = strtoull(e, &end, 16);
        if (*end != ':') break; const char *v = end + 1; int k = 0; uint32_t u = 0; float f = 0;
        if (v[0] == '|') { k = 2; u = (uint32_t)strtoul(v + 1, &end, 0); } else if (v[0] == '&' && v[1] == '~') { k = 3; u = (uint32_t)strtoul(v + 2, &end, 0); }
        else if (v[0] == 'u') { k = 1; u = (uint32_t)strtoul(v + 1, &end, 0); } else if (v[0] == '2' && v[1] == '^') { f = exp2f(strtof(v + 2, &end)); } else f = strtof(v, &end);
        if (a) { g_pk_w[g_pk_n].a = a; g_pk_w[g_pk_n].off = off; g_pk_w[g_pk_n].ind = ind; g_pk_w[g_pk_n].kind = k; g_pk_w[g_pk_n].u = u; g_pk_w[g_pk_n].f = f; g_pk_n++; } e = *end == ';' ? end + 1 : NULL; }
    if (g_pk_n) VKLOG("NXRT_GUEST_POKE: %d word(s)", g_pk_n);
    const char *us = getenv("NXRT_GUEST_POKE_US"); if (g_pk_n && us && atoi(us) > 0) { pthread_t t; pthread_create(&t, NULL, guest_poke_thread, (void *)(uintptr_t)atoi(us)); pthread_detach(t); }
}
static void gwatch_frame(uint64_t frame) {
    static struct { uint64_t a, nlog, same; uint32_t n, prev[64], pm[64]; } w[4]; static int nw, wi, mir; static uint64_t nmax = 4000;
    if (!wi) { wi = 1; const char *e = getenv("NXRT_VK_GWATCH");
        while (e && *e && nw < 4) { char *end; w[nw].a = strtoull(e, &end, 16); w[nw].n = *end == ',' ? (uint32_t)strtoul(end + 1, &end, 0) : 64; if (w[nw].n > 256) w[nw].n = 256; w[nw].n &= ~3u; if (w[nw].a && w[nw].n) nw++; e = *end == ';' ? end + 1 : NULL; }
        e = getenv("NXRT_VK_GWATCH_MIRROR"); mir = e && atoi(e); e = getenv("NXRT_VK_GWATCH_N"); if (e) nmax = strtoull(e, NULL, 0); }
    for (int i = 0; i < nw; i++) {
        if (w[i].nlog >= nmax) continue;
        uint32_t cur[64], m[64]; uint32_t nwd = w[i].n / 4; memcpy(cur, (const void *)(uintptr_t)w[i].a, w[i].n);
        int hm = 0; if (mir) { uint8_t b[256]; if (!vkm_peek(w[i].a, b, w[i].n)) { memcpy(m, b, w[i].n); hm = 1; } }
        int d = 0; for (uint32_t k = 0; k < nwd; k++) d |= cur[k] != w[i].prev[k] || (hm && m[k] != w[i].pm[k]);
        if (!d) { w[i].same++; continue; }
        char t[2400]; int o = snprintf(t, sizeof t, "gwatch %#llx frame %llu (%llu unchanged before): guest", (unsigned long long)w[i].a, (unsigned long long)frame, (unsigned long long)w[i].same); w[i].same = 0;
        for (uint32_t k = 0; k < nwd && o < (int)sizeof t - 400; k++) { float f; memcpy(&f, &cur[k], 4); o += snprintf(t + o, sizeof t - o, " w%u=%#x(%g)", k, cur[k], f); }
        if (hm) { o += snprintf(t + o, sizeof t - o, " | mirror"); for (uint32_t k = 0; k < nwd && o < (int)sizeof t - 40; k++) { float f; memcpy(&f, &m[k], 4); o += snprintf(t + o, sizeof t - o, " %#x(%g)", m[k], f); } }
        VKLOG("%s", t); w[i].nlog++; memcpy(w[i].prev, cur, w[i].n); if (hm) memcpy(w[i].pm, m, w[i].n);
    }
}
uint64_t vks_scan_cur_frame(void) { return g_scan_frame; }
// NXRT_MEMSEARCH=<frame>:<hex word>[,<hex word>...] (diagnostic, Linux): at that present, search every readable private mapping in the guest's range (0x7000000000..0x8000000000)
// for the 32-bit words in a row (4-byte aligned) and log up to 32 hits with the 16 words before and after each
static void memsearch(uint64_t frame) {
#ifndef _WIN32
    static int init; static uint64_t at; static uint32_t pat[16]; static int np;
    if (!init) { init = 1; const char *e = getenv("NXRT_MEMSEARCH"); if (!e) return; char *end; at = strtoull(e, &end, 0); e = *end == ':' ? end + 1 : NULL;
        while (e && *e && np < 16) { pat[np++] = (uint32_t)strtoul(e, &end, 16); e = *end == ',' ? end + 1 : NULL; } }
    if (!np || frame != at) return;
    static uint64_t rlo = 0x7000000000ull, rhi = 0x8000000000ull; { const char *r = getenv("NXRT_MEMSEARCH_RANGE"); if (r) { char *end; rlo = strtoull(r, &end, 16); if (*end == '-') rhi = strtoull(end + 1, NULL, 16); } }   // <hex lo>-<hex hi>
    FILE *m = fopen("/proc/self/maps", "r"); if (!m) return; char ln[512]; int hits = 0;
    while (fgets(ln, sizeof ln, m) && hits < 32) {
        unsigned long long lo, hi; char perm[8]; if (sscanf(ln, "%llx-%llx %7s", &lo, &hi, perm) != 3) continue;
        if (perm[0] != 'r' || hi <= rlo || lo >= rhi || lo < 0x7000000000ull || hi > 0x8000000000ull) continue;
        if (lo < rlo) lo = rlo; if (hi > rhi) hi = rhi;
        for (uint64_t a = lo; a + 4 * (uint64_t)np <= hi && hits < 32; a += 4) {
            const uint32_t *p = (const uint32_t *)(uintptr_t)a; if (p[0] != pat[0]) continue;
            int k; for (k = 1; k < np; k++) if (p[k] != pat[k]) break;
            if (k < np) continue;
            char t[1800]; int o = snprintf(t, sizeof t, "memsearch frame %llu hit %#llx (map %llx-%llx %s):", (unsigned long long)frame, (unsigned long long)a, lo, hi, perm);
            for (int j = -16; j < 16 + np && o < (int)sizeof t - 30; j++) { uint64_t q = a + 4 * (int64_t)j; if (q < lo || q + 4 > hi) continue; uint32_t v = *(const uint32_t *)(uintptr_t)q; float f; memcpy(&f, &v, 4);
                o += snprintf(t + o, sizeof t - o, "%s%g", j == 0 ? " [" : " ", f); if (j == np - 1) o += snprintf(t + o, sizeof t - o, "]"); }
            VKLOG("%s", t); hits++;
        }
    }
    fclose(m); VKLOG("memsearch frame %llu: %d hit(s)", (unsigned long long)frame, hits);
#else
    (void)frame;
#endif
}
void vkr_frame(uint64_t frame) {
    ab_poll(frame); g_scan_frame = frame; guest_poke(); memsearch(frame);
    gwatch_frame(frame); copylog_report(frame);
    static vkr_stats_t prev; static uint64_t pf;
    long pr = atomic_exchange(&g_pd_req, 0); if (pr > 0) { g_pd_frame = frame; g_pd_last = frame + (uint64_t)pr - 1; }   // DM_CTL passdump / texdump: from this frame on
    if (atomic_exchange(&g_td_req, 0)) g_td_frame = frame;
    g_pd = g_pd_frame && frame >= g_pd_frame && frame <= g_pd_last;
    if (!g_pd && vks_stat_pending()) { vks_finish(); vks_stat_report(); }
    if (vks_scan_pending()) { vks_finish(); vks_scan_report(); }
    if (frame % 300 == 0) vks_scan_summary();
    if (g_td_frame && frame == g_td_frame + 1 && g_ntd) {   // the dump frame's images, now that its work is done
        vks_finish();
        pk_flush();
        for (int i = 0; i < g_ntd; i++) {
            vkt_image *im = g_td[i]; char path[1200]; const char *lp = nxrt_log_path();
            for (unsigned zi = 0; zi < (im->vtype == VK_IMAGE_VIEW_TYPE_3D ? (im->depth > 5 ? 5 : im->depth) : 1); zi++) {
                unsigned z = im->vtype == VK_IMAGE_VIEW_TYPE_3D && im->depth > 5 ? zi * (im->depth - 1) / 4 : zi;   // a volume: five slices, first to last
                snprintf(path, sizeof path, "%s.tex-%llu-ser%llu-%s-%ux%u-z%u.png", lp && *lp ? lp : "nxrt", (unsigned long long)g_td_frame, (unsigned long long)im->serial, im->fi->name, im->w, im->h, z);
                VKLOG("texture dump: ser%llu %s %ux%ux%u %s", (unsigned long long)im->serial, im->fi->name, im->w, im->h, im->depth, vks_dump_image(im, z, path) ? "not dumped" : path);
                if (im->w * im->h <= 16 && im->fi->vbpb <= 16 && !(im->fi->flags & VKF_COMPRESSED) && !(im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) {   // tiny ones: the values themselves
                    uint8_t raw[256]; NVNcopyRegion rr = {0, 0, 0, (int)im->w, (int)im->h, 1}; char t[600]; int o = 0;
                    if (!vkt_readback_region(im, 0, 0, &rr, raw, (size_t)im->w * im->h * im->fi->vbpb))
                        for (uint32_t k = 0; k < im->w * im->h * im->fi->vbpb / 4 && o < 560; k++) { float f; uint32_t u; memcpy(&f, raw + 4 * k, 4); memcpy(&u, raw + 4 * k, 4); o += snprintf(t + o, sizeof t - o, " %g(%#x)", f, u); }
                    VKLOG("texture dump: ser%llu values:%s", (unsigned long long)im->serial, t);
                }
            }
        }
        g_ntd = 0;
    }
    g_td_on = g_td_frame && frame == g_td_frame;
    if (g_pd) PD("---- the frame after present %llu ----", (unsigned long long)frame);

    if (frame % 300 || frame == pf) return;
    static uint64_t pgn, pgs; uint64_t gn = atomic_load(&g_gpu_ns), gs = atomic_load(&g_gpu_subs);
    if (gs > pgs) VKLOG("GPU time up to frame %llu: %.2f ms per frame (%.2f ms per submission, %.1f submissions per frame; timestamps around each submission's replay)",
                        (unsigned long long)frame, (gn - pgn) / 1e6 / (double)(frame - pf), (gn - pgn) / 1e6 / (double)(gs - pgs), (double)(gs - pgs) / (double)(frame - pf));
    pgn = gn; pgs = gs;
    vkr_stats_t s = g_st; uint64_t df = frame - pf; vkp_stats_t ps; vkp_stats(&ps); uint64_t shl, shf; vksh_stats(&shl, &shf);
    VKLOG("replay up to frame %llu, per frame: %.1f draws (%.1f skipped: no shader %llu, no pipeline %llu, no target %llu in all), %.1f passes, %.1f dispatches, %.1f clears, "
          "%.1f texture copies, %.1f buffer copies / updates / fills, %.1f descriptor sets, %.1f write-backs; %llu graphics + %llu compute pipelines (%.1f ms creating, worst %.1f ms), %llu shaders (%llu unusable)",
          (unsigned long long)frame, (double)(s.draws - prev.draws) / df, (double)(s.draws_skipped - prev.draws_skipped) / df, (unsigned long long)s.skip_no_shader,
          (unsigned long long)s.skip_no_pipeline, (unsigned long long)s.skip_no_target, (double)(s.passes - prev.passes) / df, (double)(s.dispatches - prev.dispatches) / df,
          (double)(s.clears - prev.clears) / df, (double)(s.tex_copies - prev.tex_copies) / df, (double)(s.copies + s.updates + s.fills - prev.copies - prev.updates - prev.fills) / df,
          (double)(s.desc_sets - prev.desc_sets) / df, (double)(s.writebacks - prev.writebacks) / df, (unsigned long long)ps.graphics, (unsigned long long)ps.compute,
          ps.create_ns / 1e6, ps.max_ns / 1e6, (unsigned long long)shl, (unsigned long long)shf);
    if (s.pool_draws != prev.pool_draws || s.skip_bigarray != prev.skip_bigarray) VKLOG("up to frame %llu: %.1f draws / dispatches a frame with the bindless pool set bound (%llu in all), %llu skipped for want of it (%llu in all); %llu pool descriptors written", (unsigned long long)frame,
          (double)(s.pool_draws - prev.pool_draws) / df, (unsigned long long)s.pool_draws, (unsigned long long)(s.skip_bigarray - prev.skip_bigarray), (unsigned long long)s.skip_bigarray, (unsigned long long)BL.writes);
    if (s.rep_waits != prev.rep_waits || s.handles_bad != prev.handles_bad) VKLOG("up to frame %llu: %llu conditional renders waited for counter reports (%llu in all), %llu texture handles outside the pool (%llu in all)", (unsigned long long)frame,
          (unsigned long long)(s.rep_waits - prev.rep_waits), (unsigned long long)s.rep_waits, (unsigned long long)(s.handles_bad - prev.handles_bad), (unsigned long long)s.handles_bad);
    if (ps.prewarmed) VKLOG("pipelines made ahead by the prewarm up to frame %llu: %llu (%.1f ms on its threads)", (unsigned long long)frame, (unsigned long long)ps.prewarmed, ps.prewarm_ns / 1e6);
    prev = s; pf = frame;
}

int vkr_init(void) {
    guest_poke_init();
    if (vksh_init() || vkpl_init()) return -1;
    if (!g_vk.has.null_descriptor) VKRED("the device has no null descriptors (VK_EXT_robustness2): unbound textures and images are not replayed correctly");
    VkBufferUsageFlags bu = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (mkbuf(ZERO_SIZE, bu, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &R0.zero, &R0.zero_m, "replay zeros") || mkbuf(ZERO_SIZE, bu, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &R0.sink, &R0.sink_m, "replay write sink") ||
        mkbuf(2048, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &R0.support, &R0.support_m, "support buffer") ||
        mkbuf(65536 * 24, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &R0.quads, &R0.quads_m, "quad indices") ||
        mkbuf(BOUNCE_SIZE, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &R0.bounce, &R0.bounce_m, "storage bounce")) return -1;
    // the support buffer in Ryujinx's layout (nvn_mtl.m support_buf): render scale 1.0 in each of the 73 entries from byte 192, the rest zero
    memset(R0.support_m.map, 0, 2048);
    for (int i = 0; i < 73; i++) { float one = 1.0f; memcpy((uint8_t *)R0.support_m.map + 192 + 16 * i, &one, 4); }
    uint32_t *q = R0.quads_m.map;
    for (uint32_t i = 0; i < 65536; i++) { uint32_t b = 4 * i; q[6 * i] = b; q[6 * i + 1] = b + 1; q[6 * i + 2] = b + 2; q[6 * i + 3] = b; q[6 * i + 4] = b + 2; q[6 * i + 5] = b + 3; }
    VkCommandBuffer cb = vk_once_begin();
    vkCmdFillBuffer(cb, R0.zero, 0, ZERO_SIZE, 0); vkCmdFillBuffer(cb, R0.sink, 0, ZERO_SIZE, 0);
    vk_mem_barrier(cb, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    if (vk_once_end(cb)) return -1;
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0, VK_FALSE, 1, VK_FALSE, VK_COMPARE_OP_NEVER, 0, VK_LOD_CLAMP_NONE, VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, VK_FALSE};
    if (vkCreateSampler(g_vk.dev, &sci, NULL, &R0.samp) != VK_SUCCESS) return -1;
    VkQueryPoolCreateInfo qci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_OCCLUSION, NQ, 0};
    if (vkCreateQueryPool(g_vk.dev, &qci, NULL, &R0.qp) != VK_SUCCESS) return -1;
    vkResetQueryPool(g_vk.dev, R0.qp, 0, NQ);
    VkQueryPoolCreateInfo tci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, 2 * NTQ, 0};
    if (g_vk.props.limits.timestampComputeAndGraphics && vkCreateQueryPool(g_vk.dev, &tci, NULL, &g_tqp) == VK_SUCCESS) vkResetQueryPool(g_vk.dev, g_tqp, 0, 2 * NTQ);
    S = rs_get(0);
    const char *e = getenv("NXRT_VK_PASSDUMP"); if (e) { char *end; g_pd_frame = strtoull(e, &end, 0); g_pd_last = *end == '-' ? strtoull(end + 1, NULL, 0) : g_pd_frame; }
    if ((e = getenv("NXRT_VK_TEXDUMP"))) g_td_frame = strtoull(e, NULL, 0);
    vkpl_prewarm_start();   // M6 phase B: pipelines listed before, made ahead in the background
    return 0;
}
void vkr_shutdown(void) {
    bl_shutdown();
    while (g_states) { rs *s = g_states; g_states = s->next; free(s); }
    S = NULL;
    for (uint32_t i = 0; i < g_progc.cap; i++) free(g_progc.vals[i]);
    vk_hm_free(&g_progc);
    while (g_free_chunks) { rchunk *c = g_free_chunks; g_free_chunks = c->next; vkDestroyBuffer(g_vk.dev, c->buf, NULL); vk_free_mem(&c->mem); free(c); }
    while (g_free_pools) { rdpool *p = g_free_pools; g_free_pools = p->next; vkDestroyDescriptorPool(g_vk.dev, p->pool, NULL); free(p); }
    if (R0.qp) vkDestroyQueryPool(g_vk.dev, R0.qp, NULL);
    if (g_tqp) vkDestroyQueryPool(g_vk.dev, g_tqp, NULL);
    g_tqp = VK_NULL_HANDLE;
    if (R0.samp) vkDestroySampler(g_vk.dev, R0.samp, NULL);
    VkBuffer bs[5] = {R0.zero, R0.sink, R0.support, R0.quads, R0.bounce}; vk_alloc *ms[5] = {&R0.zero_m, &R0.sink_m, &R0.support_m, &R0.quads_m, &R0.bounce_m};
    for (int i = 0; i < 5; i++) if (bs[i]) { vkDestroyBuffer(g_vk.dev, bs[i], NULL); vk_free_mem(ms[i]); }
    memset(&R0, 0, sizeof R0);
    for (int i = 0; i < g_ndct; i++) { for (int j = 0; j < 4; j++) if (g_dct[i].view[j]) vkDestroyImageView(g_vk.dev, g_dct[i].view[j], NULL); vkDestroyImage(g_vk.dev, g_dct[i].img, NULL); vk_free_mem(&g_dct[i].mem); }
    g_ndct = 0; vk_hm_free(&g_dct_of);
    for (uint32_t i = 0; i < g_cpipe.cap; i++) if (g_cpipe.vals[i]) vkDestroyPipeline(g_vk.dev, (VkPipeline)g_cpipe.vals[i], NULL);
    vk_hm_free(&g_cpipe);
    for (int i = 0; i < 4; i++) if (g_cmod[i]) { vkDestroyShaderModule(g_vk.dev, g_cmod[i], NULL); g_cmod[i] = VK_NULL_HANDLE; }
    if (g_cpl) { vkDestroyPipelineLayout(g_vk.dev, g_cpl, NULL); g_cpl = VK_NULL_HANDLE; }
    vkpl_shutdown(); vksh_shutdown();
}
