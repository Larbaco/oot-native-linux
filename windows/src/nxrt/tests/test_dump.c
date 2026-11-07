/* The real modules through nxload: NXW_DUMP names an ExeFS folder (2.0.1: installer-test-win\build201\extract\ExeFS); exit 77 (skipped) without it.
   Checks the modules at nxload's bases, their protections (text and ro read-only and never executable, data and bss writable), the forced
   take-overs bound to host-call tokens in the GOT, guest symbols, QueryMemory's view of a module, the code table's span, and the heap the game would
   ask for (the pool less the modules). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "nxrt/runtime.h"

NXRT_EARLY_RESERVE()

static int g_fail;
#define T(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static DWORD prot(uint64_t a) { MEMORY_BASIC_INFORMATION m; return VirtualQuery((void *)(uintptr_t)a, &m, sizeof m) ? m.Protect : 0; }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *dump = getenv("NXW_DUMP");
    if (!dump || !*dump) { printf("NXW_DUMP not set: skipped\n"); return 77; }
    if (nxrt_init(&(nxrt_options){.quiet = 1})) { printf("FAIL nxrt_init\n"); return 1; }
    uint64_t t0 = plat_now_ns();
    if (nxrt_load_program(dump)) { printf("FAIL loading %s\n", dump); return 1; }
    printf("loaded in %.1f ms\n", (plat_now_ns() - t0) / 1e6);

    T(nxrt_module_count() == 3);
    nxrt_module_info m[3];
    for (int i = 0; i < nxrt_module_count() && i < 3; i++) {
        T(nxrt_module_get(i, &m[i]) == 0);
        printf("%-8s %#llx +%#llx text %#llx ro %#llx data %#llx build %s\n", m[i].name, (unsigned long long)m[i].base, (unsigned long long)m[i].size,
               (unsigned long long)m[i].text_end, (unsigned long long)m[i].ro_off, (unsigned long long)m[i].data_off, m[i].build_id);
        T(prot(m[i].base) == PAGE_READONLY && prot(m[i].base + m[i].ro_off) == PAGE_READONLY);   /* text: data only, not executable */
        T(prot(m[i].base + m[i].data_off) == PAGE_READWRITE && prot(m[i].base + m[i].size - 1) == PAGE_READWRITE);
        uint32_t mod0 = *(uint32_t *)(uintptr_t)(m[i].base + 4);
        T(!memcmp((void *)(uintptr_t)(m[i].base + mod0), "MOD0", 4));
    }
    T(!strcmp(m[0].name, "main") && !strcmp(m[1].name, "subsdk0") && !strcmp(m[2].name, "sdk"));
    T(m[0].base == 0x7100000000ull && m[1].base == m[0].base + m[0].size && m[2].base == m[1].base + m[1].size);
    int v201 = !strncmp(m[0].build_id, "76a489f3bd3642fa5e18bd83d32c98ba", 32);
    if (v201) T(m[1].base == 0x7103d20000ull && m[2].base == 0x7103e60000ull);

    /* guest symbols: nnMain in main's text, nnosInitialize in sdk's */
    uint64_t nn = nxrt_guest_sym("nnMain"), os = nxrt_guest_sym("nnosInitialize");
    T(nn >= m[0].base && nn < m[0].base + m[0].text_end && os >= m[2].base && os < m[2].base + m[2].text_end);
    printf("nnMain main+%#llx, nnosInitialize sdk+%#llx\n", (unsigned long long)(nn - m[0].base), (unsigned long long)(os - m[2].base));

    /* forced take-overs: main's GOT slots for them hold host-call tokens (nvnBootstrapLoader is main's only NVN import) */
    uint64_t tok = nxrt_token_of("nvnBootstrapLoader");
    int found = 0, tokens = 0;
    for (uint64_t a = m[0].base + m[0].data_off; a < m[0].base + m[0].size; a += 8) {
        uint64_t v = *(uint64_t *)(uintptr_t)a;
        if (v == tok) found++;
        if (nxrt_token_name(v)) tokens++;
    }
    printf("main's data: %d slots hold host-call tokens, %d of them nvnBootstrapLoader's\n", tokens, found);
    T(tok && found >= 1 && tokens >= 300);
    T(nxrt_token_of("_ZN2nn2oe10InitializeEv") != 0);

    /* QueryMemory sees the module as Horizon lays it out */
    nxrt_thread_main_init();
    NxwGuestCpu *c = nxrt_cur_cpu();
    struct { uint64_t base, size; uint32_t state, attr, perm, ipc, device, pad; } mi;
    c->x[0] = (uint64_t)(uintptr_t)&mi; c->x[2] = nn; rt_svc(c, 0x06);
    T(c->x[0] == 0 && mi.base == m[0].base && mi.size == m[0].ro_off && mi.state == 3 && mi.perm == 5);
    c->x[0] = (uint64_t)(uintptr_t)&mi; c->x[2] = m[2].base + m[2].data_off + 8; rt_svc(c, 0x06);
    T(c->x[0] == 0 && mi.state == 4 && mi.perm == 3 && mi.base + mi.size == m[2].base + m[2].size);

    /* the code table spans the tokens and the modules; nothing is translated yet, so every guest function misses */
    T(rt_code_base == NXRT_TOKEN_BASE && rt_code_base + rt_code_words * 4 >= m[2].base + m[2].size);
    T(rt_code_table[(nn - rt_code_base) >> 2] == 0);

    /* the heap the game sizes as the pool less what the modules take */
    uint64_t want = 0x280000000ull - ((m[0].size + m[1].size + m[2].size + 0x1fffff) & ~0x1fffffull);
    c->x[1] = want; rt_svc(c, 0x01); T(c->x[0] == 0);
    c->x[1] = want + 0x200000; rt_svc(c, 0x01); T(c->x[0] == 0x10801);
    printf("heap of %.2f GB accepted (the game's request: pool minus %.1f MB of modules)\n", want / 1073741824.0, (0x280000000ull - want) / 1048576.0);
    nxrt_log_summary();
    printf(g_fail ? "nxrt.dump: FAILED\n" : "nxrt.dump: ok\n");
    return g_fail;
}
