// Helpers shared by the semantics: register and flag access, guest memory, immediates, FP bit tricks, vector lanes.
// Everything here is always_inline: with the lifter's constant instruction word it folds into a few IR instructions.
#pragma once
#include "nxw/a64_gen.hpp"
#include "nxw/rt.h"
#include "nxw/sem.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

#define NXW_INLINE inline __attribute__((always_inline))
// One semantic function per decoder op. gen_ops.py collects the names into sem_ops.inc for the interpreter.
#define SEM(NAME) extern "C" uint64_t nxw_sem_##NAME([[maybe_unused]] ::nxw::sem::State* s, [[maybe_unused]] uint32_t w, \
                                                   [[maybe_unused]] uint64_t pc)

namespace nxw::sem {

using a64::Op;
template <Op O> using Fl = a64::Fields<O>;

using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i128 = __int128;
using u128 = unsigned __int128;

// ---------------------------------------------------------------- fields common to many encodings
NXW_INLINE unsigned Rd(u32 w) { return w & 31; }
NXW_INLINE unsigned Rn(u32 w) { return w >> 5 & 31; }
NXW_INLINE unsigned Rm(u32 w) { return w >> 16 & 31; }
NXW_INLINE unsigned Ra(u32 w) { return w >> 10 & 31; }
NXW_INLINE bool Qb(u32 w) { return w >> 30 & 1; }
NXW_INLINE unsigned Size(u32 w) { return w >> 22 & 3; }

NXW_INLINE u64 ones(unsigned n) { return n >= 64 ? ~0ull : (1ull << n) - 1; }
NXW_INLINE i64 sext(u64 v, unsigned bits) { return i64(v << (64 - bits)) >> (64 - bits); }

// ---------------------------------------------------------------- general registers
NXW_INLINE u64 X(const State* s, unsigned n) { return n == 31 ? 0 : s->x[n]; }        // register 31 = zero register
NXW_INLINE u64 XS(const State* s, unsigned n) { return n == 31 ? s->sp : s->x[n]; }   // register 31 = SP
NXW_INLINE void wX(State* s, unsigned n, u64 v) {
    if (n != 31) s->x[n] = v;
}
NXW_INLINE void wXS(State* s, unsigned n, u64 v) {
    if (n == 31) s->sp = v;
    else s->x[n] = v;
}
NXW_INLINE u64 R(const State* s, bool sf, unsigned n) { return sf ? X(s, n) : u32(X(s, n)); }
NXW_INLINE u64 RS(const State* s, bool sf, unsigned n) { return sf ? XS(s, n) : u32(XS(s, n)); }
NXW_INLINE void wR(State* s, bool sf, unsigned n, u64 v) { wX(s, n, sf ? v : u32(v)); }
NXW_INLINE void wRS(State* s, bool sf, unsigned n, u64 v) { wXS(s, n, sf ? v : u32(v)); }

// ---------------------------------------------------------------- flags and conditions
NXW_INLINE void set_nz(State* s, bool sf, u64 r) {
    if (sf) s->n = r >> 63, s->z = r == 0;
    else s->n = r >> 31 & 1, s->z = u32(r) == 0;
}
NXW_INLINE void set_nzcv(State* s, unsigned nzcv) { s->n = nzcv >> 3 & 1, s->z = nzcv >> 2 & 1, s->c = nzcv >> 1 & 1, s->v = nzcv & 1; }

// AddWithCarry: the result, and NZCV when flags is set
NXW_INLINE u64 add_with_carry(State* s, bool sf, u64 a, u64 b, bool carry, bool flags) {
    if (sf) {
        u64 t, r;
        const bool c1 = __builtin_add_overflow(a, b, &t), c2 = __builtin_add_overflow(t, u64(carry), &r);
        if (flags) s->n = r >> 63, s->z = r == 0, s->c = c1 | c2, s->v = ((~(a ^ b) & (a ^ r)) >> 63) & 1;
        return r;
    }
    const u32 a32 = u32(a), b32 = u32(b);
    const u64 wide = u64(a32) + b32 + carry;
    const u32 r = u32(wide);
    if (flags) s->n = r >> 31, s->z = r == 0, s->c = wide >> 32 & 1, s->v = ((~(a32 ^ b32) & (a32 ^ r)) >> 31) & 1;
    return r;
}

NXW_INLINE bool cond_holds(const State* s, unsigned c) {
    bool r;
    switch (c >> 1) {
    case 0: r = s->z; break;
    case 1: r = s->c; break;
    case 2: r = s->n; break;
    case 3: r = s->v; break;
    case 4: r = s->c && !s->z; break;
    case 5: r = s->n == s->v; break;
    case 6: r = s->n == s->v && !s->z; break;
    default: r = true; break;
    }
    return (c & 1) && c != 15 ? !r : r;
}

// ---------------------------------------------------------------- operands
NXW_INLINE u64 shift_reg(u64 v, unsigned type, unsigned amount, bool sf) {
    if (!sf) {
        const u32 x = u32(v);
        switch (type) {
        case 0: return u32(x << amount);
        case 1: return x >> amount;
        case 2: return u32(i32(x) >> amount);
        default: return std::rotr(x, int(amount));
        }
    }
    switch (type) {
    case 0: return v << amount;
    case 1: return v >> amount;
    case 2: return u64(i64(v) >> amount);
    default: return std::rotr(v, int(amount));
    }
}

NXW_INLINE u64 extend_reg(u64 v, unsigned option, unsigned shift) {
    u64 r;
    switch (option) {
    case 0: r = u8(v); break;
    case 1: r = u16(v); break;
    case 2: r = u32(v); break;
    case 4: r = u64(i64(i8(v))); break;
    case 5: r = u64(i64(i16(v))); break;
    case 6: r = u64(i64(i32(v))); break;
    default: r = v; break;
    }
    return r << shift;
}

struct BitMasks {
    u64 wmask, tmask;
};
// DecodeBitMasks (Arm ARM): N:imms:immr for an element size and rotation
NXW_INLINE BitMasks decode_bit_masks(unsigned N, unsigned imms, unsigned immr, unsigned datasize) {
    const unsigned combined = N << 6 | (~imms & 0x3f);
    const int len = combined ? 31 - __builtin_clz(combined) : 0;
    const unsigned esize = 1u << len, levels = esize - 1;
    const unsigned S = imms & levels, R = immr & levels, d = (S - R) & levels;
    const u64 emask = ones(esize);
    u64 welem = ones(S + 1), telem = ones(d + 1);
    if (R) welem = ((welem >> R) | (welem << (esize - R))) & emask;
    u64 wm = 0, tm = 0;
    for (unsigned i = 0; i < datasize; i += esize) wm |= welem << i, tm |= telem << i;
    return {wm, tm};
}
NXW_INLINE u64 ror(u64 v, unsigned r, unsigned width) {
    if (width == 32) return std::rotr(u32(v), int(r));
    return std::rotr(v, int(r));
}

// ---------------------------------------------------------------- guest memory: identity mapped, volatile (DESIGN D7)
template <class T> struct __attribute__((packed, may_alias)) Unaligned {
    T v;
};
template <class T> NXW_INLINE T ld(u64 a) { return reinterpret_cast<volatile const Unaligned<T>*>(uintptr_t(a))->v; }
template <class T> NXW_INLINE void st(u64 a, T v) { reinterpret_cast<volatile Unaligned<T>*>(uintptr_t(a))->v = v; }
NXW_INLINE V128 ld128(u64 a) { return ld<V128>(a); }
NXW_INLINE void st128(u64 a, V128 v) { st<V128>(a, v); }

template <class T> NXW_INLINE volatile T* aptr(u64 a) { return reinterpret_cast<volatile T*>(uintptr_t(a)); }

// Memory orders of acquire / release bits
NXW_INLINE int order(bool acq, bool rel) {
    return acq && rel ? __ATOMIC_ACQ_REL : acq ? __ATOMIC_ACQUIRE : rel ? __ATOMIC_RELEASE : __ATOMIC_RELAXED;
}
NXW_INLINE void spin_hint() {
#if defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __builtin_arm_yield();
#endif
}

// ---------------------------------------------------------------- vector registers
template <class T> using LV = std::array<T, 16 / sizeof(T)>;

template <class T> NXW_INLINE LV<T> vget(const State* s, unsigned n) { return std::bit_cast<LV<T>>(s->q[n]); }
// Writes a vector register; with q false (a 64-bit operation) the upper half becomes zero
template <class T> NXW_INLINE void vset(State* s, unsigned n, LV<T> r, bool q = true) {
    if (!q)
        for (unsigned i = 8 / sizeof(T); i < 16 / sizeof(T); i++) r[i] = 0;
    s->q[n] = std::bit_cast<V128>(r);
}
template <class T> NXW_INLINE T lane(const State* s, unsigned n, unsigned i) { return vget<T>(s, n)[i]; }
template <class T> NXW_INLINE void set_lane(State* s, unsigned n, unsigned i, T v) {
    LV<T> r = vget<T>(s, n);
    r[i] = v;
    s->q[n] = std::bit_cast<V128>(r);
}
// A scalar written to a SIMD&FP register: all other bits become zero
template <class T> NXW_INLINE void vset_scalar(State* s, unsigned n, T v) {
    LV<T> r{};
    r[0] = v;
    s->q[n] = std::bit_cast<V128>(r);
}

// Integer types by size field
template <unsigned SZ> struct UT;
template <> struct UT<0> { using u = u8; using i = i8; };
template <> struct UT<1> { using u = u16; using i = i16; };
template <> struct UT<2> { using u = u32; using i = i32; };
template <> struct UT<3> { using u = u64; using i = i64; };
template <class T> using Wider = std::conditional_t<std::is_signed_v<T>,
    std::conditional_t<sizeof(T) == 1, i16, std::conditional_t<sizeof(T) == 2, i32, std::conditional_t<sizeof(T) == 4, i64, i128>>>,
    std::conditional_t<sizeof(T) == 1, u16, std::conditional_t<sizeof(T) == 2, u32, std::conditional_t<sizeof(T) == 4, u64, u128>>>>;
template <class T> using Narrower = std::conditional_t<std::is_signed_v<T>,
    std::conditional_t<sizeof(T) == 2, i8, std::conditional_t<sizeof(T) == 4, i16, i32>>,
    std::conditional_t<sizeof(T) == 2, u8, std::conditional_t<sizeof(T) == 4, u16, u32>>>;

// Dispatch a size field (0..3) to a function templated on the unsigned lane type
#define NXW_BY_SIZE(SZ, ...) \
    do { \
        switch (SZ) { \
        case 0: { using T = u8; __VA_ARGS__; } break; \
        case 1: { using T = u16; __VA_ARGS__; } break; \
        case 2: { using T = u32; __VA_ARGS__; } break; \
        default: { using T = u64; __VA_ARGS__; } break; \
        } \
    } while (0)

template <class T> NXW_INLINE T sat(Wider<T> v) {
    constexpr Wider<T> lo = std::numeric_limits<T>::min(), hi = std::numeric_limits<T>::max();
    return T(v < lo ? lo : v > hi ? hi : v);
}
// Saturate a 128-bit intermediate to a type
template <class T> NXW_INLINE T sat128(i128 v) {
    constexpr i128 lo = std::numeric_limits<T>::min(), hi = std::numeric_limits<T>::max();
    return T(v < lo ? lo : v > hi ? hi : v);
}

// ---------------------------------------------------------------- floating point
NXW_INLINE u32 fbits(float f) { return std::bit_cast<u32>(f); }
NXW_INLINE u64 fbits(double d) { return std::bit_cast<u64>(d); }
NXW_INLINE float ffrom(u32 u) { return std::bit_cast<float>(u); }
NXW_INLINE double ffrom(u64 u) { return std::bit_cast<double>(u); }

template <class T> struct FT;
template <> struct FT<float> {
    using U = u32;
    static constexpr U sign = 0x80000000u, quiet = 0x00400000u, exp_mask = 0x7f800000u, frac_mask = 0x007fffffu, dnan = 0x7fc00000u;
    static constexpr int frac_bits = 23, exp_bits = 8, bias = 127;
};
template <> struct FT<double> {
    using U = u64;
    static constexpr U sign = 0x8000000000000000ull, quiet = 0x0008000000000000ull, exp_mask = 0x7ff0000000000000ull,
                       frac_mask = 0x000fffffffffffffull, dnan = 0x7ff8000000000000ull;
    static constexpr int frac_bits = 52, exp_bits = 11, bias = 1023;
};

template <class T> NXW_INLINE typename FT<T>::U bits_of(T x) { return std::bit_cast<typename FT<T>::U>(x); }
template <class T> NXW_INLINE T from_bits(typename FT<T>::U u) { return std::bit_cast<T>(u); }
template <class T> NXW_INLINE bool is_nan(T x) { return (bits_of(x) & ~FT<T>::sign) > FT<T>::exp_mask; }
template <class T> NXW_INLINE bool is_snan(T x) { return is_nan(x) && !(bits_of(x) & FT<T>::quiet); }
template <class T> NXW_INLINE T quiet(T x) { return from_bits<T>(bits_of(x) | FT<T>::quiet); }
template <class T> NXW_INLINE T default_nan() { return from_bits<T>(FT<T>::dnan); }
template <class T> NXW_INLINE bool is_inf(T x) { return (bits_of(x) & ~FT<T>::sign) == FT<T>::exp_mask; }
template <class T> NXW_INLINE bool is_zero(T x) { return (bits_of(x) & ~FT<T>::sign) == 0; }
template <class T> NXW_INLINE bool sign_of(T x) { return bits_of(x) & FT<T>::sign; }

// FPProcessNaNs: the NaN an operation returns when an operand is a NaN (signalling first, then operand order), quieted
template <class T> NXW_INLINE T process_nans(T a, T b) {
    if (is_snan(a)) return quiet(a);
    if (is_snan(b)) return quiet(b);
    if (is_nan(a)) return a;
    return quiet(b);
}
template <class T> NXW_INLINE T process_nans3(T a, T b, T c) {
    if (is_snan(a)) return quiet(a);
    if (is_snan(b)) return quiet(b);
    if (is_snan(c)) return quiet(c);
    if (is_nan(a)) return a;
    if (is_nan(b)) return b;
    return quiet(c);
}

// Arm's NaN rules for ordinary arithmetic are applied only in the accurate-NaN build (DESIGN D6, research/03 §3.5); the default
// build returns the host's NaN (same NaN-ness, possibly another sign or payload)
#ifndef NXW_SEM_ACCURATE_NAN
#define NXW_SEM_ACCURATE_NAN 0
#endif
template <class T, class Fn> NXW_INLINE T fp_binop(T a, T b, Fn fn) {
    if constexpr (NXW_SEM_ACCURATE_NAN) {
        if (is_nan(a) || is_nan(b)) return process_nans(a, b);
        const T r = fn(a, b);
        return is_nan(r) ? default_nan<T>() : r;
    } else {
        return fn(a, b);
    }
}
template <class T> NXW_INLINE T fp_unop_nan(T a, T r) {
    if constexpr (NXW_SEM_ACCURATE_NAN) {
        if (is_nan(a)) return quiet(a);
        return is_nan(r) ? default_nan<T>() : r;
    } else {
        return r;
    }
}
// FPMulAdd: a + n * m with one rounding
template <class T> NXW_INLINE T fp_muladd(T a, T n, T m) {
    if constexpr (NXW_SEM_ACCURATE_NAN) {
        if (is_nan(a) || is_nan(n) || is_nan(m)) {
            // an invalid product (inf * 0) with a quiet NaN addend gives the default NaN
            if (!is_snan(a) && !is_snan(n) && !is_snan(m) && is_nan(a) &&
                ((is_inf(n) && is_zero(m)) || (is_zero(n) && is_inf(m))))
                return default_nan<T>();
            return process_nans3(a, n, m);
        }
        const T r = __builtin_elementwise_fma(n, m, a);
        return is_nan(r) ? default_nan<T>() : r;
    } else {
        return __builtin_elementwise_fma(n, m, a);
    }
}

// FPMin / FPMax: NaNs propagate, -0 < +0
template <class T> NXW_INLINE T fp_min(T a, T b) {
    if (is_nan(a) || is_nan(b)) return process_nans(a, b);
    if (a == b) return sign_of(a) ? a : b;
    return a < b ? a : b;
}
template <class T> NXW_INLINE T fp_max(T a, T b) {
    if (is_nan(a) || is_nan(b)) return process_nans(a, b);
    if (a == b) return sign_of(a) ? b : a;
    return a > b ? a : b;
}
// FPMinNum / FPMaxNum: a single quiet NaN is ignored
template <class T> NXW_INLINE T fp_minnum(T a, T b) {
    const bool qa = is_nan(a) && !is_snan(a), qb = is_nan(b) && !is_snan(b);
    if (qa && !qb && !is_nan(b)) a = from_bits<T>(FT<T>::exp_mask);   // +inf: never chosen over b
    else if (qb && !qa && !is_nan(a)) b = from_bits<T>(FT<T>::exp_mask);
    return fp_min(a, b);
}
template <class T> NXW_INLINE T fp_maxnum(T a, T b) {
    const bool qa = is_nan(a) && !is_snan(a), qb = is_nan(b) && !is_snan(b);
    if (qa && !qb && !is_nan(b)) a = from_bits<T>(FT<T>::exp_mask | FT<T>::sign);   // -inf
    else if (qb && !qa && !is_nan(a)) b = from_bits<T>(FT<T>::exp_mask | FT<T>::sign);
    return fp_max(a, b);
}

// FPCompare as NZCV: unordered 0011, equal 0110, less 1000, greater 0010
template <class T> NXW_INLINE unsigned fp_compare(T a, T b) {
    if (is_nan(a) || is_nan(b)) return 0x3;
    if (a == b) return 0x6;
    return a < b ? 0x8 : 0x2;
}

// Rounding modes of FPToFixed / FPRoundInt: N (ties to even), P (+inf), M (-inf), Z (zero), A (ties away), X/I (current: N)
enum class Rnd { N, P, M, Z, A };
template <class T> NXW_INLINE T round_to(T x, Rnd r) {
    switch (r) {
    case Rnd::N: return __builtin_elementwise_roundeven(x);
    case Rnd::P: return __builtin_elementwise_ceil(x);
    case Rnd::M: return __builtin_elementwise_floor(x);
    case Rnd::Z: return __builtin_elementwise_trunc(x);
    default: return __builtin_elementwise_round(x);
    }
}
// FPToFixed with saturation: NaN gives 0
template <class I, class T> NXW_INLINE I fp_to_int(T x, Rnd r) {
    if (is_nan(x)) return 0;
    const T v = round_to(x, r);
    constexpr T lo = T(std::numeric_limits<I>::min());
    constexpr T hi = std::is_signed_v<I> ? -lo : T(2) * T(u64(1) << (sizeof(I) * 8 - 1));   // 2^bits for unsigned
    if (v < lo) return std::numeric_limits<I>::min();
    if (v >= hi) return std::numeric_limits<I>::max();
    return I(v);
}
// 2^e as a constant bit pattern (e within the normal range of T), so no libcall is ever needed
template <class T> NXW_INLINE T exp2i(int e) {
    if constexpr (std::is_same_v<T, double>) return ffrom(u64(1023 + e) << 52);
    else return ffrom(u32(127 + e) << 23);
}

// v >> sh, rounded to nearest, ties to even
NXW_INLINE u64 shift_rne(u64 v, unsigned sh) {
    if (sh == 0) return v;
    if (sh >= 64) return 0;
    u64 q = v >> sh;
    const u64 rem = v & ((1ull << sh) - 1), half = 1ull << (sh - 1);
    if (rem > half || (rem == half && (q & 1))) q++;
    return q;
}

// Half precision (IEEE, FPCR.AHP = 0) conversions, round to nearest even, by integer arithmetic only
NXW_INLINE float half_to_float(u16 h) {
    const u32 sign = u32(h & 0x8000) << 16, exp = h >> 10 & 0x1f, frac = h & 0x3ff;
    if (exp == 0x1f) return ffrom(sign | 0x7f800000u | frac << 13 | (frac ? 0x00400000u : 0));   // NaNs come out quiet
    if (exp == 0) {
        const float r = float(frac) * exp2i<float>(-24);   // exact
        return sign ? -r : r;
    }
    return ffrom(sign | (exp + 112) << 23 | frac << 13);
}
template <class T> NXW_INLINE u16 float_to_half(T x) {
    using U = typename FT<T>::U;
    constexpr int fb = FT<T>::frac_bits;
    const U b = bits_of(x);
    const u16 sign = u16(b >> (sizeof(T) * 8 - 16)) & 0x8000;
    if (is_nan(x)) return u16(sign | 0x7e00 | u16((b & FT<T>::frac_mask) >> (fb - 10)));
    if (is_inf(x)) return sign | 0x7c00;
    const int e = int((b & FT<T>::exp_mask) >> fb);
    const u64 sig = u64(b & FT<T>::frac_mask) | (e ? u64(1) << fb : 0);
    const int E = (e ? e : 1) - FT<T>::bias;              // value = sig * 2^(E - fb)
    if (!sig) return sign;
    if (E >= -14) {
        if (E > 15) return sign | 0x7c00;
        u64 hs = shift_rne(sig, unsigned(fb - 10));
        int he = E;
        if (hs == 2048) hs = 1024, he++;
        if (hs < 1024) {                                  // input subnormal in T but normal range in half cannot happen
            return u16(sign | hs);
        }
        if (he > 15) return sign | 0x7c00;
        return u16(sign | unsigned(he + 15) << 10 | (hs & 0x3ff));
    }
    const u64 hs = shift_rne(sig, unsigned(fb - 24 - E));   // units of 2^-24; 1024 becomes the smallest normal
    return u16(sign | hs);
}

}  // namespace nxw::sem
