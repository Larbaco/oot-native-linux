/* The bootstrap's order on the real modules (NXW_DUMP; skipped without it), as the Switch loader and nn::init::Start run it and nxnative replays it
   (dm/host/main.c): a recorder is registered as the guest function at each step's address, then nxrt_guest_main runs. Expected: _init_libc0,
   nnosInitialize(main thread handle, 0), _init_libc1, nninitInitializeSdkModule, SetHostArgc(1), SetHostArgv(["game"]), nninitStartup, _init_libc2,
   main's, subsdk0's and sdk's DT_INIT in load order, nnMain; each entered on the guest main thread with x30 = RETURN_TO_HOST and a guest stack. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nxrt/runtime.h"

NXRT_EARLY_RESERVE()

static int g_fail;
#define T(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

enum { NSTEP = 12 };
static const char *const k_names[NSTEP] = {
    "_init_libc0", "nnosInitialize", "_init_libc1", "nninitInitializeSdkModule", "_ZN2nn2os11SetHostArgcEi", "_ZN2nn2os11SetHostArgvEPPc",
    "nninitStartup", "_init_libc2", "DT_INIT main", "DT_INIT subsdk0", "DT_INIT sdk", "nnMain"};
static int g_seen[64], g_n;
static uint64_t g_x0[NSTEP], g_x1[NSTEP];
static void record(int k, NxwGuestCpu *c) {
    if (g_n < 64) g_seen[g_n++] = k;
    g_x0[k] = c->x[0]; g_x1[k] = c->x[1];
    const nxrt_layout *l = nxrt_as_layout();
    T(c == nxrt_cur_cpu());
    T(c->x[30] == NXRT_RETURN_TO_HOST);
    T(c->sp > l->stack && c->sp <= l->stack + l->stack_size && !(c->sp & 15));   /* the main thread's stack ends at the top of the stack region */
    T(c->tpidrro_el0 != 0);
    if (g_fail) printf("step %d: x30 %#llx sp %#llx (stack %#llx+%#llx) tpidrro %#llx\n", k, (unsigned long long)c->x[30], (unsigned long long)c->sp,
                       (unsigned long long)l->stack, (unsigned long long)l->stack_size, (unsigned long long)c->tpidrro_el0);
}
#define REC(k) static void rec##k(NxwGuestCpu *c) { record(k, c); }
REC(0) REC(1) REC(2) REC(3) REC(4) REC(5) REC(6) REC(7) REC(8) REC(9) REC(10) REC(11)
static const NxwGuestFn k_rec[NSTEP] = {rec0, rec1, rec2, rec3, rec4, rec5, rec6, rec7, rec8, rec9, rec10, rec11};

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *dump = getenv("NXW_DUMP");
    if (!dump || !*dump) { printf("NXW_DUMP not set: skipped\n"); return 77; }
    if (nxrt_init(&(nxrt_options){.quiet = 1})) { printf("FAIL nxrt_init\n"); return 1; }
    if (nxrt_load_program(dump)) { printf("FAIL loading %s\n", dump); return 1; }
    uint8_t *img = (uint8_t *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(img + ((IMAGE_DOS_HEADER *)img)->e_lfanew);
    T(nxrt_code_attach(img, nt->OptionalHeader.SizeOfImage) == 0);
    for (int k = 0; k < NSTEP; k++) {
        uint64_t va = 0;
        if (!strncmp(k_names[k], "DT_INIT ", 8)) {
            nxrt_module_info m; T(nxrt_module_get(k - 8, &m) == 0 && !strcmp(m.name, k_names[k] + 8) && m.init);
            va = m.base + m.init;
        } else va = nxrt_guest_sym(k_names[k]);
        T(va != 0);
        T(nxrt_code_set(va, k_rec[k]) == 0);
    }
    nxrt_guest_main();
    printf("order:");
    for (int i = 0; i < g_n; i++) printf(" %s", k_names[g_seen[i]]);
    printf("\n");
    T(g_n == NSTEP);
    for (int i = 0; i < g_n && i < NSTEP; i++) T(g_seen[i] == i);
    T(g_x0[1] != 0 && g_x1[1] == 0);                 /* nnosInitialize(main thread handle, argument area) */
    T(g_x0[4] == 1);                                 /* SetHostArgc(1) */
    const char *const *argv = (const char *const *)(uintptr_t)g_x0[5];
    T(argv && argv[0] && !strcmp(argv[0], "game"));  /* SetHostArgv: argv[0] is "game", as nxnative's */
    printf(g_fail ? "nxrt.boot_order: FAILED\n" : "nxrt.boot_order: ok\n");
    return g_fail;
}
