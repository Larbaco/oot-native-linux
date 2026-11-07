// AdvSIMD: vector and scalar integer and FP operations, permutes, immediates, shifts, element forms, crypto (AES, SHA-1,
// SHA-256, PMULL). Written per lane over std::array lanes; LLVM turns the fixed-trip loops into vector code. Each family helper
// reads the common fields (Q at 30, size at 23:22, Rm 20:16, Rn 9:5, Rd 4:0) from the word itself.
#include "fpx.hpp"

using namespace nxw::sem;

namespace {

template <bool Sg, unsigned SZ> using LT = std::conditional_t<Sg, typename UT<SZ>::i, typename UT<SZ>::u>;
template <class T> using Sg_t = std::make_signed_t<T>;
template <class T> using Us_t = std::make_unsigned_t<T>;
template <class T> constexpr unsigned bits_v = sizeof(T) * 8;

// Calls f(T{}) with T the lane type of a size field (signed or unsigned)
template <bool Sg, class Fn> NXW_INLINE void by_size(unsigned sz, Fn&& f) {
    switch (sz) {
    case 0: f(LT<Sg, 0>{}); break;
    case 1: f(LT<Sg, 1>{}); break;
    case 2: f(LT<Sg, 2>{}); break;
    default: f(LT<Sg, 3>{}); break;
    }
}
template <class Fn> NXW_INLINE void by_fsz(bool dbl, Fn&& f) {
    if (dbl) f(double{});
    else f(float{});
}
NXW_INLINE unsigned lanes(bool q, unsigned bytes) { return (q ? 16u : 8u) / bytes; }

template <class T> NXW_INLINE T mask_of(bool b) { return b ? T(~Us_t<T>(0)) : T(0); }

// ---------------------------------------------------------------- integer families
// Vd[i] = fn(Vn[i], Vm[i], Vd[i])
template <bool Sg, class Fn> NXW_INLINE void v3(State* s, u32 w, Fn fn) {
    by_size<Sg>(Size(w), [&](auto tag) {
        using T = decltype(tag);
        const bool q = Qb(w);
        const unsigned n = lanes(q, sizeof(T));
        const LV<T> a = vget<T>(s, Rn(w)), b = vget<T>(s, Rm(w)), d = vget<T>(s, Rd(w));
        LV<T> r{};
        for (unsigned i = 0; i < n; i++) r[i] = T(fn(a[i], b[i], d[i]));
        vset<T>(s, Rd(w), r, q);
    });
}
// Scalar form of v3: one element of the size, the rest of Vd zero
template <bool Sg, class Fn> NXW_INLINE void s3(State* s, u32 w, Fn fn) {
    by_size<Sg>(Size(w), [&](auto tag) {
        using T = decltype(tag);
        vset_scalar<T>(s, Rd(w), T(fn(lane<T>(s, Rn(w), 0), lane<T>(s, Rm(w), 0), lane<T>(s, Rd(w), 0))));
    });
}
// Vd[i] = fn(Vn[i], Vd[i])
template <bool Sg, class Fn> NXW_INLINE void v1(State* s, u32 w, Fn fn) {
    by_size<Sg>(Size(w), [&](auto tag) {
        using T = decltype(tag);
        const bool q = Qb(w);
        const unsigned n = lanes(q, sizeof(T));
        const LV<T> a = vget<T>(s, Rn(w)), d = vget<T>(s, Rd(w));
        LV<T> r{};
        for (unsigned i = 0; i < n; i++) r[i] = T(fn(a[i], d[i]));
        vset<T>(s, Rd(w), r, q);
    });
}
template <bool Sg, class Fn> NXW_INLINE void s1(State* s, u32 w, Fn fn) {
    by_size<Sg>(Size(w), [&](auto tag) {
        using T = decltype(tag);
        vset_scalar<T>(s, Rd(w), T(fn(lane<T>(s, Rn(w), 0), lane<T>(s, Rd(w), 0))));
    });
}
// Pairwise: Vd[i] = fn(concat(Vn, Vm)[2i], concat[2i+1])
template <bool Sg, class Fn> NXW_INLINE void vpair(State* s, u32 w, Fn fn) {
    by_size<Sg>(Size(w), [&](auto tag) {
        using T = decltype(tag);
        const bool q = Qb(w);
        const unsigned n = lanes(q, sizeof(T));
        const LV<T> a = vget<T>(s, Rn(w)), b = vget<T>(s, Rm(w));
        T cat[32] = {};
        for (unsigned i = 0; i < n; i++) cat[i] = a[i], cat[n + i] = b[i];
        LV<T> r{};
        for (unsigned i = 0; i < n; i++) r[i] = T(fn(cat[2 * i], cat[2 * i + 1]));
        vset<T>(s, Rd(w), r, q);
    });
}
// Long: Vd (2x wide) [i] = fn(wide(Vn[i + part]), wide(Vm[i + part]), Vd[i]); part = upper half when Q ("2" forms)
template <bool Sg, class Fn> NXW_INLINE void vlong(State* s, u32 w, Fn fn) {
    by_size<Sg>(Size(w), [&](auto tag) {
        using T = decltype(tag);
        if constexpr (sizeof(T) < 8) {
            using W = Wider<T>;
            const unsigned n = 8 / sizeof(T), part = Qb(w) ? n : 0;
            const LV<T> a = vget<T>(s, Rn(w)), b = vget<T>(s, Rm(w));
            const LV<W> d = vget<W>(s, Rd(w));
            LV<W> r{};
            for (unsigned i = 0; i < n; i++) r[i] = W(fn(W(a[i + part]), W(b[i + part]), d[i]));
            vset<W>(s, Rd(w), r);
        }
    });
}
// Wide: Vd[i] = fn(Vn (wide)[i], wide(Vm[i + part]))
template <bool Sg, class Fn> NXW_INLINE void vwide(State* s, u32 w, Fn fn) {
    by_size<Sg>(Size(w), [&](auto tag) {
        using T = decltype(tag);
        if constexpr (sizeof(T) < 8) {
            using W = Wider<T>;
            const unsigned n = 8 / sizeof(T), part = Qb(w) ? n : 0;
            const LV<W> a = vget<W>(s, Rn(w));
            const LV<T> b = vget<T>(s, Rm(w));
            LV<W> r{};
            for (unsigned i = 0; i < n; i++) r[i] = W(fn(a[i], W(b[i + part])));
            vset<W>(s, Rd(w), r);
        }
    });
}
// Narrow into half of Vd: Q = 0 writes the lower half and clears the upper, Q = 1 writes the upper half and keeps the lower
template <class T> NXW_INLINE void set_narrow(State* s, unsigned d, const T* v, unsigned n, bool q) {
    LV<T> r = q ? vget<T>(s, d) : LV<T>{};
    for (unsigned i = 0; i < n; i++) r[(q ? n : 0) + i] = v[i];
    vset<T>(s, d, r);
}
// Narrowing: fn(wide Vn[i], wide Vm[i]) -> narrow (size names the narrow lanes)
template <bool Sg, class Fn> NXW_INLINE void vnarrow(State* s, u32 w, Fn fn) {
    by_size<Sg>(Size(w), [&](auto tag) {
        using T = decltype(tag);
        if constexpr (sizeof(T) < 8) {
            using W = Wider<T>;
            const unsigned n = 8 / sizeof(T);
            const LV<W> a = vget<W>(s, Rn(w)), b = vget<W>(s, Rm(w));
            T v[8];
            for (unsigned i = 0; i < n; i++) v[i] = T(fn(a[i], b[i]));
            set_narrow<T>(s, Rd(w), v, n, Qb(w));
        }
    });
}

// Shift by a signed register amount (SSHL, USHL, SRSHL, URSHL, SQSHL, UQSHL, SQRSHL, UQRSHL); returns the value before
// truncation or saturation, in 128 bits
template <class T> NXW_INLINE i128 shift_by(T a, i8 sh, bool round) {
    const int es = int(bits_v<T>);
    const i128 v = i128(a);
    if (sh >= 0) {
        if (sh >= es) return v == 0 ? 0 : (v < 0 ? -(i128(1) << 100) : (i128(1) << 100));   // out of range of every lane type
        return v << sh;
    }
    int rs = -sh;
    if (rs > es + 1) rs = es + 1;
    if (round) return (v + (i128(1) << (rs - 1))) >> rs;
    return v >> rs;
}
template <class T> NXW_INLINE T trunc_or_sat(i128 v, bool saturate) { return saturate ? sat128<T>(v) : T(v); }

// Doubling multiplies (SQDMULH, SQRDMULH, SQDMULL, ...): 2 * a * b in 128 bits
template <class T> NXW_INLINE i128 dmul(T a, T b) { return 2 * i128(a) * i128(b); }
template <class T> NXW_INLINE T sqdmulh(T a, T b, bool round) {
    return sat128<T>((dmul(a, b) + (round ? i128(1) << (bits_v<T> - 1) : 0)) >> bits_v<T>);
}

// Carry-less multiply
NXW_INLINE u128 clmul(u64 a, u64 b, unsigned bits) {
    u128 r = 0;
    for (unsigned i = 0; i < bits; i++)
        if (b >> i & 1) r ^= u128(a) << i;
    return r;
}

// ---------------------------------------------------------------- FP families (sz bit 22: single / double lanes)
template <class Fn> NXW_INLINE void fv3(State* s, u32 w, Fn fn) {
    by_fsz(w >> 22 & 1, [&](auto tag) {
        using T = decltype(tag);
        const bool q = Qb(w);
        const unsigned n = lanes(q, sizeof(T));
        const LV<T> a = vget<T>(s, Rn(w)), b = vget<T>(s, Rm(w)), d = vget<T>(s, Rd(w));
        LV<T> r{};
        for (unsigned i = 0; i < n; i++) r[i] = fn(a[i], b[i], d[i]);
        vset<T>(s, Rd(w), r, q);
    });
}
template <class Fn> NXW_INLINE void fs3(State* s, u32 w, Fn fn) {
    by_fsz(w >> 22 & 1, [&](auto tag) {
        using T = decltype(tag);
        vset_scalar<T>(s, Rd(w), fn(lane<T>(s, Rn(w), 0), lane<T>(s, Rm(w), 0), lane<T>(s, Rd(w), 0)));
    });
}
// FP compares into masks
template <class Fn> NXW_INLINE void fvcmp(State* s, u32 w, bool zero, Fn fn) {
    by_fsz(w >> 22 & 1, [&](auto tag) {
        using T = decltype(tag);
        using U = typename FT<T>::U;
        const bool q = Qb(w);
        const unsigned n = lanes(q, sizeof(T));
        const LV<T> a = vget<T>(s, Rn(w)), b = vget<T>(s, Rm(w));
        LV<U> r{};
        for (unsigned i = 0; i < n; i++) r[i] = mask_of<U>(fn(a[i], zero ? T(0) : b[i]));
        vset<U>(s, Rd(w), r, q);
    });
}
template <class Fn> NXW_INLINE void fscmp(State* s, u32 w, bool zero, Fn fn) {
    by_fsz(w >> 22 & 1, [&](auto tag) {
        using T = decltype(tag);
        using U = typename FT<T>::U;
        vset_scalar<U>(s, Rd(w), mask_of<U>(fn(lane<T>(s, Rn(w), 0), zero ? T(0) : lane<T>(s, Rm(w), 0))));
    });
}
template <class Fn> NXW_INLINE void fv1(State* s, u32 w, Fn fn) {
    by_fsz(w >> 22 & 1, [&](auto tag) {
        using T = decltype(tag);
        const bool q = Qb(w);
        const unsigned n = lanes(q, sizeof(T));
        const LV<T> a = vget<T>(s, Rn(w));
        LV<T> r{};
        for (unsigned i = 0; i < n; i++) r[i] = fn(a[i]);
        vset<T>(s, Rd(w), r, q);
    });
}
template <class Fn> NXW_INLINE void fs1(State* s, u32 w, Fn fn) {
    by_fsz(w >> 22 & 1, [&](auto tag) {
        using T = decltype(tag);
        vset_scalar<T>(s, Rd(w), fn(lane<T>(s, Rn(w), 0)));
    });
}
template <class Fn> NXW_INLINE void fvpair(State* s, u32 w, Fn fn) {
    by_fsz(w >> 22 & 1, [&](auto tag) {
        using T = decltype(tag);
        const bool q = Qb(w);
        const unsigned n = lanes(q, sizeof(T));
        const LV<T> a = vget<T>(s, Rn(w)), b = vget<T>(s, Rm(w));
        T cat[8] = {};
        for (unsigned i = 0; i < n; i++) cat[i] = a[i], cat[n + i] = b[i];
        LV<T> r{};
        for (unsigned i = 0; i < n; i++) r[i] = fn(cat[2 * i], cat[2 * i + 1]);
        vset<T>(s, Rd(w), r, q);
    });
}
// Scalar pairwise (FADDP s0, v1.2s): the two lowest lanes of Vn
template <class Fn> NXW_INLINE void fspair(State* s, u32 w, Fn fn) {
    by_fsz(w >> 22 & 1, [&](auto tag) {
        using T = decltype(tag);
        vset_scalar<T>(s, Rd(w), fn(lane<T>(s, Rn(w), 0), lane<T>(s, Rn(w), 1)));
    });
}
// FP to integer lanes, integer lanes to FP
template <bool Signed> NXW_INLINE void fcvt_v(State* s, u32 w, Rnd rnd, int fbits, bool scalar) {
    by_fsz(w >> 22 & 1, [&](auto tag) {
        using T = decltype(tag);
        using U = typename FT<T>::U;
        using I = std::conditional_t<Signed, Sg_t<U>, U>;
        const bool q = scalar ? false : Qb(w);
        const unsigned n = scalar ? 1 : lanes(q, sizeof(T));
        const LV<T> a = vget<T>(s, Rn(w));
        LV<U> r{};
        for (unsigned i = 0; i < n; i++) r[i] = U(fp_to_int<I>(a[i] * exp2i<T>(fbits), rnd));
        if (scalar) vset_scalar<U>(s, Rd(w), r[0]);
        else vset<U>(s, Rd(w), r, q);
    });
}
template <bool Signed> NXW_INLINE void cvtf_v(State* s, u32 w, int fbits, bool scalar) {
    by_fsz(w >> 22 & 1, [&](auto tag) {
        using T = decltype(tag);
        using U = typename FT<T>::U;
        using I = std::conditional_t<Signed, Sg_t<U>, U>;
        const bool q = scalar ? false : Qb(w);
        const unsigned n = scalar ? 1 : lanes(q, sizeof(T));
        const LV<U> a = vget<U>(s, Rn(w));
        LV<T> r{};
        for (unsigned i = 0; i < n; i++) r[i] = T(I(a[i])) * exp2i<T>(-fbits);
        if (scalar) vset_scalar<T>(s, Rd(w), r[0]);
        else vset<T>(s, Rd(w), r, q);
    });
}

// ---------------------------------------------------------------- immediate shifts
NXW_INLINE unsigned hsz(u32 w) { return unsigned(31 - __builtin_clz(w >> 19 & 15)); }   // log2(esize / 8) from immh
NXW_INLINE unsigned immhb(u32 w) { return w >> 16 & 0x7f; }
NXW_INLINE int rshift_amt(u32 w) { return int(16u << hsz(w)) - int(immhb(w)); }   // 2 * esize - immh:immb
NXW_INLINE int lshift_amt(u32 w) { return int(immhb(w)) - int(8u << hsz(w)); }

template <bool Sg, class Fn> NXW_INLINE void vshift(State* s, u32 w, bool scalar, Fn fn) {
    by_size<Sg>(hsz(w), [&](auto tag) {
        using T = decltype(tag);
        const bool q = scalar ? false : Qb(w);
        const unsigned n = scalar ? 1 : lanes(q, sizeof(T));
        const LV<T> a = vget<T>(s, Rn(w)), d = vget<T>(s, Rd(w));
        LV<T> r{};
        for (unsigned i = 0; i < n; i++) r[i] = T(fn(a[i], d[i]));
        if (scalar) vset_scalar<T>(s, Rd(w), r[0]);
        else vset<T>(s, Rd(w), r, q);
    });
}
// Narrowing shift right: wide source lanes, narrow (hsz) results
template <bool Sg, class Fn> NXW_INLINE void vshift_narrow(State* s, u32 w, bool scalar, Fn fn) {
    by_size<Sg>(hsz(w), [&](auto tag) {
        using T = decltype(tag);
        if constexpr (sizeof(T) < 8) {
            using W = Wider<T>;
            const unsigned n = scalar ? 1 : 8 / sizeof(T);
            const LV<W> a = vget<W>(s, Rn(w));
            T v[8];
            for (unsigned i = 0; i < n; i++) v[i] = fn(a[i], T{});
            if (scalar) vset_scalar<T>(s, Rd(w), v[0]);
            else set_narrow<T>(s, Rd(w), v, n, Qb(w));
        }
    });
}
template <class T> NXW_INLINE i128 rshift(T a, int sh, bool round) {
    const i128 v = i128(a);
    return round ? (v + (i128(1) << (sh - 1))) >> sh : v >> sh;
}

// ---------------------------------------------------------------- element selection
struct Elem {
    unsigned m, index;
};
NXW_INLINE Elem elem_int(u32 w) {
    const unsigned L = w >> 21 & 1, M = w >> 20 & 1, H = w >> 11 & 1, rm = w >> 16 & 15;
    if (Size(w) == 1) return {rm, H << 2 | L << 1 | M};
    return {M << 4 | rm, H << 1 | L};
}
NXW_INLINE Elem elem_fp(u32 w) {
    const unsigned L = w >> 21 & 1, M = w >> 20 & 1, H = w >> 11 & 1, rm = w >> 16 & 15;
    if (!(w >> 22 & 1)) return {M << 4 | rm, H << 1 | L};
    return {M << 4 | rm, H};
}
// imm5 of DUP, INS, SMOV, UMOV: lane size and index
struct Imm5 {
    unsigned size, index;
};
NXW_INLINE Imm5 imm5_of(u32 w) {
    const unsigned i = w >> 16 & 31;
    const unsigned size = unsigned(std::countr_zero(i));
    return {size, i >> (size + 1)};
}

// AdvSIMDExpandImm (cmode/op as the decoder's op split them)
NXW_INLINE u64 imm8_of(u32 w) { return (w >> 16 & 7) << 5 | (w >> 5 & 31); }
NXW_INLINE u64 rep32(u64 v) { return v | v << 32; }
NXW_INLINE u64 rep16(u64 v) { return v | v << 16 | v << 32 | v << 48; }
NXW_INLINE void set_imm(State* s, u32 w, u64 v64, bool q) {
    LV<u64> r{v64, q ? v64 : 0};
    s->q[Rd(w)] = std::bit_cast<V128>(r);
}
NXW_INLINE void orr_imm(State* s, u32 w, u64 v64, bool bic) {
    LV<u64> r = vget<u64>(s, Rd(w));
    r[0] = bic ? r[0] & ~v64 : r[0] | v64;
    r[1] = Qb(w) ? (bic ? r[1] & ~v64 : r[1] | v64) : 0;
    s->q[Rd(w)] = std::bit_cast<V128>(r);
}

}  // namespace

// ================================================================ three same (vector)
#define V3(NAME, SG, ...) \
    SEM(NAME) { \
        v3<SG>(s, w, [](auto a, auto b, auto d) { \
            using T = decltype(a); \
            (void)b, (void)d; \
            return T(__VA_ARGS__); \
        }); \
        return 0; \
    }
V3(ADD, false, a + b)
V3(SUB, false, a - b)
V3(MUL, false, a * b)
V3(MLA, false, d + a * b)
V3(MLS, false, d - a * b)
V3(AND_v, false, a & b)
V3(BIC_v, false, a & ~b)
V3(ORR_v, false, a | b)
V3(ORN_v, false, a | ~b)
V3(EOR_v, false, a ^ b)
V3(BSL_v, false, (d & a) | (~d & b))
V3(BIT_v, false, (d & ~b) | (a & b))
V3(BIF_v, false, (d & b) | (a & ~b))
V3(CMEQ, false, mask_of<T>(a == b))
V3(CMTST, false, mask_of<T>((a & b) != 0))
V3(CMHI, false, mask_of<T>(a > b))
V3(CMHS, false, mask_of<T>(a >= b))
V3(CMGT, true, mask_of<T>(a > b))
V3(CMGE, true, mask_of<T>(a >= b))
V3(SMAX, true, a > b ? a : b)
V3(SMIN, true, a < b ? a : b)
V3(UMAX, false, a > b ? a : b)
V3(UMIN, false, a < b ? a : b)
V3(SABD, true, a > b ? Wider<T>(a) - b : Wider<T>(b) - a)
V3(UABD, false, a > b ? a - b : b - a)
V3(SABA, true, d + (a > b ? Wider<T>(a) - b : Wider<T>(b) - a))
V3(UABA, false, d + (a > b ? a - b : b - a))
V3(SHADD, true, (Wider<T>(a) + b) >> 1)
V3(UHADD, false, (Wider<T>(a) + b) >> 1)
V3(SRHADD, true, (Wider<T>(a) + b + 1) >> 1)
V3(URHADD, false, (Wider<T>(a) + b + 1) >> 1)
V3(SHSUB, true, (Wider<T>(a) - b) >> 1)
V3(UHSUB, false, (Wider<T>(a) - b) >> 1)
V3(SQADD, true, sat128<T>(i128(a) + b))
V3(UQADD, false, sat128<T>(i128(a) + b))
V3(SQSUB, true, sat128<T>(i128(a) - b))
V3(UQSUB, false, sat128<T>(i128(a) - b))
V3(SSHL, true, trunc_or_sat<T>(shift_by(a, i8(b), false), false))
V3(USHL, false, trunc_or_sat<T>(shift_by(a, i8(b), false), false))
V3(SRSHL, true, trunc_or_sat<T>(shift_by(a, i8(b), true), false))
V3(URSHL, false, trunc_or_sat<T>(shift_by(a, i8(b), true), false))
V3(SQSHL, true, trunc_or_sat<T>(shift_by(a, i8(b), false), true))
V3(UQSHL, false, trunc_or_sat<T>(shift_by(a, i8(b), false), true))
V3(SQRSHL, true, trunc_or_sat<T>(shift_by(a, i8(b), true), true))
V3(UQRSHL, false, trunc_or_sat<T>(shift_by(a, i8(b), true), true))
V3(SQDMULH, true, sqdmulh(a, b, false))
V3(SQRDMULH, true, sqdmulh(a, b, true))
V3(PMUL, false, u8(clmul(a, b, 8)))

#define VP(NAME, SG, ...) \
    SEM(NAME) { \
        vpair<SG>(s, w, [](auto a, auto b) { \
            using T = decltype(a); \
            return T(__VA_ARGS__); \
        }); \
        return 0; \
    }
VP(ADDP, false, a + b)
VP(SMAXP, true, a > b ? a : b)
VP(SMINP, true, a < b ? a : b)
VP(UMAXP, false, a > b ? a : b)
VP(UMINP, false, a < b ? a : b)

// ================================================================ three same (scalar; the 64-bit-only ones have size 11)
#define S3(NAME, SG, ...) \
    SEM(NAME) { \
        s3<SG>(s, w, [](auto a, auto b, auto d) { \
            using T = decltype(a); \
            (void)b, (void)d; \
            return T(__VA_ARGS__); \
        }); \
        return 0; \
    }
S3(ADD_s, false, a + b)
S3(SUB_s, false, a - b)
S3(CMEQ_s, false, mask_of<T>(a == b))
S3(CMTST_s, false, mask_of<T>((a & b) != 0))
S3(CMHI_s, false, mask_of<T>(a > b))
S3(CMHS_s, false, mask_of<T>(a >= b))
S3(CMGT_s, true, mask_of<T>(a > b))
S3(CMGE_s, true, mask_of<T>(a >= b))
S3(SQADD_s, true, sat128<T>(i128(a) + b))
S3(UQADD_s, false, sat128<T>(i128(a) + b))
S3(SQSUB_s, true, sat128<T>(i128(a) - b))
S3(UQSUB_s, false, sat128<T>(i128(a) - b))
S3(SSHL_s, true, trunc_or_sat<T>(shift_by(a, i8(b), false), false))
S3(USHL_s, false, trunc_or_sat<T>(shift_by(a, i8(b), false), false))
S3(SRSHL_s, true, trunc_or_sat<T>(shift_by(a, i8(b), true), false))
S3(URSHL_s, false, trunc_or_sat<T>(shift_by(a, i8(b), true), false))
S3(SQSHL_s, true, trunc_or_sat<T>(shift_by(a, i8(b), false), true))
S3(UQSHL_s, false, trunc_or_sat<T>(shift_by(a, i8(b), false), true))
S3(SQRSHL_s, true, trunc_or_sat<T>(shift_by(a, i8(b), true), true))
S3(UQRSHL_s, false, trunc_or_sat<T>(shift_by(a, i8(b), true), true))
S3(SQDMULH_s, true, sqdmulh(a, b, false))
S3(SQRDMULH_s, true, sqdmulh(a, b, true))

SEM(ADDP_s) {   // addp d0, v1.2d
    vset_scalar<u64>(s, Rd(w), lane<u64>(s, Rn(w), 0) + lane<u64>(s, Rn(w), 1));
    return 0;
}

// ================================================================ two-register misc (vector and scalar)
#define V1(NAME, SG, ...) \
    SEM(NAME) { \
        v1<SG>(s, w, [](auto a, auto d) { \
            using T = decltype(a); \
            (void)d; \
            return T(__VA_ARGS__); \
        }); \
        return 0; \
    }
#define S1(NAME, SG, ...) \
    SEM(NAME) { \
        s1<SG>(s, w, [](auto a, auto d) { \
            using T = decltype(a); \
            (void)d; \
            return T(__VA_ARGS__); \
        }); \
        return 0; \
    }
V1(NEG, false, T(0) - a)
V1(ABS, true, a < 0 ? T(Us_t<T>(0) - Us_t<T>(a)) : a)
V1(SQABS, true, sat128<T>(a < 0 ? -i128(a) : i128(a)))
V1(SQNEG, true, sat128<T>(-i128(a)))
V1(CMGT_zero, true, mask_of<T>(a > 0))
V1(CMGE_zero, true, mask_of<T>(a >= 0))
V1(CMEQ_zero, true, mask_of<T>(a == 0))
V1(CMLE_zero, true, mask_of<T>(a <= 0))
V1(CMLT_zero, true, mask_of<T>(a < 0))
V1(CLZ_v, false, std::countl_zero(a))
V1(CLS_v, true, std::countl_zero(Us_t<T>(a ^ T(a >> 1))) - 1)
V1(CNT, false, std::popcount(a))
V1(NOT, false, ~a)
V1(RBIT_v, false, __builtin_bitreverse8(u8(a)))
V1(SUQADD, true, sat128<T>(i128(d) + i128(Us_t<T>(a))))
V1(USQADD, false, sat128<T>(i128(d) + i128(Sg_t<T>(a))))
S1(NEG_s, false, T(0) - a)
S1(ABS_s, true, a < 0 ? T(Us_t<T>(0) - Us_t<T>(a)) : a)
S1(SQABS_s, true, sat128<T>(a < 0 ? -i128(a) : i128(a)))
S1(SQNEG_s, true, sat128<T>(-i128(a)))
S1(CMGT_zero_s, true, mask_of<T>(a > 0))
S1(CMGE_zero_s, true, mask_of<T>(a >= 0))
S1(CMEQ_zero_s, true, mask_of<T>(a == 0))
S1(CMLE_zero_s, true, mask_of<T>(a <= 0))
S1(CMLT_zero_s, true, mask_of<T>(a < 0))
S1(SUQADD_s, true, sat128<T>(i128(d) + i128(Us_t<T>(a))))
S1(USQADD_s, false, sat128<T>(i128(d) + i128(Sg_t<T>(a))))

// Reverse elements inside containers of 2^C bytes
NXW_INLINE void rev_in(State* s, u32 w, unsigned container) {
    const unsigned es = 1u << Size(w), n = lanes(Qb(w), 1);
    const LV<u8> a = vget<u8>(s, Rn(w));
    LV<u8> r{};
    for (unsigned i = 0; i < n; i++) {
        const unsigned base = i & ~(container - 1), off = i & (container - 1);
        const unsigned el = off / es, byte = off % es;
        r[i] = a[base + (container / es - 1 - el) * es + byte];
    }
    vset<u8>(s, Rd(w), r, Qb(w));
}
SEM(REV64) {
    rev_in(s, w, 8);
    return 0;
}
SEM(REV32_v) {
    rev_in(s, w, 4);
    return 0;
}
SEM(REV16_v) {
    rev_in(s, w, 2);
    return 0;
}

// Pairwise add long
template <bool Sg, bool Acc> NXW_INLINE void addlp(State* s, u32 w) {
    by_size<Sg>(Size(w), [&](auto tag) {
        using T = decltype(tag);
        if constexpr (sizeof(T) < 8) {
            using W = Wider<T>;
            const bool q = Qb(w);
            const unsigned n = lanes(q, sizeof(W));
            const LV<T> a = vget<T>(s, Rn(w));
            const LV<W> d = vget<W>(s, Rd(w));
            LV<W> r{};
            for (unsigned i = 0; i < n; i++) r[i] = W((Acc ? d[i] : W(0)) + W(a[2 * i]) + W(a[2 * i + 1]));
            vset<W>(s, Rd(w), r, q);
        }
    });
}
SEM(SADDLP) {
    addlp<true, false>(s, w);
    return 0;
}
SEM(UADDLP) {
    addlp<false, false>(s, w);
    return 0;
}
SEM(SADALP) {
    addlp<true, true>(s, w);
    return 0;
}
SEM(UADALP) {
    addlp<false, true>(s, w);
    return 0;
}

// Narrowing moves: size names the narrow lanes
template <bool SgIn, class Fn> NXW_INLINE void xtn(State* s, u32 w, bool scalar, Fn fn) {
    by_size<SgIn>(Size(w), [&](auto tag) {
        using T = decltype(tag);
        if constexpr (sizeof(T) < 8) {
            using W = Wider<T>;
            const unsigned n = scalar ? 1 : 8 / sizeof(T);
            const LV<W> a = vget<W>(s, Rn(w));
            Us_t<T> v[8];
            for (unsigned i = 0; i < n; i++) v[i] = Us_t<T>(fn(a[i], T{}));
            if (scalar) vset_scalar<Us_t<T>>(s, Rd(w), v[0]);
            else set_narrow<Us_t<T>>(s, Rd(w), v, n, Qb(w));
        }
    });
}
SEM(XTN) {
    xtn<false>(s, w, false, [](auto a, auto t) { return decltype(t)(a); });
    return 0;
}
SEM(SQXTN) {
    xtn<true>(s, w, false, [](auto a, auto t) { return sat128<decltype(t)>(i128(a)); });
    return 0;
}
SEM(UQXTN) {
    xtn<false>(s, w, false, [](auto a, auto t) { return sat128<decltype(t)>(i128(a)); });
    return 0;
}
SEM(SQXTUN) {
    xtn<true>(s, w, false, [](auto a, auto t) { return sat128<Us_t<decltype(t)>>(i128(a)); });
    return 0;
}
SEM(SQXTN_s) {
    xtn<true>(s, w, true, [](auto a, auto t) { return sat128<decltype(t)>(i128(a)); });
    return 0;
}
SEM(UQXTN_s) {
    xtn<false>(s, w, true, [](auto a, auto t) { return sat128<decltype(t)>(i128(a)); });
    return 0;
}
SEM(SQXTUN_s) {
    xtn<true>(s, w, true, [](auto a, auto t) { return sat128<Us_t<decltype(t)>>(i128(a)); });
    return 0;
}
SEM(SHLL) {   // shift left long by the element size
    by_size<false>(Size(w), [&](auto tag) {
        using T = decltype(tag);
        if constexpr (sizeof(T) < 8) {
            using W = Wider<T>;
            const unsigned n = 8 / sizeof(T), part = Qb(w) ? n : 0;
            const LV<T> a = vget<T>(s, Rn(w));
            LV<W> r{};
            for (unsigned i = 0; i < n; i++) r[i] = W(W(a[i + part]) << bits_v<T>);
            vset<W>(s, Rd(w), r);
        }
    });
    return 0;
}

// ================================================================ across lanes
template <bool Sg, bool Long, class Fn> NXW_INLINE void across(State* s, u32 w, Fn fn) {
    by_size<Sg>(Size(w), [&](auto tag) {
        using T = decltype(tag);
        if constexpr (sizeof(T) < 8) {
            using R = std::conditional_t<Long, Wider<T>, T>;
            const unsigned n = lanes(Qb(w), sizeof(T));
            const LV<T> a = vget<T>(s, Rn(w));
            R acc = R(a[0]);
            for (unsigned i = 1; i < n; i++) acc = R(fn(acc, R(a[i])));
            vset_scalar<Us_t<R>>(s, Rd(w), Us_t<R>(acc));
        }
    });
}
#define ACROSS(NAME, SG, LONG, ...) \
    SEM(NAME) { \
        across<SG, LONG>(s, w, [](auto a, auto b) { \
            using T = decltype(a); \
            return T(__VA_ARGS__); \
        }); \
        return 0; \
    }
ACROSS(ADDV, false, false, a + b)
ACROSS(SADDLV, true, true, a + b)
ACROSS(UADDLV, false, true, a + b)
ACROSS(SMAXV, true, false, a > b ? a : b)
ACROSS(SMINV, true, false, a < b ? a : b)
ACROSS(UMAXV, false, false, a > b ? a : b)
ACROSS(UMINV, false, false, a < b ? a : b)

// FP across lanes (4S only): the Arm ARM's pairwise tree, op(op(e0, e1), op(e2, e3))
#define FACROSS(NAME, FN) \
    SEM(NAME) { \
        const LV<float> a = vget<float>(s, Rn(w)); \
        vset_scalar<float>(s, Rd(w), FN(FN(a[0], a[1]), FN(a[2], a[3]))); \
        return 0; \
    }
FACROSS(FMAXV, fp_max)
FACROSS(FMINV, fp_min)
FACROSS(FMAXNMV, fp_maxnum)
FACROSS(FMINNMV, fp_minnum)

// ================================================================ three different (long, wide, narrow)
#define VL(NAME, SG, ...) \
    SEM(NAME) { \
        vlong<SG>(s, w, [](auto a, auto b, auto d) { \
            using W = decltype(a); \
            (void)d; \
            return W(__VA_ARGS__); \
        }); \
        return 0; \
    }
VL(SADDL, true, a + b)
VL(UADDL, false, a + b)
VL(SSUBL, true, a - b)
VL(USUBL, false, a - b)
VL(SABDL, true, a > b ? a - b : b - a)
VL(UABDL, false, a > b ? a - b : b - a)
VL(SABAL, true, d + (a > b ? a - b : b - a))
VL(UABAL, false, d + (a > b ? a - b : b - a))
VL(SMULL, true, a * b)
VL(UMULL, false, a * b)
VL(SMLAL, true, d + a * b)
VL(UMLAL, false, d + a * b)
VL(SMLSL, true, d - a * b)
VL(UMLSL, false, d - a * b)
VL(SQDMULL, true, sat128<W>(2 * i128(a) * i128(b)))
VL(SQDMLAL, true, sat128<W>(i128(d) + sat128<W>(2 * i128(a) * i128(b))))
VL(SQDMLSL, true, sat128<W>(i128(d) - sat128<W>(2 * i128(a) * i128(b))))

#define VW(NAME, SG, ...) \
    SEM(NAME) { \
        vwide<SG>(s, w, [](auto a, auto b) { \
            using W = decltype(a); \
            return W(__VA_ARGS__); \
        }); \
        return 0; \
    }
VW(SADDW, true, a + b)
VW(UADDW, false, a + b)
VW(SSUBW, true, a - b)
VW(USUBW, false, a - b)

#define VN(NAME, ...) \
    SEM(NAME) { \
        vnarrow<false>(s, w, [](auto a, auto b) { \
            using W = decltype(a); \
            constexpr unsigned half = sizeof(W) * 4; \
            return (__VA_ARGS__) >> half; \
        }); \
        return 0; \
    }
VN(ADDHN, W(a + b))
VN(SUBHN, W(a - b))
VN(RADDHN, W(a + b + (W(1) << (half - 1))))
VN(RSUBHN, W(a - b + (W(1) << (half - 1))))

SEM(PMULL) {
    const bool q = Qb(w);
    if (Size(w) == 3) {
        const u128 r = clmul(lane<u64>(s, Rn(w), q), lane<u64>(s, Rm(w), q), 64);
        s->q[Rd(w)] = std::bit_cast<V128>(r);
    } else {
        const LV<u8> a = vget<u8>(s, Rn(w)), b = vget<u8>(s, Rm(w));
        LV<u16> r{};
        for (unsigned i = 0; i < 8; i++) r[i] = u16(clmul(a[i + (q ? 8 : 0)], b[i + (q ? 8 : 0)], 8));
        vset<u16>(s, Rd(w), r);
    }
    return 0;
}

// ================================================================ copy, permute, table
SEM(DUP_elem) {
    const Imm5 e = imm5_of(w);
    by_size<false>(e.size, [&](auto tag) {
        using T = decltype(tag);
        const T v = lane<T>(s, Rn(w), e.index);
        LV<T> r{};
        for (unsigned i = 0; i < lanes(Qb(w), sizeof(T)); i++) r[i] = v;
        vset<T>(s, Rd(w), r, Qb(w));
    });
    return 0;
}
SEM(DUP_elem_s) {
    const Imm5 e = imm5_of(w);
    by_size<false>(e.size, [&](auto tag) {
        using T = decltype(tag);
        vset_scalar<T>(s, Rd(w), lane<T>(s, Rn(w), e.index));
    });
    return 0;
}
SEM(DUP_gen) {
    const Imm5 e = imm5_of(w);
    by_size<false>(e.size, [&](auto tag) {
        using T = decltype(tag);
        const T v = T(X(s, Rn(w)));
        LV<T> r{};
        for (unsigned i = 0; i < lanes(Qb(w), sizeof(T)); i++) r[i] = v;
        vset<T>(s, Rd(w), r, Qb(w));
    });
    return 0;
}
SEM(SMOV) {
    const Imm5 e = imm5_of(w);
    by_size<true>(e.size, [&](auto tag) {
        using T = decltype(tag);
        const i64 v = lane<T>(s, Rn(w), e.index);
        wX(s, Rd(w), Qb(w) ? u64(v) : u32(v));
    });
    return 0;
}
SEM(UMOV) {
    const Imm5 e = imm5_of(w);
    by_size<false>(e.size, [&](auto tag) {
        using T = decltype(tag);
        wX(s, Rd(w), u64(lane<T>(s, Rn(w), e.index)));
    });
    return 0;
}
SEM(INS_gen) {
    const Imm5 e = imm5_of(w);
    by_size<false>(e.size, [&](auto tag) {
        using T = decltype(tag);
        set_lane<T>(s, Rd(w), e.index, T(X(s, Rn(w))));
    });
    return 0;
}
SEM(INS_elem) {
    const Imm5 e = imm5_of(w);
    const unsigned src = (w >> 11 & 15) >> e.size;
    by_size<false>(e.size, [&](auto tag) {
        using T = decltype(tag);
        set_lane<T>(s, Rd(w), e.index, lane<T>(s, Rn(w), src));
    });
    return 0;
}

#define PERM(NAME, ...) \
    SEM(NAME) { \
        by_size<false>(Size(w), [&](auto tag) { \
            using T = decltype(tag); \
            const bool q = Qb(w); \
            const unsigned n = lanes(q, sizeof(T)), h = n / 2; \
            const LV<T> a = vget<T>(s, Rn(w)), b = vget<T>(s, Rm(w)); \
            T cat[32] = {}; \
            for (unsigned i = 0; i < n; i++) cat[i] = a[i], cat[n + i] = b[i]; \
            LV<T> r{}; \
            for (unsigned i = 0; i < n; i++) r[i] = (__VA_ARGS__); \
            (void)h; \
            vset<T>(s, Rd(w), r, q); \
        }); \
        return 0; \
    }
PERM(UZP1, cat[2 * i])
PERM(UZP2, cat[2 * i + 1])
PERM(TRN1, (i & 1) ? b[i - 1] : a[i])
PERM(TRN2, (i & 1) ? b[i] : a[i + 1])
PERM(ZIP1, (i & 1) ? b[i / 2] : a[i / 2])
PERM(ZIP2, (i & 1) ? b[h + i / 2] : a[h + i / 2])

SEM(EXT) {
    const bool q = Qb(w);
    const unsigned n = q ? 16 : 8, pos = w >> 11 & 15;
    const LV<u8> a = vget<u8>(s, Rn(w)), b = vget<u8>(s, Rm(w));
    LV<u8> r{};
    for (unsigned i = 0; i < n; i++) r[i] = i + pos < n ? a[i + pos] : b[i + pos - n];
    vset<u8>(s, Rd(w), r, q);
    return 0;
}

template <bool Tbx> NXW_INLINE void tbl(State* s, u32 w) {
    const bool q = Qb(w);
    const unsigned regs = (w >> 13 & 3) + 1, n = q ? 16 : 8;
    u8 table[64];
    for (unsigned r = 0; r < regs; r++) {
        const LV<u8> t = vget<u8>(s, (Rn(w) + r) % 32);
        for (unsigned i = 0; i < 16; i++) table[r * 16 + i] = t[i];
    }
    const LV<u8> idx = vget<u8>(s, Rm(w)), d = vget<u8>(s, Rd(w));
    LV<u8> r{};
    for (unsigned i = 0; i < n; i++) r[i] = idx[i] < regs * 16 ? table[idx[i]] : (Tbx ? d[i] : 0);
    vset<u8>(s, Rd(w), r, q);
}
SEM(TBL) {
    tbl<false>(s, w);
    return 0;
}
SEM(TBX) {
    tbl<true>(s, w);
    return 0;
}

// ================================================================ modified immediates
SEM(MOVI_32) {
    set_imm(s, w, rep32(imm8_of(w) << ((w >> 13 & 3) * 8)), Qb(w));
    return 0;
}
SEM(MVNI_32) {
    set_imm(s, w, ~rep32(imm8_of(w) << ((w >> 13 & 3) * 8)), Qb(w));
    return 0;
}
SEM(ORR_imm_32) {
    orr_imm(s, w, rep32(imm8_of(w) << ((w >> 13 & 3) * 8)), false);
    return 0;
}
SEM(BIC_imm_32) {
    orr_imm(s, w, rep32(imm8_of(w) << ((w >> 13 & 3) * 8)), true);
    return 0;
}
SEM(MOVI_16) {
    set_imm(s, w, rep16(imm8_of(w) << ((w >> 13 & 1) * 8)), Qb(w));
    return 0;
}
SEM(MVNI_16) {
    set_imm(s, w, ~rep16(imm8_of(w) << ((w >> 13 & 1) * 8)), Qb(w));
    return 0;
}
SEM(ORR_imm_16) {
    orr_imm(s, w, rep16(imm8_of(w) << ((w >> 13 & 1) * 8)), false);
    return 0;
}
SEM(BIC_imm_16) {
    orr_imm(s, w, rep16(imm8_of(w) << ((w >> 13 & 1) * 8)), true);
    return 0;
}
NXW_INLINE u64 msl(u32 w) { return (w >> 12 & 1) ? imm8_of(w) << 16 | 0xffff : imm8_of(w) << 8 | 0xff; }
SEM(MOVI_msl) {
    set_imm(s, w, rep32(msl(w)), Qb(w));
    return 0;
}
SEM(MVNI_msl) {
    set_imm(s, w, ~rep32(msl(w)), Qb(w));
    return 0;
}
SEM(MOVI_8) {
    set_imm(s, w, imm8_of(w) * 0x0101010101010101ull, Qb(w));
    return 0;
}
SEM(MOVI_64) {
    const u64 i = imm8_of(w);
    u64 v = 0;
    for (unsigned b = 0; b < 8; b++)
        if (i >> b & 1) v |= u64(0xff) << (b * 8);
    set_imm(s, w, v, Qb(w));
    return 0;
}
SEM(FMOV_v_s) {
    const u32 i = u32(imm8_of(w)), b6 = i >> 6 & 1;
    const u32 v = (i >> 7) << 31 | (b6 ^ 1) << 30 | (b6 ? 0x1fu << 25 : 0) | (i >> 4 & 3) << 23 | (i & 15) << 19;
    set_imm(s, w, rep32(v), Qb(w));
    return 0;
}
SEM(FMOV_v_d) {
    const u64 i = imm8_of(w), b6 = i >> 6 & 1;
    const u64 v = (i >> 7) << 63 | (b6 ^ 1) << 62 | (b6 ? u64(0xff) << 54 : 0) | (i >> 4 & 3) << 52 | (i & 15) << 48;
    set_imm(s, w, v, true);
    return 0;
}

// ================================================================ shifts by immediate (vector and scalar)
#define VSH(NAME, SCALAR, SG, ...) \
    SEM(NAME) { \
        const int rs = rshift_amt(w), ls = lshift_amt(w); \
        vshift<SG>(s, w, SCALAR, [rs, ls](auto a, auto d) { \
            using T = decltype(a); \
            (void)rs, (void)ls, (void)d; \
            return T(__VA_ARGS__); \
        }); \
        return 0; \
    }
#define VSH2(NAME, SG, ...) VSH(NAME, false, SG, __VA_ARGS__) VSH(NAME##_s, true, SG, __VA_ARGS__)
VSH2(SSHR, true, rshift(a, rs, false))
VSH2(USHR, false, rshift(a, rs, false))
VSH2(SRSHR, true, rshift(a, rs, true))
VSH2(URSHR, false, rshift(a, rs, true))
VSH2(SSRA, true, d + T(rshift(a, rs, false)))
VSH2(USRA, false, d + T(rshift(a, rs, false)))
VSH2(SRSRA, true, d + T(rshift(a, rs, true)))
VSH2(URSRA, false, d + T(rshift(a, rs, true)))
VSH2(SHL, false, i128(a) << ls)
VSH2(SQSHL_imm, true, sat128<T>(i128(a) << ls))
VSH2(UQSHL_imm, false, sat128<T>(i128(a) << ls))
VSH2(SQSHLU, true, sat128<Us_t<T>>(i128(a) << ls))
// insert: keep the destination bits the shift does not reach
VSH2(SRI, false, rs >= int(bits_v<T>) ? d : T((d & ~T(T(~T(0)) >> rs)) | T(a >> rs)))
VSH2(SLI, false, T((d & T(~T(T(~T(0)) << ls))) | T(a << ls)))

#define VSHN(NAME, SCALAR, SG, ...) \
    SEM(NAME) { \
        const int rs = rshift_amt(w); \
        vshift_narrow<SG>(s, w, SCALAR, [rs](auto a, auto t) { \
            using T = decltype(t); \
            (void)rs; \
            return T(__VA_ARGS__); \
        }); \
        return 0; \
    }
#define VSHN2(NAME, SG, ...) VSHN(NAME##_v, false, SG, __VA_ARGS__) VSHN(NAME##_s, true, SG, __VA_ARGS__)
VSHN(SHRN_v, false, false, rshift(a, rs, false))
VSHN(RSHRN_v, false, false, rshift(a, rs, true))
VSHN2(SQSHRN, true, sat128<T>(rshift(a, rs, false)))
VSHN2(SQRSHRN, true, sat128<T>(rshift(a, rs, true)))
VSHN2(UQSHRN, false, sat128<T>(rshift(a, rs, false)))
VSHN2(UQRSHRN, false, sat128<T>(rshift(a, rs, true)))
VSHN2(SQSHRUN, true, sat128<Us_t<T>>(rshift(a, rs, false)))
VSHN2(SQRSHRUN, true, sat128<Us_t<T>>(rshift(a, rs, true)))

template <bool Sg> NXW_INLINE void shll_imm(State* s, u32 w) {
    const int ls = lshift_amt(w);
    by_size<Sg>(hsz(w), [&](auto tag) {
        using T = decltype(tag);
        if constexpr (sizeof(T) < 8) {
            using W = Wider<T>;
            const unsigned n = 8 / sizeof(T), part = Qb(w) ? n : 0;
            const LV<T> a = vget<T>(s, Rn(w));
            LV<W> r{};
            for (unsigned i = 0; i < n; i++) r[i] = W(W(a[i + part]) << ls);
            vset<W>(s, Rd(w), r);
        }
    });
}
SEM(SSHLL) {
    shll_imm<true>(s, w);
    return 0;
}
SEM(USHLL) {
    shll_imm<false>(s, w);
    return 0;
}

// Fixed-point conversions by immediate: fbits = 2 * esize - immh:immb
SEM(SCVTF_fix_v) {
    cvtf_v<true>(s, w, rshift_amt(w), false);
    return 0;
}
SEM(UCVTF_fix_v) {
    cvtf_v<false>(s, w, rshift_amt(w), false);
    return 0;
}
SEM(FCVTZS_fix_v) {
    fcvt_v<true>(s, w, Rnd::Z, rshift_amt(w), false);
    return 0;
}
SEM(FCVTZU_fix_v) {
    fcvt_v<false>(s, w, Rnd::Z, rshift_amt(w), false);
    return 0;
}
// The scalar forms use the same immh:immb layout; sz is implied by immh (bit 22 is immh<3>)
SEM(SCVTF_fix_s) {
    cvtf_v<true>(s, w, rshift_amt(w), true);
    return 0;
}
SEM(UCVTF_fix_s) {
    cvtf_v<false>(s, w, rshift_amt(w), true);
    return 0;
}
SEM(FCVTZS_fix_s) {
    fcvt_v<true>(s, w, Rnd::Z, rshift_amt(w), true);
    return 0;
}
SEM(FCVTZU_fix_s) {
    fcvt_v<false>(s, w, Rnd::Z, rshift_amt(w), true);
    return 0;
}

// ================================================================ by element (integer)
template <bool Sg, class Fn> NXW_INLINE void velem(State* s, u32 w, bool scalar, Fn fn) {
    const Elem e = elem_int(w);
    by_size<Sg>(Size(w), [&](auto tag) {
        using T = decltype(tag);
        const bool q = scalar ? false : Qb(w);
        const unsigned n = scalar ? 1 : lanes(q, sizeof(T));
        const LV<T> a = vget<T>(s, Rn(w)), d = vget<T>(s, Rd(w));
        const T b = lane<T>(s, e.m, e.index);
        LV<T> r{};
        for (unsigned i = 0; i < n; i++) r[i] = T(fn(a[i], b, d[i]));
        if (scalar) vset_scalar<T>(s, Rd(w), r[0]);
        else vset<T>(s, Rd(w), r, q);
    });
}
template <bool Sg, class Fn> NXW_INLINE void velem_long(State* s, u32 w, bool scalar, Fn fn) {
    const Elem e = elem_int(w);
    by_size<Sg>(Size(w), [&](auto tag) {
        using T = decltype(tag);
        if constexpr (sizeof(T) < 8) {
            using W = Wider<T>;
            const unsigned n = scalar ? 1 : 8 / sizeof(T), part = (!scalar && Qb(w)) ? n : 0;
            const LV<T> a = vget<T>(s, Rn(w));
            const LV<W> d = vget<W>(s, Rd(w));
            const W b = W(lane<T>(s, e.m, e.index));
            LV<W> r{};
            for (unsigned i = 0; i < n; i++) r[i] = W(fn(W(a[i + part]), b, d[i]));
            if (scalar) vset_scalar<W>(s, Rd(w), r[0]);
            else vset<W>(s, Rd(w), r);
        }
    });
}
#define VE(NAME, SG, SCALAR, ...) \
    SEM(NAME) { \
        velem<SG>(s, w, SCALAR, [](auto a, auto b, auto d) { \
            using T = decltype(a); \
            (void)d; \
            return T(__VA_ARGS__); \
        }); \
        return 0; \
    }
#define VEL(NAME, SG, SCALAR, ...) \
    SEM(NAME) { \
        velem_long<SG>(s, w, SCALAR, [](auto a, auto b, auto d) { \
            using W = decltype(a); \
            (void)d; \
            return W(__VA_ARGS__); \
        }); \
        return 0; \
    }
VE(MUL_ve, false, false, a * b)
VE(MLA_ve, false, false, d + a * b)
VE(MLS_ve, false, false, d - a * b)
VE(SQDMULH_ve, true, false, sqdmulh(a, b, false))
VE(SQRDMULH_ve, true, false, sqdmulh(a, b, true))
VE(SQDMULH_si, true, true, sqdmulh(a, b, false))
VE(SQRDMULH_si, true, true, sqdmulh(a, b, true))
VEL(SMULL_ve, true, false, a * b)
VEL(UMULL_ve, false, false, a * b)
VEL(SMLAL_ve, true, false, d + a * b)
VEL(UMLAL_ve, false, false, d + a * b)
VEL(SMLSL_ve, true, false, d - a * b)
VEL(UMLSL_ve, false, false, d - a * b)
VEL(SQDMULL_ve, true, false, sat128<W>(2 * i128(a) * i128(b)))
VEL(SQDMLAL_ve, true, false, sat128<W>(i128(d) + sat128<W>(2 * i128(a) * i128(b))))
VEL(SQDMLSL_ve, true, false, sat128<W>(i128(d) - sat128<W>(2 * i128(a) * i128(b))))
VEL(SQDMULL_si, true, true, sat128<W>(2 * i128(a) * i128(b)))
VEL(SQDMLAL_si, true, true, sat128<W>(i128(d) + sat128<W>(2 * i128(a) * i128(b))))
VEL(SQDMLSL_si, true, true, sat128<W>(i128(d) - sat128<W>(2 * i128(a) * i128(b))))
// scalar long forms with register operands
#define SL(NAME, ...) \
    SEM(NAME) { \
        by_size<true>(Size(w), [&](auto tag) { \
            using T = decltype(tag); \
            if constexpr (sizeof(T) == 2 || sizeof(T) == 4) { \
                using W = Wider<T>; \
                const W a = lane<T>(s, Rn(w), 0), b = lane<T>(s, Rm(w), 0), d = lane<W>(s, Rd(w), 0); \
                (void)d; \
                vset_scalar<W>(s, Rd(w), W(__VA_ARGS__)); \
            } \
        }); \
        return 0; \
    }
SL(SQDMULL_s, sat128<W>(2 * i128(a) * i128(b)))
SL(SQDMLAL_s, sat128<W>(i128(d) + sat128<W>(2 * i128(a) * i128(b))))
SL(SQDMLSL_s, sat128<W>(i128(d) - sat128<W>(2 * i128(a) * i128(b))))

// ================================================================ FP vector
#define FV3(NAME, ...) \
    SEM(NAME) { \
        fv3(s, w, [](auto a, auto b, auto d) { \
            using T = decltype(a); \
            (void)d; \
            return T(__VA_ARGS__); \
        }); \
        return 0; \
    }
FV3(FADD_v, fp_binop(a, b, [](T x, T y) { return x + y; }))
FV3(FSUB_v, fp_binop(a, b, [](T x, T y) { return x - y; }))
FV3(FMUL_v, fp_binop(a, b, [](T x, T y) { return x * y; }))
FV3(FDIV_v, fp_binop(a, b, [](T x, T y) { return x / y; }))
FV3(FABD_v, from_bits<T>(bits_of(fp_binop(a, b, [](T x, T y) { return x - y; })) & ~FT<T>::sign))
FV3(FMLA_v, fp_muladd(d, a, b))
FV3(FMLS_v, fp_muladd(d, -a, b))
FV3(FMAX_v, fp_max(a, b))
FV3(FMIN_v, fp_min(a, b))
FV3(FMAXNM_v, fp_maxnum(a, b))
FV3(FMINNM_v, fp_minnum(a, b))
FV3(FMULX_v, mulx(a, b))
FV3(FRECPS_v, recip_step(a, b))
FV3(FRSQRTS_v, rsqrt_step(a, b))

#define FS3(NAME, ...) \
    SEM(NAME) { \
        fs3(s, w, [](auto a, auto b, auto d) { \
            using T = decltype(a); \
            (void)d; \
            return T(__VA_ARGS__); \
        }); \
        return 0; \
    }
FS3(FABD_s, from_bits<T>(bits_of(fp_binop(a, b, [](T x, T y) { return x - y; })) & ~FT<T>::sign))
FS3(FMULX_s, mulx(a, b))
FS3(FRECPS_s, recip_step(a, b))
FS3(FRSQRTS_s, rsqrt_step(a, b))

#define FVP(NAME, ...) \
    SEM(NAME) { \
        fvpair(s, w, [](auto a, auto b) { \
            using T = decltype(a); \
            return T(__VA_ARGS__); \
        }); \
        return 0; \
    }
FVP(FADDP_v, fp_binop(a, b, [](T x, T y) { return x + y; }))
FVP(FMAXP_v, fp_max(a, b))
FVP(FMINP_v, fp_min(a, b))
FVP(FMAXNMP_v, fp_maxnum(a, b))
FVP(FMINNMP_v, fp_minnum(a, b))
#define FSP(NAME, ...) \
    SEM(NAME) { \
        fspair(s, w, [](auto a, auto b) { \
            using T = decltype(a); \
            return T(__VA_ARGS__); \
        }); \
        return 0; \
    }
FSP(FADDP_s, fp_binop(a, b, [](T x, T y) { return x + y; }))
FSP(FMAXP_s, fp_max(a, b))
FSP(FMINP_s, fp_min(a, b))
FSP(FMAXNMP_s, fp_maxnum(a, b))
FSP(FMINNMP_s, fp_minnum(a, b))

#define FCMPV(NAME, ZERO, ...) \
    SEM(NAME) { \
        fvcmp(s, w, ZERO, [](auto a, auto b) { return bool(__VA_ARGS__); }); \
        return 0; \
    }
#define FCMPS(NAME, ZERO, ...) \
    SEM(NAME) { \
        fscmp(s, w, ZERO, [](auto a, auto b) { return bool(__VA_ARGS__); }); \
        return 0; \
    }
FCMPV(FCMEQ_v, false, a == b)
FCMPV(FCMGE_v, false, a >= b)
FCMPV(FCMGT_v, false, a > b)
FCMPV(FACGE_v, false, __builtin_elementwise_abs(a) >= __builtin_elementwise_abs(b))
FCMPV(FACGT_v, false, __builtin_elementwise_abs(a) > __builtin_elementwise_abs(b))
FCMPV(FCMEQ_zero_v, true, a == b)
FCMPV(FCMGE_zero_v, true, a >= b)
FCMPV(FCMGT_zero_v, true, a > b)
FCMPV(FCMLE_zero_v, true, a <= b)
FCMPV(FCMLT_zero_v, true, a < b)
FCMPS(FCMEQ_s, false, a == b)
FCMPS(FCMGE_s, false, a >= b)
FCMPS(FCMGT_s, false, a > b)
FCMPS(FACGE_s, false, __builtin_elementwise_abs(a) >= __builtin_elementwise_abs(b))
FCMPS(FACGT_s, false, __builtin_elementwise_abs(a) > __builtin_elementwise_abs(b))
FCMPS(FCMEQ_zero_s, true, a == b)
FCMPS(FCMGE_zero_s, true, a >= b)
FCMPS(FCMGT_zero_s, true, a > b)
FCMPS(FCMLE_zero_s, true, a <= b)
FCMPS(FCMLT_zero_s, true, a < b)

#define FV1(NAME, ...) \
    SEM(NAME) { \
        fv1(s, w, [](auto a) { \
            using T = decltype(a); \
            return T(__VA_ARGS__); \
        }); \
        return 0; \
    }
#define FS1(NAME, ...) \
    SEM(NAME) { \
        fs1(s, w, [](auto a) { \
            using T = decltype(a); \
            return T(__VA_ARGS__); \
        }); \
        return 0; \
    }
FV1(FABS_v, from_bits<T>(bits_of(a) & ~FT<T>::sign))
FV1(FNEG_v, from_bits<T>(bits_of(a) ^ FT<T>::sign))
FV1(FSQRT_v, fp_unop_nan(a, __builtin_elementwise_sqrt(a)))
FV1(FRINTN_v, fp_unop_nan(a, round_to(a, Rnd::N)))
FV1(FRINTP_v, fp_unop_nan(a, round_to(a, Rnd::P)))
FV1(FRINTM_v, fp_unop_nan(a, round_to(a, Rnd::M)))
FV1(FRINTZ_v, fp_unop_nan(a, round_to(a, Rnd::Z)))
FV1(FRINTA_v, fp_unop_nan(a, round_to(a, Rnd::A)))
FV1(FRINTX_v, fp_unop_nan(a, round_to(a, Rnd::N)))
FV1(FRINTI_v, fp_unop_nan(a, round_to(a, Rnd::N)))
FV1(FRINT32Z_v, frint_int(a, Rnd::Z, 32))
FV1(FRINT32X_v, frint_int(a, Rnd::N, 32))
FV1(FRINT64Z_v, frint_int(a, Rnd::Z, 64))
FV1(FRINT64X_v, frint_int(a, Rnd::N, 64))
FV1(FRECPE_v, recip_estimate(a))
FV1(FRSQRTE_v, rsqrt_estimate(a))
FS1(FRECPE_s, recip_estimate(a))
FS1(FRSQRTE_s, rsqrt_estimate(a))
FS1(FRECPX_s, recpx(a))

#define FCVTV(NAME, SIGNED, RND) \
    SEM(NAME##_v) { \
        fcvt_v<SIGNED>(s, w, RND, 0, false); \
        return 0; \
    } \
    SEM(NAME##_s) { \
        fcvt_v<SIGNED>(s, w, RND, 0, true); \
        return 0; \
    }
FCVTV(FCVTNS, true, Rnd::N)
FCVTV(FCVTNU, false, Rnd::N)
FCVTV(FCVTPS, true, Rnd::P)
FCVTV(FCVTPU, false, Rnd::P)
FCVTV(FCVTMS, true, Rnd::M)
FCVTV(FCVTMU, false, Rnd::M)
FCVTV(FCVTZS, true, Rnd::Z)
FCVTV(FCVTZU, false, Rnd::Z)
FCVTV(FCVTAS, true, Rnd::A)
FCVTV(FCVTAU, false, Rnd::A)
SEM(SCVTF_v) {
    cvtf_v<true>(s, w, 0, false);
    return 0;
}
SEM(UCVTF_v) {
    cvtf_v<false>(s, w, 0, false);
    return 0;
}
SEM(SCVTF_s) {
    cvtf_v<true>(s, w, 0, true);
    return 0;
}
SEM(UCVTF_s) {
    cvtf_v<false>(s, w, 0, true);
    return 0;
}

// Unsigned estimates (32-bit lanes)
SEM(URECPE) {
    const bool q = Qb(w);
    const LV<u32> a = vget<u32>(s, Rn(w));
    LV<u32> r{};
    for (unsigned i = 0; i < lanes(q, 4); i++)
        r[i] = !(a[i] >> 31) ? ~0u : 0x80000000u | (recip_estimate_int(a[i] >> 23) & 0xff) << 23;
    vset<u32>(s, Rd(w), r, q);
    return 0;
}
SEM(URSQRTE) {
    const bool q = Qb(w);
    const LV<u32> a = vget<u32>(s, Rn(w));
    LV<u32> r{};
    for (unsigned i = 0; i < lanes(q, 4); i++) r[i] = !(a[i] >> 30) ? ~0u : (rsqrt_estimate_int(a[i] >> 23) & 0x1ff) << 23;
    vset<u32>(s, Rd(w), r, q);
    return 0;
}

// Widening and narrowing FP conversions (sz 0: half <-> single, 1: single <-> double)
SEM(FCVTL) {
    const bool q = Qb(w);
    if (w >> 22 & 1) {
        const LV<float> a = vget<float>(s, Rn(w));
        LV<double> r{double(a[q ? 2 : 0]), double(a[q ? 3 : 1])};
        vset<double>(s, Rd(w), r);
    } else {
        const LV<u16> a = vget<u16>(s, Rn(w));
        LV<float> r{};
        for (unsigned i = 0; i < 4; i++) r[i] = half_to_float(a[i + (q ? 4 : 0)]);
        vset<float>(s, Rd(w), r);
    }
    return 0;
}
SEM(FCVTN) {
    const bool q = Qb(w);
    if (w >> 22 & 1) {
        const LV<double> a = vget<double>(s, Rn(w));
        const float v[2] = {float(a[0]), float(a[1])};
        set_narrow<float>(s, Rd(w), v, 2, q);
    } else {
        const LV<float> a = vget<float>(s, Rn(w));
        u16 v[4];
        for (unsigned i = 0; i < 4; i++) v[i] = float_to_half(a[i]);
        set_narrow<u16>(s, Rd(w), v, 4, q);
    }
    return 0;
}
SEM(FCVTXN) {
    const LV<double> a = vget<double>(s, Rn(w));
    const float v[2] = {cvt_round_odd(a[0]), cvt_round_odd(a[1])};
    set_narrow<float>(s, Rd(w), v, 2, Qb(w));
    return 0;
}
SEM(FCVTXN_s) {
    vset_scalar<float>(s, Rd(w), cvt_round_odd(lane<double>(s, Rn(w), 0)));
    return 0;
}

// FP by element
template <class Fn> NXW_INLINE void fvelem(State* s, u32 w, bool scalar, Fn fn) {
    const Elem e = elem_fp(w);
    by_fsz(w >> 22 & 1, [&](auto tag) {
        using T = decltype(tag);
        const bool q = scalar ? false : Qb(w);
        const unsigned n = scalar ? 1 : lanes(q, sizeof(T));
        const LV<T> a = vget<T>(s, Rn(w)), d = vget<T>(s, Rd(w));
        const T b = lane<T>(s, e.m, e.index);
        LV<T> r{};
        for (unsigned i = 0; i < n; i++) r[i] = fn(a[i], b, d[i]);
        if (scalar) vset_scalar<T>(s, Rd(w), r[0]);
        else vset<T>(s, Rd(w), r, q);
    });
}
#define FVE(NAME, SCALAR, ...) \
    SEM(NAME) { \
        fvelem(s, w, SCALAR, [](auto a, auto b, auto d) { \
            using T = decltype(a); \
            (void)d; \
            return T(__VA_ARGS__); \
        }); \
        return 0; \
    }
FVE(FMLA_ve, false, fp_muladd(d, a, b))
FVE(FMLS_ve, false, fp_muladd(d, -a, b))
FVE(FMUL_ve, false, fp_binop(a, b, [](T x, T y) { return x * y; }))
FVE(FMULX_ve, false, mulx(a, b))
FVE(FMLA_si, true, fp_muladd(d, a, b))
FVE(FMLS_si, true, fp_muladd(d, -a, b))
FVE(FMUL_si, true, fp_binop(a, b, [](T x, T y) { return x * y; }))
FVE(FMULX_si, true, mulx(a, b))

// ================================================================ cryptography
namespace {

constexpr u8 gf_mul(u8 a, u8 b) {
    u8 r = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) r ^= a;
        const bool hi = a & 0x80;
        a = u8(a << 1);
        if (hi) a ^= 0x1b;
        b >>= 1;
    }
    return r;
}
constexpr u8 gf_inv(u8 x) {   // x^254
    u8 r = 1, p = x;
    for (int e = 254; e; e >>= 1, p = gf_mul(p, p))
        if (e & 1) r = gf_mul(r, p);
    return x ? r : 0;
}
constexpr std::array<u8, 256> make_sbox(bool inverse) {
    std::array<u8, 256> fwd{}, inv{};
    for (int x = 0; x < 256; x++) {
        const u8 b = gf_inv(u8(x));
        const u8 sv = u8(b ^ u8(b << 1 | b >> 7) ^ u8(b << 2 | b >> 6) ^ u8(b << 3 | b >> 5) ^ u8(b << 4 | b >> 4) ^ 0x63);
        fwd[size_t(x)] = sv;
        inv[sv] = u8(x);
    }
    return inverse ? inv : fwd;
}
constexpr std::array<u8, 256> sbox = make_sbox(false), inv_sbox = make_sbox(true);

NXW_INLINE LV<u8> aes_round(LV<u8> st, bool decrypt) {
    LV<u8> r{};
    for (unsigned c = 0; c < 4; c++)
        for (unsigned row = 0; row < 4; row++) {
            const unsigned src = decrypt ? (c + 4 - row) % 4 : (c + row) % 4;
            const u8 b = st[row + 4 * src];
            r[row + 4 * c] = decrypt ? inv_sbox[b] : sbox[b];
        }
    return r;
}
NXW_INLINE LV<u8> mix_columns(LV<u8> st, bool inverse) {
    LV<u8> r{};
    for (unsigned c = 0; c < 4; c++) {
        const u8 a0 = st[4 * c], a1 = st[4 * c + 1], a2 = st[4 * c + 2], a3 = st[4 * c + 3];
        if (!inverse) {
            r[4 * c] = u8(gf_mul(a0, 2) ^ gf_mul(a1, 3) ^ a2 ^ a3);
            r[4 * c + 1] = u8(a0 ^ gf_mul(a1, 2) ^ gf_mul(a2, 3) ^ a3);
            r[4 * c + 2] = u8(a0 ^ a1 ^ gf_mul(a2, 2) ^ gf_mul(a3, 3));
            r[4 * c + 3] = u8(gf_mul(a0, 3) ^ a1 ^ a2 ^ gf_mul(a3, 2));
        } else {
            r[4 * c] = u8(gf_mul(a0, 14) ^ gf_mul(a1, 11) ^ gf_mul(a2, 13) ^ gf_mul(a3, 9));
            r[4 * c + 1] = u8(gf_mul(a0, 9) ^ gf_mul(a1, 14) ^ gf_mul(a2, 11) ^ gf_mul(a3, 13));
            r[4 * c + 2] = u8(gf_mul(a0, 13) ^ gf_mul(a1, 9) ^ gf_mul(a2, 14) ^ gf_mul(a3, 11));
            r[4 * c + 3] = u8(gf_mul(a0, 11) ^ gf_mul(a1, 13) ^ gf_mul(a2, 9) ^ gf_mul(a3, 14));
        }
    }
    return r;
}

NXW_INLINE u32 rol32(u32 v, int r) { return std::rotl(v, r); }
NXW_INLINE u32 ror32(u32 v, int r) { return std::rotr(v, r); }
NXW_INLINE u32 elt(u128 v, unsigned e) { return u32(v >> (32 * e)); }
NXW_INLINE u128 vq(const State* s, unsigned n) { return std::bit_cast<u128>(s->q[n]); }
NXW_INLINE void set_vq(State* s, unsigned n, u128 v) { s->q[n] = std::bit_cast<V128>(v); }

template <int Kind> NXW_INLINE void sha1_hash(State* s, u32 w) {   // 0 choose, 1 parity, 2 majority
    u128 x = vq(s, Rd(w));
    u32 y = lane<u32>(s, Rn(w), 0);
    const u128 wk = vq(s, Rm(w));
    for (unsigned e = 0; e < 4; e++) {
        const u32 b = elt(x, 1), c = elt(x, 2), d = elt(x, 3);
        u32 t;
        if (Kind == 0) t = (b & c) | (~b & d);
        else if (Kind == 1) t = b ^ c ^ d;
        else t = (b & c) | (b & d) | (c & d);
        y = y + rol32(elt(x, 0), 5) + t + elt(wk, e);
        x = (x & ~(u128(0xffffffffu) << 32)) | u128(rol32(b, 30)) << 32;
        const u32 top = elt(x, 3);
        x = x << 32 | y;
        y = top;
    }
    set_vq(s, Rd(w), x);
}

NXW_INLINE u32 sigma0(u32 x) { return ror32(x, 2) ^ ror32(x, 13) ^ ror32(x, 22); }
NXW_INLINE u32 sigma1(u32 x) { return ror32(x, 6) ^ ror32(x, 11) ^ ror32(x, 25); }
NXW_INLINE u128 sha256_hash(u128 x, u128 y, u128 wk, bool part1) {
    for (unsigned e = 0; e < 4; e++) {
        const u32 chs = (elt(y, 0) & elt(y, 1)) | (~elt(y, 0) & elt(y, 2));
        const u32 maj = (elt(x, 0) & elt(x, 1)) | (elt(x, 0) & elt(x, 2)) | (elt(x, 1) & elt(x, 2));
        const u32 t = elt(y, 3) + sigma1(elt(y, 0)) + chs + elt(wk, e);
        const u32 x3 = t + elt(x, 3);
        const u32 y3 = t + sigma0(elt(x, 0)) + maj;
        x = (x & ~(u128(0xffffffffu) << 96)) | u128(x3) << 96;
        y = (y & ~(u128(0xffffffffu) << 96)) | u128(y3) << 96;
        const u128 nx = x << 32 | (y >> 96), ny = y << 32 | (x >> 96);
        x = nx, y = ny;
    }
    return part1 ? x : y;
}

}  // namespace

NXW_INLINE LV<u8> xor16(LV<u8> a, const LV<u8>& b) {
    for (unsigned i = 0; i < 16; i++) a[i] ^= b[i];
    return a;
}
SEM(AESE) {   // SubBytes(ShiftRows(Vd ^ Vn))
    s->q[Rd(w)] = std::bit_cast<V128>(aes_round(xor16(vget<u8>(s, Rd(w)), vget<u8>(s, Rn(w))), false));
    return 0;
}
SEM(AESD) {   // InvSubBytes(InvShiftRows(Vd ^ Vn))
    s->q[Rd(w)] = std::bit_cast<V128>(aes_round(xor16(vget<u8>(s, Rd(w)), vget<u8>(s, Rn(w))), true));
    return 0;
}
SEM(AESMC) {
    s->q[Rd(w)] = std::bit_cast<V128>(mix_columns(vget<u8>(s, Rn(w)), false));
    return 0;
}
SEM(AESIMC) {
    s->q[Rd(w)] = std::bit_cast<V128>(mix_columns(vget<u8>(s, Rn(w)), true));
    return 0;
}
SEM(SHA1C) {
    sha1_hash<0>(s, w);
    return 0;
}
SEM(SHA1P) {
    sha1_hash<1>(s, w);
    return 0;
}
SEM(SHA1M) {
    sha1_hash<2>(s, w);
    return 0;
}
SEM(SHA1H) {
    vset_scalar<u32>(s, Rd(w), rol32(lane<u32>(s, Rn(w), 0), 30));
    return 0;
}
SEM(SHA1SU0) {
    const u128 d = vq(s, Rd(w)), n = vq(s, Rn(w)), m = vq(s, Rm(w));
    set_vq(s, Rd(w), ((n << 64) | (d >> 64)) ^ d ^ m);
    return 0;
}
SEM(SHA1SU1) {
    const u128 t = vq(s, Rd(w)) ^ (vq(s, Rn(w)) >> 32);
    const u32 r0 = rol32(elt(t, 0), 1), r1 = rol32(elt(t, 1), 1), r2 = rol32(elt(t, 2), 1);
    const u32 r3 = rol32(elt(t, 3), 1) ^ rol32(elt(t, 0), 2);
    set_vq(s, Rd(w), u128(r3) << 96 | u128(r2) << 64 | u128(r1) << 32 | r0);
    return 0;
}
SEM(SHA256H) {
    set_vq(s, Rd(w), sha256_hash(vq(s, Rd(w)), vq(s, Rn(w)), vq(s, Rm(w)), true));
    return 0;
}
SEM(SHA256H2) {
    set_vq(s, Rd(w), sha256_hash(vq(s, Rn(w)), vq(s, Rd(w)), vq(s, Rm(w)), false));
    return 0;
}
SEM(SHA256SU0) {
    const u128 d = vq(s, Rd(w)), n = vq(s, Rn(w));
    const u128 t = (n << 96) | (d >> 32);
    u128 r = 0;
    for (unsigned e = 0; e < 4; e++) {
        const u32 x = elt(t, e);
        r |= u128(u32((ror32(x, 7) ^ ror32(x, 18) ^ (x >> 3)) + elt(d, e))) << (32 * e);
    }
    set_vq(s, Rd(w), r);
    return 0;
}
SEM(SHA256SU1) {
    const u128 d = vq(s, Rd(w)), n = vq(s, Rn(w)), m = vq(s, Rm(w));
    const u128 t0 = (m << 96) | (n >> 32);
    u32 r[4];
    for (unsigned e = 0; e < 4; e++) {
        const u32 x = e < 2 ? elt(m, 2 + e) : r[e - 2];
        r[e] = (ror32(x, 17) ^ ror32(x, 19) ^ (x >> 10)) + elt(d, e) + elt(t0, e);
    }
    set_vq(s, Rd(w), u128(r[3]) << 96 | u128(r[2]) << 64 | u128(r[1]) << 32 | r[0]);
    return 0;
}

// ---------------------------------------------------------------- dot product (FEAT_DotProd): each 32-bit lane accumulates the four byte products of its group
template <bool Sg> NXW_INLINE void dotprod(State* s, u32 w, bool by_elem) {
    using B = std::conditional_t<Sg, i8, u8>;
    using A = std::conditional_t<Sg, i32, u32>;
    const bool q = Qb(w);
    const unsigned n = q ? 4 : 2;
    const LV<B> a = vget<B>(s, Rn(w));
    LV<B> b;
    unsigned rm;
    unsigned idx = 0;
    if (by_elem) {   // the Rm group (32-bit element index H:L) of the second operand
        rm = (w >> 16 & 15) | (w >> 20 & 1) << 4;
        idx = (w >> 11 & 1) << 1 | (w >> 21 & 1);
        b = vget<B>(s, rm);
    } else {
        rm = Rm(w);
        b = vget<B>(s, rm);
    }
    LV<A> d = vget<A>(s, Rd(w));
    for (unsigned i = 0; i < n; i++) {
        A acc = d[i];
        const unsigned bi = by_elem ? idx * 4 : i * 4;
        for (unsigned j = 0; j < 4; j++) acc += A(A(a[i * 4 + j]) * A(b[bi + j]));
        d[i] = acc;
    }
    vset<A>(s, Rd(w), d, q);
}
SEM(UDOT) {
    dotprod<false>(s, w, false);
    return 0;
}
SEM(SDOT) {
    dotprod<true>(s, w, false);
    return 0;
}
SEM(UDOT_ve) {
    dotprod<false>(s, w, true);
    return 0;
}
SEM(SDOT_ve) {
    dotprod<true>(s, w, true);
    return 0;
}
