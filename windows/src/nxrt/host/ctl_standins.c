// What the ported hle_hid.c (DM_CTL) and hle_fs.c call in nxnative files that are not ported: the renderer's texture and pass dumps (nvn_mtl.m), the
// Unreal cvar writes (hle_fpslock.c) and the MiiModel stand-in (hle_mii.c). peek and memdump read guest memory as nxnative's do; the rest say what is missing.
// A renderer that has screenshots and dumps sets dm_gfx_ctl when it starts (the Vulkan backend: gfx_vk.c); the null backend leaves it empty.
#include "dm.h"
#include "ctl_gfx.h"

dm_gfx_ctl_ops dm_gfx_ctl;

void dm_ctl_peek(uint64_t a, int n) {
    for (int i = 0; i < n && i < 64; i++) {
        float f; if (plat_read(a + 4 * (uint64_t)i, &f, 4)) { LOG("DM_CTL peek %#llx: unreadable", (unsigned long long)(a + 4 * (uint64_t)i)); return; }
        LOG("DM_CTL peek %#llx: %g", (unsigned long long)(a + 4 * (uint64_t)i), f);
    }
}
void dm_ctl_memdump(uint64_t a, uint64_t n, const char *path) {
    FILE *f = fopen(path, "wb"); if (!f) { LOG("[RED] DM_CTL memdump: cannot write %s", path); return; }
    uint8_t buf[4096]; uint64_t done = 0;
    while (done < n) { size_t k = n - done < sizeof buf ? (size_t)(n - done) : sizeof buf; if (plat_read(a + done, buf, k)) break; fwrite(buf, 1, k, f); done += k; }
    fclose(f); LOG("DM_CTL memdump %#llx: %llu of %llu bytes → %s", (unsigned long long)a, (unsigned long long)done, (unsigned long long)n, path);
}
void dm_ctl_cvar_i32(uint64_t off, int32_t v) { LOG("DM_CTL cvari %#llx = %d: no engine cvars on this host (hle_fpslock.c is not ported)", (unsigned long long)off, v); }
void dm_ctl_cvar_f32(uint64_t off, float v) { LOG("DM_CTL cvarf %#llx = %g: no engine cvars on this host (hle_fpslock.c is not ported)", (unsigned long long)off, v); }
void dm_gfx_texlist(int target) { LOG("DM_CTL texlist %d: no renderer yet", target); }
void dm_gfx_texraw(uint64_t serial, const char *path) { LOG("DM_CTL texraw %llu → %s: no renderer yet", (unsigned long long)serial, path); }
void dm_gfx_texdump_next(const char *path) {
    if (dm_gfx_ctl.texdump_next) dm_gfx_ctl.texdump_next(path); else LOG("DM_CTL texdump → %s: this renderer has no texture dumps", path);
}
void dm_gfx_passdump_next(long n) {
    if (dm_gfx_ctl.passdump_next) dm_gfx_ctl.passdump_next(n); else LOG("DM_CTL passdump %ld: this renderer has no pass dumps", n);
}
void dm_gfx_shot_at(const char *path) {
    if (dm_gfx_ctl.shot_at) dm_gfx_ctl.shot_at(path); else LOG("DM_CTL: no screenshot of %s: this renderer has none (--gfx vulkan has)", path);
}
int dm_mii_standin(const char *dir) { LOG("[RED] fs: MiiModel stand-in for %s: hle_mii.c is not ported", dir); return 0; }
