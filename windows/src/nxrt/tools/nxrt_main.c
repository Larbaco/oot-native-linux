/* nxrt.exe: the runtime. It reserves the guest address space, logs the banner, loads and maps the game's modules from an ExeFS folder, binds the host
   replacements and, given the game DLL, runs the bootstrap (nn::init::Start's order) through nnMain on a guest main thread.
   nxrt <exefs folder> [--dll <game.dll>] [--romfs <RomFS.img or folder>] [--save <folder>] [--temp <folder>] [--log <folder>] [--gfx null|vulkan] [--quiet]
   --romfs / --save / --temp set DM_ROMFS / DM_SAVE / DM_TEMP for hle_fs.c (the save folder is a scratch copy: the game writes there).
   --gfx picks the graphics backend (default null, or NXRT_GFX; src/nxrt/gfx/gfx_ops.h).
   --abi-info prints the runtime ABI this binary was built with (constants and the host-call registry) and exits; it loads no game. */
#ifdef _WIN32
#include <windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nxrt/runtime.h"
#include "plat.h"
#ifdef _WIN32
static int setenv(const char *n, const char *v, int o) { (void)o; return _putenv_s(n, v); }
#endif

NXRT_EARLY_RESERVE()

void nxrt_guest_main(void);
int nxrt_gfx_select(const char *name);   /* gfx/gfx_select.c */
void nxrt_abi_info(FILE *out);           /* host/hle.c */
uint32_t plat_thread_start(unsigned (*fn)(void *), void *arg, size_t stack_reserve);

static unsigned guest_main_thread(void *arg) { (void)arg; nxrt_guest_main(); nxrt_log_summary(); return 0; }

int main(int argc, char **argv) {
    const char *exefs = NULL, *dll = NULL, *gfx = NULL;
    nxrt_options o = {0};
    int abi_info = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dll") && i + 1 < argc) dll = argv[++i];
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) o.log_dir = argv[++i];
        else if (!strcmp(argv[i], "--romfs") && i + 1 < argc) setenv("DM_ROMFS", argv[++i], 1);
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) setenv("DM_SAVE", argv[++i], 1);
        else if (!strcmp(argv[i], "--temp") && i + 1 < argc) setenv("DM_TEMP", argv[++i], 1);
        else if (!strcmp(argv[i], "--gfx") && i + 1 < argc) gfx = argv[++i];
        else if (!strcmp(argv[i], "--quiet")) o.quiet = 1;
        else if (!strcmp(argv[i], "--abi-info")) abi_info = 1;
        else if (!exefs && argv[i][0] != '-') exefs = argv[i];
        else { fprintf(stderr, "usage: nxrt <exefs folder> [--dll <game.dll>] [--romfs <image>] [--save <folder>] [--temp <folder>] [--log <folder>] [--gfx null|vulkan] [--quiet] [--abi-info]\n"); return 2; }
    }
    if (abi_info) { nxrt_abi_info(stdout); return 0; }
    if (nxrt_init(&o)) return 3;
    if (nxrt_gfx_select(gfx)) { fprintf(stderr, "unknown graphics backend: %s (see the log)\n", gfx ? gfx : getenv("NXRT_GFX")); return 2; }
    if (!exefs) { nxrt_log_summary(); fprintf(stderr, "no ExeFS folder given: address space and log checked, nothing loaded\n"); return 0; }
    if (nxrt_load_program(exefs)) return 4;
    nxrt_log_summary();
    if (!dll) { fprintf(stderr, "no game DLL given (--dll): the modules are mapped and bound; guest code cannot run without the recompiled DLL\n"); return 0; }
    if (nxrt_attach_game_dll(dll)) return 5;
    /* The guest main thread gets a large host stack: recompiled frames nest as deep as the guest's calls */
    if (!plat_thread_start(guest_main_thread, NULL, 64u << 20)) return 6;
    for (;;) plat_sleep_ms(1000);   /* the game ends the process (svcExitProcess / Break) */
}
