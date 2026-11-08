// Shaders for the replay (vk_shader.h). The binding kinds and their meaning are nxnative's (nvn_refl.m, nvn_mtl.m encode_args), as nxshader's reflection
// names them (dmsh_abi.h DMSH_KIND_*); the SPIR-V side is read from the module itself (the SPIR-V specification's physical layout).
#include "vk_shader.h"
#include "vk_json.h"
#include "vk_c0.h"
#include "vk_hmap.h"
#include "nxs_rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static vk_hmap g_sh = {17};   // key → vks_shader (a failed one stays, marked, so it is not retried)
static plat_rwlock_t g_shl = PLAT_RWLOCK_INIT;   // M6 phase B: the table, the variants and the c1 bank, shared by the replay and the pipeline prewarm (vk_pipe.c)
static VkDescriptorSetLayout g_empty;
static uint64_t g_loaded, g_failed;
static struct { VkBuffer buf; vk_alloc mem; VkDeviceSize used, size; } g_c1;   // the c1 bank (host-visible, written once per shader)
static vks_shader g_failed_mark;

// The Switch 2 compute bank's tables from the reflection's s2_compute_layout: nubo / nssbo (control +0xb1 / +0xb2), the roots' room when the cache has "roots" (the eight word counts of
// control +0xa9..+0xb0: tools/s2_ctl_layout.py) and the code's own texture base. NXRT_VK_S2_ROOTS=0: the uniform buffers at 0x10 as before the roots were known; NXRT_VK_S2_CTL_LAYOUT=1: the counts only
// The eight root-table word counts the cache carries ("roots"), for placing pushed constants (vk_c0.h vkc0_push_to_bank). 0 when the cache has none (older cache) or NXRT_VK_S2_ROOTS=0 (the
// uniform buffers are back at 0x10 then, so there is no room for the roots)
int vksh_s2_roots(const struct vj *l, uint8_t out[VKC0_ROOT_TABLES]) {
    const char *e = getenv("NXRT_VK_S2_ROOTS"); if (e && *e == '0') return 0;
    const vj *r = vj_get(l, "roots"); if (!r || r->n < VKC0_ROOT_TABLES) return 0;
    for (int i = 0; i < VKC0_ROOT_TABLES; i++) out[i] = (uint8_t)vj_int(vj_at(r, i), 0);
    return 1;
}
vkc0_parts vksh_s2_c0_parts(const struct vj *l, unsigned nubo, unsigned nssbo) {
    const char *e = getenv("NXRT_VK_S2_ROOTS"); int legacy = e && *e == '0';   // read at each shader load
    const vj *r = vj_get(l, "roots"); int rb = -1;
    if (r && r->n >= VKC0_ROOT_TABLES) { uint8_t rr[VKC0_ROOT_TABLES]; for (int i = 0; i < VKC0_ROOT_TABLES; i++) rr[i] = (uint8_t)vj_int(vj_at(r, i), 0); rb = (int)vkc0_roots_bytes(rr); }
    return vkc0_derive(nubo, nssbo, rb, (uint32_t)vj_int(vj_get(l, "tex_base"), 0), !getenv("NXRT_VK_S2_CTL_LAYOUT"), legacy);
}

int vksh_set_of(int stage) { return stage == VKS_CS ? 0 : stage == VKS_VS ? 0 : stage == VKS_FS ? 1 : stage == VKS_GS ? 2 : stage == VKS_TCS ? 3 : 4; }
VkShaderStageFlagBits vksh_vk_stage(int stage) {
    switch (stage) {
    case VKS_VS: return VK_SHADER_STAGE_VERTEX_BIT; case VKS_FS: return VK_SHADER_STAGE_FRAGMENT_BIT; case VKS_GS: return VK_SHADER_STAGE_GEOMETRY_BIT;
    case VKS_TCS: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT; case VKS_TES: return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT; default: return VK_SHADER_STAGE_COMPUTE_BIT;
    }
}
VkDescriptorSetLayout vksh_empty_dsl(void) { return g_empty; }
void vksh_stats(uint64_t *loaded, uint64_t *failed) { *loaded = g_loaded; *failed = g_failed; }

// ---------- SPIR-V ----------
enum { OP_NAME = 5, OP_TYPE_INT = 21, OP_TYPE_FLOAT = 22, OP_TYPE_IMAGE = 25, OP_TYPE_SAMPLED_IMAGE = 27, OP_TYPE_ARRAY = 28, OP_TYPE_RUNTIME_ARRAY = 29,
       OP_TYPE_POINTER = 32, OP_VARIABLE = 59, OP_DECORATE = 71, DEC_BINDING = 33, DEC_SET = 34 };
static int spv_ok(const uint32_t *w, size_t n) { return n >= 5 && w[0] == 0x07230203u && w[3] > 0 && w[3] < (1u << 22); }

int vkspv_set_descriptor_set(uint32_t *w, size_t n, uint32_t set) {
    if (!spv_ok(w, n)) return -1;
    int changed = 0;
    for (size_t i = 5; i < n; ) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc || i + wc > n) return -1;
        if (op == OP_DECORATE && wc >= 4 && w[i + 2] == DEC_SET) { w[i + 3] = set; changed++; }
        i += wc;
    }
    return changed;
}

int vkspv_remap_bindings(uint32_t **pw, size_t *pn, const vkspv_remap_t *m, int nm) {
    enum { O_CONSTANT = 43 };
    uint32_t *w = *pw; size_t n = *pn;
    if (!spv_ok(w, n)) return -1;
    uint32_t bound = w[3];
    int *mi = calloc(bound, sizeof *mi);                       // variable id -> map index + 1
    uint32_t *pointee = calloc(bound, 4), *alen = calloc(bound, 4), *cval = calloc(bound, 4), *cty = calloc(bound, 4), *vty = calloc(bound, 4);
    uint8_t *isarr = calloc(bound, 1), *iscon = calloc(bound, 1), *patch = calloc(bound, 1);
    if (!mi || !pointee || !alen || !cval || !cty || !vty || !isarr || !iscon || !patch) { free(mi); free(pointee); free(alen); free(cval); free(cty); free(vty); free(isarr); free(iscon); free(patch); return -1; }
    int changed = 0, rc = 0, npatch = 0;
    for (size_t i = 5; i < n; ) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc || i + wc > n) { rc = -1; goto done; }
        if (op == OP_DECORATE && wc >= 4 && w[i + 2] == DEC_BINDING && w[i + 1] < bound) { for (int j = 0; j < nm; j++) if (m[j].old_binding == w[i + 3]) { mi[w[i + 1]] = j + 1; break; } }
        else if (op == OP_TYPE_POINTER && wc >= 4 && w[i + 1] < bound) pointee[w[i + 1]] = w[i + 3];
        else if (op == OP_TYPE_ARRAY && wc >= 4 && w[i + 1] < bound) { isarr[w[i + 1]] = 1; alen[w[i + 1]] = w[i + 3]; }
        else if (op == O_CONSTANT && wc == 4 && w[i + 2] < bound) { iscon[w[i + 2]] = 1; cty[w[i + 2]] = w[i + 1]; cval[w[i + 2]] = w[i + 3]; }
        else if (op == OP_VARIABLE && wc >= 4 && w[i + 2] < bound) vty[w[i + 2]] = w[i + 1];
        i += wc;
    }
    for (uint32_t v = 1; v < bound; v++) if (mi[v]) {
        const vkspv_remap_t *r = &m[mi[v] - 1];
        if (r->max_len && vty[v] < bound) {
            uint32_t at = pointee[vty[v]];
            if (at < bound && isarr[at] && alen[at] < bound && iscon[alen[at]] && cval[alen[at]] > r->max_len && !patch[at]) { patch[at] = 1; npatch++; }
        }
    }
    size_t on = n + 4 * (size_t)npatch;
    uint32_t *o = malloc(on * 4);
    if (!o) { rc = -1; goto done; }
    memcpy(o, w, 5 * 4); size_t k = 5; uint32_t nid = bound;
    for (size_t i = 5; i < n; ) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (op == OP_TYPE_ARRAY && wc >= 4 && w[i + 1] < bound && patch[w[i + 1]]) {
            uint32_t cl = w[i + 3], cap = 0;   // the cap of the variable that has this array
            for (uint32_t v = 1; v < bound; v++) if (mi[v] && vty[v] < bound && pointee[vty[v]] == w[i + 1] && m[mi[v] - 1].max_len) { cap = m[mi[v] - 1].max_len; break; }
            o[k++] = (4u << 16) | O_CONSTANT; o[k++] = cty[cl]; o[k++] = nid; o[k++] = cap;
            memcpy(o + k, w + i, wc * 4); o[k + 3] = nid; k += wc; nid++;
        } else if (op == OP_DECORATE && wc >= 4 && w[i + 1] < bound && mi[w[i + 1]] && (w[i + 2] == DEC_BINDING || w[i + 2] == DEC_SET)) {
            memcpy(o + k, w + i, wc * 4);
            o[k + 3] = w[i + 2] == DEC_BINDING ? m[mi[w[i + 1]] - 1].new_binding : m[mi[w[i + 1]] - 1].new_set;
            if (w[i + 2] == DEC_BINDING) changed++;
            k += wc;
        } else { memcpy(o + k, w + i, wc * 4); k += wc; }
        i += wc;
    }
    o[3] = nid; *pw = o; *pn = k; free(w); rc = changed;
done:
    free(mi); free(pointee); free(alen); free(cval); free(cty); free(vty); free(isarr); free(iscon); free(patch);
    return rc;
}

// The translator lowers a Maxwell BRX (indirect branch through a c1 jump table) to a dispatch loop: the target is loaded once before the loop, and the body
// compares it with each target it knows and leaves to the merge on a match; the continue block is "OpBranchConditional %true <header> <merge>". A target the
// translator did not list (OoT's Layout2Shader MergeShader 94b4efd3100f9af6 reads c1 words 15..19 and lists the targets of 16..19 only) loops forever on the
// GPU: amdgpu reset the gfx ring ("ring gfx timeout") at the first title-screen press. A second form re-enters the dispatch for a target that is the top of an
// enclosing loop (the continue block branches back when the target matches, without reading c1 again): that target must leave to the enclosing loop instead. Such a loop is pure (loads, integer compares, bitcasts, stores to
// per-invocation variables stored before they are read again) and acyclic inside, so an iteration that takes the back-edge repeats itself exactly: the back-edge
// is only ever taken by an invocation that never finishes. Its condition becomes the constant that picks the merge (the back-edge stays, so the structured CFG
// is unchanged) and such an invocation leaves to the merge. Returns the loops changed, -1 for malformed SPIR-V
int vkspv_break_dispatch_loops(uint32_t *w, size_t n) {
    enum { O_TRUE = 41, O_FALSE = 42, O_VAR = 59, O_LOAD = 61, O_STORE = 62, O_BITCAST = 124, O_IADD = 128, O_ISUB = 130, O_LOR = 166, O_LAND = 167, O_LNOT = 168,
           O_LEQ = 164, O_LNE = 165, O_IEQ = 170, O_INE = 171, O_LOOP = 246, O_SEL = 247, O_LABEL = 248, O_BR = 249, O_BRC = 250 };
    if (!spv_ok(w, n)) return -1;
    uint32_t bound = w[3], fid = 0, tid = 0; int nfix = 0; uint8_t *tnt = NULL;
    uint8_t *cls = calloc(bound, 1);   // 1 true constant, 2 variable (Function / Private), 3 read-only memory (UniformConstant / Input / Uniform / PushConstant), 4 other memory
    typedef struct { uint32_t label, at, end, term; } blk;   // at: the OpLabel; end: the terminator
    size_t nb = 0, cap = 256; blk *b = malloc(cap * sizeof *b);
    if (!cls || !b) { free(cls); free(b); return 0; }
    for (size_t i = 5; i < n; ) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc || i + wc > n) { free(cls); free(b); return -1; }
        if (op == O_TRUE && wc >= 3 && w[i + 2] < bound) { cls[w[i + 2]] = 1; if (!tid) tid = w[i + 2]; }
        else if (op == O_FALSE && wc >= 3 && !fid) fid = w[i + 2];
        else if (op == O_VAR && wc >= 4 && w[i + 2] < bound) { uint32_t sc = w[i + 3]; cls[w[i + 2]] = sc == 7 || sc == 6 ? 2 : sc == 0 || sc == 1 || sc == 2 || sc == 9 ? 3 : 4; }
        else if ((op == 65 || op == 66) && wc >= 4 && w[i + 2] < bound && w[i + 3] < bound && cls[w[i + 3]] == 3) cls[w[i + 2]] = 3;   // an access chain into read-only memory
        else if (op == O_LABEL && wc >= 2) {
            if (nb == cap) { blk *nbp = realloc(b, (cap *= 2) * sizeof *b); if (!nbp) { free(cls); free(b); return 0; } b = nbp; }
            b[nb++] = (blk){w[i + 1], (uint32_t)i, 0, 0};
        } else if (nb && !b[nb - 1].end && (op == O_BR || op == O_BRC || (op >= 251 && op <= 255) || op == 4416)) { b[nb - 1].end = (uint32_t)i; b[nb - 1].term = op; }
        i += wc;
    }
    #define NXT(i) ((i) + (w[i] >> 16))
    for (size_t h = 0; fid && tid && h < nb; h++) {
        uint32_t i = NXT(b[h].at);   // the header: OpLabel, OpLoopMerge, OpBranch <first body block>
        if ((w[i] & 0xffff) != O_LOOP || (w[i] >> 16) < 4) continue;
        uint32_t mrg = w[i + 1], cont = w[i + 2], j = NXT(i);
        if (j != b[h].end || b[h].term != O_BR) continue;
        size_t c = h + 1; while (c < nb && b[c].label != cont) c++;
        if (c >= nb || c == h + 1 || c - h > 64 || b[h + 1].label != w[j + 1]) continue;
        // the continue block ends in a conditional branch between the header and the merge (on %true, or on a value the body computed)
        uint32_t e = b[c].end; if (b[c].term != O_BRC || !((w[e + 2] == b[h].label && w[e + 3] == mrg) || (w[e + 3] == b[h].label && w[e + 2] == mrg))) continue;
        // the body: blocks h+1 .. c (the continue block last), forward edges only, leaving only to the merge. must[k]: variables stored on every path to block k's
        // start. A value is tainted when it depends on a variable carried from the previous iteration (read before this iteration stored it, or stored with a
        // tainted value); no branch may depend on one, so every iteration takes the same path
        uint32_t must[64][24]; uint8_t nm[64], seen[64]; int ok = 1, again = 1;
        size_t nbody = c - h;
        if (!tnt) tnt = calloc(bound, 1);
        if (!tnt) break;
        for (int pass = 0; ok && again && pass < 6; pass++) {
            again = 0; memset(seen, 0, sizeof seen); nm[0] = 0; seen[0] = 1;
            for (size_t k = 0; ok && k < nbody; k++) {
                const blk *x = &b[h + 1 + k]; if (!seen[k] || !x->end) { ok = 0; break; }
                uint32_t st[24]; uint8_t ns = nm[k]; memcpy(st, must[k], ns * 4);
                for (uint32_t q = NXT(x->at); ok && q < x->end; q = NXT(q)) {
                    uint32_t op = w[q] & 0xffff, wc = w[q] >> 16;
                    if (op == O_SEL) continue;
                    if (op == O_STORE) {
                        uint32_t p = w[q + 1]; if (wc < 3 || p >= bound || cls[p] != 2) { ok = 0; break; }
                        if (w[q + 2] < bound && tnt[w[q + 2]] && !tnt[p]) { tnt[p] = 1; again = 1; }   // tnt[variable]: stored with a tainted value somewhere in the body
                        int got = 0; for (int t = 0; t < ns; t++) got |= st[t] == p;
                        if (!got) { if (ns >= 24) ok = 0; else st[ns++] = p; }
                        continue;
                    }
                    if (!(op == O_LOAD || op == 12 /* OpExtInst (GLSL.std.450: pure) */ || op == 81 /* OpCompositeExtract */ || op == 65 || op == 66 ||
                          (op >= 109 && op <= 115) /* conversions */ || (op >= 124 && op <= 141) /* bitcast, integer and float arithmetic */ || op == 156 || op == 157 /* IsNan, IsInf */ ||
                          (op >= 164 && op <= 191) /* logical, select, integer and float comparisons */ || (op >= 194 && op <= 200) /* shifts, bitwise */) || wc < 4 || w[q + 2] >= bound) { ok = 0; break; }
                    uint32_t r = w[q + 2]; int t = 0;
                    if (op == O_LOAD) {
                        uint32_t p = w[q + 3]; if (p >= bound || (cls[p] != 2 && cls[p] != 3)) { ok = 0; break; }
                        if (cls[p] == 2) {
                            int got = 0; for (int z = 0; z < ns; z++) got |= st[z] == p;
                            if (got) t = tnt[p];
                            else for (size_t kk = 0; kk < nbody && !t; kk++)   // carried when the body stores it at all
                                for (uint32_t rr = NXT(b[h + 1 + kk].at); rr < b[h + 1 + kk].end; rr = NXT(rr)) if ((w[rr] & 0xffff) == O_STORE && w[rr + 1] == p) { t = 1; break; }
                        }
                    } else if ((op == 65 || op == 66) && cls[w[q + 3]] != 3) { ok = 0; break; }
                    else for (uint32_t o = 3; o < wc; o++) if (w[q + o] < bound && tnt[w[q + o]]) { t = 1; break; }   // literals read as ids only add taint
                    if (t && !tnt[r]) { tnt[r] = 1; again = 1; }
                }
                if (!ok) break;
                uint32_t succ[2]; int nsu = 0;
                if (x->term == O_BR) succ[nsu++] = w[x->end + 1];
                else if (x->term == O_BRC) { if (w[x->end + 1] >= bound || tnt[w[x->end + 1]]) { ok = 0; break; } succ[nsu++] = w[x->end + 2]; succ[nsu++] = w[x->end + 3]; }
                else { ok = 0; break; }
                for (int s2 = 0; s2 < nsu; s2++) {
                    if (succ[s2] == mrg || (k + 1 == nbody && succ[s2] == b[h].label)) continue;
                    size_t t = k + 1; while (t < nbody && b[h + 1 + t].label != succ[s2]) t++;
                    if (t >= nbody) { ok = 0; break; }
                    if (!seen[t]) { seen[t] = 1; nm[t] = ns; memcpy(must[t], st, ns * 4); }
                    else { uint8_t m2 = 0; for (int a2 = 0; a2 < nm[t]; a2++) for (int z = 0; z < ns; z++) if (must[t][a2] == st[z]) { must[t][m2++] = must[t][a2]; break; } nm[t] = m2; }
                }
            }
        }
        if (again) ok = 0;
        for (size_t k = 0; k < nbody; k++) for (uint32_t q = b[h + 1 + k].at; q <= b[h + 1 + k].end && q < n; q = NXT(q)) if ((w[q] >> 16) >= 3) { if (w[q + 2] < bound) tnt[w[q + 2]] = 0; if (w[q + 1] < bound) tnt[w[q + 1]] = 0; }
        if (ok) { w[e + 1] = w[e + 2] == mrg ? tid : fid; nfix++; }
    }
    #undef NXT
    free(cls); free(b); free(tnt);
    return nfix;
}

// SignedZeroInfNanPreserve (width 32) on every entry point: Vulkan lets a compiler treat NaN, infinity and the sign of zero as don't-care in a module that does not ask for them to be
// kept, and RADV does: it folds the guards a program writes against NaN (a select on OpIsNan, min/max that drop a NaN operand, a clamp, x * 0) and reorders math as if no NaN could
// exist, so a lighting program written for hardware whose FMNMX / SAT / FSETP handle NaN exactly (Maxwell, Ampere) comes out with NaN where the hardware has a number. Eden, Lichii
// and Orchid declare the mode whenever the device has shaderSignedZeroInfNanPreserveFloat32. Needs SPV_KHR_float_controls below SPIR-V 1.4. Returns the new word count (*pw may be
// replaced), nwords when nothing changed (already declared / no entry point), 0 when the module is malformed
size_t vkspv_nan_preserve(uint32_t **pw, size_t n) {
    enum { OP_EXTENSION = 10, OP_ENTRY_POINT = 15, OP_EXECUTION_MODE = 16, OP_CAPABILITY = 17, CAP_SZINP = 4466, MODE_SZINP = 4461 };
    uint32_t *w = *pw;
    if (!spv_ok(w, n)) return 0;
    int need_ext = (w[1] >> 8 & 0xff) < 4 && (w[1] >> 16 & 0xff) <= 1;
    size_t last_cap = 0, last_ext = 0, last_mode = 0; int have_cap = 0, have_ext = 0, neps = 0; uint32_t eps[8]; uint32_t has_mode[8] = {0};
    for (size_t i = 5; i < n; ) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc || i + wc > n) return 0;
        if (op == OP_CAPABILITY) { last_cap = i + wc; if (w[i + 1] == CAP_SZINP) have_cap = 1; }
        else if (op == OP_EXTENSION) { last_ext = i + wc; if (!strncmp((const char *)&w[i + 1], "SPV_KHR_float_controls", 23)) have_ext = 1; }
        else if (op == OP_ENTRY_POINT && neps < 8) eps[neps++] = w[i + 2], last_mode = i + wc;
        else if (op == OP_EXECUTION_MODE) { last_mode = i + wc; if (w[i + 2] == MODE_SZINP && w[i + 3] == 32) for (int k = 0; k < neps; k++) if (eps[k] == w[i + 1]) has_mode[k] = 1; }
        else if (op > OP_EXECUTION_MODE && op != 330 && op != 331 && last_mode == 0) return n;   // no entry point before the first other instruction
        i += wc;
    }
    if (!neps || !last_cap || !last_mode) return n;
    int addm = 0; for (int k = 0; k < neps; k++) if (!has_mode[k]) addm++;
    if (!addm && have_cap) return n;
    size_t ext_words = need_ext && !have_ext ? 1 + 6 : 0;   // "SPV_KHR_float_controls" is 22 chars + NUL = 23 bytes = 6 words
    size_t add = (have_cap ? 0 : 2) + ext_words + (size_t)addm * 4;
    uint32_t *o = malloc((n + add) * sizeof *o); if (!o) return n;
    size_t p = 0, ins_ext = last_ext ? last_ext : last_cap;
    for (size_t i = 0; i < n; ) {
        size_t wc = i < 5 ? 5 : w[i] >> 16;
        memcpy(o + p, w + i, wc * sizeof *o); p += wc; i += wc;
        if (i == last_cap && !have_cap) { o[p++] = 2u << 16 | OP_CAPABILITY; o[p++] = CAP_SZINP; }
        if (i == ins_ext && ext_words) { o[p++] = 7u << 16 | OP_EXTENSION; char buf[24] = "SPV_KHR_float_controls"; memcpy(o + p, buf, 24); p += 6; ext_words = 0; }
        if (i == last_mode) for (int k = 0; k < neps; k++) if (!has_mode[k]) { o[p++] = 4u << 16 | OP_EXECUTION_MODE; o[p++] = eps[k]; o[p++] = MODE_SZINP; o[p++] = 32; }
    }
    free(w); *pw = o;
    return p;
}

// Position invariance for the stages before the rasterizer: every FAdd / FSub / FMul / FDiv / FMod result gets NoContraction and the Position output gets Invariant. The translator runs
// Ryujinx in its reduced (Metal) mode, which leaves both out, so a Vulkan compiler (RADV's ACO) may fuse a multiply and the add that uses it into one FMA in one vertex program and not in
// another. OoT draws a depth pre-pass (LESS, depth write) and then every material with the depth test EQUAL against it, through a different vertex program: one rounding apart and the
// material is rejected (this was a hypothesis for the white OoT scene; the measured cause was the cleared depth copy, section 24). Maxwell / Ampere never fuse a FMUL
// and a FADD, Eden / Orchid decorate every FADD / FMUL NoContraction (floating_point_add.cpp, floating_point_multiply.cpp) and Ryujinx adds Invariant to gl_Position outside its
// reduced mode. Returns the new word count (*pw may be replaced), n when nothing changed, 0 when the module is malformed
size_t vkspv_invariant_position(uint32_t **pw, size_t n, int *ndec) {
    enum { OP_DECORATE = 71, OP_MEMBER_DECORATE = 72, DEC_BUILTIN = 11, DEC_INVARIANT = 18, DEC_NOCONTRACTION = 42, BI_POSITION = 0 };
    uint32_t *w = *pw; *ndec = 0;
    if (!spv_ok(w, n)) return 0;
    uint32_t bound = w[3];
    uint8_t *has = calloc(bound, 1), *want = calloc(bound, 1); if (!has || !want) { free(has); free(want); return n; }
    size_t ins = 0; uint32_t pos_var = 0, pos_struct = 0, pos_member = 0; int pos_inv = 0, nwant = 0;
    for (size_t i = 5; i < n; ) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc || i + wc > n) { free(has); free(want); return 0; }
        int pre = op == 17 || op == 10 || op == 11 || op == 14 || op == 15 || op == 16 || op == 331 || (op >= 2 && op <= 8) || op == 330 || (op >= 71 && op <= 75) || op == 332 || op == 5632 || op == 5633;
        if (!ins && !pre) ins = i;
        if (op == OP_DECORATE && wc >= 3 && w[i + 1] < bound) {
            if (w[i + 2] == DEC_NOCONTRACTION) has[w[i + 1]] = 1;
            if (w[i + 2] == DEC_BUILTIN && wc >= 4 && w[i + 3] == BI_POSITION) pos_var = w[i + 1];
            if (w[i + 2] == DEC_INVARIANT && w[i + 1] == pos_var) pos_inv = 1;
        }
        if (op == OP_MEMBER_DECORATE && wc >= 5 && w[i + 3] == DEC_BUILTIN && w[i + 4] == BI_POSITION) { pos_struct = w[i + 1]; pos_member = w[i + 2]; }
        if (op == OP_MEMBER_DECORATE && wc >= 4 && w[i + 1] == pos_struct && w[i + 2] == pos_member && w[i + 3] == DEC_INVARIANT) pos_inv = 1;
        if ((op == 129 || op == 131 || op == 133 || op == 136 || op == 141) && wc >= 5 && w[i + 2] < bound && !want[w[i + 2]]) { want[w[i + 2]] = 1; nwant++; }
        i += wc;
    }
    for (size_t i = 5; i < n; i += w[i] >> 16) {   // an Invariant written before the BuiltIn
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (op == OP_DECORATE && wc >= 3 && w[i + 2] == DEC_INVARIANT && pos_var && w[i + 1] == pos_var) pos_inv = 1;
        if (op == OP_MEMBER_DECORATE && wc >= 4 && w[i + 3] == DEC_INVARIANT && pos_struct && w[i + 1] == pos_struct && w[i + 2] == pos_member) pos_inv = 1;
    }
    int addpos = (pos_var || pos_struct) && !pos_inv;
    size_t add = (addpos ? (pos_var ? 3 : 4) : 0); for (uint32_t r = 0; r < bound; r++) if (want[r] && !has[r]) add += 3;
    if (!ins || !add) { free(has); free(want); return n; }
    uint32_t *o = malloc((n + add) * sizeof *o); if (!o) { free(has); free(want); return n; }
    memcpy(o, w, ins * sizeof *o); size_t p = ins;
    if (addpos) { if (pos_var) { o[p++] = 3u << 16 | OP_DECORATE; o[p++] = pos_var; o[p++] = DEC_INVARIANT; } else { o[p++] = 4u << 16 | OP_MEMBER_DECORATE; o[p++] = pos_struct; o[p++] = pos_member; o[p++] = DEC_INVARIANT; } }
    for (uint32_t r = 0; r < bound; r++) if (want[r] && !has[r]) { o[p++] = 3u << 16 | OP_DECORATE; o[p++] = r; o[p++] = DEC_NOCONTRACTION; (*ndec)++; }
    memcpy(o + p, w + ins, (n - ins) * sizeof *o); p += n - ins;
    free(has); free(want); free(w); *pw = o;
    return p;
}

// gl_PerVertex declares ClipDistance[8] and CullDistance[1] (Maxwell has 8 clip distances): more than a device with maxCombinedClipAndCullDistances 8 takes.
// The cull member goes when nothing reaches it (Pokopia never culls by distance): the block's last member, no access chain names it, the block is never
// loaded, stored or built whole. Returns the new word count (*w may be replaced), n when nothing changed, 0 when it cannot be done
size_t vkspv_trim_cull(uint32_t **pw, size_t n, uint32_t limit) {
    enum { OP_MEMBER_NAME = 6, OP_TYPE_STRUCT = 30, OP_CONSTANT = 43, OP_LOAD = 61, OP_STORE = 62, OP_COPY_MEMORY = 63, OP_ACCESS_CHAIN = 65, OP_IN_BOUNDS_ACCESS_CHAIN = 66,
           OP_MEMBER_DECORATE = 72, OP_COMPOSITE_CONSTRUCT = 80, OP_COPY_OBJECT = 83, DEC_BUILTIN = 11, BI_CLIP = 3, BI_CULL = 4 };
    uint32_t *w = *pw;
    if (!spv_ok(w, n)) return 0;
    uint32_t bound = w[3], st = 0, clip_m = ~0u, cull_m = ~0u;
    for (size_t i = 5; i < n; ) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc || i + wc > n) return 0;
        if (op == OP_MEMBER_DECORATE && wc >= 5 && w[i + 3] == DEC_BUILTIN && (w[i + 4] == BI_CLIP || w[i + 4] == BI_CULL)) {
            if (st && st != w[i + 1]) return n;   // more than one block declares them: left alone
            st = w[i + 1]; if (w[i + 4] == BI_CLIP) clip_m = w[i + 2]; else cull_m = w[i + 2];
        }
        i += wc;
    }
    if (!st || clip_m == ~0u || cull_m == ~0u || st >= bound) return n;
    uint32_t *def = calloc(bound, sizeof *def), *cval = calloc(bound, sizeof *cval); uint8_t *isc = calloc(bound, 1);
    size_t si = 0; size_t out = 0;
    for (size_t i = 5; i < n; i += w[i] >> 16) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (op >= OP_TYPE_INT && op <= OP_TYPE_POINTER && wc >= 2 && w[i + 1] < bound) def[w[i + 1]] = (uint32_t)i;
        if (op == OP_TYPE_STRUCT && w[i + 1] == st) si = i;
        if (op == OP_CONSTANT && wc >= 4 && w[i + 2] < bound) { cval[w[i + 2]] = w[i + 3]; isc[w[i + 2]] = 1; }
    }
    uint32_t nmem = si ? (w[si] >> 16) - 2 : 0;
    // the members' array lengths
    uint32_t len[2] = {0, 0}, mem[2] = {clip_m, cull_m};
    for (int k = 0; k < 2 && si; k++) if (mem[k] < nmem) { uint32_t t = w[si + 2 + mem[k]];
        if (t < bound && def[t] && (w[def[t]] & 0xffff) == OP_TYPE_ARRAY && w[def[t] + 3] < bound && isc[w[def[t] + 3]]) len[k] = cval[w[def[t] + 3]]; }
    int ok = si && cull_m == nmem - 1 && len[0] && len[1] && len[0] + len[1] > limit && len[0] <= limit;
    // what refers to the block: pointers to it or to arrays of it, the variables of those
    uint8_t *blk = calloc(bound, 1), *bptr = calloc(bound, 1), *bvar = calloc(bound, 1);   // blk: the block or an array of it
    if (ok) {
        blk[st] = 1;
        for (size_t i = 5; i < n; i += w[i] >> 16) { uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
            if ((op == OP_TYPE_ARRAY || op == OP_TYPE_RUNTIME_ARRAY) && wc >= 3 && w[i + 2] < bound && blk[w[i + 2]] && w[i + 1] < bound) blk[w[i + 1]] = 2;
            if (op == OP_TYPE_POINTER && wc >= 4 && w[i + 3] < bound && blk[w[i + 3]] && w[i + 1] < bound) bptr[w[i + 1]] = blk[w[i + 3]];
            if (op == OP_VARIABLE && wc >= 4 && w[i + 1] < bound && bptr[w[i + 1]] && w[i + 2] < bound) bvar[w[i + 2]] = bptr[w[i + 1]]; }
        for (size_t i = 5; ok && i < n; i += w[i] >> 16) { uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
            if ((op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN) && wc >= 5 && w[i + 3] < bound && bvar[w[i + 3]]) {
                uint32_t k = bvar[w[i + 3]] == 2 ? 5 : 4;   // the member index follows the vertex index of an arrayed block
                if (k < wc && (w[i + k] >= bound || !isc[w[i + k]] || cval[w[i + k]] == cull_m)) ok = 0;
            }
            if ((op == OP_LOAD || op == OP_COMPOSITE_CONSTRUCT || op == OP_COPY_OBJECT) && wc >= 3 && w[i + 1] < bound && blk[w[i + 1]]) ok = 0;
            if ((op == OP_STORE || op == OP_COPY_MEMORY) && wc >= 3 && ((w[i + 1] < bound && bvar[w[i + 1]]) || (w[i + 2] < bound && bvar[w[i + 2]]))) ok = 0;
        }
    }
    if (ok) {   // the struct loses its last word; the member's name and decorations go
        uint32_t *nw = malloc(n * 4);
        memcpy(nw, w, 20); out = 5;
        for (size_t i = 5; i < n; i += w[i] >> 16) {
            uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
            if ((op == OP_MEMBER_DECORATE || op == OP_MEMBER_NAME) && wc >= 3 && w[i + 1] == st && w[i + 2] == cull_m) continue;
            if (i == si) { nw[out] = ((wc - 1) << 16) | op; memcpy(nw + out + 1, w + i + 1, (wc - 2) * 4); out += wc - 1; continue; }
            memcpy(nw + out, w + i, wc * 4); out += wc;
        }
        free(w); *pw = nw;
    }
    free(def); free(cval); free(isc); free(blk); free(bptr); free(bvar);
    return ok ? out : (si && len[0] + len[1] > limit ? 0 : n);
}

typedef struct { uint32_t *w; size_t n, cap; } wbuf;
static void wput(wbuf *b, const uint32_t *x, size_t k) { if (b->n + k > b->cap) { b->cap = (b->n + k) * 2; b->w = realloc(b->w, b->cap * 4); } memcpy(b->w + b->n, x, k * 4); b->n += k; }

// NVN's compiler aggregates an atomic add over a warp: ballot(true), the ballot's highest lane leads and adds its bit count for all of them. After some lanes have
// returned, that ballot never completes on NVIDIA's RTX 50 cards (#12, #21, #22: shader d2c50cca6e386abc, driver 617). Where the add's result is never read, a
// ballot of only the lane's own bit gives the same sum: every lane leads itself and adds 1. Only that shape is rewritten: ballot(true) whose value reaches a
// FindUMsb / FindSMsb and a bit count, the bit count an OpAtomicIAdd whose result goes nowhere (or only into a variable nothing reads). The new word count
// (*pw replaced) and *rewritten ballots; n when nothing changed
size_t vkspv_ballot_per_lane(uint32_t **pw, size_t n, int *rewritten) {
    enum { B_NAME = 5, B_EXT_INST = 12, B_TYPE_INT = 21, B_TYPE_VECTOR = 23, B_TYPE_POINTER = 32, B_CONSTANT_TRUE = 41, B_CONSTANT = 43, B_FUNCTION = 54,
           B_VARIABLE = 59, B_LOAD = 61, B_STORE = 62, B_ACCESS_CHAIN = 65, B_IN_BOUNDS_ACCESS_CHAIN = 66, B_DECORATE = 71, B_COMPOSITE_CONSTRUCT = 80,
           B_COMPOSITE_EXTRACT = 81, B_COPY_OBJECT = 83, B_BITCAST = 124, B_SHL = 196, B_AND = 199, B_BIT_COUNT = 205, B_ATOMIC_IADD = 234, B_BALLOT = 339,
           B_DEC_BUILTIN = 11, B_BI_SUBGROUP_LANE = 41, B_FIND_SMSB = 74, B_FIND_UMSB = 75, MAXB = 32 };
    uint32_t *w = *pw;
    *rewritten = 0;
    if (!spv_ok(w, n)) return n;
    uint32_t bound = w[3], tu = 0, c0 = 0, c1 = 0, lane_var = 0, lane_ptr = 0, nb = 0, bres[MAXB], bat[MAXB];
    size_t first_fn = 0;
    uint32_t *def = calloc(bound, sizeof *def); uint8_t *is_true = calloc(bound, 1);
    for (size_t i = 5; i < n; ) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc || i + wc > n) { free(def); free(is_true); return n; }
        if ((op == B_TYPE_INT || op == B_TYPE_VECTOR || op == B_TYPE_POINTER) && wc >= 2 && w[i + 1] < bound) def[w[i + 1]] = (uint32_t)i;
        if (op == B_VARIABLE && wc >= 4 && w[i + 2] < bound) def[w[i + 2]] = (uint32_t)i;
        if (op == B_TYPE_INT && wc == 4 && w[i + 2] == 32 && !w[i + 3] && !tu) tu = w[i + 1];
        if (op == B_CONSTANT_TRUE && wc == 3 && w[i + 2] < bound) is_true[w[i + 2]] = 1;
        if (op == B_CONSTANT && wc == 4 && tu && w[i + 1] == tu) { if (w[i + 3] == 0 && !c0) c0 = w[i + 2]; if (w[i + 3] == 1 && !c1) c1 = w[i + 2]; }
        if (op == B_DECORATE && wc == 4 && w[i + 2] == B_DEC_BUILTIN && w[i + 3] == B_BI_SUBGROUP_LANE) lane_var = w[i + 1];
        if (op == B_FUNCTION && !first_fn) first_fn = i;
        if (op == B_BALLOT && wc == 5 && w[i + 4] < bound && is_true[w[i + 4]] && nb < MAXB) { bres[nb] = w[i + 2]; bat[nb++] = (uint32_t)i; }
        i += wc;
    }
    // the lane's index must be an Input uint (as NVN's leader test reads it), the ballot a vector of 4 of them
    if (lane_var && lane_var < bound && def[lane_var]) { uint32_t pt = w[def[lane_var] + 1];
        if (pt < bound && def[pt] && (w[def[pt]] & 0xffff) == B_TYPE_POINTER && w[def[pt] + 3] == tu) lane_ptr = pt; }
    if (!nb || !tu || !lane_ptr || !first_fn) { free(def); free(is_true); return n; }
    // what each ballot's value reaches (bit k for ballot k): D its bits, C a bit count of them, through variables, access chains and bit casts
    uint32_t *D = calloc(bound, 4), *C = calloc(bound, 4), msb = 0, atom = 0, atom_used = 0;
    for (uint32_t k = 0; k < nb; k++) D[bres[k]] |= 1u << k;
    for (int pass = 0, changed = 1; changed && pass < 32; pass++) {
        changed = 0; atom = 0;
        #define FLOW(dst, d, c) do { uint32_t d_ = (d), c_ = (c); if ((D[dst] | d_) != D[dst] || (C[dst] | c_) != C[dst]) { D[dst] |= d_; C[dst] |= c_; changed = 1; } } while (0)
        for (size_t i = 5; i < n; i += w[i] >> 16) {
            uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
            if (op == B_STORE && wc >= 3 && w[i + 1] < bound && w[i + 2] < bound) FLOW(w[i + 1], D[w[i + 2]], C[w[i + 2]]);
            else if ((op == B_LOAD || op == B_ACCESS_CHAIN || op == B_IN_BOUNDS_ACCESS_CHAIN || op == B_COMPOSITE_EXTRACT || op == B_COPY_OBJECT || op == B_BITCAST)
                     && wc >= 4 && w[i + 2] < bound && w[i + 3] < bound) FLOW(w[i + 2], D[w[i + 3]], C[w[i + 3]]);
            else if (op == B_AND && wc == 5 && w[i + 2] < bound && w[i + 3] < bound && w[i + 4] < bound) FLOW(w[i + 2], D[w[i + 3]] | D[w[i + 4]], C[w[i + 3]] | C[w[i + 4]]);
            else if (op == B_BIT_COUNT && wc == 4 && w[i + 2] < bound && w[i + 3] < bound) FLOW(w[i + 2], 0, D[w[i + 3]] | C[w[i + 3]]);
            else if (op == B_EXT_INST && wc == 6 && (w[i + 4] == B_FIND_UMSB || w[i + 4] == B_FIND_SMSB) && w[i + 5] < bound) msb |= D[w[i + 5]];
            else if (op == B_ATOMIC_IADD && wc == 7 && w[i + 6] < bound && C[w[i + 6]]) {
                atom |= C[w[i + 6]];
                // its result read anywhere (beyond a store into a variable nothing else names) keeps those ballots as they are
                uint32_t r = w[i + 2], stored = 0;
                for (size_t j = 5; j < n && !(atom_used & C[w[i + 6]]) ; j += w[j] >> 16) {
                    if (j == i) continue;
                    uint32_t jc = w[j] >> 16, jo = w[j] & 0xffff;
                    for (uint32_t a = 1; a < jc; a++) if (w[j + a] == r) {
                        if (jo == B_STORE && a == 2 && !stored) stored = w[j + 1]; else atom_used |= C[w[i + 6]];
                        break;
                    }
                }
                if (stored) for (size_t j = 5; j < n && !(atom_used & C[w[i + 6]]); j += w[j] >> 16) {
                    uint32_t jc = w[j] >> 16, jo = w[j] & 0xffff;
                    if (jo == B_NAME || (jo == B_VARIABLE && w[j + 2] == stored) || (jo == B_STORE && w[j + 1] == stored)) continue;
                    for (uint32_t a = 1; a < jc; a++) if (w[j + a] == stored) { atom_used |= C[w[i + 6]]; break; }
                }
            }
        }
        #undef FLOW
    }
    uint32_t ok = msb & atom & ~atom_used, vt = 0;
    for (uint32_t k = 0; k < nb; k++) if (ok >> k & 1) {   // the ballot's type: a vector of 4 uints
        vt = w[bat[k] + 1];
        if (!(vt < bound && def[vt] && (w[def[vt]] & 0xffff) == B_TYPE_VECTOR && w[def[vt] + 2] == tu && w[def[vt] + 3] == 4)) ok &= ~(1u << k);
    }
    free(D); free(C); free(is_true);
    if (!ok) { free(def); return n; }
    // the rewrite: the constants 0 and 1 when missing (before the first function), each ballot → this lane's bit in .x
    wbuf o = {0};
    wput(&o, w, 5);
    for (size_t i = 5; i < n; ) {
        uint32_t wc = w[i] >> 16;
        if (i == first_fn) {
            if (!c0) { c0 = bound++; uint32_t c[4] = {4u << 16 | B_CONSTANT, tu, c0, 0}; wput(&o, c, 4); }
            if (!c1) { c1 = bound++; uint32_t c[4] = {4u << 16 | B_CONSTANT, tu, c1, 1}; wput(&o, c, 4); }
        }
        int k = -1;
        for (uint32_t b = 0; b < nb; b++) if (bat[b] == i && (ok >> b & 1)) k = (int)b;
        if (k < 0) { wput(&o, w + i, wc); i += wc; continue; }
        uint32_t lid = bound++, bit = bound++;
        uint32_t ld[4] = {4u << 16 | B_LOAD, tu, lid, lane_var}, sh[5] = {5u << 16 | B_SHL, tu, bit, c1, lid},
                 cc[7] = {7u << 16 | B_COMPOSITE_CONSTRUCT, w[i + 1], w[i + 2], bit, c0, c0, c0};
        wput(&o, ld, 4); wput(&o, sh, 5); wput(&o, cc, 7);
        (*rewritten)++;
        i += wc;
    }
    o.w[3] = bound;
    free(def); free(w);
    *pw = o.w;
    return o.n;
}

int vkspv_image_of(const uint32_t *w, size_t n, uint32_t binding, vkspv_img *o) {
    memset(o, 0, sizeof *o); o->binding = binding;
    if (!spv_ok(w, n)) return -1;
    uint32_t bound = w[3], var = 0;
    for (size_t i = 5; i < n; ) {   // the variable decorated with this binding
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc || i + wc > n) return -1;
        if (op == OP_DECORATE && wc >= 4 && w[i + 2] == DEC_BINDING && w[i + 3] == binding) { var = w[i + 1]; break; }
        i += wc;
    }
    if (!var || var >= bound) return -1;
    // types by id: what each defines (opcode, operands' offset)
    uint32_t *def = calloc(bound, sizeof *def);   // index of the defining instruction
    for (size_t i = 5; i < n; ) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc || i + wc > n) break;
        uint32_t res = 0;
        if (op >= OP_TYPE_INT && op <= OP_TYPE_POINTER && wc >= 2) res = w[i + 1];
        else if (op == OP_VARIABLE && wc >= 3) res = w[i + 2];
        if (res && res < bound) def[res] = (uint32_t)i;
        i += wc;
    }
    int rc = -1;
    uint32_t vi = def[var];
    if (vi && (w[vi] & 0xffff) == OP_VARIABLE) {
        uint32_t t = w[vi + 1];
        for (int hop = 0; hop < 8 && t < bound && def[t]; hop++) {
            uint32_t d = def[t], op = w[d] & 0xffff;
            if (op == OP_TYPE_POINTER) t = w[d + 3];
            else if (op == OP_TYPE_ARRAY || op == OP_TYPE_RUNTIME_ARRAY) t = w[d + 2];
            else if (op == OP_TYPE_SAMPLED_IMAGE) t = w[d + 2];
            else if (op == OP_TYPE_IMAGE) {
                o->is_image = 1; o->dim = (uint8_t)w[d + 3]; o->depth = w[d + 4] == 1; o->arrayed = (uint8_t)w[d + 5]; o->ms = (uint8_t)w[d + 6];
                uint32_t st = w[d + 2];
                if (st < bound && def[st]) { uint32_t sd = def[st]; o->ct = (w[sd] & 0xffff) == OP_TYPE_INT ? (w[sd + 3] ? VKS_CT_INT : VKS_CT_UINT) : VKS_CT_FLOAT; }
                break;
            } else break;
        }
        o->found = 1; rc = 0;
    }
    free(def);
    return rc;
}

static VkImageViewType view_type(uint8_t dim, uint8_t arrayed) {
    switch (dim) {
    case 0: return arrayed ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
    case 2: return VK_IMAGE_VIEW_TYPE_3D;
    case 3: return arrayed ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
    default: return arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;   // 2D, rect
    }
}

// ---------- Vertex input retyping (vksh_vin_variant) ----------
enum { OP_TYPE_VECTOR = 23, OP_ACCESS_CHAIN = 65, OP_IN_BOUNDS_ACCESS_CHAIN = 66, OP_LOAD = 61, OP_BITCAST = 124, DEC_LOCATION = 30, SC_INPUT = 1 };
uint32_t *vkspv_retype_inputs(const uint32_t *w, size_t n, uint32_t mask, const uint8_t ct[32], size_t *out_n) {
    if (!spv_ok(w, n) || !mask) return NULL;
    uint32_t bound = w[3];
    // what the module defines: scalar types by class, vectors, input pointers, the input variables by location
    uint32_t *def = calloc(bound, sizeof *def), loc_of_var[32] = {0}, scal[3] = {0}, first_var = 0;
    for (size_t i = 5; i < n; ) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc || i + wc > n) { free(def); return NULL; }
        if (op == OP_DECORATE && wc >= 4 && w[i + 2] == DEC_LOCATION && w[i + 3] < 32) loc_of_var[w[i + 3]] = w[i + 1];   // the last one wins; inputs are checked below
        uint32_t res = 0;
        if (op >= OP_TYPE_INT && op <= OP_TYPE_POINTER && wc >= 2) res = w[i + 1];
        else if (op == OP_VARIABLE && wc >= 4) { res = w[i + 2]; if (!first_var && w[i + 3] != 7) first_var = (uint32_t)i; }
        if (res && res < bound) def[res] = (uint32_t)i;
        if (op == OP_TYPE_INT && wc == 4 && w[i + 2] == 32) scal[w[i + 3] ? VKS_CT_INT : VKS_CT_UINT] = w[i + 1];
        if (op == OP_TYPE_FLOAT && wc == 3 && w[i + 2] == 32) scal[VKS_CT_FLOAT] = w[i + 1];
        i += wc;
    }
    // the input variables to retype (their Location decorations may sit on outputs too: only Input variables count)
    uint32_t var_of[32] = {0}, var_ct[32];
    for (int l = 0; l < 32; l++) {
        if (!(mask >> l & 1)) continue;
        uint32_t v = 0;
        for (size_t i = 5; i < n; i += w[i] >> 16) if ((w[i] & 0xffff) == OP_DECORATE && (w[i] >> 16) >= 4 && w[i + 2] == DEC_LOCATION && w[i + 3] == (uint32_t)l) {
            uint32_t c = w[i + 1]; if (c < bound && def[c] && (w[def[c]] & 0xffff) == OP_VARIABLE && w[def[c] + 3] == SC_INPUT) v = c; }
        if (v) { var_of[l] = v; var_ct[l] = ct[l]; }
    }
    (void)loc_of_var;
    // the new types go right before the first variable retyped; the existing types they name must be defined before that
    first_var = 0;
    for (int l = 0; l < 32; l++) if (var_of[l] && (!first_var || def[var_of[l]] < first_var)) first_var = def[var_of[l]];
    if (!first_var) { free(def); return NULL; }
    for (int k = 0; k < 3; k++) if (scal[k] && def[scal[k]] > first_var) { free(def); return NULL; }
    // new types, placed before the first global variable: the scalars of each class, vectors of them, input pointers to both
    wbuf types = {0}; uint32_t vec_of[3][5] = {{0}}, pv[3][5] = {{0}}, ps[3] = {0};
    for (size_t i = 5; i < n; i += w[i] >> 16) {   // existing vectors and input pointers
        uint32_t op = w[i] & 0xffff;
        for (int k = 0; k < 3; k++) {
            if (op == OP_TYPE_VECTOR && w[i + 2] == scal[k] && w[i + 3] <= 4) vec_of[k][w[i + 3]] = w[i + 1];
            if (op == OP_TYPE_POINTER && w[i + 2] == SC_INPUT && w[i + 3] == scal[k] && scal[k]) ps[k] = w[i + 1];
        }
    }
    for (size_t i = 5; i < n; i += w[i] >> 16) if ((w[i] & 0xffff) == OP_TYPE_POINTER && w[i + 2] == SC_INPUT)
        for (int k = 0; k < 3; k++) for (int c = 2; c <= 4; c++) if (vec_of[k][c] && w[i + 3] == vec_of[k][c]) pv[k][c] = w[i + 1];
    #define NEWT(...) do { uint32_t t_[] = {__VA_ARGS__}; wput(&types, t_, sizeof t_ / 4); } while (0)
    for (int l = 0; l < 32; l++) {
        if (!var_of[l]) continue;
        int k = var_ct[l];
        if (!scal[k]) { scal[k] = bound++; if (k == VKS_CT_FLOAT) NEWT(3u << 16 | OP_TYPE_FLOAT, scal[k], 32); else NEWT(4u << 16 | OP_TYPE_INT, scal[k], 32, k == VKS_CT_INT); }
        if (!ps[k]) { ps[k] = bound++; NEWT(4u << 16 | OP_TYPE_POINTER, ps[k], SC_INPUT, scal[k]); }
        uint32_t pt = w[def[var_of[l]] + 1], pointee = w[def[pt] + 3], pop = w[def[pointee]] & 0xffff, nc = pop == OP_TYPE_VECTOR ? w[def[pointee] + 3] : 1;
        if (nc > 1 && nc <= 4) {
            if (!vec_of[k][nc]) { vec_of[k][nc] = bound++; NEWT(4u << 16 | OP_TYPE_VECTOR, vec_of[k][nc], scal[k], nc); }
            if (!pv[k][nc]) { pv[k][nc] = bound++; NEWT(4u << 16 | OP_TYPE_POINTER, pv[k][nc], SC_INPUT, vec_of[k][nc]); }
        }
    }
    #undef NEWT
    // the rewrite: variables take the new pointer type; access chains into them the scalar pointer; loads through either load the new type and bit-cast
    uint32_t *repl = calloc(bound + 1, sizeof *repl);   // pointer id → class + 1 of what it now points to
    wbuf o = {0};
    wput(&o, w, 5);
    for (size_t i = 5; i < n; ) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (i == first_var && types.n) wput(&o, types.w, types.n);
        uint32_t x[64]; memcpy(x, w + i, (wc < 64 ? wc : 64) * 4);
        if (wc > 64) { wput(&o, w + i, wc); i += wc; continue; }
        if (op == OP_VARIABLE) {
            for (int l = 0; l < 32; l++) if (var_of[l] && x[2] == var_of[l]) {
                int k = var_ct[l]; uint32_t pointee = w[def[x[1]] + 3], nc = (w[def[pointee]] & 0xffff) == OP_TYPE_VECTOR ? w[def[pointee] + 3] : 1;
                x[1] = nc > 1 ? pv[k][nc] : ps[k]; repl[x[2]] = (uint32_t)k + 1;
            }
            wput(&o, x, wc);
        } else if ((op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN) && x[3] < bound && repl[x[3]]) {
            int k = (int)repl[x[3]] - 1; x[1] = ps[k]; repl[x[2]] = (uint32_t)k + 1;   // one index into a vector: the scalar
            wput(&o, x, wc);
        } else if (op == OP_LOAD && x[3] < bound && repl[x[3]]) {
            int k = (int)repl[x[3]] - 1; uint32_t want = x[1], old = x[2], nid = bound++;
            uint32_t wop = w[def[want]] & 0xffff, nc = wop == OP_TYPE_VECTOR ? w[def[want] + 3] : 1;
            x[1] = nc > 1 ? vec_of[k][nc] : scal[k]; x[2] = nid;
            wput(&o, x, wc);
            uint32_t bc[4] = {4u << 16 | OP_BITCAST, want, old, nid}; wput(&o, bc, 4);
        } else wput(&o, x, wc);
        i += wc;
    }
    o.w[3] = bound;
    free(def); free(repl); free(types.w);
    *out_n = o.n;
    return o.w;
}
static const vks_shader *vin_variant_l(const vks_shader *vs_, uint32_t mask, const uint8_t ct[32]) {
    vks_shader *vs = (vks_shader *)vs_;
    if (!mask || !vs->words) return vs;
    uint64_t key = mask; for (int l = 0; l < 32; l++) if (mask >> l & 1) key = key * 31 + ct[l] + 1;
    for (int i = 0; i < vs->nvar; i++) if (vs->var[i].key == key) return vs->var[i].s;
    if (vs->nvar == 8) { static int n; if (n++ < 4) VKRED("shader %s: more than 8 vertex input variants", vs->key); return vs; }
    size_t nn; uint32_t *nw = vkspv_retype_inputs(vs->words, vs->nwords, mask, ct, &nn);
    VkShaderModule mod = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, nn * 4, nw};
    if (!nw || vkCreateShaderModule(g_vk.dev, &mci, NULL, &mod) != VK_SUCCESS) { free(nw); static int n; if (n++ < 4) VKRED("shader %s: its inputs %#x could not be retyped", vs->key, mask); return vs; }
    vks_shader *s = malloc(sizeof *s); *s = *vs;
    s->mod = mod; s->words = nw; s->nwords = nn; s->base = vs; s->nvar = 0;
    for (int i = 0; i < s->nin; i++) if (mask >> s->in[i].loc & 1) s->in[i].ct = ct[s->in[i].loc];
    s->vin_mask = mask; memcpy(s->vin_ct, ct, sizeof s->vin_ct);
    vs->var[vs->nvar].key = key; vs->var[vs->nvar].s = s; vs->nvar++;
    static int nl; if (nl++ < 16) VKLOG("vertex shader %s: inputs %#x declared as their formats' types (variant %d)", vs->key, mask, vs->nvar);
    return s;
}
const vks_shader *vksh_vin_variant(const vks_shader *vs, uint32_t mask, const uint8_t ct[32]) {
    if (!mask) return vs;
    plat_rw_wrlock(&g_shl); const vks_shader *r = vin_variant_l(vs, mask, ct); plat_rw_wrunlock(&g_shl);
    return r;
}

// ---------- Fragment variants (M6 phase B): dual-source blending, the alpha test ----------
enum { OP_ENTRY_POINT = 15, OP_TYPE_VOID = 19, OP_TYPE_BOOL = 20, OP_TYPE_STRUCT = 30, OP_CONSTANT = 43, OP_FUNCTION = 54, OP_FUNCTION_END = 56, OP_MEMBER_DECORATE = 72,
       OP_ISUB = 130, OP_LOGICAL_OR = 166, OP_LOGICAL_AND = 167, OP_INOT_EQUAL = 171, OP_FORD_EQUAL = 180, OP_FORD_LESS = 184, OP_FORD_GREATER = 186, OP_BITWISE_AND = 199,
       OP_SELECTION_MERGE = 247, OP_LABEL = 248, OP_BRANCH_COND = 250, OP_KILL = 252, OP_RETURN = 253,
       DEC_BLOCK = 2, DEC_BUILTIN = 11, DEC_INDEX = 32, DEC_OFFSET = 35, SC_OUTPUT = 3, SC_PUSH = 9 };
// The non-builtin Output variables by Location (-1 when absent; -2 when a location holds more than one; a second blend source, Index 1, is not counted); returns how many
static int spv_outputs(const uint32_t *w, size_t n, int32_t var_at[8]) {
    uint32_t bound = w[3]; int cnt = 0;
    uint8_t *out = calloc(bound, 1), *bi = calloc(bound, 1);
    for (size_t i = 5; i < n; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (op == OP_VARIABLE && wc >= 4 && w[i + 3] == SC_OUTPUT && w[i + 2] < bound) out[w[i + 2]] = 1;
        if (op == OP_DECORATE && wc >= 3 && (w[i + 2] == DEC_BUILTIN || (w[i + 2] == DEC_INDEX && wc >= 4 && w[i + 3] == 1)) && w[i + 1] < bound) bi[w[i + 1]] = 1;
    }
    for (int l = 0; l < 8; l++) var_at[l] = -1;
    for (size_t i = 5; i < n; i += w[i] >> 16)
        if ((w[i] & 0xffff) == OP_DECORATE && (w[i] >> 16) >= 4 && w[i + 2] == DEC_LOCATION && w[i + 1] < bound && out[w[i + 1]] && !bi[w[i + 1]] && w[i + 3] < 8) {
            uint32_t l = w[i + 3]; var_at[l] = var_at[l] == -1 ? (int32_t)w[i + 1] : -2; cnt++;
        }
    free(out); free(bi);
    return cnt;
}
// Dual-source blending (nxshader's DualSource pass, nxnative's fs_dual): output 1 becomes Location 0 Index 1, the second source of target 0. Only for a shader writing
// exactly outputs 0 and 1. New words or NULL
uint32_t *vkspv_dual_source(const uint32_t *w, size_t n, size_t *out_n) {
    if (!spv_ok(w, n)) return NULL;
    int32_t at[8]; if (spv_outputs(w, n, at) != 2 || at[0] < 0 || at[1] < 0) return NULL;
    wbuf o = {0}; wput(&o, w, 5);
    for (size_t i = 5; i < n; i += w[i] >> 16) {
        uint32_t wc = w[i] >> 16;
        if ((w[i] & 0xffff) == OP_DECORATE && wc == 4 && w[i + 1] == (uint32_t)at[1] && w[i + 2] == DEC_LOCATION) {
            uint32_t d[8] = {4u << 16 | OP_DECORATE, w[i + 1], DEC_LOCATION, 0, 4u << 16 | OP_DECORATE, w[i + 1], DEC_INDEX, 1}; wput(&o, d, 8);
        } else wput(&o, w + i, wc);
    }
    *out_n = o.n; return o.w;
}
// The alpha test (nxnative's dmsh_alpha.py library, as nvn_mtl.m at_fn / at_bind use it): before each return of the entry point, colour 0's alpha is compared with the
// reference by NVN's alpha function, both in a push constant block {uint function; float reference;} (fragment stage, offset 0, 8 bytes), and the fragment is discarded
// when the comparison fails. The function and reference are data, so one pipeline serves every value. Functions NEVER = 1 … ALWAYS = 8: function − 1 is a mask of
// less (1), equal (2), greater (4). Needs colour 0 as a float vec4 output. New words or NULL
uint32_t *vkspv_alpha_test(const uint32_t *w, size_t n, size_t *out_n) {
    if (!spv_ok(w, n)) return NULL;
    int32_t at[8]; spv_outputs(w, n, at);
    if (at[0] < 0) return NULL;
    uint32_t bound = w[3], *def = calloc(bound, sizeof *def), entry = 0, f32 = 0, u32 = 0, tbool = 0, pof = 0;
    size_t first_type = 0, first_fn = 0;
    for (size_t i = 5; i < n; ) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc || i + wc > n) { free(def); return NULL; }
        if (op >= OP_TYPE_VOID && op <= OP_TYPE_POINTER && wc >= 2 && w[i + 1] < bound) { def[w[i + 1]] = (uint32_t)i; if (!first_type) first_type = i; }
        if (op == OP_VARIABLE && wc >= 4 && w[i + 2] < bound) def[w[i + 2]] = (uint32_t)i;
        if (op == OP_ENTRY_POINT && wc >= 3 && !entry) entry = w[i + 2];
        if (op == OP_TYPE_FLOAT && wc == 3 && w[i + 2] == 32) f32 = w[i + 1];
        if (op == OP_TYPE_INT && wc == 4 && w[i + 2] == 32 && !w[i + 3]) u32 = w[i + 1];
        if (op == OP_TYPE_BOOL && wc == 2) tbool = w[i + 1];
        if (op == OP_FUNCTION && !first_fn) first_fn = i;
        i += wc;
    }
    // colour 0: an Output pointer to a vector of 4 floats
    uint32_t vi = def[at[0]], pt = vi ? w[vi + 1] : 0, pti = pt < bound ? def[pt] : 0, vt = pti && (w[pti] & 0xffff) == OP_TYPE_POINTER ? w[pti + 3] : 0, vti = vt < bound ? def[vt] : 0;
    int ok = entry && f32 && first_type && first_fn && vti && (w[vti] & 0xffff) == OP_TYPE_VECTOR && w[vti + 2] == f32 && w[vti + 3] == 4;
    for (size_t i = 5; ok && i < n; i += w[i] >> 16)   // an existing Output float pointer (a second declaration of a non-aggregate type is invalid)
        if ((w[i] & 0xffff) == OP_TYPE_POINTER && w[i + 2] == SC_OUTPUT && w[i + 3] == f32) pof = w[i + 1];
    free(def);
    if (!ok) return NULL;
    // new ids, declarations and decorations
    wbuf ty = {0}, dec = {0};
    #define T_(...) do { uint32_t t_[] = {__VA_ARGS__}; wput(&ty, t_, sizeof t_ / 4); } while (0)
    if (!u32) { u32 = bound++; T_(4u << 16 | OP_TYPE_INT, u32, 32, 0); }
    if (!tbool) { tbool = bound++; T_(2u << 16 | OP_TYPE_BOOL, tbool); }
    if (!pof) { pof = bound++; T_(4u << 16 | OP_TYPE_POINTER, pof, SC_OUTPUT, f32); }
    uint32_t st = bound++, pst = bound++, pu = bound++, pfl = bound++, var = bound++, c[5];
    T_(4u << 16 | OP_TYPE_STRUCT, st, u32, f32); T_(4u << 16 | OP_TYPE_POINTER, pst, SC_PUSH, st);
    T_(4u << 16 | OP_TYPE_POINTER, pu, SC_PUSH, u32); T_(4u << 16 | OP_TYPE_POINTER, pfl, SC_PUSH, f32);
    T_(4u << 16 | OP_VARIABLE, pst, var, SC_PUSH);
    for (uint32_t k = 0; k < 5; k++) { c[k] = bound++; T_(4u << 16 | OP_CONSTANT, u32, c[k], k); }
    #undef T_
    uint32_t d[] = {3u << 16 | OP_DECORATE, st, DEC_BLOCK, 5u << 16 | OP_MEMBER_DECORATE, st, 0, DEC_OFFSET, 0, 5u << 16 | OP_MEMBER_DECORATE, st, 1, DEC_OFFSET, 4};
    wput(&dec, d, sizeof d / 4);
    // the rewrite: decorations before the first type, declarations before the first function, the test before each return of the entry point
    wbuf o = {0}; wput(&o, w, 5);
    uint32_t in_fn = 0;
    for (size_t i = 5; i < n; i += w[i] >> 16) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (i == first_type) wput(&o, dec.w, dec.n);
        if (i == first_fn) wput(&o, ty.w, ty.n);
        if (op == OP_FUNCTION) in_fn = w[i + 2];
        if (op == OP_RETURN && in_fn == entry) {
            uint32_t pf = bound++, f = bound++, pr = bound++, r = bound++, pa = bound++, a = bound++, m = bound++, lt = bound++, eq = bound++, gt = bound++;
            uint32_t b0 = bound++, h0 = bound++, p0 = bound++, b1 = bound++, h1 = bound++, p1 = bound++, b2 = bound++, h2 = bound++, p2 = bound++, q = bound++, pass = bound++;
            uint32_t ok_l = bound++, kill_l = bound++;
            uint32_t s[] = {
                5u << 16 | OP_ACCESS_CHAIN, pu, pf, var, c[0],   4u << 16 | OP_LOAD, u32, f, pf,
                5u << 16 | OP_ACCESS_CHAIN, pfl, pr, var, c[1],  4u << 16 | OP_LOAD, f32, r, pr,
                5u << 16 | OP_ACCESS_CHAIN, pof, pa, (uint32_t)at[0], c[3], 4u << 16 | OP_LOAD, f32, a, pa,
                5u << 16 | OP_ISUB, u32, m, f, c[1],
                5u << 16 | OP_FORD_LESS, tbool, lt, a, r, 5u << 16 | OP_FORD_EQUAL, tbool, eq, a, r, 5u << 16 | OP_FORD_GREATER, tbool, gt, a, r,
                5u << 16 | OP_BITWISE_AND, u32, b0, m, c[1], 5u << 16 | OP_INOT_EQUAL, tbool, h0, b0, c[0], 5u << 16 | OP_LOGICAL_AND, tbool, p0, h0, lt,
                5u << 16 | OP_BITWISE_AND, u32, b1, m, c[2], 5u << 16 | OP_INOT_EQUAL, tbool, h1, b1, c[0], 5u << 16 | OP_LOGICAL_AND, tbool, p1, h1, eq,
                5u << 16 | OP_BITWISE_AND, u32, b2, m, c[4], 5u << 16 | OP_INOT_EQUAL, tbool, h2, b2, c[0], 5u << 16 | OP_LOGICAL_AND, tbool, p2, h2, gt,
                5u << 16 | OP_LOGICAL_OR, tbool, q, p0, p1, 5u << 16 | OP_LOGICAL_OR, tbool, pass, q, p2,
                3u << 16 | OP_SELECTION_MERGE, ok_l, 0, 4u << 16 | OP_BRANCH_COND, pass, ok_l, kill_l,
                2u << 16 | OP_LABEL, kill_l, 1u << 16 | OP_KILL, 2u << 16 | OP_LABEL, ok_l};
            wput(&o, s, sizeof s / 4);
        }
        if (op == OP_FUNCTION_END) in_fn = 0;
        if (op == OP_ENTRY_POINT && w[i + 2] == entry && w[1] >= 0x00010400) {   // SPIR-V 1.4 on: an entry point lists every global it uses
            uint32_t h = (wc + 1) << 16 | op; wput(&o, &h, 1); wput(&o, w + i + 1, wc - 1); wput(&o, &var, 1); continue;
        }
        wput(&o, w + i, wc);
    }
    o.w[3] = bound;
    free(ty.w); free(dec.w);
    *out_n = o.n; return o.w;
}
static const vks_shader *fs_variant_l(const vks_shader *fs_, int flags) {
    vks_shader *fs = (vks_shader *)fs_;
    if (!flags || !fs->words || fs->stage != VKS_FS) return fs;
    uint64_t key = 0x100u | (unsigned)flags;
    for (int i = 0; i < fs->nvar; i++) if (fs->var[i].key == key) return fs->var[i].s;
    if (fs->nvar == 8) return fs;
    size_t n = fs->nwords, nn = 0; uint32_t *w = NULL; int dual = 0, alpha = 0;
    if (flags & VKSH_FS_DUAL) {
        if ((w = vkspv_dual_source(fs->words, n, &nn))) { n = nn; dual = 1; }
        else { static int k; if (k++ < 8) VKRED("shader %s: no dual-source variant (it does not write exactly outputs 0 and 1): the second source counts as zero", fs->key); }
    }
    if (flags & VKSH_FS_ALPHA) {
        uint32_t *a = vkspv_alpha_test(w ? w : fs->words, n, &nn);
        if (a) { free(w); w = a; n = nn; alpha = 1; }
        else { static int k; if (k++ < 8) VKRED("shader %s: no alpha test variant (its colour 0 is not a float vec4): drawn without the test", fs->key); }
    }
    VkShaderModule mod = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, n * 4, w};
    if (!w || vkCreateShaderModule(g_vk.dev, &mci, NULL, &mod) != VK_SUCCESS) { free(w); fs->var[fs->nvar].key = key; fs->var[fs->nvar].s = fs; fs->nvar++; return fs; }   // the base, remembered
    vks_shader *s = malloc(sizeof *s); *s = *fs;
    s->mod = mod; s->words = w; s->nwords = n; s->base = fs; s->nvar = 0; s->dual = dual; s->alpha = alpha; s->fs_flags = (uint8_t)flags;
    if (dual) s->out_mask = fs->out_mask & ~2u;   // output 1 is target 0's second source now, not a target of its own
    fs->var[fs->nvar].key = key; fs->var[fs->nvar].s = s; fs->nvar++;
    static int nl; if (nl++ < 16) VKLOG("fragment shader %s: %s%s%s variant", fs->key, dual ? "dual-source" : "", dual && alpha ? " + " : "", alpha ? "alpha test" : "");
    return s;
}
const vks_shader *vksh_fs_variant(const vks_shader *fs, int flags) {
    if (!flags) return fs;
    plat_rw_wrlock(&g_shl); const vks_shader *r = fs_variant_l(fs, flags); plat_rw_wrunlock(&g_shl);
    return r;
}

// ---------- The c1 bank ----------
static int c1_store(vks_shader *s, const uint8_t *data, uint32_t len) {
    VkDeviceSize need = ((VkDeviceSize)len + 255) & ~(VkDeviceSize)255, align = g_vk.props.limits.minUniformBufferOffsetAlignment;
    if (align < 256) align = 256;
    if (!g_c1.buf || g_c1.used + need > g_c1.size) {
        // a new bank; the old one stays (shaders keep their slices for the run)
        VkDeviceSize sz = 4u << 20; if (need > sz) sz = need;
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, sz, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
        VkBuffer b; vk_alloc m;
        if (vkCreateBuffer(g_vk.dev, &bci, NULL, &b) != VK_SUCCESS) return -1;
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, b, &mr);
        if (vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, &m)) { vkDestroyBuffer(g_vk.dev, b, NULL); return -1; }
        vkBindBufferMemory(g_vk.dev, b, m.mem, m.off);
        memset(m.map, 0, (size_t)sz);
        vk_name((uint64_t)b, VK_OBJECT_TYPE_BUFFER, "c1 bank");
        g_c1.buf = b; g_c1.mem = m; g_c1.used = 0; g_c1.size = sz;
    }
    VkDeviceSize off = (g_c1.used + align - 1) / align * align;
    memcpy((uint8_t *)g_c1.mem.map + off, data, len);
    g_c1.used = off + need;
    s->c1buf = g_c1.buf; s->c1off = off; s->c1_len = len;
    return 0;
}

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

// ---------- From a module and its reflection ----------
static int stage_of(const char *s) {
    return !strcmp(s, "vs") ? VKS_VS : !strcmp(s, "ps") ? VKS_FS : !strcmp(s, "gs") ? VKS_GS : !strcmp(s, "cs") ? VKS_CS : !strcmp(s, "hs") || !strcmp(s, "tcs") ? VKS_TCS : !strcmp(s, "ds") || !strcmp(s, "tes") ? VKS_TES : -1;
}
static VkDescriptorType desc_type(const char *d) {
    if (!strcmp(d, "uniform_buffer")) return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    if (!strcmp(d, "storage_buffer")) return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    if (!strcmp(d, "combined_image_sampler")) return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    if (!strcmp(d, "sampled_image")) return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    if (!strcmp(d, "sampler")) return VK_DESCRIPTOR_TYPE_SAMPLER;
    if (!strcmp(d, "storage_image")) return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    if (!strcmp(d, "uniform_texel_buffer")) return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    if (!strcmp(d, "storage_texel_buffer")) return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    return VK_DESCRIPTOR_TYPE_MAX_ENUM;
}
static int ct_of(const char *t) { return !t ? VKS_CT_FLOAT : !strcmp(t, "uint") ? VKS_CT_UINT : !strcmp(t, "int") ? VKS_CT_INT : VKS_CT_FLOAT; }

void vksh_free(vks_shader *s) {
    if (!s || s == &g_failed_mark) return;
    if (s->mod) vkDestroyShaderModule(g_vk.dev, s->mod, NULL);
    for (int i = 0; i < s->nvar; i++) if (s->var[i].s != s) vksh_free(s->var[i].s);   // a variant that could not be made is the shader itself
    if (s->dsl && !s->base) vkDestroyDescriptorSetLayout(g_vk.dev, s->dsl, NULL);
    if (!s->base) free(s->b);
    free(s->words); free(s);
}

// ---------- OoT visual workaround: scale a uniform-buffer float the shader loads (the exposure of the log-bloom final, Orchid's 0.65) ----------
// vkspv_scale_cbuf_load: every OpLoad of <var>[0][i1][i2] (an access chain on the uniform buffer variable named <var>, member 0, vec4 / array element i1, component i2) is followed by an
// OpFMul with a new float constant: the load's result id moves to a new id and the original id becomes the product, so every later use sees the scaled value. Returns the loads changed
// (0 when the shader has not the expected shape; *words / *n updated when it grew), -1 for malformed SPIR-V
static int vkspv_scale_cbuf_load(uint32_t **words, size_t *n, const char *var, uint32_t i1, uint32_t i2, float factor) {
    uint32_t *w = *words; size_t nw = *n;
    if (!spv_ok(w, nw)) return -1;
    uint32_t bound = w[3], vid = 0; size_t nl = strlen(var);
    uint32_t *cval = calloc(bound, 4); uint8_t *isc = calloc(bound, 1);   // 32-bit constants by id
    if (!cval || !isc) { free(cval); free(isc); return 0; }
    for (size_t i = 5; i < nw; ) {   // names, constants
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff; if (!wc || i + wc > nw) { free(cval); free(isc); return -1; }
        if (op == 5 && wc >= 3 && !vid && (wc - 2) * 4 >= nl + 1 && !strncmp((const char *)&w[i + 2], var, nl) && ((const char *)&w[i + 2])[nl] == 0) vid = w[i + 1];
        else if (op == 43 && wc == 4 && w[i + 2] < bound) { cval[w[i + 2]] = w[i + 3]; isc[w[i + 2]] = 1; }
        i += wc;
    }
    int found = 0; uint32_t *ptrs = NULL; size_t nptr = 0;
    if (vid) {
        ptrs = calloc(4096, 4);
        for (size_t i = 5; ptrs && i < nw; ) {
            uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
            if (op == 65 && wc == 7 && w[i + 3] == vid && w[i + 4] < bound && w[i + 5] < bound && w[i + 6] < bound && isc[w[i + 4]] && isc[w[i + 5]] && isc[w[i + 6]]
                && cval[w[i + 4]] == 0 && cval[w[i + 5]] == i1 && cval[w[i + 6]] == i2 && nptr < 4096) ptrs[nptr++] = w[i + 2];
            i += wc;
        }
    }
    free(cval); free(isc);
    if (!nptr) { free(ptrs); return 0; }
    // the new words: one pass, copying, with an OpConstant before the first OpFunction and an OpFMul after each matching OpLoad
    uint32_t *o = malloc((nw + 4 + nptr * 5) * 4); if (!o) { free(ptrs); return 0; }
    size_t k = 0; uint32_t cid = bound++, ftype = 0; int cdone = 0; union { float f; uint32_t u; } fv = {.f = factor};
    for (size_t i = 0; i < 5; i++) o[k++] = w[i];
    for (size_t i = 5; i < nw; ) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (op == 54 && !cdone) { cdone = 1; o[k++] = 4u << 16 | 43; o[k++] = ftype ? ftype : 0; o[k++] = cid; o[k++] = fv.u; }   // the float type id is fixed below
        if (op == 61 && wc == 4) {
            int hit = 0; for (size_t q = 0; q < nptr; q++) if (ptrs[q] == w[i + 3]) hit = 1;
            if (hit) {
                uint32_t res = w[i + 2], nid = bound++; ftype = w[i + 1];
                o[k++] = w[i] ; o[k++] = w[i + 1]; o[k++] = nid; o[k++] = w[i + 3];
                o[k++] = 5u << 16 | 133; o[k++] = ftype; o[k++] = res; o[k++] = nid; o[k++] = cid;   // OpFMul %float %res %nid %cid
                found++; i += wc; continue;
            }
        }
        for (size_t q = 0; q < wc; q++) o[k++] = w[i + q];
        i += wc;
    }
    free(ptrs);
    if (!found || !ftype || !cdone) { free(o); return 0; }
    // the constant's type: the load's result type (set when the first match was seen, after the OpConstant was written: patch it)
    for (size_t i = 5; i < k; ) { uint32_t wc = o[i] >> 16, op = o[i] & 0xffff; if (op == 43 && wc == 4 && o[i + 2] == cid) { o[i + 1] = ftype; break; } i += wc; }
    o[3] = bound; free(w); *words = o; *n = k;
    return found;
}

// vkspv_scale_slice: the fog integration compute's slice thickness exp2(a) - exp2(b) (an OpFAdd of an exp2 and a negated exp2, the value read straight from the ExtInst or through a function
// variable stored once) is multiplied by <factor> right after it is computed (Orchid's 0.25). Exactly one such add must exist; returns 1 when changed, 0 when the shape differs, -1 malformed
static int vkspv_scale_slice(uint32_t **words, size_t *n, float factor) {
    enum { O_EXT = 12, O_LOAD = 61, O_STORE = 62, O_BITCAST = 124, O_FADD = 129, O_FMUL = 133, O_XOR = 198, O_CONST = 43 };
    uint32_t *w = *words; size_t nw = *n; if (!spv_ok(w, nw)) return -1;
    uint32_t bound = w[3]; uint32_t *op_of = calloc(bound, 4), *at = calloc(bound, 4), *storeval = calloc(bound, 4), *nstore = calloc(bound, 4), *cval = calloc(bound, 4);
    if (!op_of || !at || !storeval || !nstore || !cval) { free(op_of); free(at); free(storeval); free(nstore); free(cval); return 0; }
    uint32_t glsl = 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff; if (!wc || i + wc > nw) { free(op_of); free(at); free(storeval); free(nstore); free(cval); return -1; }
        if (op == 11 && wc >= 3 && !glsl) glsl = w[i + 1];   // OpExtInstImport (assume the first is GLSL.std.450)
        uint32_t res = 0;
        if (op == O_EXT || op == O_LOAD || op == O_BITCAST || op == O_FADD || op == O_XOR || op == O_CONST) res = w[i + 2];
        if (res && res < bound) { op_of[res] = op; at[res] = (uint32_t)i; if (op == O_CONST && wc == 4) cval[res] = w[i + 3]; }
        if (op == O_STORE && wc >= 3 && w[i + 1] < bound) { storeval[w[i + 1]] = w[i + 2]; nstore[w[i + 1]]++; }
        i += wc;
    }
    // value id -> exp2 / negated exp2 (look through single-store variables, bitcasts)
    #define VAL(id) ((id) < bound ? (id) : 0)
    int nmatch = 0; uint32_t fadd_res = 0, fadd_ty = 0; size_t fadd_at = 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (op == O_FADD && wc == 5) {
            int e[2] = {0, 0}, ne[2] = {0, 0};
            for (int q = 0; q < 2; q++) {
                uint32_t v = VAL(w[i + 3 + q]); int neg = 0;
                for (int d = 0; d < 6 && v; d++) {
                    if (op_of[v] == O_LOAD) { uint32_t var = w[at[v] + 3]; v = var < bound && nstore[var] == 1 ? storeval[var] : 0; continue; }
                    if (op_of[v] == O_BITCAST) { v = VAL(w[at[v] + 3]); continue; }   // float <-> int views
                    if (op_of[v] == O_XOR) { uint32_t k = VAL(w[at[v] + 4]); if (k && op_of[k] == O_CONST && cval[k] == 0x80000000u) { neg = 1; v = VAL(w[at[v] + 3]); continue; } v = 0; break; }
                    if (op_of[v] == O_EXT) { if (w[at[v] + 3] == glsl && w[at[v] + 4] == 29) { if (neg) ne[q] = 1; else e[q] = 1; } v = 0; break; }
                    v = 0;
                }
            }
            if ((ne[0] && e[1]) || (ne[1] && e[0])) { nmatch++; fadd_res = w[i + 2]; fadd_ty = w[i + 1]; fadd_at = i; }
        }
        i += wc;
    }
    free(op_of); free(at); free(storeval); free(nstore); free(cval);
    if (nmatch != 1) return 0;
    uint32_t *o = malloc((nw + 12) * 4); if (!o) return 0;
    size_t k = 0; uint32_t cid = bound++, nid = bound++; union { float f; uint32_t u; } fv = {.f = factor}; int cdone = 0;
    for (size_t i = 0; i < 5; i++) o[k++] = w[i];
    for (size_t i = 5; i < nw; ) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (op == 54 && !cdone) { cdone = 1; o[k++] = 4u << 16 | O_CONST; o[k++] = fadd_ty; o[k++] = cid; o[k++] = fv.u; }
        if (i == fadd_at) { o[k++] = w[i]; o[k++] = w[i + 1]; o[k++] = nid; o[k++] = w[i + 3]; o[k++] = w[i + 4]; o[k++] = 5u << 16 | O_FMUL; o[k++] = fadd_ty; o[k++] = fadd_res; o[k++] = nid; o[k++] = cid; i += wc; continue; }
        for (size_t q = 0; q < wc; q++) o[k++] = w[i + q];
        i += wc;
    }
    if (!cdone) { free(o); return 0; }
    o[3] = bound; free(w); *words = o; *n = k;
    return 1;
}

// vkspv_scale_samples: every OpImageSampleImplicitLod of the shader (the tone map's world colour and bloom) has its result clamped to <clampv> (rgb, when > 0) and multiplied by <scale> (rgb; alpha
// untouched): OoT's world HDR target is ~20-30x too bright while the auto-exposure compute (066d63c3) is not translated, and the bloom's ^1.5 makes a plain exposure scale useless. Returns the samples changed
static int vkspv_scale_samples(uint32_t **words, size_t *n, float scale, float clampv) {
    uint32_t *w = *words; size_t nw = *n; if (!spv_ok(w, nw)) return -1;
    uint32_t bound = w[3], glsl = 0, v4t = 0, f32t = 0; int ns = 0;
    for (size_t i = 5; i < nw; ) { uint32_t wc = w[i] >> 16, op = w[i] & 0xffff; if (!wc || i + wc > nw) return -1;
        if (op == 11 && !glsl) glsl = w[i + 1];
        if (op == 87 && wc >= 5) { ns++; v4t = w[i + 1]; }
        if (op == 22 && wc == 3 && w[i + 2] == 32) f32t = w[i + 1];
        i += wc; }
    if (!ns || !glsl || !f32t) return 0;
    uint32_t *o = malloc((nw + 48 + (size_t)ns * 12) * 4); if (!o) return 0;
    uint32_t cs = bound++, c1 = bound++, cc = bound++, cinf = bound++, vs = bound++, vc = bound++;   // scale, 1, clamp, big, (s,s,s,1), (c,c,c,big)
    union { float f; uint32_t u; } fs = {.f = scale}, fc = {.f = clampv > 0 ? clampv : 3.0e38f}, f1 = {.f = 1.0f}, fb = {.f = 3.0e38f};
    size_t k = 0; int cdone = 0, changed = 0; for (size_t i = 0; i < 5; i++) o[k++] = w[i];
    for (size_t i = 5; i < nw; ) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (op == 54 && !cdone) { cdone = 1;
            o[k++] = 4u << 16 | 43; o[k++] = f32t; o[k++] = cs; o[k++] = fs.u;  o[k++] = 4u << 16 | 43; o[k++] = f32t; o[k++] = c1; o[k++] = f1.u;
            o[k++] = 4u << 16 | 43; o[k++] = f32t; o[k++] = cc; o[k++] = fc.u;  o[k++] = 4u << 16 | 43; o[k++] = f32t; o[k++] = cinf; o[k++] = fb.u;
            o[k++] = 7u << 16 | 44; o[k++] = v4t; o[k++] = vs; o[k++] = cs; o[k++] = cs; o[k++] = cs; o[k++] = c1;
            o[k++] = 7u << 16 | 44; o[k++] = v4t; o[k++] = vc; o[k++] = cc; o[k++] = cc; o[k++] = cc; o[k++] = cinf; }
        if (op == 87 && wc >= 5) {
            uint32_t res = w[i + 2], n1 = bound++, n2 = bound++;
            o[k++] = w[i]; o[k++] = w[i + 1]; o[k++] = n1; for (size_t q = 3; q < wc; q++) o[k++] = w[i + q];
            o[k++] = 7u << 16 | 12; o[k++] = w[i + 1]; o[k++] = n2; o[k++] = glsl; o[k++] = 37; o[k++] = n1; o[k++] = vc;   // FMin
            o[k++] = 5u << 16 | 133; o[k++] = w[i + 1]; o[k++] = res; o[k++] = n2; o[k++] = vs;   // FMul
            changed++; i += wc; continue; }
        for (size_t q = 0; q < wc; q++) o[k++] = w[i + q];
        i += wc;
    }
    if (!cdone) { free(o); return 0; }
    o[3] = bound; free(w); *words = o; *n = k; return changed;
}

vks_shader *vksh_make(const char *key, const uint8_t *spv, size_t spv_size, const char *json, size_t json_size, const char **why) {
    *why = NULL;
    vj *j = vj_parse(json, json_size);
    if (!j) { *why = "its reflection is not JSON"; return NULL; }
    vks_shader *s = calloc(1, sizeof *s);
    snprintf(s->key, sizeof s->key, "%s", key);
    s->stage = stage_of(vj_str(vj_get(j, "stage"), ""));
    if (s->stage < 0) { *why = "unknown stage"; goto fail; }
    s->set = vksh_set_of(s->stage);
    size_t nw = spv_size / 4;
    uint32_t *w = malloc(spv_size); memcpy(w, spv, spv_size);
    if (vkspv_set_descriptor_set(w, nw, (uint32_t)s->set) < 0) { free(w); *why = "malformed SPIR-V"; goto fail; }
    {   // OoT visual workaround (docs/FASE5-PRIMEIRA-IMAGEM.md, section 14): NXRT_OOT_EXPOSURE=<factor> scales the exposure constant (cbuf 3 + 0x38) of the log-bloom final fragment shader
        static const struct { const char *key, *var, *env; uint32_t i1, i2; float def; } tab[] = {{"42cd3b20a2171413", "fp_c3", "NXRT_OOT_EXPOSURE", 3, 2, 1.0f}};
        if (!strcmp(key, "2acc27dcb1ee7400")) {   // the froxel fog integration compute: NXRT_OOT_FOG=<factor> scales its slice thickness
            const char *e = getenv("NXRT_OOT_FOG"); float f = e && *e ? (float)atof(e) : 1.0f;
            if (f > 0 && f != 1.0f) { int c = vkspv_scale_slice(&w, &nw, f); VKLOG("OoT visual workaround (NXRT_OOT_FOG=%g): shader %s, slice thickness %s", f, key, c > 0 ? "scaled" : "not found (the shader has not the expected shape: unchanged)"); }
        }
        if (!strcmp(key, "42cd3b20a2171413")) {   // NXRT_OOT_HDR_SCALE=<s> [NXRT_OOT_HDR_CLAMP=<max>]: the world HDR and the bloom are scaled (and clamped) before the tone map
            const char *e = getenv("NXRT_OOT_HDR_SCALE"), *ec = getenv("NXRT_OOT_HDR_CLAMP"); float f = e && *e ? (float)atof(e) : 1.0f, cl = ec && *ec ? (float)atof(ec) : 0.0f;
            if ((f > 0 && f != 1.0f) || cl > 0) { int c = vkspv_scale_samples(&w, &nw, f > 0 ? f : 1.0f, cl); VKLOG("OoT visual workaround (NXRT_OOT_HDR_SCALE=%g, NXRT_OOT_HDR_CLAMP=%g): shader %s, %d sample%s changed", f, cl, key, c, c == 1 ? "" : "s"); }
        }
        for (size_t t = 0; t < sizeof tab / sizeof *tab; t++) {
            if (strcmp(key, tab[t].key)) continue;
            const char *e = getenv(tab[t].env); float f = e && *e ? (float)atof(e) : tab[t].def;
            if (f > 0 && f != 1.0f) { int c = vkspv_scale_cbuf_load(&w, &nw, tab[t].var, tab[t].i1, tab[t].i2, f);
                VKLOG("OoT visual workaround (%s=%g): shader %s, %d load%s scaled%s", tab[t].env, f, key, c, c == 1 ? "" : "s", c > 0 ? "" : " (the shader has not the expected shape: unchanged)"); }
        }
    }
    { int nl = vkspv_break_dispatch_loops(w, nw); if (nl > 0) { static int nlog; if (nlog++ < 32) VKLOG("shader %s: %d indirect-branch dispatch loop%s can no longer spin forever on a target the translator did not list", key, nl, nl > 1 ? "s" : ""); } }
    if (s->stage != VKS_FS && s->stage != VKS_CS && g_vk.props.limits.maxCombinedClipAndCullDistances) {
        size_t tn = vkspv_trim_cull(&w, nw, g_vk.props.limits.maxCombinedClipAndCullDistances);
        if (tn) nw = tn; else { static int n; if (n++ < 4) VKRED("shader %s: more clip and cull distances than the device takes, and the cull ones are used", key); }
    }
    static int ballot_fix = -1;   // NXRT_VK_BALLOT_FIX=0 leaves the ballots as translated (to compare)
    if (ballot_fix < 0) { const char *e = getenv("NXRT_VK_BALLOT_FIX"); ballot_fix = !(e && *e == '0'); }
    if (ballot_fix) {
        int nb = 0; size_t bn = vkspv_ballot_per_lane(&w, nw, &nb);
        if (nb) { nw = bn; VKLOG("shader %s: %d subgroup ballot%s for a counter made per lane (RTX 50 hang, #12)", key, nb, nb > 1 ? "s" : ""); }
    }
    if (s->stage != VKS_FS && s->stage != VKS_CS) {   // NXRT_VK_INVARIANT=1 (opt-in: it did not change OoT's EQUAL depth test, section 24, whose cause was the depth copy cleared): see vkspv_invariant_position
        static int inv = -1; if (inv < 0) { const char *e = getenv("NXRT_VK_INVARIANT"); inv = e && *e == '1'; if (inv) VKLOG("NXRT_VK_INVARIANT=1: vertex programs get NoContraction / Invariant Position"); }
        if (inv) { int nd = 0; size_t nn = vkspv_invariant_position(&w, nw, &nd);
            if (!nn) { free(w); *why = "malformed SPIR-V"; goto fail; }
            if (nn != nw) { nw = nn; static int nlog; if (nlog++ < 8) VKLOG("shader %s: Position invariant, %d FP result%s NoContraction (depth EQUAL against another program's pre-pass)", key, nd, nd == 1 ? "" : "s"); } }
    }
    {   // NXRT_VK_NAN_PRESERVE=1 (off by default: it did not change the OoT NaN, section 22) declares SignedZeroInfNanPreserve 32 on every module, as Eden / Lichii / Orchid do
        static int nanp = -1;
        if (nanp < 0) { const char *e = getenv("NXRT_VK_NAN_PRESERVE"); VkPhysicalDeviceVulkan12Properties p12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
            VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p12}; vkGetPhysicalDeviceProperties2(g_vk.pd, &p2);
            nanp = e && *e == '1' && p12.shaderSignedZeroInfNanPreserveFloat32; if (e && *e == '1') VKLOG("SignedZeroInfNanPreserve (32) on every shader: %s (device %s it)", nanp ? "on" : "off", p12.shaderSignedZeroInfNanPreserveFloat32 ? "has" : "lacks"); }
        if (nanp) { size_t nn = vkspv_nan_preserve(&w, nw); if (!nn) { free(w); *why = "malformed SPIR-V"; goto fail; } nw = nn; }
    }
    // bindings
    const vj *bs = vj_get(j, "bindings");
    s->nb = bs ? bs->n : 0; s->b = calloc(s->nb ? s->nb : 1, sizeof *s->b);
    VkDescriptorSetLayoutBinding *lb = calloc(s->nb ? s->nb : 1, sizeof *lb);
    for (int i = 0; i < s->nb; i++) {
        const vj *e = vj_at(bs, i), *nv = vj_get(e, "nvn"); vks_bind *b = &s->b[i];
        b->binding = (uint32_t)vj_int(vj_get(e, "binding"), 0); b->count = (uint32_t)vj_int(vj_get(e, "count"), 1);
        b->type = desc_type(vj_str(vj_get(e, "descriptor"), ""));
        if (vj_int(vj_get(e, "set"), 0) != 0) { *why = "a descriptor outside set 0"; free(w); free(lb); goto fail; }
        if (b->type == VK_DESCRIPTOR_TYPE_MAX_ENUM) { *why = "an unknown descriptor type"; free(w); free(lb); goto fail; }
        const char *k = vj_str(vj_get(nv, "kind"), "?");
        b->reg = (int16_t)vj_int(vj_get(nv, "reg"), 0); b->bank = (uint8_t)vj_int(vj_get(nv, "bank"), 0);
        b->kind = !strcmp(k, "support") ? VKB_SUPPORT : !strcmp(k, "cbuf") ? VKB_CBUF : !strcmp(k, "c1") ? VKB_C1 : !strcmp(k, "drvcb") ? VKB_DRV : !strcmp(k, "sbuf") ? VKB_SBUF :
                  !strcmp(k, "tex") ? VKB_TEX : !strcmp(k, "tbuf") ? VKB_TBUF : !strcmp(k, "img") ? VKB_IMG : !strcmp(k, "gmem") ? VKB_GMEM :
                  !strcmp(k, "tex_pool") ? VKB_TEXPOOL : !strcmp(k, "samp_pool") ? VKB_SAMPPOOL : VKB_UNKNOWN;
        if (b->kind == VKB_UNKNOWN) { static int n; if (n++ < 16) VKRED("shader %s binding %u: kind %s is not replayed yet (bound as empty)", key, b->binding, k); }
        // A storage buffer at a c0 byte past the program's computed table has a null reg, which reads as slot 0 here. 4,061 bindings of the corpus do (BNSH compute at c0 bytes 272, 336, 352), so slot 0 may well
        // be right for many of them (a single bound buffer in a packed bank): logged, not changed, until a run shows what the game binds (docs FASE5-GMEM)
        if (b->kind == VKB_SBUF && vj_int(vj_get(nv, "reg"), -1) < 0) { static int n; if (n++ < 16) VKLOG("shader %s binding %u: a storage buffer at c0 byte %d has no computed slot: taken as slot 0", key, b->binding, vj_int(vj_get(nv, "c0_offset"), -1)); }
        b->rw = vj_true(vj_get(nv, "rw")); b->c0_off = (uint16_t)vj_int(vj_get(nv, "c0_offset"), 0);
        const vj *sm = vj_get(nv, "sampler");
        if (sm) { const char *sk = vj_str(vj_get(sm, "kind"), ""); b->samp_reg = (int16_t)vj_int(vj_get(sm, "reg"), 0);
            b->samp_kind = !strcmp(sk, "samp_tex") ? VKB_SAMP_TEX : !strcmp(sk, "samp") ? VKB_SAMP_SLOT : !strcmp(sk, "samp_id") ? VKB_SAMP_ID : VKB_SAMP_NONE; }
        else if (b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) { b->samp_kind = VKB_SAMP_TEX; b->samp_reg = b->reg; }
        vkspv_img im; b->vtype = VK_IMAGE_VIEW_TYPE_2D;
        if (!vkspv_image_of(w, nw, b->binding, &im) && im.is_image) { b->dim = im.dim; b->vtype = view_type(im.dim, im.arrayed); b->ct = im.ct; b->depth = im.depth; b->ms = im.ms; }
        lb[i] = (VkDescriptorSetLayoutBinding){b->binding, b->type, b->count, (VkShaderStageFlags)vksh_vk_stage(s->stage), NULL};
    }
    // Switch 2 compute: the c0 bank is [0x10][uniform buffers 16 B each][storage buffers 16 B each][textures]; the counts are in the program's control block (s2_compute_layout nubo / nssbo, tools/s2_ctl_layout.py).
    // The reflection numbers storage descriptors from a base guessed from the uniform buffers the code names (cN, N >= 3), so a uniform buffer read through a storage view was taken for a storage slot and the
    // program's own storage buffers (c0 bytes 272, 336, 352 ...) had no slot and were bound as slot 0. With the counts: a descriptor before 0x10 + 16 * nubo is uniform buffer (off - 0x10) / 16, after it storage buffer (off - base) / 16
    { const vj *s2l = vj_get(j, "s2_compute_layout");
      if (s2l && vj_get(s2l, "nubo") && !getenv("NXRT_VK_S2_OLD_LAYOUT")) {
        int nubo = vj_int(vj_get(s2l, "nubo"), 0), nssbo = vj_int(vj_get(s2l, "nssbo"), 0);
        vkc0_parts cp = vksh_s2_c0_parts(s2l, (unsigned)nubo, (unsigned)nssbo);   // same rule as s2_tex_base below: the roots' room (vk_c0.h) sits between the head and the uniform buffers
        int base = (int)cp.ssbo_base, ubase = (int)cp.ubo_base;
        for (int i = 0; i < s->nb; i++) if (s->b[i].kind == VKB_SBUF) {
            int off = vj_int(vj_get(vj_get(vj_at(bs, i), "nvn"), "c0_offset"), -1); vks_bind *b = &s->b[i];
            if (off < 0x10 || (off - 0x10) % 16) continue;
            if (off < ubase) continue;   // inside the root tables: words, not a buffer
            if (off < base) { b->from_ubo = 1; b->reg = (int16_t)((off - ubase) / 16); }
            else { int r = (off - base) / 16; b->from_ubo = 0; b->reg = (int16_t)(r < nssbo && r < 16 ? r : 16); }   // 16: no slot (the replay keeps 16)
            b->remapped = 1;
        } } }
    // untracked global memory: the words the reflection found its pointers in (nxs_spv.py GlobalMemoryBda.roots), as storage slots of this shader
    for (int i = 0; i < s->nb; i++) if (s->b[i].kind == VKB_GMEM) {
        const vj *nv = vj_get(vj_at(bs, i), "nvn"), *rts = vj_get(nv, "roots");
        s->gmem_open = vj_true(vj_get(nv, "open"));
        for (int r = 0; rts && r < rts->n; r++) {
            const vj *e = vj_at(rts, r); uint32_t bd = (uint32_t)vj_int(vj_at(e, 0), 0), word = (uint32_t)vj_int(vj_at(e, 1), 0); int reg = -1;
            int ubo = 0; for (int k = 0; k < s->nb; k++) if (s->b[k].binding == bd && s->b[k].kind == VKB_SBUF) { reg = s->b[k].remapped ? s->b[k].reg : vj_int(vj_get(vj_get(vj_at(bs, k), "nvn"), "reg"), -1); ubo = s->b[k].from_ubo; }   // only a slot the reflection is sure of
            if (reg < 0 || reg >= 16 || s->gmem_nroots >= 16) { s->gmem_open = 1; continue; }   // a storage buffer the replay has no slot for: its contents cannot be read
            s->gmem_root[s->gmem_nroots].reg = (uint16_t)reg; s->gmem_root[s->gmem_nroots].ubo = (uint8_t)ubo; s->gmem_root[s->gmem_nroots++].word = word;
        }
    }
    // The bindless pools (tex_pool: a SAMPLED_IMAGE array indexed by the guest's texture handle, samp_pool: a SAMPLER array by its sampler field) move out of the shader's set into the
    // persistent pool set (VKS_POOL_SET, vksh_pool_dsl): binding 0 the samplers, binding 1 the images, the arrays no longer than the device's update-after-bind limits. The translator
    // numbers the bindings after them by the arrays' lengths (1,052,698 ...): the remaining bindings are renumbered densely
    int nlb = s->nb, npool = 0;
    for (int i = 0; i < s->nb; i++) npool += s->b[i].kind == VKB_TEXPOOL || s->b[i].kind == VKB_SAMPPOOL;
    if (npool) {
        uint32_t ntex = 0, nsamp = 0; VkDescriptorSetLayout pd = vksh_pool_dsl(&ntex, &nsamp);
        int ok = pd != VK_NULL_HANDLE;
        for (int i = 0; i < s->nb && ok; i++) { const vks_bind *b = &s->b[i];
            ok = !((b->kind == VKB_TEXPOOL && b->type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) || (b->kind == VKB_SAMPPOOL && b->type != VK_DESCRIPTOR_TYPE_SAMPLER)); }
        if (!ok) {   // no pool set: the draws are skipped, as before
            for (int i = 0; i < s->nb; i++) { const vks_bind *b = &s->b[i]; if (b->count > 256 && b->count > s->big_array) { s->big_array = b->count; s->big_binding = b->binding; } }
        } else {
            vkspv_remap_t *rm = calloc(s->nb, sizeof *rm); nlb = 0;
            for (int i = 0; i < s->nb; i++) {
                vks_bind *b = &s->b[i];
                if (b->kind == VKB_TEXPOOL || b->kind == VKB_SAMPPOOL) {
                    int t = b->kind == VKB_TEXPOOL; rm[i] = (vkspv_remap_t){b->binding, VKS_POOL_SET, t ? 1u : 0u, t ? ntex : nsamp};
                    if (b->count > (t ? ntex : nsamp)) { static int n; if (n++ < 4) VKLOG("shader %s: its %s array of %u elements is cut to %u (the device's update-after-bind limit)", key, t ? "tex_pool" : "samp_pool", b->count, t ? ntex : nsamp); }
                    b->binding = t ? 1u : 0u; continue;
                }
                uint32_t rank = 0; for (int q = 0; q < s->nb; q++) if (s->b[q].kind != VKB_TEXPOOL && s->b[q].kind != VKB_SAMPPOOL && s->b[q].binding < b->binding) rank++;
                rm[i] = (vkspv_remap_t){b->binding, (uint32_t)s->set, rank, 0}; b->binding = rank;
                lb[nlb++] = (VkDescriptorSetLayoutBinding){rank, b->type, b->count, (VkShaderStageFlags)vksh_vk_stage(s->stage), NULL};
            }
            int rr = vkspv_remap_bindings(&w, &nw, rm, s->nb); free(rm);
            if (rr < 0) { *why = "the bindless pool bindings could not be remapped"; free(w); free(lb); goto fail; }
            s->pool = 1;
        }
    }
    // interface
    const vj *in = vj_get(j, "inputs");
    for (int i = 0; in && i < in->n && s->stage == VKS_VS && s->nin < 32; i++) {
        const vj *e = vj_at(in, i);
        s->in[s->nin].loc = (uint8_t)vj_int(vj_get(e, "location"), 0); s->in[s->nin].ct = (uint8_t)ct_of(vj_str(vj_get(e, "type"), "float"));
        s->in[s->nin].ncomp = (uint8_t)vj_int(vj_get(e, "components"), 4); s->nin++;
    }
    const vj *out = vj_get(j, "outputs");
    for (int i = 0; out && i < out->n && s->stage == VKS_FS; i++) {
        const vj *e = vj_at(out, i); int l = vj_int(vj_get(e, "location"), -1);
        if (l >= 0 && l < 8) { s->out_mask |= 1u << l; s->out_ct[l] = (uint8_t)ct_of(vj_str(vj_get(e, "type"), "float")); }
    }
    s->uses_subgroups = vj_true(vj_get(j, "uses_subgroups")) ? 1 : 0;
    const vj *ls = vj_get(j, "local_size");
    for (int i = 0; i < 3; i++) s->local[i] = (uint32_t)vj_int(vj_at(ls, i), 1);
    const vj *dr = vj_get(j, "drv_reads");
    for (int i = 0; dr && i < dr->n; i++) { int bank = atoi(dr->keys[i]); if (bank >= 0 && bank < 3) s->drv_read[bank] = 1; }
    const vj *s2 = vj_get(j, "s2_compute_layout");
    if (s2) { s->s2cs = 1; s->s2_ssbo_base = (uint32_t)vj_int(vj_get(s2, "ssbo_base"), 0); s->s2_tex_base = (uint32_t)vj_int(vj_get(s2, "tex_base"), 0); s->s2_ntex = (uint32_t)vj_int(vj_get(s2, "n_tex"), 0);
        if (vj_get(s2, "nubo") && !getenv("NXRT_VK_S2_OLD_LAYOUT")) {
            // The control block's uniform / storage counts (bytes 0xb1 / 0xb2) miss entries some programs keep before the storage table (70 of the 3,967 OoT compute programs, among them the
            // auto exposure pair b9932240 / 3f1e6bf1: 8 more 16-byte entries, so their counter buffer sat past the table and read as address 0). The bank size in the same block (ctl 0xb8, high
            // half) minus its texture / image words and the code's own tex_base agree on where the textures start (3,944 of 3,967); the storage table is the nssbo entries just before them
            vkc0_parts cp = vksh_s2_c0_parts(s2, (unsigned)vj_int(vj_get(s2, "nubo"), 0), (unsigned)vj_int(vj_get(s2, "nssbo"), 0));
            s->s2_tex_base = cp.tex_base; s->s2_ssbo_base = cp.ssbo_base; s->s2_ubo_base = cp.ubo_base; s->s2_has_roots = (uint8_t)vksh_s2_roots(s2, s->s2_roots); }
        else s->s2_ubo_base = 0x10; }
    // c1: the bytes nxshader took from the shader's record (its translation job), published beside the reflection
    int has_c1 = 0; for (int i = 0; i < s->nb; i++) has_c1 |= s->b[i].kind == VKB_C1;
    if (has_c1) {
        const char *hex = vj_str(vj_get(j, "c1"), NULL);
        if (!hex) { s->c1_missing = 1; static int n; if (n++ < 8) VKRED("shader %s reads its c1 segment but the reflection has no \"c1\" (an older nxshader publish): it reads zeros", key); }
        else {
            size_t hl = strlen(hex) / 2; uint8_t *c1 = malloc(hl ? hl : 1);
            for (size_t k = 0; k < hl; k++) { int a = hexval(hex[2 * k]), b = hexval(hex[2 * k + 1]); c1[k] = (uint8_t)(a < 0 || b < 0 ? 0 : a * 16 + b); }
            if (hl && c1_store(s, c1, (uint32_t)hl)) VKRED("shader %s: no memory for its c1 segment", key);
            free(c1);
        }
    }
    // Vulkan objects
    VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, nw * 4, w};
    VkResult r = vkCreateShaderModule(g_vk.dev, &mci, NULL, &s->mod);
    if (s->stage == VKS_VS || s->stage == VKS_FS) { s->words = w; s->nwords = nw; } else free(w);   // the sources of their variants
    if (r != VK_SUCCESS) { free(lb); *why = "vkCreateShaderModule failed"; goto fail; }
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, (uint32_t)nlb, lb};
    r = vkCreateDescriptorSetLayout(g_vk.dev, &dci, NULL, &s->dsl);
    free(lb);
    if (r != VK_SUCCESS) { *why = "vkCreateDescriptorSetLayout failed"; goto fail; }
    vj_free(j);
    return s;
fail:
    vj_free(j); vksh_free(s);
    return NULL;
}

static vks_shader *get_l(const nxs_shader *n) {   // g_shl held
    vks_shader *s = vk_hm_get(&g_sh, n->key);
    if (s == &g_failed_mark) return NULL;
    if (!s) {
        const char *why = NULL;
        s = vksh_make(n->key, n->spv, n->spv_size, n->json, n->json_size, &why);
        if (!s) { g_failed++; VKRED("shader %s cannot be used: %s; its draws are skipped", n->key, why ? why : "?"); vk_hm_put(&g_sh, n->key, &g_failed_mark); return NULL; }
        g_loaded++;
        vk_hm_put(&g_sh, n->key, s);
        { char k[128]; int o = 0; int cnt[VKB_UNKNOWN + 1] = {0}; for (int i = 0; i < s->nb; i++) cnt[s->b[i].kind]++;
          static const char *nm[] = {"", "support", "cbuf", "c1", "drv", "sbuf", "tex", "tbuf", "img", "texpool", "samppool", "gmem", "?"};
          for (int i = 1; i <= VKB_UNKNOWN; i++) if (cnt[i]) o += snprintf(k + o, sizeof k - o, " %s %d", nm[i], cnt[i]);
          VKLOG("shader %s (%s):%s%s%s", n->key, s->stage == VKS_VS ? "vertex" : s->stage == VKS_FS ? "fragment" : s->stage == VKS_CS ? "compute" : s->stage == VKS_GS ? "geometry" : "tessellation", k,
                s->drv_read[0] || s->drv_read[2] ? " (reads driver segments)" : "", s->s2cs ? " (Switch 2 compute bank)" : s->pool ? " (bindless pools)" : ""); }
    }
    s->uses++;
    return s;
}
const vks_shader *vksh_get(const prog_t *p, unsigned i) {
    const nxs_shader *n = NULL;
    if (!p || nxs_resolve(p, i, &n) != 1 || !n) return NULL;
    plat_rw_wrlock(&g_shl); vks_shader *s = get_l(n); plat_rw_wrunlock(&g_shl);
    return s;
}
const vks_shader *vksh_by_key(const char *key) {
    const nxs_shader *n = nxs_peek(key);
    if (!n) return NULL;
    plat_rw_wrlock(&g_shl); vks_shader *s = get_l(n); plat_rw_wrunlock(&g_shl);
    return s;
}

// The pool set's layout (made in vksh_init, read-only afterwards)
static VkDescriptorSetLayout g_pdsl; static uint32_t g_pntex, g_pnsamp;
VkDescriptorSetLayout vksh_pool_dsl(uint32_t *ntex, uint32_t *nsamp) { if (ntex) *ntex = g_pntex; if (nsamp) *nsamp = g_pnsamp; return g_pdsl; }
static void pool_dsl_make(void) {
    const char *e = getenv("NXRT_VK_BINDLESS");
    if (e && e[0] == '0') { VKLOG("bindless pools: off (NXRT_VK_BINDLESS=0): shaders that read tex_pool / samp_pool are skipped"); return; }
    const VkPhysicalDeviceVulkan12Features *f = &g_vk.f12;
    if (!f->descriptorBindingSampledImageUpdateAfterBind || !f->descriptorBindingPartiallyBound || !f->descriptorBindingVariableDescriptorCount || !f->runtimeDescriptorArray) {
        VKRED("bindless pools: the device lacks update-after-bind / partially bound / variable descriptor count: shaders that read tex_pool / samp_pool are skipped"); return; }
    VkPhysicalDeviceVulkan12Properties p12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p12};
    vkGetPhysicalDeviceProperties2(g_vk.pd, &p2);
    if (g_vk.props.limits.maxBoundDescriptorSets <= VKS_POOL_SET) { VKRED("bindless pools: the device binds only %u descriptor sets", g_vk.props.limits.maxBoundDescriptorSets); return; }
    // The pool set's sampled images count against the per-stage update-after-bind limits next to every shader's own combined image samplers, its samplers next to theirs and the
    // per-stage resource total: leave the shaders room (their sets hold a few dozen of each)
    uint32_t lim_img = p12.maxDescriptorSetUpdateAfterBindSampledImages < p12.maxPerStageDescriptorUpdateAfterBindSampledImages ? p12.maxDescriptorSetUpdateAfterBindSampledImages : p12.maxPerStageDescriptorUpdateAfterBindSampledImages;
    uint32_t lim_smp = p12.maxDescriptorSetUpdateAfterBindSamplers < p12.maxPerStageDescriptorUpdateAfterBindSamplers ? p12.maxDescriptorSetUpdateAfterBindSamplers : p12.maxPerStageDescriptorUpdateAfterBindSamplers;
    uint32_t res = p12.maxPerStageUpdateAfterBindResources;
    uint32_t nsamp = lim_smp > 1024 ? lim_smp - 1024 : 0; if (nsamp > 4096) nsamp = 4096;
    uint32_t ntex = lim_img > 1024 ? lim_img - 1024 : 0; if (ntex > 1048576) ntex = 1048576;
    if (res > nsamp + 2048 && ntex > res - nsamp - 2048) ntex = res - nsamp - 2048;
    if (ntex < 4096 || nsamp < 256) { VKRED("bindless pools: the device's update-after-bind limits leave %u images / %u samplers: too small", ntex, nsamp); return; }
    const VkShaderStageFlags st = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutBinding lb[2] = {{0, VK_DESCRIPTOR_TYPE_SAMPLER, nsamp, st, NULL}, {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, ntex, st, NULL}};
    VkDescriptorBindingFlags bf[2] = {VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,   // (the sampled-image feature covers samplers)
                                      VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT};
    VkDescriptorSetLayoutBindingFlagsCreateInfo fi = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, NULL, 2, bf};
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &fi, VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, 2, lb};
    if (vkCreateDescriptorSetLayout(g_vk.dev, &dci, NULL, &g_pdsl) != VK_SUCCESS) { g_pdsl = VK_NULL_HANDLE; VKRED("bindless pools: the pool set layout could not be created: those shaders are skipped"); return; }
    g_pntex = ntex; g_pnsamp = nsamp;
    VKLOG("bindless pools: a persistent set of up to %u sampled images and %u samplers (update-after-bind, partially bound, variable count); device limits: set %u / %u, per stage %u / %u, resources %u",
          ntex, nsamp, p12.maxDescriptorSetUpdateAfterBindSampledImages, p12.maxDescriptorSetUpdateAfterBindSamplers, p12.maxPerStageDescriptorUpdateAfterBindSampledImages, p12.maxPerStageDescriptorUpdateAfterBindSamplers, res);
}
int vksh_init(void) {
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL};
    if (vkCreateDescriptorSetLayout(g_vk.dev, &dci, NULL, &g_empty) != VK_SUCCESS) return -1;
    pool_dsl_make();
    return 0;
}
void vksh_shutdown(void) {
    for (uint32_t i = 0; i < g_sh.cap; i++) if (g_sh.vals[i]) vksh_free(g_sh.vals[i]);
    vk_hm_free(&g_sh);
    if (g_empty) vkDestroyDescriptorSetLayout(g_vk.dev, g_empty, NULL);
    g_empty = VK_NULL_HANDLE;
    if (g_pdsl) vkDestroyDescriptorSetLayout(g_vk.dev, g_pdsl, NULL);
    g_pdsl = VK_NULL_HANDLE; g_pntex = g_pnsamp = 0;
}
