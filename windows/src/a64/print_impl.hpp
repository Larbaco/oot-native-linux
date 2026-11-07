// Printer used by the generated code (a64_gen.cpp): one method per operand kind of the table's formats, LLVM 23 spelling.
#pragma once
#include <cstdint>
#include <span>
#include <string>

#include "nxw/a64.hpp"

namespace nxw::a64 {

namespace detail {

struct NameEntry {
    uint32_t key;
    const char* text;
};
// name tables generated from names.tbl
extern const std::span<const NameEntry> names_sysreg_r, names_sysreg_w, names_pstate, names_hint, names_dmb, names_dsb,
    names_isb, names_prfm, names_rprfm, names_sys, names_sysz;
const char* lookup(std::span<const NameEntry> t, uint32_t key);

constexpr int64_t sext(uint64_t v, unsigned bits) { return static_cast<int64_t>(v << (64 - bits)) >> (64 - bits); }
// DecodeBitMasks(N, imms, immr, immediate=true) replicated to regsize bits; 0 when the encoding is reserved
uint64_t bitmask(uint32_t N, uint32_t immr, uint32_t imms, unsigned regsize);
// AdvSIMDExpandImm(op, cmode, imm8)
uint64_t simd_imm(uint32_t op, uint32_t cmode, uint32_t imm8);
inline bool logimm_ok(uint32_t sf, uint32_t N, uint32_t immr, uint32_t imms) { return (sf || !N) && bitmask(N, immr, imms, 32 << sf); }

// LLVM's preference between MOVZ, MOVN and ORR-immediate for the "mov" alias (AArch64AddressingModes.h)
constexpr bool movz_ok(uint64_t v, int shift, int width) {
    if (width == 32) v &= 0xffffffffull;
    if (v == 0 && shift != 0) return false;
    return (v & ~(0xffffull << shift)) == 0;
}
constexpr bool any_movz(uint64_t v, int width) {
    for (int s = 0; s <= width - 16; s += 16)
        if (movz_ok(v, s, width)) return true;
    return false;
}
constexpr bool any_movw(uint64_t v, int width) {
    if (any_movz(v, width)) return true;
    v = ~v;
    if (width == 32) v &= 0xffffffffull;
    return any_movz(v, width);
}
constexpr bool movn_alias(uint32_t sf, uint32_t imm16, uint32_t hw) {
    int width = sf ? 64 : 32, shift = int(hw) * 16;
    uint64_t v = ~(uint64_t(imm16) << shift);
    if (width == 32) v &= 0xffffffffull;
    if (any_movz(v, width)) return false;
    v = ~v;
    if (width == 32) v &= 0xffffffffull;
    return movz_ok(v, shift, width);
}

// mnemonic pieces
inline constexpr const char* kAr[] = {"", "l", "a", "al"};
inline constexpr const char* kBh[] = {"b", "h", "", ""};
inline constexpr const char* kCrc[] = {"b", "h", "w", "x"};
constexpr const char* ar(uint32_t acquire, uint32_t release) { return kAr[acquire * 2 + release]; }
constexpr const char* bh(uint32_t size) { return kBh[size & 3]; }
constexpr const char* crcsz(uint32_t sz) { return kCrc[sz & 3]; }
const char* pacname(uint32_t aut, uint32_t Z, uint32_t D, uint32_t key);

// SIMD&FP operand helpers
constexpr uint32_t fpsz(uint32_t ftype) { return ftype == 0 ? 2 : ftype == 1 ? 3 : 1; }       // FP type -> b/h/s/d/q index
constexpr uint32_t esz(uint32_t imm5) { return imm5 & 1 ? 0 : imm5 & 2 ? 1 : imm5 & 4 ? 2 : imm5 & 8 ? 3 : 4; }
constexpr uint32_t hsz(uint32_t immh) { return immh & 8 ? 3 : immh & 4 ? 2 : immh & 2 ? 1 : 0; }
constexpr uint32_t elreg(uint32_t size, uint32_t M, uint32_t Rm) { return size == 1 ? Rm : M << 4 | Rm; }
constexpr uint32_t elidx(uint32_t size, uint32_t H, uint32_t L, uint32_t M) { return size == 1 ? H << 2 | L << 1 | M : H << 1 | L; }
constexpr uint32_t lane_sz(uint32_t kind, uint32_t size) { return kind < 2 ? kind : size == 0 ? 2 : 3; }
constexpr uint32_t lane_idx(uint32_t kind, uint32_t Q, uint32_t S, uint32_t size) {
    return kind == 0 ? Q << 3 | S << 2 | size : kind == 1 ? Q << 2 | S << 1 | size >> 1 : size == 0 ? Q << 1 | S : Q;
}
constexpr bool lane_ok(uint32_t kind, uint32_t S, uint32_t size) {
    return kind == 0 || (kind == 1 && !(size & 1)) || (kind == 2 && (size == 0 || (size == 1 && !S)));
}

}  // namespace detail

struct Printer {
    std::string& o;
    uint64_t pc;

    void lit(const char* s) { o += s; }
    void s(const char* t) { o += t; }
    void num(uint64_t v);                 // decimal, no '#'
    void hexs(int64_t v);                 // 0x.. / -0x..
    void hexu(uint64_t v);                // 0x..

    // general registers
    void r(uint32_t sf, uint32_t n);      // w/x, 31 = zr
    void rsp(uint32_t sf, uint32_t n);    // w/x, 31 = wsp/sp
    void w(uint32_t n) { r(0, n); }
    void x(uint32_t n) { r(1, n); }
    void wsp(uint32_t n) { rsp(0, n); }
    void xsp(uint32_t n) { rsp(1, n); }
    void xpair(uint32_t n);               // x0, x1 (casp)
    void wpair(uint32_t n);

    // immediates
    void imm(int64_t v);                  // #0x.. (signed hex)
    void uimm(uint64_t v);                // #0x.. (unsigned hex)
    void dec(int64_t v);                  // #n
    void target(int64_t off);             // pc + off, as 0x..
    void page(int64_t off);               // (pc & ~0xfff) + off
    void cond(uint32_t c);
    void fimm(uint32_t imm8);             // #1.00000000
    void hex016(uint64_t v);              // #%#016llx (movi 64-bit)

    // shifted / extended operands
    void shift(uint32_t type, uint32_t amount);                // ", lsl #3"; nothing for lsl #0
    void aimm(uint32_t imm12, uint32_t sh);                     // #0x10[, lsl #12]
    void ext(uint32_t sf, bool sp, uint32_t option, uint32_t amount);   // ", uxtw #2", lsl form next to sp
    void memext(uint32_t option, uint32_t S, uint32_t amount);  // register offset: ", sxtw #2", ", lsl #3", nothing
    void off(int64_t v);                  // ", #0x.." unless 0
    void offz(int64_t v);                 // ", #0x.." always

    // SIMD&FP
    void va(uint32_t n, uint32_t size, uint32_t q);              // v0.4s
    void vt(uint32_t n, const char* arr);                        // v0.<arr>
    void vs(uint32_t n, uint32_t sz);                            // b/h/s/d/q n
    void ve(uint32_t n, uint32_t size, uint32_t idx);            // v0.s[1]
    void vl(uint32_t n, uint32_t count, uint32_t size, uint32_t q);   // { v0.4s, v1.4s }
    void vlt(uint32_t n, uint32_t count, const char* arr);
    void vll(uint32_t n, uint32_t count, uint32_t size, uint32_t idx); // { v0.s, v1.s }[1]
    void q2(uint32_t q) { if (q) o += '2'; }                       // second-half suffix
    void postinc(uint32_t rm, uint32_t bytes);                      // "#bytes" (decimal) for Rm = 31, else xm

    // system
    void sysreg(uint32_t enc, bool read);   // enc = o0:op1:CRn:CRm:op2 (op0 = 2 + o0)
    void sysreg0(uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2);   // S0_op1_Cn_Cm_op2
    void sysreg128(uint32_t op0, uint32_t enc, bool read);   // MRRS/MSRR: enc = op1:CRn:CRm:op2; same names as MRS/MSR
    void xpair_opt(uint32_t n) { if (n != 31) { o += ", "; x(n); o += ", "; x(n + 1); } }
    void name(std::span<const detail::NameEntry> t, uint32_t key, const char* fallback_prefix, int64_t fallback);
    void txt(std::span<const detail::NameEntry> t, uint32_t key);
    void prfop(uint32_t rt);
    void sysop(uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2, uint32_t rt);
    void pair(uint32_t sf, uint32_t n) { sf ? xpair(n) : wpair(n); }
    void ldra_off(int64_t off, uint32_t wb);   // ", #off]!" / ", #off]" / "]"
    void pacrn(uint32_t z, uint32_t n);        // ", xn|sp" unless the Z form
};

void print_body(Printer& P, Op op, uint32_t w);

}  // namespace nxw::a64
