// from nxnative dm/host/main.c (MIT, © seedds): start-up and the bootstrap that replays nn::init::Start (sdk+0x2b0470) before nnMain.
// Windows: the log banner and the reservation check come first; the guest main thread is a host thread with its own NxwGuestCpu, TLS blocks and a guest
// stack at the top of the stack region; every step of the bootstrap is a guest call into recompiled code. Dropped: the iOS preload thread, CFBundle, rlimits.
#include "dm.h"
#include "nxrt/runtime.h"

void crash_install(void);
void crash_set_dump_hook(void (*fn)(void));
void svc_thread_dump(void);
void stubs_report(void);
void nxrt_tokens_init(void);
void nxrt_mem_init(void);
uint64_t nxrt_main_stack(size_t size);
void nxrt_set_cur_cpu(NxwGuestCpu *c);
void *guest_thread_init(NxwGuestCpu *cpu);
int nxrt_load_modules(const char *exefs);
int nxrt_log_open(const nxrt_options *o);
void nxrt_log_banner(void);
void hid_keys_init(void);

struct dm_flags g_flag;
uint64_t g_t0_ns;
static void flags_init(void) {
    g_flag.thr = !!getenv("DM_THR"); g_flag.svc = !!getenv("DM_SVC"); g_flag.fs = !!getenv("DM_FS");
    g_flag.dbg = !!getenv("DM_DBG"); g_flag.imports = !!getenv("DM_IMPORTS");
    g_flag.bootprof = !!getenv("DM_BOOTPROF");
}

int nxrt_init(const nxrt_options *o) {
    static int done, rc;
    if (done) return rc;
    done = 1;
    g_t0_ns = now_ns();
    flags_init();
    nxrt_as_reserve_early();   // normally done already by the executable's TLS callback; a no-op then
    if (nxrt_log_open(o)) return rc = -1;
    crash_install();
    crash_set_dump_hook(svc_thread_dump);
    plat_timer_resolution();
    nxrt_log_banner();
    if (!nxrt_as_ok()) {
        LOG("[RED] the guest address space is not fully reserved. Something in this process took part of it before nxrt could: the fallback is a launcher that "
            "creates nxrt suspended, reserves the ranges in it with VirtualAlloc2 and resumes it (docs/m3-runtime.md)");
        return rc = -2;
    }
    nxrt_tokens_init();
    nxrt_mem_init();
    nxrt_interp_attach();
    return rc = 0;
}

int nxrt_load_program(const char *exefs) {
    uint64_t t0 = now_ns();
    if (nxrt_load_modules(exefs)) return -1;
    // The code table covers the tokens and the modules: every guest address a branch can reach
    uint64_t end = (uint64_t)(uintptr_t)g_mod[g_nmod - 1].base + g_mod[g_nmod - 1].size;
    if (nxrt_code_table_create(NXRT_TOKEN_BASE, end)) { LOG("[RED] cannot reserve the code table"); return -1; }
    nxrt_trace_init();
    dm_sdk_major();
    hid_keys_init();   // DM_KEYS / DM_CTL / DM_REC scripted input (hle_hid.c); after the modules, as nxnative's main.c: look_cam matches main's build ID
    { void nxrt_prof_env(void); nxrt_prof_env(); }   // NXRT_PROF=<start s>,<seconds>: the sampling profiler (prof.c)
    stubs_report();
    {
        static const struct { const char *build, *ver; } known[] = {DM_GAME_DISPLAY_VERSIONS};
        const char *shown = "a build the profile does not know";
        for (size_t i = 0; i < sizeof known / sizeof known[0]; i++)
            if (!strncmp(dm_main_build(), known[i].build, 32)) { shown = strcmp(known[i].ver, "?") ? known[i].ver : DM_GAME_DEFAULT_VERSION; break; }
        LOG("program loaded in %.1f ms: game build %s (%s)", (now_ns() - t0) / 1e6, dm_main_build(), shown);
    }
    return 0;
}

void nxrt_log_summary(void) {
    nxrt_hle_stats s; nxrt_hle_get_stats(&s);
    LOG("host replacements: %d forced + %d swhle + %d NVN = %d in the registry (%d ported, %d with nxnative's constant result, %d stubbed), %d named stubs, %d relocation slots bound",
        s.forced, s.swhle, s.nvn, s.forced + s.swhle + s.nvn, s.ported, s.trivial, s.stubbed, s.named_stubs, s.bound_relocs);
}

// The calling host thread becomes the guest main thread: a register file, Horizon TLS blocks, a guest stack. The npdm's main thread (2.0.1 and 1.0.5):
// priority 44, core 0, a 1 MB stack, the values svc_thread_main_init and this use
uint32_t nxrt_thread_main_init(void) {
    NxwGuestCpu *c = plat_aligned_alloc(sizeof *c, 64);
    if (!c) die("no memory for the main thread's cpu");
    memset(c, 0, sizeof *c);
    nxrt_set_cur_cpu(c);
    guest_thread_init(c);
    c->sp = nxrt_main_stack(1u << 20);
    c->x[30] = NXRT_RETURN_TO_HOST;
    return nxrt_svc_thread_main_init();
}

// Game arguments: argv[0] is fixed to "game" (on a real device nn::init takes them from the process argument area and hands them to the SDK through SetHostArgc/Argv)
static char *g_argv[64] = {"game"}; static int g_argc = 1;
static uint64_t req(const char *n) { uint64_t p = nxrt_guest_sym(n); if (!p) die("missing symbol %s", n); return p; }
#define CALL(name, ...) nxrt_guest_call(req(name), (int)(sizeof((uint64_t[]){0, ##__VA_ARGS__}) / 8) - 1, (uint64_t[]){0, ##__VA_ARGS__} + 1)

// Copy Start step by step: _init_libc1 = __nnmusl_init_locks + __init_ssp. The three callbacks passed in by rtld: A only sets an rtld flag (skipped),
// B = each module's DT_INIT (before nnMain), C = destructors (not done)
void nxrt_guest_main(void) {
    uint32_t hmain = nxrt_thread_main_init();   // Main thread handle: x0 of nnosInitialize; x1 = pointer to the argument area
    CALL("_init_libc0");
    LOG(">>> nnosInitialize"); CALL("nnosInitialize", hmain, 0);
    CALL("_init_libc1");
    LOG(">>> nninitInitializeSdkModule"); CALL("nninitInitializeSdkModule");
    // The game arguments are in place before nninitStartup, as on hardware (nn::os takes them from the process argument area in nnosInitialize)
    CALL("_ZN2nn2os11SetHostArgcEi", (uint64_t)g_argc);
    CALL("_ZN2nn2os11SetHostArgvEPPc", (uint64_t)(uintptr_t)g_argv);
    LOG(">>> nninitStartup"); CALL("nninitStartup");
    CALL("_init_libc2");
    run_module_inits();   // rtld callback B: each module's DT_INIT (global constructors)
    { void dm_guest_spawn_selftest(void); dm_guest_spawn_selftest(); }   // DM_GUEST_SPAWN_TEST self-test of host-created guest threads (hle_guestthread.c)
    LOG(">>> nnMain"); CALL("nnMain");
    LOG("nnMain returned");
}
