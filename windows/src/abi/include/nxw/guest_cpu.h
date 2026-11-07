/* The ABI between recompiled game code (the game DLL), the interpreter and the runtime (nxrt). Field offsets are the ABI:
   bump NXW_ABI_VERSION on any change. C and C++.
   The first 800 bytes are nxnative's register frame exactly (dm/host/entry.S, _dm_gate): x0..x30 at +0, q0..q31 at +256,
   nzcv / fpsr / fpcr at +768 / +776 / +784, sp at +792. So nxnative's svc_dispatch(num, regs) and its other handlers take
   (uint64_t *)cpu unchanged (DESIGN D2). nzcv is kept in the NZCV register's own format (N, Z, C, V in bits 31..28). */
#ifndef NXW_GUEST_CPU_H
#define NXW_GUEST_CPU_H
#include <stddef.h>
#include <stdint.h>

#define NXW_ABI_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NxwHostThread NxwHostThread;   /* runtime-private */

typedef struct NxwGuestCpu {
    /* nxnative's register frame */
    uint64_t x[31];                     /* +0     X0..X30 */
    uint64_t pad0;                      /* +248 */
    _Alignas(16) uint8_t q[32][16];     /* +256   V0..V31 */
    uint64_t nzcv;                      /* +768 */
    uint64_t fpsr;                      /* +776 */
    uint64_t fpcr;                      /* +784 */
    uint64_t sp;                        /* +792 */
    /* recompiler and runtime state */
    uint64_t pc;                        /* +800   valid only while the runtime holds control (trap, SVC, miss, backtrace) */
    uint64_t tpidr_el0;                 /* +808 */
    uint64_t tpidrro_el0;               /* +816 */
    uint64_t excl_addr;                 /* +824   exclusive monitor (sdk's 5 LL/SC sites); ~0 = clear */
    _Alignas(16) uint8_t excl_val[16];  /* +832 */
    uint64_t unwind_pc, unwind_sp;      /* +848   pending non-local transfer (EH resume, longjmp) */
    NxwHostThread *thread;              /* +864 */
    uint64_t reserved[3];               /* +872 */
} NxwGuestCpu;                          /* 896 bytes */

typedef void (*NxwGuestFn)(NxwGuestCpu *cpu);   /* every translated function, every HLE adapter, every interpreter trampoline */

#ifdef __cplusplus
}
#define NXW_STATIC_ASSERT static_assert
#else
#define NXW_STATIC_ASSERT _Static_assert
#endif

NXW_STATIC_ASSERT(offsetof(NxwGuestCpu, q) == 256, "q0 at +256 (entry.S)");
NXW_STATIC_ASSERT(offsetof(NxwGuestCpu, nzcv) == 768, "nzcv at +768 (entry.S)");
NXW_STATIC_ASSERT(offsetof(NxwGuestCpu, fpsr) == 776, "fpsr at +776 (entry.S)");
NXW_STATIC_ASSERT(offsetof(NxwGuestCpu, fpcr) == 784, "fpcr at +784 (entry.S)");
NXW_STATIC_ASSERT(offsetof(NxwGuestCpu, sp) == 792, "sp at +792 (entry.S)");
NXW_STATIC_ASSERT(offsetof(NxwGuestCpu, pc) == 800, "end of nxnative's frame");
NXW_STATIC_ASSERT(offsetof(NxwGuestCpu, excl_val) == 832, "ABI 1");
NXW_STATIC_ASSERT(offsetof(NxwGuestCpu, thread) == 864, "ABI 1");
NXW_STATIC_ASSERT(sizeof(NxwGuestCpu) == 896, "ABI 1");

#endif
