// Exact FP operations that x86 has no instruction for (DESIGN D6): FRECPE / FRSQRTE through the Arm ARM's RecipEstimate and
// RecipSqrtEstimate (8-bit estimates, FEAT_RPRES off), FRECPS / FRSQRTS fused steps, FMULX, FRINT32/64, FCVTXN's round to odd.
// FPCR is 0 (round to nearest, no flush to zero, no default NaN; DESIGN §8.1).
#pragma once
#include "common.hpp"

namespace nxw::sem {

NXW_INLINE unsigned recip_estimate_int(unsigned a) {   // 256 <= a < 512
    a = a * 2 + 1;
    const unsigned b = (1u << 19) / a;
    return (b + 1) / 2;
}
NXW_INLINE unsigned rsqrt_estimate_int(unsigned a) {   // 128 <= a < 512
    if (a < 256) a = a * 2 + 1;
    else a = ((a >> 1) << 1) + 1, a *= 2;
    unsigned b = 512;
    while (u64(a) * (b + 1) * (b + 1) < (u64(1) << 28)) b++;
    return (b + 1) / 2;
}

// The operand's fraction as 52 bits and its biased exponent (FPRecipEstimate's unified form)
template <class T> NXW_INLINE void unpack52(T x, u64& frac, int& exp) {
    if constexpr (std::is_same_v<T, float>) frac = u64(fbits(x) & 0x7fffff) << 29, exp = int(fbits(x) >> 23 & 0xff);
    else frac = fbits(x) & FT<double>::frac_mask, exp = int(fbits(x) >> 52 & 0x7ff);
}
template <class T> NXW_INLINE T pack52(bool sign, int exp, u64 frac) {
    if constexpr (std::is_same_v<T, float>) return ffrom(u32(sign) << 31 | u32(exp & 0xff) << 23 | u32(frac >> 29));
    else return ffrom(u64(sign) << 63 | u64(exp & 0x7ff) << 52 | (frac & FT<double>::frac_mask));
}

template <class T> NXW_INLINE T recip_estimate(T x) {
    const bool sign = sign_of(x);
    if (is_nan(x)) return quiet(x);
    if (is_inf(x)) return from_bits<T>(sign ? FT<T>::sign : 0);
    if (is_zero(x)) return from_bits<T>((sign ? FT<T>::sign : 0) | FT<T>::exp_mask);
    // |x| < 2^-128 (single) or 2^-1024 (double): the reciprocal overflows; round to nearest gives infinity
    constexpr typename FT<T>::U tiny = std::is_same_v<T, float> ? 0x00200000u : u64(0x0004000000000000ull);
    if ((bits_of(x) & ~FT<T>::sign) < tiny) return from_bits<T>((sign ? FT<T>::sign : 0) | FT<T>::exp_mask);
    u64 frac;
    int exp;
    unpack52(x, frac, exp);
    if (exp == 0) {
        if (!(frac >> 51 & 1)) exp = -1, frac = (frac << 2) & FT<double>::frac_mask;
        else frac = (frac << 1) & FT<double>::frac_mask;
    }
    const unsigned scaled = 256 | unsigned(frac >> 44);
    int rexp = (std::is_same_v<T, float> ? 253 : 2045) - exp;
    const unsigned est = recip_estimate_int(scaled);
    frac = u64(est & 0xff) << 44;
    if (rexp == 0) frac = u64(1) << 51 | frac >> 1;
    else if (rexp == -1) frac = u64(1) << 50 | frac >> 2, rexp = 0;
    return pack52<T>(sign, rexp, frac);
}

template <class T> NXW_INLINE T rsqrt_estimate(T x) {
    const bool sign = sign_of(x);
    if (is_nan(x)) return quiet(x);
    if (is_zero(x)) return from_bits<T>((sign ? FT<T>::sign : 0) | FT<T>::exp_mask);
    if (sign) return default_nan<T>();
    if (is_inf(x)) return T(0);
    u64 frac;
    int exp;
    unpack52(x, frac, exp);
    if (exp == 0) {
        while (!(frac >> 51 & 1)) frac = (frac << 1) & FT<double>::frac_mask, exp--;
        frac = (frac << 1) & FT<double>::frac_mask;
    }
    const unsigned scaled = (exp & 1) ? (128 | unsigned(frac >> 45)) : (256 | unsigned(frac >> 44));
    const int rexp = ((std::is_same_v<T, float> ? 380 : 3068) - exp) / 2;
    const unsigned est = rsqrt_estimate_int(scaled);
    return pack52<T>(false, rexp, u64(est & 0xff) << 44);
}

// FPRecipStepFused: 2 - a * b, fused; inf * 0 gives 2
template <class T> NXW_INLINE T recip_step(T a, T b) {
    const T na = -a;
    if (is_nan(na) || is_nan(b)) return process_nans(na, b);
    if ((is_inf(na) && is_zero(b)) || (is_zero(na) && is_inf(b))) return T(2);
    return __builtin_elementwise_fma(na, b, T(2));
}
// FPRSqrtStepFused: (3 - a * b) / 2 with one rounding; inf * 0 gives 1.5
template <class T> NXW_INLINE T rsqrt_step(T a, T b) {
    const T na = -a;
    if (is_nan(na) || is_nan(b)) return process_nans(na, b);
    if ((is_inf(na) && is_zero(b)) || (is_zero(na) && is_inf(b))) return T(1.5);
    if (is_inf(na) || is_inf(b)) return __builtin_elementwise_fma(na, b, T(3));
    T r = __builtin_elementwise_fma(na, b, T(3)) * T(0.5);   // exact halving: the sum is never subnormal
    if (is_inf(r)) {   // the sum overflowed although its half may not: halve the larger operand first (exact)
        if (__builtin_elementwise_abs(na) >= __builtin_elementwise_abs(b)) r = __builtin_elementwise_fma(na * T(0.5), b, T(1.5));
        else r = __builtin_elementwise_fma(na, b * T(0.5), T(1.5));
    }
    return r;
}
// FPMulX: as FMUL, but inf * 0 gives 2 with the product's sign
template <class T> NXW_INLINE T mulx(T a, T b) {
    if (is_nan(a) || is_nan(b)) return process_nans(a, b);
    if ((is_inf(a) && is_zero(b)) || (is_zero(a) && is_inf(b))) return (sign_of(a) != sign_of(b)) ? T(-2) : T(2);
    return a * b;
}
// FRINT32* / FRINT64*: round to an integral value that fits the integer size, else the most negative one
template <class T> NXW_INLINE T frint_int(T x, Rnd r, int bits) {
    const T lo = -exp2i<T>(bits - 1);
    if (is_nan(x) || is_inf(x)) return lo;
    const T v = round_to(x, r);
    if (v < lo || v >= exp2i<T>(bits - 1)) return lo;
    return v;
}
// FPRecpX: the exponent inverted (a zero exponent gives the largest normal one), fraction zero
template <class T> NXW_INLINE T recpx(T a) {
    if (is_nan(a)) return quiet(a);
    using U = typename FT<T>::U;
    const U b = bits_of(a), e = b & FT<T>::exp_mask;
    const U ne = e == 0 ? FT<T>::exp_mask - (U(1) << FT<T>::frac_bits) : (~b & FT<T>::exp_mask);
    return from_bits<T>((b & FT<T>::sign) | ne);
}
// Double to single with round to odd (FCVTXN)
NXW_INLINE float cvt_round_odd(double d) {
    if (is_nan(d)) return float(d);
    const float f = float(d);   // round to nearest; correct it to round toward zero, then set the lowest bit if inexact
    if (is_inf(f) && !is_inf(d)) return ffrom(fbits(f) - 1 | 1);
    const double back = double(f);
    if (back == d) return f;
    u32 b = fbits(f);
    if (__builtin_fabs(back) > __builtin_fabs(d)) b -= 1;   // rounded away from zero: step back toward zero
    return ffrom(b | 1);
}

}  // namespace nxw::sem
