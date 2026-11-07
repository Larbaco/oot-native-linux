// Loads and stores: every addressing mode, pairs, ordered and exclusive accesses, LSE atomics, and the AdvSIMD structure
// loads and stores. Guest memory is identity-mapped; plain accesses are volatile, atomics are LLVM atomics (DESIGN D7).
#include "common.hpp"

using namespace nxw::sem;

namespace {

NXW_INLINE void ldr_int(State* s, unsigned rt, u64 a, unsigned bytes, bool sign, bool x64) {
    u64 v;
    switch (bytes) {
    case 1: v = sign ? u64(i64(ld<i8>(a))) : ld<u8>(a); break;
    case 2: v = sign ? u64(i64(ld<i16>(a))) : ld<u16>(a); break;
    case 4: v = sign ? u64(i64(ld<i32>(a))) : ld<u32>(a); break;
    default: v = ld<u64>(a); break;
    }
    wX(s, rt, x64 ? v : u32(v));
}
NXW_INLINE void str_int(const State* s, unsigned rt, u64 a, unsigned bytes) {
    const u64 v = X(s, rt);
    switch (bytes) {
    case 1: st<u8>(a, u8(v)); break;
    case 2: st<u16>(a, u16(v)); break;
    case 4: st<u32>(a, u32(v)); break;
    default: st<u64>(a, v); break;
    }
}
NXW_INLINE void ldr_fp(State* s, unsigned rt, u64 a, unsigned bytes) {
    switch (bytes) {
    case 1: vset_scalar<u8>(s, rt, ld<u8>(a)); break;
    case 2: vset_scalar<u16>(s, rt, ld<u16>(a)); break;
    case 4: vset_scalar<u32>(s, rt, ld<u32>(a)); break;
    case 8: vset_scalar<u64>(s, rt, ld<u64>(a)); break;
    default: s->q[rt] = ld128(a); break;
    }
}
NXW_INLINE void str_fp(const State* s, unsigned rt, u64 a, unsigned bytes) {
    switch (bytes) {
    case 1: st<u8>(a, lane<u8>(s, rt, 0)); break;
    case 2: st<u16>(a, lane<u16>(s, rt, 0)); break;
    case 4: st<u32>(a, lane<u32>(s, rt, 0)); break;
    case 8: st<u64>(a, lane<u64>(s, rt, 0)); break;
    default: st128(a, s->q[rt]); break;
    }
}
NXW_INLINE unsigned log2b(unsigned bytes) { return unsigned(std::countr_zero(bytes)); }

}  // namespace

// Addressing modes. BODY uses a (the address) and bytes.
#define LS_IMM(NAME, BYTES, BODY) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        const unsigned bytes = BYTES; \
        const u64 a = XS(s, f.Rn) + u64(f.imm12) * bytes; \
        BODY; \
        return 0; \
    }
#define LS_UNSC(NAME, BYTES, BODY) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        [[maybe_unused]] const unsigned bytes = BYTES; \
        const u64 a = XS(s, f.Rn) + u64(sext(f.imm9, 9)); \
        BODY; \
        return 0; \
    }
#define LS_POST(NAME, BYTES, BODY) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        [[maybe_unused]] const unsigned bytes = BYTES; \
        const u64 a = XS(s, f.Rn); \
        BODY; \
        wXS(s, f.Rn, a + u64(sext(f.imm9, 9))); \
        return 0; \
    }
#define LS_PRE(NAME, BYTES, BODY) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        [[maybe_unused]] const unsigned bytes = BYTES; \
        const u64 a = XS(s, f.Rn) + u64(sext(f.imm9, 9)); \
        BODY; \
        wXS(s, f.Rn, a); \
        return 0; \
    }
#define LS_REG(NAME, BYTES, BODY) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        [[maybe_unused]] const unsigned bytes = BYTES; \
        const u64 a = XS(s, f.Rn) + extend_reg(X(s, f.Rm), f.option, f.S ? log2b(bytes) : 0); \
        BODY; \
        return 0; \
    }
#define LS_ALL5(PREFIX, BYTES, BODY) \
    LS_IMM(PREFIX##_imm, BYTES, BODY) \
    LS_POST(PREFIX##_post, BYTES, BODY) \
    LS_PRE(PREFIX##_pre, BYTES, BODY) \
    LS_REG(PREFIX##_reg, BYTES, BODY)

// ---------------------------------------------------------------- general-register loads and stores
LS_ALL5(STRB, 1, str_int(s, f.Rt, a, 1))
LS_ALL5(LDRB, 1, ldr_int(s, f.Rt, a, 1, false, false))
LS_ALL5(LDRSB, 1, ldr_int(s, f.Rt, a, 1, true, !f.opc0))
LS_ALL5(STRH, 2, str_int(s, f.Rt, a, 2))
LS_ALL5(LDRH, 2, ldr_int(s, f.Rt, a, 2, false, false))
LS_ALL5(LDRSH, 2, ldr_int(s, f.Rt, a, 2, true, !f.opc0))
LS_ALL5(STR, 4u << f.sf, str_int(s, f.Rt, a, bytes))
LS_ALL5(LDR, 4u << f.sf, ldr_int(s, f.Rt, a, bytes, false, f.sf))
LS_ALL5(LDRSW, 4, ldr_int(s, f.Rt, a, 4, true, true))
LS_IMM(PRFM_imm, 8, (void)a)
LS_REG(PRFM_reg, 8, (void)a)

LS_UNSC(STURB, 1, str_int(s, f.Rt, a, 1))
LS_UNSC(LDURB, 1, ldr_int(s, f.Rt, a, 1, false, false))
LS_UNSC(LDURSB, 1, ldr_int(s, f.Rt, a, 1, true, !f.opc0))
LS_UNSC(STURH, 2, str_int(s, f.Rt, a, 2))
LS_UNSC(LDURH, 2, ldr_int(s, f.Rt, a, 2, false, false))
LS_UNSC(LDURSH, 2, ldr_int(s, f.Rt, a, 2, true, !f.opc0))
LS_UNSC(STUR, 4u << f.sf, str_int(s, f.Rt, a, bytes))
LS_UNSC(LDUR, 4u << f.sf, ldr_int(s, f.Rt, a, bytes, false, f.sf))
LS_UNSC(LDURSW, 4, ldr_int(s, f.Rt, a, 4, true, true))
LS_UNSC(PRFUM, 8, (void)a)
// unprivileged forms behave as the ordinary ones at EL0
LS_UNSC(STTRB, 1, str_int(s, f.Rt, a, 1))
LS_UNSC(LDTRB, 1, ldr_int(s, f.Rt, a, 1, false, false))
LS_UNSC(LDTRSB, 1, ldr_int(s, f.Rt, a, 1, true, !f.opc0))
LS_UNSC(STTRH, 2, str_int(s, f.Rt, a, 2))
LS_UNSC(LDTRH, 2, ldr_int(s, f.Rt, a, 2, false, false))
LS_UNSC(LDTRSH, 2, ldr_int(s, f.Rt, a, 2, true, !f.opc0))
LS_UNSC(STTR, 4u << f.sf, str_int(s, f.Rt, a, bytes))
LS_UNSC(LDTR, 4u << f.sf, ldr_int(s, f.Rt, a, bytes, false, f.sf))
LS_UNSC(LDTRSW, 4, ldr_int(s, f.Rt, a, 4, true, true))

// ---------------------------------------------------------------- SIMD&FP register loads and stores
#define FPB (f.opc1 ? 16u : 1u << f.size)
LS_ALL5(STR_fp, FPB, str_fp(s, f.Rt, a, bytes))
LS_ALL5(LDR_fp, FPB, ldr_fp(s, f.Rt, a, bytes))
LS_UNSC(STUR_fp, FPB, str_fp(s, f.Rt, a, bytes))
LS_UNSC(LDUR_fp, FPB, ldr_fp(s, f.Rt, a, bytes))

// ---------------------------------------------------------------- literal loads
SEM(LDR_lit) {
    const Fl<Op::LDR_lit> f(w);
    ldr_int(s, f.Rt, pc + u64(sext(f.imm19, 19) * 4), 4u << f.sf, false, f.sf);
    return 0;
}
SEM(LDRSW_lit) {
    const Fl<Op::LDRSW_lit> f(w);
    ldr_int(s, f.Rt, pc + u64(sext(f.imm19, 19) * 4), 4, true, true);
    return 0;
}
SEM(PRFM_lit) { return 0; }
SEM(LDR_lit_fp) {
    const Fl<Op::LDR_lit_fp> f(w);
    ldr_fp(s, f.Rt, pc + u64(sext(f.imm19, 19) * 4), 4u << f.opc);
    return 0;
}

// ---------------------------------------------------------------- pairs
// MODE 0 offset, 1 pre-index, 2 post-index
#define PAIR(NAME, MODE, BYTES, BODY) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        const unsigned bytes = BYTES; \
        const u64 base = XS(s, f.Rn), off = u64(sext(f.imm7, 7) * i64(bytes)); \
        const u64 a = MODE == 2 ? base : base + off; \
        BODY; \
        if (MODE) wXS(s, f.Rn, base + off); \
        return 0; \
    }
#define PAIR_INT_LD(bytes, sign, x64) \
    do { \
        const u64 a2 = a + bytes; \
        u64 v1, v2; \
        if (bytes == 8) v1 = ld<u64>(a), v2 = ld<u64>(a2); \
        else if (sign) v1 = u64(i64(ld<i32>(a))), v2 = u64(i64(ld<i32>(a2))); \
        else v1 = ld<u32>(a), v2 = ld<u32>(a2); \
        wX(s, f.Rt, v1); \
        wX(s, f.Rt2, v2); \
        (void)x64; \
    } while (0)
#define PAIR_INT_ST(bytes) \
    do { \
        const u64 v1 = X(s, f.Rt), v2 = X(s, f.Rt2); \
        if (bytes == 8) st<u64>(a, v1), st<u64>(a + 8, v2); \
        else st<u32>(a, u32(v1)), st<u32>(a + 4, u32(v2)); \
    } while (0)
#define PAIR_FP_LD() \
    do { \
        ldr_fp(s, f.Rt, a, bytes); \
        ldr_fp(s, f.Rt2, a + bytes, bytes); \
    } while (0)
#define PAIR_FP_ST() \
    do { \
        str_fp(s, f.Rt, a, bytes); \
        str_fp(s, f.Rt2, a + bytes, bytes); \
    } while (0)

PAIR(STNP, 0, 4u << f.sf, PAIR_INT_ST(bytes))
PAIR(LDNP, 0, 4u << f.sf, PAIR_INT_LD(bytes, false, f.sf))
PAIR(STP_off, 0, 4u << f.sf, PAIR_INT_ST(bytes))
PAIR(LDP_off, 0, 4u << f.sf, PAIR_INT_LD(bytes, false, f.sf))
PAIR(STP_pre, 1, 4u << f.sf, PAIR_INT_ST(bytes))
PAIR(LDP_pre, 1, 4u << f.sf, PAIR_INT_LD(bytes, false, f.sf))
PAIR(STP_post, 2, 4u << f.sf, PAIR_INT_ST(bytes))
PAIR(LDP_post, 2, 4u << f.sf, PAIR_INT_LD(bytes, false, f.sf))
PAIR(LDPSW_off, 0, 4, PAIR_INT_LD(4, true, true))
PAIR(LDPSW_pre, 1, 4, PAIR_INT_LD(4, true, true))
PAIR(LDPSW_post, 2, 4, PAIR_INT_LD(4, true, true))
PAIR(STNP_fp, 0, 4u << f.opc, PAIR_FP_ST())
PAIR(LDNP_fp, 0, 4u << f.opc, PAIR_FP_LD())
PAIR(STP_fp_off, 0, 4u << f.opc, PAIR_FP_ST())
PAIR(LDP_fp_off, 0, 4u << f.opc, PAIR_FP_LD())
PAIR(STP_fp_pre, 1, 4u << f.opc, PAIR_FP_ST())
PAIR(LDP_fp_pre, 1, 4u << f.opc, PAIR_FP_LD())
PAIR(STP_fp_post, 2, 4u << f.opc, PAIR_FP_ST())
PAIR(LDP_fp_post, 2, 4u << f.opc, PAIR_FP_LD())

// ---------------------------------------------------------------- ordered (acquire / release) accesses
namespace {
template <class T> NXW_INLINE u64 load_ord(u64 a, int ord) { return __atomic_load_n(aptr<T>(a), ord); }
template <class T> NXW_INLINE void store_ord(u64 a, u64 v, int ord) { __atomic_store_n(aptr<T>(a), T(v), ord); }
NXW_INLINE u64 load_sized(u64 a, unsigned bytes, int ord) {
    switch (bytes) {
    case 1: return load_ord<u8>(a, ord);
    case 2: return load_ord<u16>(a, ord);
    case 4: return load_ord<u32>(a, ord);
    default: return load_ord<u64>(a, ord);
    }
}
NXW_INLINE void store_sized(u64 a, unsigned bytes, u64 v, int ord) {
    switch (bytes) {
    case 1: store_ord<u8>(a, v, ord); break;
    case 2: store_ord<u16>(a, v, ord); break;
    case 4: store_ord<u32>(a, v, ord); break;
    default: store_ord<u64>(a, v, ord); break;
    }
}
}  // namespace

// LDAR is sequentially consistent and STLR too (an STLR then LDAR pair must not be reordered; research/03 §3.6); LDAPR (RCpc)
// is an acquire load
#define LDA(NAME, BYTES, ORD) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        wX(s, f.Rt, load_sized(XS(s, f.Rn), BYTES, ORD)); \
        return 0; \
    }
#define STL(NAME, BYTES, ORD) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        store_sized(XS(s, f.Rn), BYTES, X(s, f.Rt), ORD); \
        return 0; \
    }
LDA(LDARB, 1, __ATOMIC_SEQ_CST)
LDA(LDARH, 2, __ATOMIC_SEQ_CST)
LDA(LDAR, 4u << f.sf, __ATOMIC_SEQ_CST)
LDA(LDLARB, 1, __ATOMIC_SEQ_CST)
LDA(LDLARH, 2, __ATOMIC_SEQ_CST)
LDA(LDLAR, 4u << f.sf, __ATOMIC_SEQ_CST)
STL(STLRB, 1, __ATOMIC_SEQ_CST)
STL(STLRH, 2, __ATOMIC_SEQ_CST)
STL(STLR, 4u << f.sf, __ATOMIC_SEQ_CST)
STL(STLLRB, 1, __ATOMIC_SEQ_CST)
STL(STLLRH, 2, __ATOMIC_SEQ_CST)
STL(STLLR, 4u << f.sf, __ATOMIC_SEQ_CST)
LDA(LDAPR, 1u << f.size, __ATOMIC_ACQUIRE)

#define LDAPUR_(NAME, BYTES, SIGN, X64) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        u64 v = load_sized(XS(s, f.Rn) + u64(sext(f.imm9, 9)), BYTES, __ATOMIC_ACQUIRE); \
        if (SIGN) v = u64(sext(v, (BYTES) * 8)); \
        wX(s, f.Rt, (X64) ? v : u32(v)); \
        return 0; \
    }
#define STLUR_(NAME, BYTES) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        store_sized(XS(s, f.Rn) + u64(sext(f.imm9, 9)), BYTES, X(s, f.Rt), __ATOMIC_SEQ_CST); \
        return 0; \
    }
LDAPUR_(LDAPURB, 1, false, false)
LDAPUR_(LDAPURSB, 1, true, !f.opc0)
LDAPUR_(LDAPURH, 2, false, false)
LDAPUR_(LDAPURSH, 2, true, !f.opc0)
LDAPUR_(LDAPUR, 4u << f.sf, false, f.sf)
LDAPUR_(LDAPURSW, 4, true, true)
STLUR_(STLURB, 1)
STLUR_(STLURH, 2)
STLUR_(STLUR, 4u << f.sf)

// ---------------------------------------------------------------- exclusives: a monitor of (address, value), the store a CAS
// against the value loaded (research/03 §3.6; the ABA case is accepted, as in Dynarmic and ARMeilleure)
#define LDX(NAME, BYTES, ACQ) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        const u64 a = XS(s, f.Rn); \
        const u64 v = load_sized(a, BYTES, (ACQ) ? __ATOMIC_ACQUIRE : __ATOMIC_RELAXED); \
        s->excl_addr = a, s->excl_val[0] = v; \
        wX(s, f.Rt, v); \
        return 0; \
    }
namespace {
template <class T> NXW_INLINE bool cas_sized(u64 a, u64 expected, u64 desired, bool rel) {
    T e = T(expected);
    return __atomic_compare_exchange_n(aptr<T>(a), &e, T(desired), false, rel ? __ATOMIC_RELEASE : __ATOMIC_RELAXED,
                                       __ATOMIC_RELAXED);
}
NXW_INLINE u32 store_excl(State* s, u64 a, unsigned bytes, u64 v, bool rel) {
    const bool mine = s->excl_addr == a;
    s->excl_addr = ~0ull;
    if (!mine) return 1;
    bool ok;
    switch (bytes) {
    case 1: ok = cas_sized<u8>(a, s->excl_val[0], v, rel); break;
    case 2: ok = cas_sized<u16>(a, s->excl_val[0], v, rel); break;
    case 4: ok = cas_sized<u32>(a, s->excl_val[0], v, rel); break;
    default: ok = cas_sized<u64>(a, s->excl_val[0], v, rel); break;
    }
    return ok ? 0 : 1;
}
}  // namespace
#define STX(NAME, BYTES, REL) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        const u32 status = store_excl(s, XS(s, f.Rn), BYTES, X(s, f.Rt), REL); \
        wX(s, f.Rs, status); \
        return 0; \
    }
LDX(LDXRB, 1, false)
LDX(LDAXRB, 1, true)
LDX(LDXRH, 2, false)
LDX(LDAXRH, 2, true)
LDX(LDXR, 4u << f.sf, false)
LDX(LDAXR, 4u << f.sf, true)
STX(STXRB, 1, false)
STX(STLXRB, 1, true)
STX(STXRH, 2, false)
STX(STLXRH, 2, true)
STX(STXR, 4u << f.sf, false)
STX(STLXR, 4u << f.sf, true)

#define LDXP_(NAME, ACQ) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        const u64 a = XS(s, f.Rn); \
        if (f.sf) { \
            const u128 v = __atomic_load_n(aptr<u128>(a), (ACQ) ? __ATOMIC_ACQUIRE : __ATOMIC_RELAXED); \
            s->excl_val[0] = u64(v), s->excl_val[1] = u64(v >> 64); \
            wX(s, f.Rt, u64(v)), wX(s, f.Rt2, u64(v >> 64)); \
        } else { \
            const u64 v = __atomic_load_n(aptr<u64>(a), (ACQ) ? __ATOMIC_ACQUIRE : __ATOMIC_RELAXED); \
            s->excl_val[0] = v; \
            wX(s, f.Rt, u32(v)), wX(s, f.Rt2, v >> 32); \
        } \
        s->excl_addr = a; \
        return 0; \
    }
#define STXP_(NAME, REL) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        const u64 a = XS(s, f.Rn); \
        const bool mine = s->excl_addr == a; \
        s->excl_addr = ~0ull; \
        bool ok = false; \
        if (mine) { \
            if (f.sf) { \
                u128 e = u128(s->excl_val[1]) << 64 | s->excl_val[0]; \
                const u128 d = u128(X(s, f.Rt2)) << 64 | X(s, f.Rt); \
                ok = __atomic_compare_exchange_n(aptr<u128>(a), &e, d, false, (REL) ? __ATOMIC_RELEASE : __ATOMIC_RELAXED, \
                                                 __ATOMIC_RELAXED); \
            } else { \
                ok = cas_sized<u64>(a, s->excl_val[0], X(s, f.Rt2) << 32 | u32(X(s, f.Rt)), REL); \
            } \
        } \
        wX(s, f.Rs, ok ? 0 : 1); \
        return 0; \
    }
LDXP_(LDXP, false)
LDXP_(LDAXP, true)
STXP_(STXP, false)
STXP_(STLXP, true)

// ---------------------------------------------------------------- LSE
SEM(CAS) {
    const Fl<Op::CAS> f(w);
    const u64 a = XS(s, f.Rn);
    const int succ = order(f.L, f.o0), fail = f.L ? __ATOMIC_ACQUIRE : __ATOMIC_RELAXED;
    NXW_BY_SIZE(f.size, {
        T e = T(X(s, f.Rs));
        __atomic_compare_exchange_n(aptr<T>(a), &e, T(X(s, f.Rt)), false, succ, fail);
        wX(s, f.Rs, u64(e));
    });
    return 0;
}
SEM(CASP) {
    const Fl<Op::CASP> f(w);
    const u64 a = XS(s, f.Rn);
    const int succ = order(f.L, f.o0), fail = f.L ? __ATOMIC_ACQUIRE : __ATOMIC_RELAXED;
    if (f.sz) {
        u128 e = u128(X(s, f.Rs + 1)) << 64 | X(s, f.Rs);
        const u128 d = u128(X(s, f.Rt + 1)) << 64 | X(s, f.Rt);
        __atomic_compare_exchange_n(aptr<u128>(a), &e, d, false, succ, fail);
        wX(s, f.Rs, u64(e)), wX(s, f.Rs + 1, u64(e >> 64));
    } else {
        u64 e = X(s, f.Rs + 1) << 32 | u32(X(s, f.Rs));
        const u64 d = X(s, f.Rt + 1) << 32 | u32(X(s, f.Rt));
        __atomic_compare_exchange_n(aptr<u64>(a), &e, d, false, succ, fail);
        wX(s, f.Rs, u32(e)), wX(s, f.Rs + 1, e >> 32);
    }
    return 0;
}

#define LSE(NAME, EXPR) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        const u64 a = XS(s, f.Rn); \
        const int ord = order(f.A, f.R); \
        NXW_BY_SIZE(f.size, { \
            using S = std::make_signed_t<T>; \
            const T v = T(X(s, f.Rs)); \
            (void)sizeof(S); \
            const T old = T(EXPR); \
            wX(s, f.Rt, u64(old)); \
        }); \
        return 0; \
    }
LSE(LDADD, __atomic_fetch_add(aptr<T>(a), v, ord))
LSE(LDCLR, __atomic_fetch_and(aptr<T>(a), T(~v), ord))
LSE(LDEOR, __atomic_fetch_xor(aptr<T>(a), v, ord))
LSE(LDSET, __atomic_fetch_or(aptr<T>(a), v, ord))
LSE(LDSMAX, __atomic_fetch_max(aptr<S>(a), S(v), ord))
LSE(LDSMIN, __atomic_fetch_min(aptr<S>(a), S(v), ord))
LSE(LDUMAX, __atomic_fetch_max(aptr<T>(a), v, ord))
LSE(LDUMIN, __atomic_fetch_min(aptr<T>(a), v, ord))
LSE(SWP, __atomic_exchange_n(aptr<T>(a), v, ord))

// ---------------------------------------------------------------- AdvSIMD structure loads and stores
namespace {

// Multiple structures: selem-way interleaved (LDn/STn), or regs consecutive registers of contiguous data (LD1/ST1 with
// 2-4 registers: selem 1)
NXW_INLINE void ld_multi(State* s, unsigned t, u64 a, unsigned size, bool q, unsigned selem, unsigned regs, bool load) {
    const unsigned bytes = q ? 16 : 8;
    if (selem == 1) {   // LD1 / ST1: each register is contiguous
        for (unsigned r = 0; r < regs; r++, a += bytes) {
            const unsigned reg = (t + r) % 32;
            if (load) {
                if (q) s->q[reg] = ld128(a);
                else vset_scalar<u64>(s, reg, ld<u64>(a));
            } else {
                if (q) st128(a, s->q[reg]);
                else st<u64>(a, lane<u64>(s, reg, 0));
            }
        }
        return;
    }
    NXW_BY_SIZE(size, {
        const unsigned elems = bytes / sizeof(T);
        LV<T> v[4];
        for (unsigned r = 0; r < selem; r++) v[r] = load ? LV<T>{} : vget<T>(s, (t + r) % 32);
        for (unsigned e = 0; e < elems; e++)
            for (unsigned r = 0; r < selem; r++, a += sizeof(T)) {
                if (load) v[r][e] = ld<T>(a);
                else st<T>(a, v[r][e]);
            }
        if (load)
            for (unsigned r = 0; r < selem; r++) vset<T>(s, (t + r) % 32, v[r], q);
    });
}

struct LaneSel {
    unsigned size, index;
};
NXW_INLINE LaneSel lane_sel(unsigned kind, bool q, unsigned S, unsigned size) {
    switch (kind) {
    case 0: return {0, unsigned(q) << 3 | S << 2 | size};
    case 1: return {1, unsigned(q) << 2 | S << 1 | size >> 1};
    default:
        if (size & 1) return {3, unsigned(q)};
        return {2, unsigned(q) << 1 | S};
    }
}
NXW_INLINE void ld_lane(State* s, unsigned t, u64 a, LaneSel l, unsigned selem, bool load) {
    NXW_BY_SIZE(l.size, {
        for (unsigned r = 0; r < selem; r++, a += sizeof(T)) {
            const unsigned reg = (t + r) % 32;
            if (load) set_lane<T>(s, reg, l.index, ld<T>(a));
            else st<T>(a, lane<T>(s, reg, l.index));
        }
    });
}
NXW_INLINE void ld_rep(State* s, unsigned t, u64 a, unsigned size, bool q, unsigned selem) {
    const unsigned eb = 1u << size;
    for (unsigned r = 0; r < selem; r++) {
        const unsigned reg = (t + r) % 32;
        NXW_BY_SIZE(size, {
            const T e = ld<T>(a);
            LV<T> v{};
            for (unsigned i = 0; i < (q ? 16 : 8) / sizeof(T); i++) v[i] = e;
            vset<T>(s, reg, v, q);
        });
        a += eb;
    }
}
NXW_INLINE void post_index(State* s, unsigned n, unsigned m, u64 base, u64 imm) { wXS(s, n, base + (m == 31 ? imm : X(s, m))); }

}  // namespace

#define LDST_MULTI(NAME, SELEM, REGS, LOAD) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        ld_multi(s, f.Rt, XS(s, f.Rn), f.size, f.Q, SELEM, REGS, LOAD); \
        return 0; \
    } \
    SEM(NAME##_post) { \
        const Fl<Op::NAME##_post> f(w); \
        const u64 a = XS(s, f.Rn); \
        ld_multi(s, f.Rt, a, f.size, f.Q, SELEM, REGS, LOAD); \
        post_index(s, f.Rn, f.Rm, a, u64(SELEM) * (REGS) * (f.Q ? 16 : 8)); \
        return 0; \
    }
LDST_MULTI(LD1_1, 1, 1, true)
LDST_MULTI(LD1_2, 1, 2, true)
LDST_MULTI(LD1_3, 1, 3, true)
LDST_MULTI(LD1_4, 1, 4, true)
LDST_MULTI(LD2, 2, 1, true)
LDST_MULTI(LD3, 3, 1, true)
LDST_MULTI(LD4, 4, 1, true)
LDST_MULTI(ST1_1, 1, 1, false)
LDST_MULTI(ST1_2, 1, 2, false)
LDST_MULTI(ST1_3, 1, 3, false)
LDST_MULTI(ST1_4, 1, 4, false)
LDST_MULTI(ST2, 2, 1, false)
LDST_MULTI(ST3, 3, 1, false)
LDST_MULTI(ST4, 4, 1, false)

#define LDST_LANE(NAME, SELEM, LOAD) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        ld_lane(s, f.Rt, XS(s, f.Rn), lane_sel(f.kind, f.Q, f.S, f.size), SELEM, LOAD); \
        return 0; \
    } \
    SEM(NAME##_post) { \
        const Fl<Op::NAME##_post> f(w); \
        const u64 a = XS(s, f.Rn); \
        const LaneSel l = lane_sel(f.kind, f.Q, f.S, f.size); \
        ld_lane(s, f.Rt, a, l, SELEM, LOAD); \
        post_index(s, f.Rn, f.Rm, a, u64(SELEM) << l.size); \
        return 0; \
    }
LDST_LANE(LD1_lane, 1, true)
LDST_LANE(LD2_lane, 2, true)
LDST_LANE(LD3_lane, 3, true)
LDST_LANE(LD4_lane, 4, true)
LDST_LANE(ST1_lane, 1, false)
LDST_LANE(ST2_lane, 2, false)
LDST_LANE(ST3_lane, 3, false)
LDST_LANE(ST4_lane, 4, false)

#define LD_REP(NAME, SELEM) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        ld_rep(s, f.Rt, XS(s, f.Rn), f.size, f.Q, SELEM); \
        return 0; \
    } \
    SEM(NAME##_post) { \
        const Fl<Op::NAME##_post> f(w); \
        const u64 a = XS(s, f.Rn); \
        ld_rep(s, f.Rt, a, f.size, f.Q, SELEM); \
        post_index(s, f.Rn, f.Rm, a, u64(SELEM) << f.size); \
        return 0; \
    }
LD_REP(LD1R, 1)
LD_REP(LD2R, 2)
LD_REP(LD3R, 3)
LD_REP(LD4R, 4)
