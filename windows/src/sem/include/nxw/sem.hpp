// The instruction semantics, written once and compiled twice: by clang to LLVM bitcode, which nxlift calls and inlines, and
// natively into the interpreter (and on the Mac for oracle tests). One function per decoder op:
//
//     extern "C" uint64_t nxw_sem_<Op>(nxw::sem::State* s, uint32_t word, uint64_t pc);
//
// It decodes its own fields from the word, so with a constant word (the lifter's case) everything but the operation folds away.
// The return value is 0 for ordinary instructions, the taken condition (0 or 1) for conditional branches, and the target for
// B, BL, BR, BLR and RET. Control transfer itself (calls, returns, tail calls, SVC, traps) is the caller's job: the lifter's or
// the interpreter's. docs/m3-lifter.md describes the contract.
#pragma once
#include "nxw/guest_cpu.h"

#include <cstdint>

namespace nxw::sem {

typedef uint8_t V128 __attribute__((vector_size(16), aligned(16)));

// The register state a lifted function (or the interpreter) works on. It lives in a local variable, so LLVM promotes every
// field to SSA values; nxw_sem_enter/leave copy it from and to NxwGuestCpu at function entry and at every exit (calls, returns,
// tail calls, SVCs, runtime helpers). Flags are four separate bits here and packed only in NxwGuestCpu (DESIGN D2).
struct alignas(16) State {
    V128 q[32];
    uint64_t x[32];   // x[31] is never used: register 31 is SP or the zero register, by instruction
    uint64_t sp;
    uint64_t fpcr, fpsr;
    uint64_t tpidr_el0, tpidrro_el0;
    uint64_t excl_addr;      // exclusive monitor: address (~0 = clear) and the value loaded
    uint64_t excl_val[2];
    bool n, z, c, v;
};

}  // namespace nxw::sem

extern "C" {
void nxw_sem_enter(nxw::sem::State* s, const NxwGuestCpu* cpu);
void nxw_sem_leave(const nxw::sem::State* s, NxwGuestCpu* cpu);
// Only some fields: m0 bit i = x<i> (i < 31), bit 31 = sp, bit 32 + i = q<i>; m1 = lm_* bits below
void nxw_sem_leave_mask(const nxw::sem::State* s, NxwGuestCpu* cpu, uint64_t m0, uint64_t m1);
// The D4 lookup: the translated function for a guest address, through rt_code_table, else rt_miss
NxwGuestFn nxw_sem_lookup(NxwGuestCpu* cpu, uint64_t target);
// Whether MRS/MSR of this system register (o0:op1:CRn:CRm:op2, as in nxanalyze) is handled inline by nxw_sem_MRS / MSR_reg
// (1) or by the runtime's rt_mrs / rt_msr (0)
int nxw_sem_sysreg_inline(uint32_t sysreg, int write);
}

namespace nxw::sem {

// System register numbers (o0:op1:CRn:CRm:op2)
inline constexpr uint32_t sr_nzcv = 0x5a10, sr_fpcr = 0x5a20, sr_fpsr = 0x5a21, sr_tpidr_el0 = 0x5e82, sr_tpidrro_el0 = 0x5e83,
                          sr_cntpct_el0 = 0x5f01, sr_cntvct_el0 = 0x5f02, sr_cntfrq_el0 = 0x5f00, sr_ctr_el0 = 0x5801,
                          sr_dczid_el0 = 0x5807;
inline constexpr uint64_t counter_hz = 31'250'000;   // CNTFRQ_EL0 as the SDK assumes it (DESIGN §8.2)

// nxw_sem_leave_mask's second mask
enum : uint64_t { lm_nzcv = 1, lm_fpcr = 2, lm_fpsr = 4, lm_tpidr = 8, lm_tpidrro = 16, lm_excl = 32 };

}  // namespace nxw::sem
