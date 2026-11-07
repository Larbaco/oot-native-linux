/* What lifted code and the interpreter need from the runtime (nxrt): extern symbols the runtime defines. C and C++.
   Lifted functions are void fn_<guest address>(NxwGuestCpu*); HLE adapters are void adapter_<symbol>(NxwGuestCpu*) (nxrt's
   naming), one per name that program.db binds to host code. Everything else the game DLL calls is listed here; nxrt's
   nxrt/rt_abi.h declares the same symbols as exports of nxrt.exe. */
#ifndef NXW_RT_H
#define NXW_RT_H
#include "nxw/guest_cpu.h"

#ifdef __cplusplus
extern "C" {
#define NXW_NORETURN [[noreturn]]
#else
#define NXW_NORETURN _Noreturn
#endif

/* SVC #imm: arguments and results in cpu->x[] (nxnative's svc_dispatch convention) */
void rt_svc(NxwGuestCpu *cpu, uint32_t imm);
/* MRS / MSR of system registers the semantics do not handle inline (nxw_sem_sysreg_inline): cntpct_el0, fpcr writes, ... */
uint64_t rt_mrs(NxwGuestCpu *cpu, uint32_t sysreg);
void rt_msr(NxwGuestCpu *cpu, uint32_t sysreg, uint64_t value);
/* UDF, BRK, HLT, unallocated or unsupported instructions, impossible control flow. why = NXW_TRAP_* | detail << 8 */
NXW_NORETURN void rt_trap(NxwGuestCpu *cpu, uint64_t pc, uint32_t why);
/* An indirect branch target with no rt_code_table entry: an HLE adapter, an interpreter trampoline (which reads its entry
   address from cpu->pc, so rt_miss sets it), or a fatal error. Lifted code stores its registers to cpu before the lookup. */
NxwGuestFn rt_miss(NxwGuestCpu *cpu, uint64_t target);
/* Spin-loop heads, YIELD, WFE and WFI: a host yield with back-off and a safepoint. kind: NXW_YIELD_*; site: the guest address.
   The state is in cpu (stored before the call, reloaded after it). */
void rt_yield(NxwGuestCpu *cpu, uint32_t kind, uint64_t site);
/* The four sdk functions that rewrite SP or x30 (DispatchFiberContext, UserExceptionHandler, longjmp, libunwind's jumpto) are
   lifted as a call of this with cpu->pc = the function and the state in cpu: a report and a stop until DESIGN D5's bridge */
NXW_NORETURN void rt_nonlocal(NxwGuestCpu *cpu);
/* Function-entry tracing: lifted functions call rt_trace_entry(cpu, entry) on entry while rt_trace_on is non-zero (objects
   lifted with Options::entry_trace). It must not change cpu. */
extern volatile uint8_t rt_trace_on;
void rt_trace_entry(NxwGuestCpu *cpu, uint64_t entry);

/* The D4 table: rt_code_table[(target - rt_code_base) >> 2] is the host function's offset from rt_host_base (0 = miss);
   rt_code_words entries */
extern const uint32_t *rt_code_table;
extern uint64_t rt_code_base, rt_code_words, rt_host_base;

/* why & 0xff; the values 1-6 are nxrt's NXRT_TRAP_* (src/nxrt/include/nxrt/rt_abi.h) */
enum {
    NXW_TRAP_UDF = 1,          /* detail: imm16 */
    NXW_TRAP_BRK = 2,          /* detail: imm16 */
    NXW_TRAP_HLT = 3,          /* detail: imm16 */
    NXW_TRAP_UNREACHABLE = 4,  /* detail: 1 a noreturn call returned, 2 control fell off a function, 3 a branch outside the CFG */
    NXW_TRAP_UNDECODABLE = 5,  /* an unallocated word */
    NXW_TRAP_JUMP_TABLE = 6,   /* a jump-table target outside the decoded table */
    NXW_TRAP_UNSUPPORTED = 7,  /* decoded, but no semantics; detail: the decoder op */
    NXW_TRAP_ERET = 8,
    NXW_TRAP_SYSTEM = 9,       /* an instruction a game cannot run at EL0 (HVC, SMC, MSR of PSTATE, ...); detail: the op */
};
/* rt_yield's kind (nxrt's NXRT_YIELD_*) */
enum { NXW_YIELD_HINT = 0, NXW_YIELD_WFE = 1, NXW_YIELD_SPIN = 2 };

#define NXW_TRAP_NORETURN (NXW_TRAP_UNREACHABLE | 1 << 8)
#define NXW_TRAP_FALLOFF (NXW_TRAP_UNREACHABLE | 2 << 8)
#define NXW_TRAP_OFF_CFG (NXW_TRAP_UNREACHABLE | 3 << 8)

#ifdef __cplusplus
}
#endif
#endif
