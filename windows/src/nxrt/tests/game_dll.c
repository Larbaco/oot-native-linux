/* A stand-in game DLL for nxrt.gamedll (test_game_dll.c): the nxw_game_* exports of nxrt/rt_abi.h and three functions written as lifted code would
   behave, importing the runtime's data and functions from the test executable. The function list is filled at load: plain C has no image-relative
   constants (nxlift's object uses COFF image-relative relocations instead). */
#include <windows.h>
#include "nxrt/rt_abi.h"

#define VA_A (NXRT_MODULE_BASE + 0x100)
#define VA_B (NXRT_MODULE_BASE + 0x200)
#define VA_C (NXRT_MODULE_BASE + 0x300)

extern IMAGE_DOS_HEADER __ImageBase;

static void fn_a(NxwGuestCpu *c) { c->x[0] = c->x[0] * 3 + 1; }
/* an indirect call as nxw_sem_lookup makes it: the table, else rt_miss */
static void fn_b(NxwGuestCpu *c) {
    const uint64_t t = c->x[1], i = (t - rt_code_base) >> 2;
    const uint32_t r = i < rt_code_words ? rt_code_table[i] : 0;
    (r ? (NxwGuestFn)(uintptr_t)(rt_host_base + r) : rt_miss(c, t))(c);
}
static void fn_c(NxwGuestCpu *c) { rt_yield(c, NXRT_YIELD_HINT, VA_C); c->x[0] = 7; }

__declspec(dllexport) const uint32_t nxw_game_abi = NXW_ABI_VERSION;
__declspec(dllexport) const char nxw_game_build_id[41] = "";   /* no modules are loaded: nothing to compare */
__declspec(dllexport) const uint32_t nxw_game_function_count = 3;
__declspec(dllexport) NxwGameFunction nxw_game_functions[3];

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void)h; (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        const uint64_t base = (uint64_t)(uintptr_t)&__ImageBase;
        nxw_game_functions[0] = (NxwGameFunction){VA_A, (uint32_t)((uint64_t)(uintptr_t)fn_a - base), 0};
        nxw_game_functions[1] = (NxwGameFunction){VA_B, (uint32_t)((uint64_t)(uintptr_t)fn_b - base), 0};
        nxw_game_functions[2] = (NxwGameFunction){VA_C, (uint32_t)((uint64_t)(uintptr_t)fn_c - base), 0};
    }
    return TRUE;
}
