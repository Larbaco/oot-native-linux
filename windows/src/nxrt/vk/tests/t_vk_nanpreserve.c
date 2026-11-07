/* vkspv_nan_preserve: SignedZeroInfNanPreserve 32 (and SPV_KHR_float_controls under SPIR-V 1.4) added to every entry point, once. CPU only: the module is checked word by word
   and, when spirv-val is on the PATH, validated (Vulkan 1.3). */
#include "vk_test.h"
#include "vk_shader.h"

NXRT_EARLY_RESERVE()

// a fragment shader, SPIR-V 1.3 (spirv-as of: OpEntryPoint Fragment, OriginUpperLeft, one OpFMul)
static const uint32_t frag13[] = {0x07230203, 0x00010300, 0x00070000, 0x0000000c, 0x00000000, 0x00020011, 0x00000001, 0x0003000e, 0x00000000, 0x00000001, 0x0007000f, 0x00000004, 0x00000001, 0x6e69616d, 0x00000000, 0x00000002, 0x00000003, 0x00030010, 0x00000001, 0x00000007, 0x00040047, 0x00000002, 0x0000001e, 0x00000000, 0x00040047, 0x00000003, 0x0000001e, 0x00000000, 0x00020013, 0x00000004, 0x00030021, 0x00000005, 0x00000004, 0x00030016, 0x00000006, 0x00000020, 0x00040020, 0x00000007, 0x00000003, 0x00000006, 0x00040020, 0x00000008, 0x00000001, 0x00000006, 0x0004003b, 0x00000007, 0x00000002, 0x00000003, 0x0004003b, 0x00000008, 0x00000003, 0x00000001, 0x00050036, 0x00000004, 0x00000001, 0x00000000, 0x00000005, 0x000200f8, 0x00000009, 0x0004003d, 0x00000006, 0x0000000a, 0x00000003, 0x00050085, 0x00000006, 0x0000000b, 0x0000000a, 0x0000000a, 0x0003003e, 0x00000002, 0x0000000b, 0x000100fd, 0x00010038};

static int count_op(const uint32_t *w, size_t n, uint32_t op) { int c = 0; for (size_t i = 5; i < n && (w[i] >> 16); i += w[i] >> 16) c += (w[i] & 0xffff) == op; return c; }
static int has_mode(const uint32_t *w, size_t n) { for (size_t i = 5; i < n && (w[i] >> 16); i += w[i] >> 16) if ((w[i] & 0xffff) == 16 && w[i + 2] == 4461 && w[i + 3] == 32) return 1; return 0; }
static int validate(const uint32_t *w, size_t n, const char *name) {
    char p[256]; snprintf(p, sizeof p, "/tmp/nxrt_nanpreserve_%s.spv", name); FILE *f = fopen(p, "wb"); if (!f) return -1; fwrite(w, 4, n, f); fclose(f);
    if (system("spirv-val --version >/dev/null 2>&1")) return -1;
    char cmd[400]; snprintf(cmd, sizeof cmd, "spirv-val --target-env vulkan1.3 %s", p); int rc = system(cmd); rc = rc == 0; if (!rc) { snprintf(cmd, sizeof cmd, "spirv-dis %s | head -12", p); system(cmd); } remove(p); return rc;
}

int main(void) {
    size_t n = sizeof frag13 / 4; uint32_t *w = malloc(n * 4); memcpy(w, frag13, n * 4);
    CHECK(count_op(w, n, 17) == 1 && count_op(w, n, 10) == 0 && !has_mode(w, n), "the test module has one capability, no extension, no float-controls mode");
    size_t m = vkspv_nan_preserve(&w, n);
    CHECK(m > n, "the module grew (%zu -> %zu words)", n, m);
    CHECK(count_op(w, m, 17) == 2 && count_op(w, m, 10) == 1 && count_op(w, m, 16) == 2 && has_mode(w, m), "capability, SPV_KHR_float_controls (1.3 needs it) and SignedZeroInfNanPreserve 32 are there");
    int v = validate(w, m, "a"); CHECK(v != 0, "the module validates (spirv-val: %d, -1 = not available)", v);
    size_t again = vkspv_nan_preserve(&w, m);
    CHECK(again == m && count_op(w, m, 16) == 2, "a second call changes nothing");
    // SPIR-V 1.4: the mode needs no extension
    uint32_t *w14 = malloc(n * 4); memcpy(w14, frag13, n * 4); w14[1] = 0x00010400; size_t m14 = vkspv_nan_preserve(&w14, n);
    CHECK(m14 > n && count_op(w14, m14, 10) == 0 && has_mode(w14, m14), "1.4: capability and mode, no extension (%zu words)", m14);
    // truncated / not a module
    uint32_t junk[6] = {0x07230203u, 0x10300, 0, 99, 0, 0x40001}; uint32_t *jp = malloc(sizeof junk); memcpy(jp, junk, sizeof junk);
    CHECK(vkspv_nan_preserve(&jp, 6) == 0, "a module whose last instruction runs past the end is refused");
    free(w); free(w14); free(jp);
    printf("nanpreserve: %d checks, %d failed\n", g_checks, g_fail); return g_fail ? 1 : 0;
}
