/* The runtime's own API: what nxrt.exe, its tests and (later) the integration with the lifter's output call. C and C++. */
#ifndef NXRT_RUNTIME_H
#define NXRT_RUNTIME_H
#include "nxrt/rt_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Address space (host/addrspace.c, DESIGN D3) ---- */
enum { NXRT_AS_TOKENS, NXRT_AS_MODULES, NXRT_AS_GUEST, NXRT_AS_COUNT };
typedef struct nxrt_range {
    const char *name;
    uint64_t base, size;
    int reserved;                 /* 1 = ours (a placeholder) */
    uint64_t conflict_base;       /* when not reserved: the allocation in the way (VirtualQuery) */
    uint64_t conflict_size;
    unsigned conflict_state, conflict_type;
    char conflict_owner[160];     /* the module or mapped file holding it, when known */
} nxrt_range;
/* Reserve every guest range as a placeholder. Idempotent; safe in a TLS callback (Win32 only, no CRT). Returns 1 when all succeeded. */
int nxrt_as_reserve_early(void);
const nxrt_range *nxrt_as_range(int i);
int nxrt_as_ok(void);
/* The guest block's partition (nxnative's layout, profile sizes): unused 256 MB, alias, stack, heap */
typedef struct nxrt_layout { uint64_t as_base, as_size, alias, alias_size, stack, stack_size, heap, heap_size, app_pool; } nxrt_layout;
const nxrt_layout *nxrt_as_layout(void);
#ifndef _WIN32
/* Linux: the image is loaded low (or, position-independent, high) and the guest range lies in between, where nothing maps by itself: the reservation is made by a
   constructor, which runs before main and before anything the program itself allocates */
#define NXRT_EARLY_RESERVE() __attribute__((constructor)) static void nxrt_early_reserve_(void) { nxrt_as_reserve_early(); }
#else
/* Expands to a TLS callback that reserves the guest ranges before the CRT and any DLL initialisation: put it in each executable once */
#define NXRT_EARLY_RESERVE()                                                                                   \
    static void __stdcall nxrt_tls_cb_(void *h, unsigned long reason, void *r) {                              \
        (void)h; (void)r; if (reason == 1) nxrt_as_reserve_early();                                           \
    }                                                                                                         \
    __pragma(comment(linker, "/INCLUDE:_tls_used"))                                                           \
    __pragma(comment(linker, "/INCLUDE:nxrt_tls_callback"))                                                   \
    __attribute__((section(".CRT$XLB"), used)) void (__stdcall *const nxrt_tls_callback)(void *, unsigned long, void *) = nxrt_tls_cb_;
#endif

/* ---- Start-up (host/main.c) ---- */
typedef struct nxrt_options {
    const char *log_dir;      /* NULL: NXRT_LOG_DIR, else .\_local\nxrt when it exists, else the current folder */
    int quiet;                /* no copy of the log on stderr */
} nxrt_options;
int nxrt_init(const nxrt_options *o);          /* log, banner, reservation check, fault handlers, clocks; 0 = ok */
int nxrt_load_program(const char *exefs_dir);   /* nxload, map the modules, bind host replacements; 0 = ok */
/* The bootstrap on the calling thread, which becomes the guest main thread: nn::init::Start's order, each module's DT_INIT, then nnMain (host/main.c) */
void nxrt_guest_main(void);
void nxrt_log_summary(void);
const char *nxrt_log_path(void);

/* ---- Modules (host/loader.cpp) ---- */
typedef struct nxrt_module_info { const char *name; uint64_t base, size, text_end, ro_off, data_off; const char *build_id; uint64_t init; } nxrt_module_info;   /* init: DT_INIT, a module offset (0: none) */
int nxrt_module_count(void);
int nxrt_module_get(int i, nxrt_module_info *out);
uint64_t nxrt_guest_sym(const char *name);       /* the first module in load order exporting the name, 0 if none */

/* ---- Code table, tokens, guest calls (host/rt.c) ---- */
int nxrt_code_table_create(uint64_t base, uint64_t end);   /* the guest range the table covers (tokens and modules) */
int nxrt_code_attach(void *dll_base, size_t image_size);   /* the host image the table's RVAs count from; places the token thunks after it */
int nxrt_code_set(uint64_t guest_va, NxwGuestFn fn);        /* fn must lie within 4 GB above the attached image; 0 = ok */
NxwGuestFn nxrt_code_lookup(uint64_t guest_va);             /* table, then rt_miss */
int nxrt_attach_game_dll(const char *path);                 /* LoadLibrary + the export contract in rt_abi.h; 0 = ok */
/* Call a guest function from the host on the current guest thread's cpu: integer/pointer arguments in x0..x(n-1), x30 = RETURN_TO_HOST;
   returns x0. Callee-saved state is the callee's to keep; x30 is restored afterwards. */
uint64_t nxrt_guest_call(uint64_t guest_va, int n, const uint64_t *args);
NxwGuestCpu *nxrt_cur_cpu(void);                  /* the calling guest thread's cpu, NULL on a host thread */
/* Interpreter hook for misses inside a module; nxrt_interp_attach installs src/interp's (nxrt_init does, unless NXRT_NO_INTERP is set) */
void nxrt_set_interpreter(NxwGuestFn (*entry_for)(uint64_t guest_va));
void nxrt_interp_attach(void);
/* NXRT_TRACE's function-entry trace (rt.c); after the code table exists */
void nxrt_trace_init(void);
/* "sdk+0x1234 nnosInitialize+0x10", "host:<token name>", or the raw address: logs, traces, crash reports (loader.cpp) */
void nxrt_symbolize(uint64_t guest_va, char *out, size_t n);
/* The guest function whose translated code holds a host address in the game DLL (0 if none; *off = the offset into its host code) */
uint64_t nxrt_host_to_guest(uint64_t host_addr, uint64_t *off);

/* ---- Host-call tokens (host/hle.c) ---- */
uint64_t nxrt_token_of(const char *name);         /* the guest-visible address of a host replacement, 0 if none */
const char *nxrt_token_name(uint64_t guest_va);   /* the replacement behind a token address, NULL if none */
/* A run-time token: the guest-visible address of a host function with a hand-written adapter (vtables and thread entries the host plants) */
uint64_t nxrt_host_fn_token(const char *name, NxwGuestFn adapter);
int nxrt_token_count(void);
typedef struct nxrt_hle_stats { int forced, swhle, nvn, ported, trivial, stubbed, named_stubs, bound_relocs; } nxrt_hle_stats;
void nxrt_hle_get_stats(nxrt_hle_stats *out);

/* ---- Guest threads (host/hle_thread.c) ---- */
uint32_t nxrt_thread_main_init(void);     /* make the calling thread the guest main thread (a cpu, TLS blocks, a guest stack); returns its handle */
uint32_t nxrt_svc_thread_main_init(void); /* nxnative's svc_thread_main_init, for a caller that has set up its cpu itself */
void nxrt_thread_dump(void);              /* every guest thread's state into the log */

#ifdef __cplusplus
}
#endif
#endif
