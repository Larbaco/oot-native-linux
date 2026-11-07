/* The game DLL contract without game data: nxrt-tests-gamedll loads nxrt_test_game.dll (tests/game_dll.c), which imports from this executable as
   the real game DLL imports from nxrt.exe, and exports nxw_game_* with three functions. Checks: the contract is accepted and rt_host_base is the
   DLL's base; the table reaches each function; an indirect call made inside the DLL through the imported table data reaches a function and a
   host-call token; a function import (rt_yield) resolves against this executable; host addresses map back to their guest function. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "nxrt/runtime.h"

NXRT_EARLY_RESERVE()

#define VA_A (NXRT_MODULE_BASE + 0x100)   /* as tests/game_dll.c */
#define VA_B (NXRT_MODULE_BASE + 0x200)
#define VA_C (NXRT_MODULE_BASE + 0x300)
static int g_fail;
#define T(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (nxrt_init(&(nxrt_options){.quiet = 1})) { printf("FAIL nxrt_init\n"); return 1; }
    T(nxrt_code_table_create(NXRT_TOKEN_BASE, NXRT_MODULE_BASE + 0x100000) == 0);
    nxrt_thread_main_init();
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, sizeof path);
    char *slash = strrchr(path, '\\');
    strcpy(slash ? slash + 1 : path, "nxrt_test_game.dll");
    T(nxrt_attach_game_dll(path) == 0);
    HMODULE h = GetModuleHandleA("nxrt_test_game.dll");
    T(h && rt_host_base == (uint64_t)(uintptr_t)h);
    NxwGuestCpu *c = nxrt_cur_cpu();
    /* each function through the table */
    c->x[0] = 5; nxrt_code_lookup(VA_A)(c); T(c->x[0] == 16);
    /* lifted-code style indirect call inside the DLL: a guest function, then a host-call token (hle_net.c's GetLastErrno, 50 until set) */
    c->x[0] = 5; c->x[1] = VA_A; nxrt_code_lookup(VA_B)(c); T(c->x[0] == 16);
    c->x[0] = 0; c->x[1] = nxrt_token_of("nnsocketGetLastErrno"); T(c->x[1] != 0);
    nxrt_code_lookup(VA_B)(c); T(c->x[0] == 50);
    /* a host-to-guest call of a function that calls back into the executable */
    T(nxrt_guest_call(VA_C, 0, NULL) == 7);
    /* host addresses inside translated code name their guest function */
    uint64_t off = 0, host_a = rt_host_base + rt_code_table[(VA_A - rt_code_base) >> 2];
    T(nxrt_host_to_guest(host_a + 1, &off) == VA_A && off == 1);
    T(nxrt_host_to_guest((uint64_t)(uintptr_t)&main, NULL) == 0);
    printf(g_fail ? "nxrt.gamedll: FAILED\n" : "nxrt.gamedll: ok\n");
    return g_fail;
}
