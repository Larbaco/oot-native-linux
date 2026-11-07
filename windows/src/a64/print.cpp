#include <bit>
#include <cinttypes>
#include <cstdio>

#include "print_impl.hpp"

namespace nxw::a64 {

namespace detail {

const char* lookup(std::span<const NameEntry> t, uint32_t key) {
    size_t lo = 0, hi = t.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (t[mid].key < key) lo = mid + 1;
        else hi = mid;
    }
    return lo < t.size() && t[lo].key == key ? t[lo].text : nullptr;
}

uint64_t bitmask(uint32_t N, uint32_t immr, uint32_t imms, unsigned regsize) {
    uint32_t combined = N << 6 | (~imms & 0x3f);
    if (combined == 0) return 0;
    int len = 31 - std::countl_zero(combined);
    if (len < 1) return 0;
    uint32_t levels = (1u << len) - 1;
    if ((imms & levels) == levels) return 0;
    uint32_t S = imms & levels, R = immr & levels;
    unsigned esize = 1u << len;
    uint64_t emask = esize == 64 ? ~0ull : (1ull << esize) - 1;
    uint64_t welem = S + 1 == 64 ? ~0ull : (1ull << (S + 1)) - 1;
    uint64_t elem = R ? ((welem >> R) | (welem << (esize - R))) & emask : welem;
    uint64_t v = 0;
    for (unsigned i = 0; i < regsize; i += esize) v |= elem << i;
    return v;
}

uint64_t simd_imm(uint32_t op, uint32_t cmode, uint32_t imm8) {
    uint64_t i8 = imm8;
    auto rep32 = [](uint64_t v) { return v | v << 32; };
    auto rep16 = [](uint64_t v) { return v | v << 16 | v << 32 | v << 48; };
    switch (cmode >> 1) {
    case 0: return rep32(i8);
    case 1: return rep32(i8 << 8);
    case 2: return rep32(i8 << 16);
    case 3: return rep32(i8 << 24);
    case 4: return rep16(i8);
    case 5: return rep16(i8 << 8);
    case 6: return (cmode & 1) ? rep32(i8 << 16 | 0xffff) : rep32(i8 << 8 | 0xff);
    default:
        if (!(cmode & 1) && !op) return i8 * 0x0101010101010101ull;
        if (!(cmode & 1) && op) {
            uint64_t v = 0;
            for (int i = 0; i < 8; ++i)
                if (imm8 >> i & 1) v |= 0xffull << (8 * i);
            return v;
        }
        return 0;   // FMOV forms are printed by fimm
    }
}

const char* pacname(uint32_t aut, uint32_t Z, uint32_t D, uint32_t key) {
    static const char* const n[] = {"pacia", "pacib", "pacda", "pacdb", "paciza", "pacizb", "pacdza", "pacdzb",
                                    "autia", "autib", "autda", "autdb", "autiza", "autizb", "autdza", "autdzb"};
    return n[aut << 3 | Z << 2 | D << 1 | key];
}

}  // namespace detail

namespace {
constexpr const char* kCond[] = {"eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"};
constexpr const char* kShift[] = {"lsl", "lsr", "asr", "ror"};
constexpr const char* kExt[] = {"uxtb", "uxth", "uxtw", "uxtx", "sxtb", "sxth", "sxtw", "sxtx"};
constexpr const char* kArr[] = {"8b", "16b", "4h", "8h", "2s", "4s", "1d", "2d"};
constexpr char kSz[] = {'b', 'h', 's', 'd', 'q'};
}  // namespace

void Printer::num(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%" PRIu64, v);
    o += b;
}
void Printer::hexu(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%" PRIx64, v);
    o += b;
}
void Printer::hexs(int64_t v) {
    if (v < 0) {
        o += '-';
        hexu(0 - static_cast<uint64_t>(v));
    } else {
        hexu(static_cast<uint64_t>(v));
    }
}

void Printer::r(uint32_t sf, uint32_t n) {
    if (n == 31) {
        o += sf ? "xzr" : "wzr";
        return;
    }
    o += sf ? 'x' : 'w';
    num(n);
}
void Printer::rsp(uint32_t sf, uint32_t n) {
    if (n == 31) o += sf ? "sp" : "wsp";
    else r(sf, n);
}
void Printer::xpair(uint32_t n) {
    x(n);
    o += ", ";
    x(n + 1);
}
void Printer::wpair(uint32_t n) {
    w(n);
    o += ", ";
    w(n + 1);
}

void Printer::imm(int64_t v) {
    o += '#';
    hexs(v);
}
void Printer::uimm(uint64_t v) {
    o += '#';
    hexu(v);
}
void Printer::dec(int64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "#%" PRId64, v);
    o += b;
}
void Printer::target(int64_t off) { hexu(pc + static_cast<uint64_t>(off)); }
void Printer::page(int64_t off) { hexu((pc & ~0xfffull) + static_cast<uint64_t>(off)); }
void Printer::cond(uint32_t c) { o += kCond[c & 15]; }

void Printer::fimm(uint32_t imm8) {
    // VFPExpandImm: sign, exponent NOT(b):b:b..., fraction cd efgh
    int sign = imm8 >> 7 & 1;
    int exp = (imm8 >> 4 & 7);
    int e = (exp & 4) ? exp - 7 : exp + 1;   // unbiased exponent: -3..4
    double mant = 1.0 + (imm8 & 15) / 16.0;
    double v = mant * (e >= 0 ? double(1 << e) : 1.0 / double(1 << -e));
    char b[32];
    std::snprintf(b, sizeof b, "#%.8f", sign ? -v : v);
    o += b;
}
void Printer::hex016(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "#%#016" PRIx64, v);
    o += b;
}

void Printer::shift(uint32_t type, uint32_t amount) {
    if (type == 0 && amount == 0) return;
    o += ", ";
    o += kShift[type & 3];
    o += " #";
    num(amount);
}
void Printer::aimm(uint32_t imm12, uint32_t sh) {
    imm(imm12);
    if (sh) o += ", lsl #12";
}
void Printer::ext(uint32_t sf, bool sp, uint32_t option, uint32_t amount) {
    if (sp && option == (sf ? 3u : 2u)) {
        if (amount) {
            o += ", lsl #";
            num(amount);
        }
        return;
    }
    o += ", ";
    o += kExt[option & 7];
    if (amount) {
        o += " #";
        num(amount);
    }
}
void Printer::memext(uint32_t option, uint32_t S, uint32_t amount) {
    bool lsl = option == 3;
    if (lsl && !S) return;
    o += ", ";
    o += lsl ? "lsl" : kExt[option & 7];
    if (S) {
        o += " #";
        num(amount);
    }
}
void Printer::off(int64_t v) {
    if (v) offz(v);
}
void Printer::offz(int64_t v) {
    o += ", ";
    imm(v);
}

void Printer::va(uint32_t n, uint32_t size, uint32_t q) {
    o += 'v';
    num(n & 31);
    o += '.';
    o += kArr[(size & 3) * 2 + (q & 1)];
}
void Printer::vt(uint32_t n, const char* arr) {
    o += 'v';
    num(n & 31);
    o += '.';
    o += arr;
}
void Printer::vs(uint32_t n, uint32_t sz) {
    o += kSz[sz];
    num(n & 31);
}
void Printer::ve(uint32_t n, uint32_t size, uint32_t idx) {
    o += 'v';
    num(n & 31);
    o += '.';
    o += kSz[size];
    o += '[';
    num(idx);
    o += ']';
}
void Printer::vl(uint32_t n, uint32_t count, uint32_t size, uint32_t q) {
    o += "{ ";
    for (uint32_t i = 0; i < count; ++i) {
        if (i) o += ", ";
        va(n + i, size, q);
    }
    o += " }";
}
void Printer::vlt(uint32_t n, uint32_t count, const char* arr) {
    o += "{ ";
    for (uint32_t i = 0; i < count; ++i) {
        if (i) o += ", ";
        vt(n + i, arr);
    }
    o += " }";
}
void Printer::vll(uint32_t n, uint32_t count, uint32_t size, uint32_t idx) {
    o += "{ ";
    for (uint32_t i = 0; i < count; ++i) {
        if (i) o += ", ";
        o += 'v';
        num((n + i) & 31);
        o += '.';
        o += kSz[size];
    }
    o += " }[";
    num(idx);
    o += ']';
}

void Printer::sysreg(uint32_t enc, bool read) {
    if (const char* s = detail::lookup(read ? detail::names_sysreg_r : detail::names_sysreg_w, enc)) {
        o += s;
        return;
    }
    char b[32];
    std::snprintf(b, sizeof b, "S%u_%u_C%u_C%u_%u", 2 + (enc >> 14 & 1), enc >> 11 & 7, enc >> 7 & 15, enc >> 3 & 15, enc & 7);
    o += b;
}
void Printer::sysreg128(uint32_t op0, uint32_t enc, bool read) {
    if (op0 >= 2) return sysreg((op0 - 2) << 14 | enc, read);
    char b[32];
    std::snprintf(b, sizeof b, "S%u_%u_C%u_C%u_%u", op0, enc >> 11 & 7, enc >> 7 & 15, enc >> 3 & 15, enc & 7);
    o += b;
}
void Printer::sysreg0(uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2) {
    char b[32];
    std::snprintf(b, sizeof b, "S0_%u_C%u_C%u_%u", op1, crn, crm, op2);
    o += b;
}
void Printer::name(std::span<const detail::NameEntry> t, uint32_t key, const char* fallback_prefix, int64_t fallback) {
    if (const char* s = detail::lookup(t, key)) {
        o += s;
        return;
    }
    o += fallback_prefix;
    imm(fallback);
}

void Printer::txt(std::span<const detail::NameEntry> t, uint32_t key) {
    const char* s = detail::lookup(t, key);
    o += s ? s : "?";
}
void Printer::prfop(uint32_t rt) { name(detail::names_prfm, rt, "", rt); }
void Printer::sysop(uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2, uint32_t rt) {
    uint32_t key = op1 << 11 | crn << 7 | crm << 3 | op2;
    if (const char* s = detail::lookup(detail::names_sys, key)) {
        o += s;
        x(rt);
        return;
    }
    if (rt == 31)
        if (const char* s = detail::lookup(detail::names_sysz, key)) {
            o += s;
            return;
        }
    o += "sys ";
    imm(op1);
    o += ", c";
    num(crn);
    o += ", c";
    num(crm);
    o += ", ";
    imm(op2);
    if (rt != 31) {
        o += ", ";
        x(rt);
    }
}
void Printer::postinc(uint32_t rm, uint32_t bytes) {
    if (rm == 31) {
        o += '#';
        num(bytes);
    } else {
        x(rm);
    }
}
void Printer::ldra_off(int64_t off, uint32_t wb) {
    if (wb) {
        offz(off);
        o += "]!";
    } else {
        off ? offz(off) : void();
        o += ']';
    }
}
void Printer::pacrn(uint32_t z, uint32_t n) {
    if (z) return;
    o += ", ";
    xsp(n);
}

void print(std::string& out, uint32_t w, uint64_t pc) {
    Printer P{out, pc};
    print_body(P, decode(w), w);
}
std::string print(uint32_t w, uint64_t pc) {
    std::string s;
    print(s, w, pc);
    return s;
}

}  // namespace nxw::a64
