/* What the game DLL imports from nxrt.exe (research/03 §5.3), and the guest address plan both sides share (DESIGN D3).
   The DLL links against nxrt.exe's import library; every symbol here is exported by nxrt.exe. C and C++. */
#ifndef NXRT_RT_ABI_H
#define NXRT_RT_ABI_H
#include "nxw/guest_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _WIN32
#define NXRT_API __attribute__((visibility("default")))
#elif defined(NXRT_BUILDING)
#define NXRT_API __declspec(dllexport)
#else
#define NXRT_API __declspec(dllimport)
#endif
#ifdef __cplusplus
#define NXRT_NORETURN [[noreturn]]
#else
#define NXRT_NORETURN _Noreturn
#endif

/* ---- Guest address plan (identity mapped: a guest address is the host address) ---- */
#define NXRT_TOKEN_BASE         0x70FFF00000ull   /* host-call tokens, just below the modules */
#define NXRT_TOKEN_SIZE         0x100000ull       /* 1 MB: 65,536 tokens of 16 bytes */
#define NXRT_TOKEN_STRIDE       16u
#define NXRT_RETURN_TO_HOST     NXRT_TOKEN_BASE   /* token 0: the x30 a host-to-guest call starts with */
#define NXRT_MODULE_BASE        0x7100000000ull   /* main, then subsdk0, sdk (nxload's bases) */
#define NXRT_MODULE_REGION_SIZE 0x10000000ull     /* 256 MB reserved for the modules (2.0.1 uses 78 MB) */
#define NXRT_GUEST_AS_BASE      0x7200000000ull   /* nxnative's block: 256 MB unused, alias, stack, heap (profile sizes) */

/* ---- Indirect branches (research/03 §5.4) ----
   One 32-bit entry per guest instruction slot in [rt_code_base, rt_code_base + 4 * rt_code_words): the RVA of the host function from
   the game DLL's base, 0 = miss. Host-call tokens have entries too (RVAs of small thunks placed after the DLL's image). Fast path:
       i = (target - rt_code_base) >> 2;  f = i < rt_code_words ? rt_code_table[i] : 0;
       fn = f ? (NxwGuestFn)(rt_host_base + f) : rt_miss(cpu, target);
   Pages of the table nothing was written to read as zero (committed on first touch). rt_host_base is the game DLL's base (the
   image the RVAs count from: lifted code and the interpreter, which lives in nxrt.exe, both read it). */
NXRT_API extern const uint32_t *rt_code_table;
NXRT_API extern uint64_t rt_code_base, rt_code_words, rt_host_base;

/* Function-entry tracing (NXRT_TRACE, docs/m3-boot.md): lifted code calls rt_trace_entry(cpu, entry) at each function entry while
   rt_trace_on is non-zero; it reads cpu and never changes it */
NXRT_API extern volatile uint8_t rt_trace_on;
NXRT_API void rt_trace_entry(NxwGuestCpu *cpu, uint64_t entry);

/* A table miss: a host-call token's adapter, rt_return_to_host for the RETURN_TO_HOST sentinel, the interpreter for code inside a module
   (when one is attached), else a crash report with the guest context. Never returns NULL. */
NXRT_API NxwGuestFn rt_miss(NxwGuestCpu *cpu, uint64_t target);

/* SVC #imm: the Horizon kernel. Arguments and results in cpu->x[], as nxnative's svc_dispatch (the register frame is nxnative's). The
   lifter stores every live register into cpu before the call and reloads x0..x7 after it (an SVC can write any of them); cpu->pc should
   hold the address of the SVC instruction (crash reports, GetThreadContext3). */
NXRT_API void rt_svc(NxwGuestCpu *cpu, uint32_t imm);

/* MRS / MSR of the rarer system registers; the number is the decoder's (src/a64): o0 << 14 | op1 << 11 | CRn << 7 | CRm << 3 | op2.
   TPIDR_EL0 / TPIDRRO_EL0 are cpu fields the lifter reads and writes inline; NZCV and FPSR/FPCR live in the frame. */
#define NXRT_SR_NZCV        0x5a10u
#define NXRT_SR_FPCR        0x5a20u
#define NXRT_SR_FPSR        0x5a21u
#define NXRT_SR_TPIDR_EL0   0x5e82u
#define NXRT_SR_TPIDRRO_EL0 0x5e83u
#define NXRT_SR_CNTFRQ_EL0  0x5f00u
#define NXRT_SR_CNTPCT_EL0  0x5f01u
#define NXRT_SR_CNTVCT_EL0  0x5f02u
#define NXRT_SR_CTR_EL0     0x5801u
#define NXRT_SR_DCZID_EL0   0x5807u
#define NXRT_SR_MIDR_EL1    0x4000u
#define NXRT_SR_MPIDR_EL1   0x4005u
NXRT_API uint64_t rt_mrs(NxwGuestCpu *cpu, uint32_t sysreg);
NXRT_API void rt_msr(NxwGuestCpu *cpu, uint32_t sysreg, uint64_t value);

/* Code that must not run: why is one of NXRT_TRAP_*; pc is the guest address. Writes a crash report and ends the process. */
#define NXRT_TRAP_UDF         1u   /* udf #imm */
#define NXRT_TRAP_BRK         2u   /* brk #imm */
#define NXRT_TRAP_HLT         3u
#define NXRT_TRAP_UNREACHABLE 4u   /* the lifter's unreachable block */
#define NXRT_TRAP_UNDECODABLE 5u
#define NXRT_TRAP_JUMP_TABLE  6u   /* a jump-table index outside the decoded table (the 118 tables bounded only by the next one) */
NXRT_NORETURN NXRT_API void rt_trap(NxwGuestCpu *cpu, uint64_t pc, uint32_t why);

/* A pending non-local transfer (cpu->unwind_pc / unwind_sp: EH resume, longjmp). v0 reports and stops (DESIGN D5, M2 report). */
NXRT_NORETURN NXRT_API void rt_nonlocal(NxwGuestCpu *cpu);

/* nn::os fibers: sdk's DispatchFiberContext(from_slot x0, to_slot x1) is lifted as a call of this, and returns normally -- on the host stack of the context
   that gets resumed (host/fiber.c). The callee-saved guest registers are exchanged in cpu. */
NXRT_API void rt_fiber_switch(NxwGuestCpu *cpu);

/* Spin-loop back edges, YIELD and WFE (DESIGN D9): a host yield or a short sleep with nxnative's back-off, and a safepoint where a thread
   another thread paused with SetThreadActivity stops. kind: NXRT_YIELD_*; site: the loop head's guest address (0 when unknown).
   Not in research/03's list: added so spin loops have one entry point. */
#define NXRT_YIELD_HINT 0u
#define NXRT_YIELD_WFE  1u
#define NXRT_YIELD_SPIN 2u
NXRT_API void rt_yield(NxwGuestCpu *cpu, uint32_t kind, uint64_t site);

/* Returning from a guest function to the host that called it (the RETURN_TO_HOST sentinel's handler): does nothing */
NXRT_API void rt_return_to_host(NxwGuestCpu *cpu);

/* Host replacements: one adapter per host-replaced symbol, named after the symbol, e.g. adapter__ZN2nn2oe10InitializeEv, adapter_nvnBootstrapLoader.
   They read the AAPCS64 arguments from cpu (x0-x7, v0-v7, then the guest stack), call the host function, write x0/x1/v0-v3 or *x8 back.
   Declared in the generated src/nxrt/gen/out/hle_adapters.h; gen/out/hle_inventory.tsv lists every one with its signature and status. */

/* ---- What nxrt expects the game DLL to export (a proposal for the lifter; checked when the DLL is attached) ----
     uint32_t nxw_game_abi;                       == NXW_ABI_VERSION
     const char nxw_game_build_id[41];            main's GNU build ID (40 hex digits)
     const NxwGameFunction nxw_game_functions[];  every translated function
     uint32_t nxw_game_function_count;          */
typedef struct NxwGameFunction {
    uint64_t guest_va;   /* entry address in the guest */
    uint32_t rva;        /* the host function's RVA in the DLL */
    uint32_t flags;      /* reserved */
} NxwGameFunction;

#ifdef __cplusplus
}
#endif
#endif
