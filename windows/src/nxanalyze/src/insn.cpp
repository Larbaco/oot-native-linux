#include "insn.hpp"

#include <array>
#include <cstring>
#include <string_view>

namespace nxanalyze {

using nxw::a64::op_fields;
using nxw::a64::op_name;
using nxw::a64::op_count;

namespace {

constexpr uint32_t gz(uint32_t r) { return r == 31 ? 0 : 1u << r; }   // register 31 is the zero register
constexpr uint32_t gs(uint32_t r) { return 1u << r; }                 // register 31 is SP
constexpr uint32_t vr(uint32_t r) { return 1u << r; }

// Per-op facts the generic paths need: where the register fields are, and whether the op writes its base back
struct OpInfo {
    int8_t rd = -1, rn = -1, rm = -1, ra = -1, rt = -1, rt2 = -1, rs = -1;   // field indices
    bool writeback = false;
};

const std::array<OpInfo, op_count>& op_info() {
    static const auto table = [] {
        std::array<OpInfo, op_count> t{};
        for (unsigned o = 0; o < op_count; o++) {
            OpInfo& x = t[o];
            const auto fs = op_fields(Op(o));
            for (size_t k = 0; k < fs.size(); k++) {
                const std::string_view n = fs[k].name;
                int8_t* slot = n == "Rd" ? &x.rd : n == "Rn" ? &x.rn : n == "Rm" ? &x.rm : n == "Ra" ? &x.ra : n == "Rt" ? &x.rt
                             : n == "Rt2" ? &x.rt2 : n == "Rs" ? &x.rs : nullptr;
                if (slot) *slot = int8_t(k);
            }
            const std::string_view n = op_name(Op(o));
            x.writeback = n.ends_with("_post") || n.ends_with("_pre");
        }
        return t;
    }();
    return table;
}

uint32_t fld(uint32_t w, Op op, int8_t idx) { return idx < 0 ? 0 : nxw::a64::field(w, op_fields(op)[size_t(idx)]); }

// SIMD&FP data processing that reads or writes general registers
Fx fp_effects(Insn i, const OpInfo& x) {
    const uint32_t w = i.w;
    Fx f;
    f.cls = Cls::fp;
    const uint32_t rd = fld(w, i.op, x.rd), rn = fld(w, i.op, x.rn);
    switch (i.op) {
    case Op::SCVTF_fix: case Op::UCVTF_fix: case Op::SCVTF_int: case Op::UCVTF_int: case Op::FMOV_from_gen: case Op::FMOV_from_top:
    case Op::DUP_gen:
        f.use = gz(rn), f.vdef = vr(rd);
        return f;
    case Op::INS_gen:
        f.use = gz(rn), f.vdef = f.vuse = vr(rd);
        return f;
    case Op::FCVTZS_fix: case Op::FCVTZU_fix: case Op::FCVTNS_int: case Op::FCVTNU_int: case Op::FCVTAS_int: case Op::FCVTAU_int:
    case Op::FCVTPS_int: case Op::FCVTPU_int: case Op::FCVTMS_int: case Op::FCVTMU_int: case Op::FCVTZS_int: case Op::FCVTZU_int:
    case Op::FMOV_to_gen: case Op::FMOV_to_top: case Op::SMOV: case Op::UMOV:
        f.def = gz(rd), f.vuse = vr(rn);
        return f;
    case Op::FJCVTZS:
        f.def = gz(rd), f.vuse = vr(rn), f.fdef = true;
        return f;
    case Op::FCMP: case Op::FCMPE: case Op::FCMP_zero: case Op::FCMPE_zero:
        f.vuse = vr(rn) | vr(fld(w, i.op, x.rm)), f.fdef = true;
        return f;
    case Op::FCCMP: case Op::FCCMPE:
        f.vuse = vr(rn) | vr(fld(w, i.op, x.rm)), f.fdef = f.fuse = true;
        return f;
    default: break;
    }
    if (x.rd >= 0) f.vdef = vr(rd);
    if (x.rn >= 0) f.vuse |= vr(rn);
    if (x.rm >= 0) f.vuse |= vr(fld(w, i.op, x.rm));
    if (x.ra >= 0) f.vuse |= vr(fld(w, i.op, x.ra));
    f.fuse = i.op == Op::FCSEL;
    return f;
}

// SIMD&FP loads and stores: the base is a general register; the data registers are vector registers
Fx fp_mem_effects(Insn i, const OpInfo& x) {
    const uint32_t w = i.w;
    Fx f;
    const bool literal = ((w >> 27) & 7) == 3;
    const bool load = literal || ((w >> 22) & 1);
    f.cls = load ? Cls::fp_load : Cls::fp_store;
    if (x.rn >= 0) {
        const uint32_t rn = fld(w, i.op, x.rn);
        f.use = gs(rn);
        if (x.writeback) f.def = gs(rn);
    }
    if (x.writeback && x.rm >= 0) f.use |= gz(fld(w, i.op, x.rm));   // structure loads/stores, register post-index
    uint32_t data = 0;
    if (x.rt >= 0) data = vr(fld(w, i.op, x.rt));
    if (x.rt2 >= 0) data |= vr(fld(w, i.op, x.rt2));
    if (i.op >= Op::ST1_4 && i.op <= Op::LD4R_post) {   // multi-register lists: up to four consecutive registers
        const uint32_t rt = fld(w, i.op, x.rt);
        for (uint32_t k = 0; k < 4; k++) data |= vr((rt + k) & 31);
    }
    (load ? f.vdef : f.vuse) |= data;
    if (load && (i.op >= Op::LD1_lane && i.op <= Op::LD4_lane_post)) f.vuse |= data;   // lane loads keep the other lanes
    return f;
}

}  // namespace

uint64_t bitmask_imm(uint32_t N, uint32_t immr, uint32_t imms, unsigned width) {
    const uint32_t v = (N << 6) | (~imms & 0x3f);
    int len = 31;
    while (len >= 0 && !((v >> len) & 1)) len--;
    if (len < 1) return 0;
    const unsigned esize = 1u << len;
    const uint32_t levels = esize - 1;
    const uint32_t s = imms & levels, r = immr & levels;
    uint64_t welem = s + 1 >= 64 ? ~0ull : (1ull << (s + 1)) - 1;
    const uint64_t emask = esize == 64 ? ~0ull : (1ull << esize) - 1;
    if (r) welem = ((welem >> r) | (welem << (esize - r))) & emask;
    uint64_t out = 0;
    for (unsigned k = 0; k < width; k += esize) out |= welem << k;
    return width == 64 ? out : out & 0xffffffffull;
}

bool is_padding(Insn i) { return i.w == 0 || i.w == 0xd503201f; }

bool is_load_op(Op op) {
    switch (op) {
    case Op::LDR_imm: case Op::LDRB_imm: case Op::LDRH_imm: case Op::LDRSW_imm: case Op::LDRSB_imm: case Op::LDRSH_imm:
    case Op::LDUR: case Op::LDURB: case Op::LDURH: case Op::LDURSW: case Op::LDURSB: case Op::LDURSH:
    case Op::LDR_reg: case Op::LDRB_reg: case Op::LDRH_reg: case Op::LDRSW_reg: case Op::LDRSB_reg: case Op::LDRSH_reg:
        return true;
    default: return false;
    }
}

const char* sysreg_name(uint32_t no) {
    switch (no) {
    case sr_nzcv: return "nzcv";
    case sr_fpcr: return "fpcr";
    case sr_fpsr: return "fpsr";
    case sr_tpidr_el0: return "tpidr_el0";
    case sr_tpidrro_el0: return "tpidrro_el0";
    case sr_cntpct_el0: return "cntpct_el0";
    case sr_cntvct_el0: return "cntvct_el0";
    case sr_cntfrq_el0: return "cntfrq_el0";
    case sr_ctr_el0: return "ctr_el0";
    case sr_dczid_el0: return "dczid_el0";
    default: return nullptr;
    }
}

FlowInfo flow(Insn i, uint64_t pc) {
    const uint32_t w = i.w;
    switch (i.op) {
    case Op::B: return {Flow::b, pc + uint64_t(sext(F<Op::B>(w).imm26, 26) * 4)};
    case Op::BL: return {Flow::bl, pc + uint64_t(sext(F<Op::BL>(w).imm26, 26) * 4)};
    case Op::B_cond: return {Flow::bcond, pc + uint64_t(sext(F<Op::B_cond>(w).imm19, 19) * 4)};
    case Op::CBZ: case Op::CBNZ: return {Flow::bcond, pc + uint64_t(sext((w >> 5) & 0x7ffff, 19) * 4)};
    case Op::TBZ: case Op::TBNZ: return {Flow::bcond, pc + uint64_t(sext((w >> 5) & 0x3fff, 14) * 4)};
    case Op::BR: return {Flow::br, 0, F<Op::BR>(w).Rn};
    case Op::BRAAZ: return {Flow::br, 0, F<Op::BRAAZ>(w).Rn};
    case Op::BRAA: return {Flow::br, 0, F<Op::BRAA>(w).Rn};
    case Op::BLR: return {Flow::blr, 0, F<Op::BLR>(w).Rn};
    case Op::BLRAAZ: return {Flow::blr, 0, F<Op::BLRAAZ>(w).Rn};
    case Op::BLRAA: return {Flow::blr, 0, F<Op::BLRAA>(w).Rn};
    case Op::RET: return {Flow::ret, 0, F<Op::RET>(w).Rn};
    case Op::RETAA: return {Flow::ret, 0, 30};
    case Op::ERET: case Op::ERETAA: case Op::DRPS: return {Flow::eret};
    case Op::UDF: case Op::BRK: case Op::HLT: case Op::Unallocated: return {Flow::trap};
    default: return {};
    }
}

Fx effects(Insn i) {
    const uint32_t w = i.w;
    const OpInfo& x = op_info()[size_t(i.op)];
    const uint32_t top = (w >> 25) & 0xf;
    if ((top & 7) == 7) return fp_effects(i, x);
    if ((top & 5) == 4 && ((w >> 26) & 1)) return fp_mem_effects(i, x);

    const uint32_t rd = fld(w, i.op, x.rd), rn = fld(w, i.op, x.rn), rm = fld(w, i.op, x.rm), ra = fld(w, i.op, x.ra),
                   rt = fld(w, i.op, x.rt), rt2 = fld(w, i.op, x.rt2), rs = fld(w, i.op, x.rs);
    Fx f;
    switch (i.op) {
    // Data processing, immediate
    case Op::ADR: case Op::ADRP: case Op::MOVZ: case Op::MOVN: f.def = gz(rd); break;
    case Op::MOVK: f.def = f.use = gz(rd); break;
    case Op::ADD_imm: case Op::SUB_imm: f.def = gs(rd), f.use = gs(rn); break;
    case Op::ADDS_imm: case Op::SUBS_imm: f.def = gz(rd), f.use = gs(rn), f.fdef = true; break;
    case Op::AND_imm: case Op::ORR_imm: case Op::EOR_imm: f.def = gs(rd), f.use = gz(rn); break;
    case Op::ANDS_imm: f.def = gz(rd), f.use = gz(rn), f.fdef = true; break;
    case Op::SBFM: case Op::UBFM: f.def = gz(rd), f.use = gz(rn); break;
    case Op::BFM: f.def = gz(rd), f.use = gz(rd) | gz(rn); break;
    case Op::EXTR: f.def = gz(rd), f.use = gz(rn) | gz(rm); break;

    // Branches, exceptions, system
    case Op::B_cond: f.cls = Cls::branch, f.fuse = true; break;
    case Op::B: f.cls = Cls::branch; break;
    case Op::BL: f.cls = Cls::call, f.def = LR; break;
    case Op::CBZ: case Op::CBNZ: case Op::TBZ: case Op::TBNZ: f.cls = Cls::branch, f.use = gz(rt); break;
    case Op::BR: case Op::BRAAZ: f.cls = Cls::indirect, f.use = gz(rn); break;
    case Op::BRAA: f.cls = Cls::indirect, f.use = gz(rn) | gs(rm); break;
    case Op::BLR: case Op::BLRAAZ: f.cls = Cls::call, f.use = gz(rn), f.def = LR; break;
    case Op::BLRAA: f.cls = Cls::call, f.use = gz(rn) | gs(rm), f.def = LR; break;
    case Op::RET: f.cls = Cls::ret, f.use = gz(rn); break;
    case Op::RETAA: f.cls = Cls::ret, f.use = LR | SP; break;
    case Op::ERET: case Op::ERETAA: case Op::DRPS: f.cls = Cls::ret; break;
    case Op::SVC: case Op::HVC: case Op::SMC: f.cls = Cls::svc, f.def = f.use = 0xff; break;   // Horizon passes and returns x0-x7
    case Op::BRK: case Op::HLT: case Op::UDF: case Op::DCPS1: case Op::DCPS2: case Op::DCPS3: case Op::Unallocated: f.cls = Cls::trap; break;
    case Op::HINT:
        f.cls = Cls::hint;
        switch (hint_no(w)) {
        case hint_xpaclri: case hint_paciasp: case hint_pacibsp: case hint_autiasp: case hint_autibsp: f.def = f.use = LR, f.use |= SP; break;
        case hint_paciaz: case hint_pacibz: case hint_autiaz: case hint_autibz: f.def = f.use = LR; break;
        case hint_pacia1716: case hint_pacib1716: case hint_autia1716: case hint_autib1716: f.def = 1u << 17, f.use = (1u << 16) | (1u << 17); break;
        default: break;
        }
        break;
    case Op::CLREX: case Op::DSB: case Op::DMB: case Op::ISB: case Op::SB: f.cls = Cls::barrier; break;
    case Op::CFINV: case Op::XAFLAG: case Op::AXFLAG: f.fdef = f.fuse = true; break;
    case Op::MSR_imm: case Op::SMSTART: case Op::MSR_PM: f.cls = Cls::sysreg; break;
    case Op::SYS: case Op::SYSP: f.cls = Cls::sys, f.use = gz(rt); break;
    case Op::SYSL: f.cls = Cls::sys, f.def = gz(rt); break;
    case Op::MSR_reg: case Op::MSRR: case Op::MSR_S0:
        f.cls = Cls::sysreg, f.use = gz(rt), f.fdef = i.op == Op::MSR_reg && sysreg_no(w) == sr_nzcv;
        break;
    case Op::MRS: case Op::MRRS: case Op::MRS_S0:
        f.cls = Cls::sysreg, f.def = gz(rt), f.fuse = i.op == Op::MRS && sysreg_no(w) == sr_nzcv;
        break;

    // Loads and stores: exclusive, ordered, compare-and-swap
    case Op::STXRB: case Op::STLXRB: case Op::STXRH: case Op::STLXRH: case Op::STXR: case Op::STLXR:
        f.cls = Cls::store_excl, f.def = gz(rs), f.use = gz(rt) | gs(rn); break;
    case Op::STXP: case Op::STLXP: f.cls = Cls::store_excl, f.def = gz(rs), f.use = gz(rt) | gz(rt2) | gs(rn); break;
    case Op::LDXRB: case Op::LDAXRB: case Op::LDXRH: case Op::LDAXRH: case Op::LDXR: case Op::LDAXR:
        f.cls = Cls::load_excl, f.def = gz(rt), f.use = gs(rn); break;
    case Op::LDXP: case Op::LDAXP: f.cls = Cls::load_excl, f.def = gz(rt) | gz(rt2), f.use = gs(rn); break;
    case Op::LDARB: case Op::LDARH: case Op::LDAR: case Op::LDLARB: case Op::LDLARH: case Op::LDLAR: case Op::LDAPR:
    case Op::LDAPURB: case Op::LDAPURSB: case Op::LDAPURH: case Op::LDAPURSH: case Op::LDAPUR: case Op::LDAPURSW:
        f.cls = Cls::load_acq, f.def = gz(rt), f.use = gs(rn); break;
    case Op::STLRB: case Op::STLRH: case Op::STLR: case Op::STLLRB: case Op::STLLRH: case Op::STLLR:
    case Op::STLURB: case Op::STLURH: case Op::STLUR:
        f.cls = Cls::store_rel, f.use = gz(rt) | gs(rn); break;
    case Op::CAS: f.cls = Cls::atomic, f.def = gz(rs), f.use = gz(rs) | gz(rt) | gs(rn); break;
    case Op::CASP: {
        const uint32_t p = gz(rs) | gz((rs + 1) & 31);
        f.cls = Cls::atomic, f.def = p, f.use = p | gz(rt) | gz((rt + 1) & 31) | gs(rn);
        break;
    }
    case Op::LDADD: case Op::LDCLR: case Op::LDEOR: case Op::LDSET: case Op::LDSMAX: case Op::LDSMIN: case Op::LDUMAX: case Op::LDUMIN:
    case Op::SWP:
        f.cls = Cls::atomic, f.def = gz(rt), f.use = gz(rs) | gs(rn); break;

    // Literal loads
    case Op::LDR_lit: case Op::LDRSW_lit: f.cls = Cls::load, f.def = gz(rt); break;
    case Op::PRFM_lit: f.cls = Cls::prefetch; break;

    // Pairs
    case Op::STNP: case Op::STP_off: f.cls = Cls::store, f.use = gz(rt) | gz(rt2) | gs(rn); break;
    case Op::STP_post: case Op::STP_pre: f.cls = Cls::store, f.use = gz(rt) | gz(rt2) | gs(rn), f.def = gs(rn); break;
    case Op::LDNP: case Op::LDP_off: case Op::LDPSW_off: f.cls = Cls::load, f.def = gz(rt) | gz(rt2), f.use = gs(rn); break;
    case Op::LDP_post: case Op::LDP_pre: case Op::LDPSW_post: case Op::LDPSW_pre:
        f.cls = Cls::load, f.def = gz(rt) | gz(rt2) | gs(rn), f.use = gs(rn); break;

    // Prefetches
    case Op::PRFUM: case Op::PRFM_imm: f.cls = Cls::prefetch, f.use = gs(rn); break;
    case Op::PRFM_reg: f.cls = Cls::prefetch, f.use = gs(rn) | gz(rm); break;
    case Op::RPRFM: f.cls = Cls::prefetch, f.use = gs(fld(w, i.op, x.rn)) | gz(rm); break;

    case Op::LDRAA: f.cls = Cls::load, f.def = gz(rt) | ((w >> 11) & 1 ? gs(rn) : 0), f.use = gs(rn); break;

    // Data processing, register
    case Op::UDIV: case Op::SDIV: case Op::LSLV: case Op::LSRV: case Op::ASRV: case Op::RORV: case Op::PACGA: case Op::CRC32:
    case Op::AND_reg: case Op::BIC_reg: case Op::ORR_reg: case Op::ORN_reg: case Op::EOR_reg: case Op::EON_reg:
    case Op::ADD_reg: case Op::SUB_reg: case Op::SMULH: case Op::UMULH:
        f.def = gz(rd), f.use = gz(rn) | gz(rm); break;
    case Op::ANDS_reg: case Op::BICS_reg: case Op::ADDS_reg: case Op::SUBS_reg: f.def = gz(rd), f.use = gz(rn) | gz(rm), f.fdef = true; break;
    case Op::RBIT: case Op::REV16: case Op::REV32: case Op::REV_w: case Op::REV_x: case Op::CLZ: case Op::CLS: f.def = gz(rd), f.use = gz(rn); break;
    case Op::PAC: case Op::AUT: f.def = gz(rd), f.use = gz(rd) | gs(rn); break;
    case Op::XPACI: f.def = f.use = gz(rd); break;
    case Op::ADD_ext: case Op::SUB_ext: f.def = gs(rd), f.use = gs(rn) | gz(rm); break;
    case Op::ADDS_ext: case Op::SUBS_ext: f.def = gz(rd), f.use = gs(rn) | gz(rm), f.fdef = true; break;
    case Op::ADC: case Op::SBC: f.def = gz(rd), f.use = gz(rn) | gz(rm), f.fuse = true; break;
    case Op::ADCS: case Op::SBCS: f.def = gz(rd), f.use = gz(rn) | gz(rm), f.fuse = f.fdef = true; break;
    case Op::RMIF: case Op::SETF: f.use = gz(rn), f.fdef = f.fuse = true; break;
    case Op::CCMN_reg: case Op::CCMP_reg: f.use = gz(rn) | gz(rm), f.fdef = f.fuse = true; break;
    case Op::CCMN_imm: case Op::CCMP_imm: f.use = gz(rn), f.fdef = f.fuse = true; break;
    case Op::CSEL: case Op::CSINC: case Op::CSINV: case Op::CSNEG: f.def = gz(rd), f.use = gz(rn) | gz(rm), f.fuse = true; break;
    case Op::MADD: case Op::MSUB: case Op::SMADDL: case Op::SMSUBL: case Op::UMADDL: case Op::UMSUBL:
        f.def = gz(rd), f.use = gz(rn) | gz(rm) | gz(ra); break;

    default:
        if ((top & 5) == 4) {   // general-register loads and stores with an immediate or register offset
            const std::string_view n = op_name(i.op);
            const bool load = n.starts_with("LD");
            f.cls = load ? Cls::load : Cls::store;
            f.use = gs(rn) | (x.rm >= 0 ? gz(rm) : 0);
            if (load) f.def = gz(rt);
            else f.use |= gz(rt);
            if (x.writeback) f.def |= gs(rn);
        }
        break;
    }
    return f;
}

}  // namespace nxanalyze
