// Integer data processing, branches (conditions and link-register writes), system instructions. Arm ARM (DDI 0487) pseudocode.
#include "common.hpp"

using namespace nxw::sem;

// ---------------------------------------------------------------- PC-relative and immediate arithmetic
SEM(ADR) {
    const Fl<Op::ADR> f(w);
    wX(s, f.Rd, pc + u64(sext(f.immhi << 2 | f.immlo, 21)));
    return 0;
}
SEM(ADRP) {
    const Fl<Op::ADRP> f(w);
    wX(s, f.Rd, (pc & ~u64(0xfff)) + u64(sext(f.immhi << 2 | f.immlo, 21) << 12));
    return 0;
}
SEM(ADD_imm) {
    const Fl<Op::ADD_imm> f(w);
    wRS(s, f.sf, f.Rd, RS(s, f.sf, f.Rn) + (u64(f.imm12) << (f.sh * 12)));
    return 0;
}
SEM(SUB_imm) {
    const Fl<Op::SUB_imm> f(w);
    wRS(s, f.sf, f.Rd, RS(s, f.sf, f.Rn) - (u64(f.imm12) << (f.sh * 12)));
    return 0;
}
SEM(ADDS_imm) {
    const Fl<Op::ADDS_imm> f(w);
    wR(s, f.sf, f.Rd, add_with_carry(s, f.sf, RS(s, f.sf, f.Rn), u64(f.imm12) << (f.sh * 12), false, true));
    return 0;
}
SEM(SUBS_imm) {
    const Fl<Op::SUBS_imm> f(w);
    wR(s, f.sf, f.Rd, add_with_carry(s, f.sf, RS(s, f.sf, f.Rn), ~(u64(f.imm12) << (f.sh * 12)), true, true));
    return 0;
}

// ---------------------------------------------------------------- logical immediate
#define LOGIMM(NAME, EXPR, SPDEST, FLAGS) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        const u64 imm = decode_bit_masks(f.N, f.imms, f.immr, f.sf ? 64 : 32).wmask; \
        const u64 a = R(s, f.sf, f.Rn); \
        const u64 r = (EXPR) & (f.sf ? ~0ull : 0xffffffffull); \
        if (FLAGS) set_nz(s, f.sf, r), s->c = false, s->v = false; \
        if (SPDEST) wRS(s, f.sf, f.Rd, r); \
        else wR(s, f.sf, f.Rd, r); \
        return 0; \
    }
LOGIMM(AND_imm, a & imm, true, false)
LOGIMM(ORR_imm, a | imm, true, false)
LOGIMM(EOR_imm, a ^ imm, true, false)
LOGIMM(ANDS_imm, a & imm, false, true)

// ---------------------------------------------------------------- move wide
SEM(MOVZ) {
    const Fl<Op::MOVZ> f(w);
    wR(s, f.sf, f.Rd, u64(f.imm16) << (f.hw * 16));
    return 0;
}
SEM(MOVN) {
    const Fl<Op::MOVN> f(w);
    wR(s, f.sf, f.Rd, ~(u64(f.imm16) << (f.hw * 16)));
    return 0;
}
SEM(MOVK) {
    const Fl<Op::MOVK> f(w);
    const unsigned sh = f.hw * 16;
    wR(s, f.sf, f.Rd, (X(s, f.Rd) & ~(u64(0xffff) << sh)) | u64(f.imm16) << sh);
    return 0;
}

// ---------------------------------------------------------------- bitfield
SEM(SBFM) {
    const Fl<Op::SBFM> f(w);
    const unsigned ds = f.sf ? 64 : 32;
    const BitMasks m = decode_bit_masks(f.N, f.imms, f.immr, ds);
    const u64 src = R(s, f.sf, f.Rn);
    const u64 bot = ror(src, f.immr, ds) & m.wmask;
    const u64 top = (src >> f.imms & 1) ? ones(ds) : 0;
    wR(s, f.sf, f.Rd, (top & ~m.tmask) | (bot & m.tmask));
    return 0;
}
SEM(BFM) {
    const Fl<Op::BFM> f(w);
    const unsigned ds = f.sf ? 64 : 32;
    const BitMasks m = decode_bit_masks(f.N, f.imms, f.immr, ds);
    const u64 dst = R(s, f.sf, f.Rd), src = R(s, f.sf, f.Rn);
    const u64 bot = (dst & ~m.wmask) | (ror(src, f.immr, ds) & m.wmask);
    wR(s, f.sf, f.Rd, (dst & ~m.tmask) | (bot & m.tmask));
    return 0;
}
SEM(UBFM) {
    const Fl<Op::UBFM> f(w);
    const unsigned ds = f.sf ? 64 : 32;
    const BitMasks m = decode_bit_masks(f.N, f.imms, f.immr, ds);
    wR(s, f.sf, f.Rd, ror(R(s, f.sf, f.Rn), f.immr, ds) & m.wmask & m.tmask);
    return 0;
}
SEM(EXTR) {
    const Fl<Op::EXTR> f(w);
    const u64 hi = R(s, f.sf, f.Rn), lo = R(s, f.sf, f.Rm);
    const unsigned lsb = f.imms;
    u64 r;
    if (f.sf) r = lsb ? (lo >> lsb) | (hi << (64 - lsb)) : lo;
    else r = lsb ? u32((u32(lo) >> lsb) | (u32(hi) << (32 - lsb))) : u32(lo);
    wR(s, f.sf, f.Rd, r);
    return 0;
}

// ---------------------------------------------------------------- shifted register: logical and add/sub
#define LOGREG(NAME, EXPR, FLAGS) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        const u64 a = R(s, f.sf, f.Rn), b = shift_reg(R(s, f.sf, f.Rm), f.shift, f.imm6, f.sf); \
        const u64 r = (EXPR) & (f.sf ? ~0ull : 0xffffffffull); \
        if (FLAGS) set_nz(s, f.sf, r), s->c = false, s->v = false; \
        wR(s, f.sf, f.Rd, r); \
        return 0; \
    }
LOGREG(AND_reg, a & b, false)
LOGREG(BIC_reg, a & ~b, false)
LOGREG(ORR_reg, a | b, false)
LOGREG(ORN_reg, a | ~b, false)
LOGREG(EOR_reg, a ^ b, false)
LOGREG(EON_reg, a ^ ~b, false)
LOGREG(ANDS_reg, a & b, true)
LOGREG(BICS_reg, a & ~b, true)

#define ADDREG(NAME, SUB, FLAGS) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        const u64 b = shift_reg(R(s, f.sf, f.Rm), f.shift, f.imm6, f.sf); \
        wR(s, f.sf, f.Rd, add_with_carry(s, f.sf, R(s, f.sf, f.Rn), SUB ? ~b : b, SUB, FLAGS)); \
        return 0; \
    }
ADDREG(ADD_reg, false, false)
ADDREG(ADDS_reg, false, true)
ADDREG(SUB_reg, true, false)
ADDREG(SUBS_reg, true, true)

#define ADDEXT(NAME, SUB, FLAGS) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        const u64 b = extend_reg(X(s, f.Rm), f.option, f.imm3); \
        const u64 r = add_with_carry(s, f.sf, RS(s, f.sf, f.Rn), SUB ? ~b : b, SUB, FLAGS); \
        if (FLAGS) wR(s, f.sf, f.Rd, r); \
        else wRS(s, f.sf, f.Rd, r); \
        return 0; \
    }
ADDEXT(ADD_ext, false, false)
ADDEXT(ADDS_ext, false, true)
ADDEXT(SUB_ext, true, false)
ADDEXT(SUBS_ext, true, true)

#define ADC_(NAME, SUB, FLAGS) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        const u64 b = R(s, f.sf, f.Rm); \
        wR(s, f.sf, f.Rd, add_with_carry(s, f.sf, R(s, f.sf, f.Rn), SUB ? ~b : b, s->c, FLAGS)); \
        return 0; \
    }
ADC_(ADC, false, false)
ADC_(ADCS, false, true)
ADC_(SBC, true, false)
ADC_(SBCS, true, true)

// ---------------------------------------------------------------- conditional compare and select
#define CCMP_(NAME, SUB, IMM) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        if (cond_holds(s, f.cond)) { \
            u64 b; \
            if constexpr (IMM) b = u64(w >> 16 & 31); \
            else b = R(s, f.sf, w >> 16 & 31); \
            add_with_carry(s, f.sf, R(s, f.sf, f.Rn), SUB ? ~b : b, SUB, true); \
        } else { \
            set_nzcv(s, f.nzcv); \
        } \
        return 0; \
    }
CCMP_(CCMN_reg, false, false)
CCMP_(CCMP_reg, true, false)
CCMP_(CCMN_imm, false, true)
CCMP_(CCMP_imm, true, true)

#define CSEL_(NAME, EXPR) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        const u64 b = R(s, f.sf, f.Rm); \
        wR(s, f.sf, f.Rd, cond_holds(s, f.cond) ? R(s, f.sf, f.Rn) : u64(EXPR)); \
        return 0; \
    }
CSEL_(CSEL, b)
CSEL_(CSINC, b + 1)
CSEL_(CSINV, ~b)
CSEL_(CSNEG, -b)

// ---------------------------------------------------------------- multiply and divide
SEM(MADD) {
    const Fl<Op::MADD> f(w);
    wR(s, f.sf, f.Rd, R(s, f.sf, f.Ra) + R(s, f.sf, f.Rn) * R(s, f.sf, f.Rm));
    return 0;
}
SEM(MSUB) {
    const Fl<Op::MSUB> f(w);
    wR(s, f.sf, f.Rd, R(s, f.sf, f.Ra) - R(s, f.sf, f.Rn) * R(s, f.sf, f.Rm));
    return 0;
}
SEM(SMADDL) {
    const Fl<Op::SMADDL> f(w);
    wX(s, f.Rd, X(s, f.Ra) + u64(i64(i32(X(s, f.Rn))) * i64(i32(X(s, f.Rm)))));
    return 0;
}
SEM(SMSUBL) {
    const Fl<Op::SMSUBL> f(w);
    wX(s, f.Rd, X(s, f.Ra) - u64(i64(i32(X(s, f.Rn))) * i64(i32(X(s, f.Rm)))));
    return 0;
}
SEM(UMADDL) {
    const Fl<Op::UMADDL> f(w);
    wX(s, f.Rd, X(s, f.Ra) + u64(u32(X(s, f.Rn))) * u64(u32(X(s, f.Rm))));
    return 0;
}
SEM(UMSUBL) {
    const Fl<Op::UMSUBL> f(w);
    wX(s, f.Rd, X(s, f.Ra) - u64(u32(X(s, f.Rn))) * u64(u32(X(s, f.Rm))));
    return 0;
}
SEM(SMULH) {
    const Fl<Op::SMULH> f(w);
    wX(s, f.Rd, u64((i128(i64(X(s, f.Rn))) * i64(X(s, f.Rm))) >> 64));
    return 0;
}
SEM(UMULH) {
    const Fl<Op::UMULH> f(w);
    wX(s, f.Rd, u64((u128(X(s, f.Rn)) * X(s, f.Rm)) >> 64));
    return 0;
}
SEM(UDIV) {
    const Fl<Op::UDIV> f(w);
    const u64 a = R(s, f.sf, f.Rn), b = R(s, f.sf, f.Rm);
    wR(s, f.sf, f.Rd, b ? a / b : 0);
    return 0;
}
SEM(SDIV) {
    const Fl<Op::SDIV> f(w);
    u64 r;
    if (f.sf) {
        const i64 a = i64(X(s, f.Rn)), b = i64(X(s, f.Rm));
        r = b == 0 ? 0 : (b == -1 ? u64(0) - u64(a) : u64(a / b));
    } else {
        const i32 a = i32(X(s, f.Rn)), b = i32(X(s, f.Rm));
        r = u32(b == 0 ? 0 : (b == -1 ? u32(0) - u32(a) : u32(a / b)));
    }
    wR(s, f.sf, f.Rd, r);
    return 0;
}

// ---------------------------------------------------------------- variable shifts, bit operations
#define SHIFTV(NAME, TYPE) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        const unsigned amt = unsigned(R(s, f.sf, f.Rm) & (f.sf ? 63 : 31)); \
        wR(s, f.sf, f.Rd, shift_reg(R(s, f.sf, f.Rn), TYPE, amt, f.sf)); \
        return 0; \
    }
SHIFTV(LSLV, 0)
SHIFTV(LSRV, 1)
SHIFTV(ASRV, 2)
SHIFTV(RORV, 3)

SEM(RBIT) {
    const Fl<Op::RBIT> f(w);
    const u64 v = R(s, f.sf, f.Rn);
    wR(s, f.sf, f.Rd, f.sf ? __builtin_bitreverse64(v) : __builtin_bitreverse32(u32(v)));
    return 0;
}
SEM(REV16) {
    const Fl<Op::REV16> f(w);
    const u64 v = R(s, f.sf, f.Rn);
    wR(s, f.sf, f.Rd, ((v >> 8) & 0x00ff00ff00ff00ffull) | ((v & 0x00ff00ff00ff00ffull) << 8));
    return 0;
}
SEM(REV32) {
    const Fl<Op::REV32> f(w);
    const u64 v = X(s, f.Rn);
    wX(s, f.Rd, u64(__builtin_bswap32(u32(v))) | u64(__builtin_bswap32(u32(v >> 32))) << 32);
    return 0;
}
SEM(REV_w) {
    const Fl<Op::REV_w> f(w);
    wX(s, f.Rd, __builtin_bswap32(u32(X(s, f.Rn))));
    return 0;
}
SEM(REV_x) {
    const Fl<Op::REV_x> f(w);
    wX(s, f.Rd, __builtin_bswap64(X(s, f.Rn)));
    return 0;
}
SEM(CLZ) {
    const Fl<Op::CLZ> f(w);
    const u64 v = R(s, f.sf, f.Rn);
    wR(s, f.sf, f.Rd, f.sf ? u64(std::countl_zero(v)) : u64(std::countl_zero(u32(v))));
    return 0;
}
SEM(CLS) {
    const Fl<Op::CLS> f(w);
    const u64 v = R(s, f.sf, f.Rn);
    u64 r;
    if (f.sf) r = u64(std::countl_zero(v ^ u64(i64(v) >> 1)) - 1);
    else r = u64(std::countl_zero(u32(v) ^ u32(i32(u32(v)) >> 1)) - 1);
    wR(s, f.sf, f.Rd, r);
    return 0;
}

// CRC32 / CRC32C: reflected polynomials 0x04C11DB7 / 0x1EDC6F41
SEM(CRC32) {
    const Fl<Op::CRC32> f(w);
    const u32 poly = f.C ? 0x82f63b78u : 0xedb88320u;
    const unsigned bytes = 1u << f.sz;
    u32 crc = u32(X(s, f.Rn));
    const u64 data = X(s, f.Rm);
    for (unsigned i = 0; i < bytes * 8; i++) {
        crc ^= u32(data >> i) & 1;
        crc = (crc >> 1) ^ (crc & 1 ? poly : 0);
    }
    wX(s, f.Rd, crc);
    return 0;
}

// Pointer authentication: nothing is ever signed (DESIGN §3), so signing and stripping leave the value alone
SEM(PAC) { return 0; }
SEM(AUT) { return 0; }
SEM(XPACI) { return 0; }
// PACGA: the generic authentication code lives in the top 32 bits (the low 32 are zero). Nothing is verified against hardware keys, so any mix of the two
// operands works, as long as the same inputs give the same code (SDK 22's setjmp signs the jmp_buf with it and longjmp checks it)
SEM(PACGA) {
    const Fl<Op::PACGA> f(w);
    u64 h = X(s, f.Rn) * 0x9e3779b97f4a7c15ull ^ XS(s, f.Rm);
    h ^= h >> 32;
    h *= 0xd6e8feb86659fd93ull;
    h ^= h >> 29;
    wX(s, f.Rd, h << 32);
    return 0;
}

// ---------------------------------------------------------------- branches
// The lifter and interpreter make the transfer; these return the target (unconditional) or whether the branch is taken.
SEM(B) { return pc + u64(sext(Fl<Op::B>(w).imm26, 26) * 4); }
SEM(BL) {
    s->x[30] = pc + 4;
    return pc + u64(sext(Fl<Op::BL>(w).imm26, 26) * 4);
}
SEM(B_cond) { return cond_holds(s, Fl<Op::B_cond>(w).cond); }
SEM(CBZ) {
    const Fl<Op::CBZ> f(w);
    return R(s, f.sf, f.Rt) == 0;
}
SEM(CBNZ) {
    const Fl<Op::CBNZ> f(w);
    return R(s, f.sf, f.Rt) != 0;
}
SEM(TBZ) {
    const Fl<Op::TBZ> f(w);
    return (X(s, f.Rt) >> (f.b5 << 5 | f.b40) & 1) == 0;
}
SEM(TBNZ) {
    const Fl<Op::TBNZ> f(w);
    return (X(s, f.Rt) >> (f.b5 << 5 | f.b40) & 1) != 0;
}
SEM(BR) { return X(s, Fl<Op::BR>(w).Rn); }
SEM(BRAAZ) { return X(s, Fl<Op::BRAAZ>(w).Rn); }
SEM(BRAA) { return X(s, Fl<Op::BRAA>(w).Rn); }
SEM(BLR) {
    const u64 t = X(s, Fl<Op::BLR>(w).Rn);
    s->x[30] = pc + 4;
    return t;
}
SEM(BLRAAZ) {
    const u64 t = X(s, Fl<Op::BLRAAZ>(w).Rn);
    s->x[30] = pc + 4;
    return t;
}
SEM(BLRAA) {
    const u64 t = X(s, Fl<Op::BLRAA>(w).Rn);
    s->x[30] = pc + 4;
    return t;
}
SEM(RET) { return X(s, Fl<Op::RET>(w).Rn); }
SEM(RETAA) { return s->x[30]; }

// ---------------------------------------------------------------- hints, barriers, system registers
SEM(HINT) {
    const Fl<Op::HINT> f(w);
    // yield and wfe spin politely; nop, the PAC hints (nothing is signed), bti, esb, csdb and the rest do nothing
    if (f.CRm == 0 && (f.op2 == 1 || f.op2 == 2)) spin_hint();
    return 0;
}
SEM(CLREX) {
    s->excl_addr = ~0ull;
    return 0;
}
// DMB/DSB: loads-only options become acquire fences, stores-only release fences, the rest full fences (research/03 §3.6)
NXW_INLINE void barrier(unsigned crm) {
    switch (crm & 3) {
    case 1: __atomic_thread_fence(__ATOMIC_ACQUIRE); break;
    case 2: __atomic_thread_fence(__ATOMIC_RELEASE); break;
    default: __atomic_thread_fence(__ATOMIC_SEQ_CST); break;
    }
}
SEM(DMB) {
    barrier(Fl<Op::DMB>(w).CRm);
    return 0;
}
SEM(DSB) {
    barrier(Fl<Op::DSB>(w).CRm);
    return 0;
}
SEM(ISB) {
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    return 0;
}
SEM(SB) {
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    return 0;
}
// SYS: cache and TLB maintenance do nothing for the host, except DC ZVA, which zeroes a 64-byte block (DCZID_EL0 = 4)
SEM(SYS) {
    const Fl<Op::SYS> f(w);
    if (f.op1 == 3 && f.CRn == 7 && f.CRm == 4 && f.op2 == 1) {
        const u64 a = X(s, f.Rt) & ~u64(63);
        for (unsigned i = 0; i < 64; i += 8) st<u64>(a + i, 0);
    }
    return 0;
}
SEM(CFINV) {
    s->c = !s->c;
    return 0;
}

// MRS / MSR of the registers nxw_sem_sysreg_inline accepts; the lifter and interpreter send the others to rt_mrs / rt_msr
SEM(MRS) {
    const Fl<Op::MRS> f(w);
    const u32 sr = w >> 5 & 0x7fff;
    u64 v = 0;
    switch (sr) {
    case sr_nzcv: v = u64(s->n) << 31 | u64(s->z) << 30 | u64(s->c) << 29 | u64(s->v) << 28; break;
    case sr_fpcr: v = s->fpcr; break;
    case sr_fpsr: v = s->fpsr; break;
    case sr_tpidr_el0: v = s->tpidr_el0; break;
    case sr_tpidrro_el0: v = s->tpidrro_el0; break;
    case sr_ctr_el0: v = 0x8444c004; break;   // Cortex-A78: 64-byte lines, PIPT instruction cache
    case sr_dczid_el0: v = 4; break;          // DC ZVA allowed, 64-byte blocks
    case sr_cntfrq_el0: v = counter_hz; break;
    default: break;
    }
    wX(s, f.Rt, v);
    return 0;
}
SEM(MSR_reg) {
    const Fl<Op::MSR_reg> f(w);
    const u32 sr = w >> 5 & 0x7fff;
    const u64 v = X(s, f.Rt);
    switch (sr) {
    case sr_nzcv: set_nzcv(s, unsigned(v >> 28 & 15)); break;
    case sr_fpsr: s->fpsr = v; break;
    case sr_tpidr_el0: s->tpidr_el0 = v; break;
    default: break;
    }
    return 0;
}
