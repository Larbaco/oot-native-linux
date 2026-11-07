// Scalar floating point: arithmetic, fused multiply-add, min/max, compares, conversions, rounding, moves.
// ft (bit 22) selects single (0) or double (1); half-precision arithmetic is not decoded (none in the game).
#include "fpx.hpp"

using namespace nxw::sem;

#define BY_FT(FTV, ...) \
    do { \
        if (FTV) { using T = double; __VA_ARGS__; } \
        else { using T = float; __VA_ARGS__; } \
    } while (0)

namespace {
template <class T> NXW_INLINE T fget(const State* s, unsigned n) { return lane<T>(s, n, 0); }
template <class T> NXW_INLINE void fset(State* s, unsigned n, T v) { vset_scalar<T>(s, n, v); }
}  // namespace

// ---------------------------------------------------------------- arithmetic
#define FP2(NAME, EXPR) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        BY_FT(f.ft, { \
            const T a = fget<T>(s, f.Rn), b = fget<T>(s, f.Rm); \
            fset<T>(s, f.Rd, EXPR); \
        }); \
        return 0; \
    }
FP2(FADD, fp_binop(a, b, [](T x, T y) { return x + y; }))
FP2(FSUB, fp_binop(a, b, [](T x, T y) { return x - y; }))
FP2(FMUL, fp_binop(a, b, [](T x, T y) { return x * y; }))
FP2(FDIV, fp_binop(a, b, [](T x, T y) { return x / y; }))
FP2(FNMUL, -fp_binop(a, b, [](T x, T y) { return x * y; }))
FP2(FMAX, fp_max(a, b))
FP2(FMIN, fp_min(a, b))
FP2(FMAXNM, fp_maxnum(a, b))
FP2(FMINNM, fp_minnum(a, b))

#define FP3(NAME, EXPR) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        BY_FT(f.ft, { \
            const T n = fget<T>(s, f.Rn), m = fget<T>(s, f.Rm), a = fget<T>(s, f.Ra); \
            fset<T>(s, f.Rd, EXPR); \
        }); \
        return 0; \
    }
FP3(FMADD, fp_muladd(a, n, m))
FP3(FMSUB, fp_muladd(a, -n, m))
FP3(FNMADD, fp_muladd(-a, -n, m))
FP3(FNMSUB, fp_muladd(-a, n, m))

#define FP1(NAME, EXPR) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        BY_FT(f.ft, { \
            const T a = fget<T>(s, f.Rn); \
            fset<T>(s, f.Rd, EXPR); \
        }); \
        return 0; \
    }
FP1(FMOV_reg, a)
FP1(FABS, from_bits<T>(bits_of(a) & ~FT<T>::sign))
FP1(FNEG, from_bits<T>(bits_of(a) ^ FT<T>::sign))
FP1(FSQRT, fp_unop_nan(a, __builtin_elementwise_sqrt(a)))
FP1(FRINTN, fp_unop_nan(a, round_to(a, Rnd::N)))
FP1(FRINTP, fp_unop_nan(a, round_to(a, Rnd::P)))
FP1(FRINTM, fp_unop_nan(a, round_to(a, Rnd::M)))
FP1(FRINTZ, fp_unop_nan(a, round_to(a, Rnd::Z)))
FP1(FRINTA, fp_unop_nan(a, round_to(a, Rnd::A)))
FP1(FRINTX, fp_unop_nan(a, round_to(a, Rnd::N)))   // the current mode is round to nearest (FPCR = 0)
FP1(FRINTI, fp_unop_nan(a, round_to(a, Rnd::N)))
FP1(FRINT32Z, frint_int(a, Rnd::Z, 32))
FP1(FRINT32X, frint_int(a, Rnd::N, 32))
FP1(FRINT64Z, frint_int(a, Rnd::Z, 64))
FP1(FRINT64X, frint_int(a, Rnd::N, 64))

// ---------------------------------------------------------------- compares and selects
#define FCMP_(NAME, ZERO) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        BY_FT(f.ft, { \
            const T a = fget<T>(s, f.Rn), b = (ZERO) ? T(0) : fget<T>(s, f.Rm); \
            set_nzcv(s, fp_compare(a, b)); \
        }); \
        return 0; \
    }
FCMP_(FCMP, false)
FCMP_(FCMPE, false)
FCMP_(FCMP_zero, true)
FCMP_(FCMPE_zero, true)

#define FCCMP_(NAME) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        if (cond_holds(s, f.cond)) BY_FT(f.ft, set_nzcv(s, fp_compare(fget<T>(s, f.Rn), fget<T>(s, f.Rm)))); \
        else set_nzcv(s, f.nzcv); \
        return 0; \
    }
FCCMP_(FCCMP)
FCCMP_(FCCMPE)

SEM(FCSEL) {
    const Fl<Op::FCSEL> f(w);
    BY_FT(f.ft, fset<T>(s, f.Rd, cond_holds(s, f.cond) ? fget<T>(s, f.Rn) : fget<T>(s, f.Rm)));
    return 0;
}

// ---------------------------------------------------------------- conversions between FP formats (ftype/opc: 0 S, 1 D, 3 H)
SEM(FCVT) {
    const Fl<Op::FCVT> f(w);
    const unsigned from = f.ftype, to = f.opc;
    if (from == 0 && to == 1) fset<double>(s, f.Rd, double(fget<float>(s, f.Rn)));
    else if (from == 1 && to == 0) fset<float>(s, f.Rd, float(fget<double>(s, f.Rn)));
    else if (from == 0 && to == 3) fset<u16>(s, f.Rd, float_to_half(fget<float>(s, f.Rn)));
    else if (from == 1 && to == 3) fset<u16>(s, f.Rd, float_to_half(fget<double>(s, f.Rn)));
    else if (from == 3 && to == 0) fset<float>(s, f.Rd, half_to_float(fget<u16>(s, f.Rn)));
    else if (from == 3 && to == 1) fset<double>(s, f.Rd, double(half_to_float(fget<u16>(s, f.Rn))));
    return 0;
}

// ---------------------------------------------------------------- FP to integer (saturating) and back
#define FCVT_INT(NAME, RND, SIGNED) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        BY_FT(f.ft, { \
            const T a = fget<T>(s, f.Rn); \
            u64 r; \
            if (f.sf) r = SIGNED ? u64(fp_to_int<i64>(a, RND)) : fp_to_int<u64>(a, RND); \
            else r = SIGNED ? u32(fp_to_int<i32>(a, RND)) : fp_to_int<u32>(a, RND); \
            wR(s, f.sf, f.Rd, r); \
        }); \
        return 0; \
    }
FCVT_INT(FCVTNS_int, Rnd::N, true)
FCVT_INT(FCVTNU_int, Rnd::N, false)
FCVT_INT(FCVTAS_int, Rnd::A, true)
FCVT_INT(FCVTAU_int, Rnd::A, false)
FCVT_INT(FCVTPS_int, Rnd::P, true)
FCVT_INT(FCVTPU_int, Rnd::P, false)
FCVT_INT(FCVTMS_int, Rnd::M, true)
FCVT_INT(FCVTMU_int, Rnd::M, false)
FCVT_INT(FCVTZS_int, Rnd::Z, true)
FCVT_INT(FCVTZU_int, Rnd::Z, false)

// Fixed point: fbits = 64 - scale; the scaling by a power of two is exact (overflow saturates anyway)
#define FCVT_FIX(NAME, SIGNED) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        const int fbits = 64 - int(f.scale); \
        BY_FT(f.ft, { \
            const T a = fget<T>(s, f.Rn) * exp2i<T>(fbits); \
            u64 r; \
            if (f.sf) r = SIGNED ? u64(fp_to_int<i64>(a, Rnd::Z)) : fp_to_int<u64>(a, Rnd::Z); \
            else r = SIGNED ? u32(fp_to_int<i32>(a, Rnd::Z)) : fp_to_int<u32>(a, Rnd::Z); \
            wR(s, f.sf, f.Rd, r); \
        }); \
        return 0; \
    }
FCVT_FIX(FCVTZS_fix, true)
FCVT_FIX(FCVTZU_fix, false)

#define CVTF(NAME, SIGNED, FIX) \
    SEM(NAME) { \
        const Fl<Op::NAME> f(w); \
        const u64 v = R(s, f.sf, f.Rn); \
        BY_FT(f.ft, { \
            T r; \
            if (f.sf) r = SIGNED ? T(i64(v)) : T(v); \
            else r = SIGNED ? T(i32(u32(v))) : T(u32(v)); \
            if constexpr (FIX) r = r * exp2i<T>(-(64 - int(w >> 10 & 63))); \
            fset<T>(s, f.Rd, r); \
        }); \
        return 0; \
    }
CVTF(SCVTF_int, true, false)
CVTF(UCVTF_int, false, false)
CVTF(SCVTF_fix, true, true)
CVTF(UCVTF_fix, false, true)

// ---------------------------------------------------------------- moves
SEM(FMOV_to_gen) {
    const Fl<Op::FMOV_to_gen> f(w);
    wR(s, f.sf, f.Rd, f.sf ? lane<u64>(s, f.Rn, 0) : lane<u32>(s, f.Rn, 0));
    return 0;
}
SEM(FMOV_from_gen) {
    const Fl<Op::FMOV_from_gen> f(w);
    if (f.sf) fset<u64>(s, f.Rd, X(s, f.Rn));
    else fset<u32>(s, f.Rd, u32(X(s, f.Rn)));
    return 0;
}
SEM(FMOV_to_top) {
    const Fl<Op::FMOV_to_top> f(w);
    wX(s, f.Rd, lane<u64>(s, f.Rn, 1));
    return 0;
}
SEM(FMOV_from_top) {
    const Fl<Op::FMOV_from_top> f(w);
    set_lane<u64>(s, f.Rd, 1, X(s, f.Rn));
    return 0;
}
// VFPExpandImm
SEM(FMOV_imm) {
    const Fl<Op::FMOV_imm> f(w);
    const u32 i = f.imm8, b6 = i >> 6 & 1;
    if (f.ft) {
        const u64 v = u64(i >> 7) << 63 | u64(b6 ^ 1) << 62 | (b6 ? u64(0xff) << 54 : 0) | u64(i >> 4 & 3) << 52 | u64(i & 15) << 48;
        fset<u64>(s, f.Rd, v);
    } else {
        const u32 v = (i >> 7) << 31 | (b6 ^ 1) << 30 | (b6 ? 0x1fu << 25 : 0) | (i >> 4 & 3) << 23 | (i & 15) << 19;
        fset<u32>(s, f.Rd, v);
    }
    return 0;
}
