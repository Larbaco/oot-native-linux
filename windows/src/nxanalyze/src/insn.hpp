// What one A64 instruction does to control flow, registers and memory, as far as the analysis needs it.
// Registers are bit masks: bit n is xn for n < 31, bit 31 is SP (the zero register is never a bit).
#pragma once
#include "nxw/a64.hpp"

#include <cstdint>

namespace nxanalyze {

using nxw::a64::Op;
template <Op O> using F = nxw::a64::Fields<O>;

inline constexpr uint32_t SP = 1u << 31;
inline constexpr uint32_t LR = 1u << 30;
inline constexpr uint32_t FP = 1u << 29;
inline constexpr uint32_t call_clobbered = 0x4007ffffu;   // x0-x18 and x30

enum class Flow : uint8_t {
    none,
    b, bl, bcond,   // bcond covers b.cond, cbz/cbnz and tbz/tbnz
    br, blr, ret,   // ret also covers retaa/retab; br/blr also the authenticated forms
    trap,           // udf, brk, hlt and unallocated words
    eret,           // eret, drps
};

enum class Cls : uint8_t {
    alu,        // integer data processing, including compares
    load, store,
    load_excl, store_excl, atomic,   // ld(a)xr, st(l)xr, LSE read-modify-write and CAS
    load_acq, store_rel,             // ldar/ldapr, stlr
    prefetch,
    branch, call, indirect, ret,
    svc, trap, sysreg, sys, barrier, hint,
    fp, fp_load, fp_store,
};

struct Fx {
    uint32_t def = 0, use = 0;    // general registers and SP
    uint32_t vdef = 0, vuse = 0;  // v0-v31
    bool fdef = false, fuse = false;   // NZCV
    Cls cls = Cls::alu;
};

struct Insn {
    uint32_t w;
    Op op;
};

struct FlowInfo {
    Flow kind = Flow::none;
    uint64_t target = 0;   // direct branches: absolute (pc + offset)
    uint32_t reg = 0;      // br/blr/ret: the target register
};

FlowInfo flow(Insn i, uint64_t pc);
Fx effects(Insn i);

// Hint numbers (CRm:op2)
enum : uint32_t {
    hint_nop = 0, hint_yield = 1, hint_wfe = 2, hint_wfi = 3, hint_sev = 4, hint_sevl = 5, hint_xpaclri = 7,
    hint_pacia1716 = 8, hint_pacib1716 = 10, hint_autia1716 = 12, hint_autib1716 = 14,
    hint_paciaz = 24, hint_paciasp = 25, hint_pacibz = 26, hint_pacibsp = 27,
    hint_autiaz = 28, hint_autiasp = 29, hint_autibz = 30, hint_autibsp = 31,
};
inline uint32_t hint_no(uint32_t w) { return (w >> 5) & 0x7f; }

// System register number o0:op1:CRn:CRm:op2 (MRS/MSR), op0 = 2 + o0
inline uint32_t sysreg_no(uint32_t w) { return (w >> 5) & 0x7fff; }
inline constexpr uint32_t sr_nzcv = 0x5a10, sr_fpcr = 0x5a20, sr_fpsr = 0x5a21, sr_tpidr_el0 = 0x5e82, sr_tpidrro_el0 = 0x5e83,
                          sr_cntpct_el0 = 0x5f01, sr_cntvct_el0 = 0x5f02, sr_cntfrq_el0 = 0x5f00, sr_ctr_el0 = 0x5801,
                          sr_dczid_el0 = 0x5807;
const char* sysreg_name(uint32_t no);   // "fpcr", or nullptr for registers the analysis does not name

inline int64_t sext(uint64_t v, unsigned bits) { return int64_t(v << (64 - bits)) >> (64 - bits); }
// Decoded bitmask immediate (AND/ORR/EOR/ANDS immediate), width 32 or 64
uint64_t bitmask_imm(uint32_t N, uint32_t immr, uint32_t imms, unsigned width);

bool is_padding(Insn i);   // udf #0 (zero word) or nop
bool is_load_op(Op op);    // a general-register load with an immediate or register offset (not pair, not exclusive)

}  // namespace nxanalyze
