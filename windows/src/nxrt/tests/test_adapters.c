/* The generated adapters against direct host calls (hle_sig_test.c), and the registry's shape: every adapter present, the ported entries' adapters
   calling the ported functions (a stub's log and neutral result, a trivial entry's constant). */
#include <stdio.h>
#include <string.h>
#include "hle_registry.h"
#include "nxrt/runtime.h"

NXRT_EARLY_RESERVE()

int nxrt_sig_tests(void);
static int g_fail;
#define T(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    g_fail |= nxrt_sig_tests();
    if (nxrt_init(&(nxrt_options){.quiet = 1})) { printf("FAIL nxrt_init\n"); return 1; }
    nxrt_hle_stats s; nxrt_hle_get_stats(&s);
    printf("registry: %d forced, %d swhle, %d NVN; %d ported, %d trivial, %d stubbed\n", s.forced, s.swhle, s.nvn, s.ported, s.trivial, s.stubbed);
    /* 918 of dm/host and dm/gfx/nvn, 23 of dm/gfx's hle_nv.c and hle_vi.c, and hle_hid.c's 32 TOUCH(N) names in place of 2 misread ones (M3) */
    T(s.forced == 973 && s.nvn == 734 && s.ported + s.trivial + s.stubbed == s.forced + s.swhle + s.nvn);
    for (unsigned i = 0; i < nxrt_hle_count; i++) T(nxrt_hle_table[i].adapter != NULL && nxrt_token_of(nxrt_hle_table[i].name) != 0);
    /* a stub: neutral result in x0/x1/v0-v3 and the caller's x8 buffer untouched unless the result goes through it */
    NxwGuestCpu c; memset(&c, 0xab, sizeof c);
    const nxrt_hle_entry *e = nxrt_hle_find("nvnCommandBufferSetRecordingMode", NXRT_HLE_NVN);   /* a Switch 2 name with no known signature (M5 records the draws) */
    T(e && !e->ported);
    if (e) { e->adapter(&c); T(c.x[0] == 0 && c.x[1] == 0 && c.q[0][0] == 0 && c.q[3][15] == 0 && c.x[2] == 0xababababababababull); }
    /* a trivial entry: nxnative's constant (ldn::Initialize returns RES_FAIL, 0x2A2) */
    e = nxrt_hle_find("_ZN2nn3ldn10InitializeEv", NXRT_HLE_FORCED);
    T(e && e->ported);
    if (e) { memset(&c, 0, sizeof c); e->adapter(&c); T((uint32_t)c.x[0] == 0x2A2); }
    /* a ported one: hle_net.c's GetLastErrno, ENETDOWN (50) until set */
    e = nxrt_hle_find("nnsocketGetLastErrno", NXRT_HLE_FORCED);
    T(e && e->ported == 1);
    if (e) { memset(&c, 0, sizeof c); e->adapter(&c); T(c.x[0] == 50); }
    /* the C library passthroughs keep the Switch's LP64 long: strtol of a 64-bit value */
    e = nxrt_hle_find("strtol", NXRT_HLE_SWHLE);
    T(e && e->ported == 1);
    if (e) { memset(&c, 0, sizeof c); c.x[0] = (uint64_t)(uintptr_t)"-12345678901"; c.x[1] = 0; c.x[2] = 10; e->adapter(&c); T((int64_t)c.x[0] == -12345678901ll); }
    printf(g_fail ? "nxrt.adapters: FAILED\n" : "nxrt.adapters: ok\n");
    return g_fail;
}
