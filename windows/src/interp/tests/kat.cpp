// Known answers for the semantics, through the interpreter: values that follow from the Arm ARM's definitions (FRECPE and
// FRSQRTE tables, NaN, zero and saturation rules, half-precision rounding, flags), standard check values (CRC-32 and CRC-32C
// of "123456789", SHA-1 and SHA-256 of "abc", the AES round of FIPS-197 Appendix B). Encodings come from llvm-mc 23.1.4.
// No game data needed. The lifter runs the same semantics; nxlift-tests checks that lifted and interpreted code agree.
#include "nxw/interp.hpp"
#include "nxw/rt.h"
#include "nxw/sem.hpp"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
const uint32_t* rt_code_table = nullptr;
uint64_t rt_code_base = 0, rt_code_words = 0, rt_host_base = 0;
void rt_svc(NxwGuestCpu*, uint32_t) { std::abort(); }
uint64_t rt_mrs(NxwGuestCpu*, uint32_t) { std::abort(); }
void rt_msr(NxwGuestCpu*, uint32_t, uint64_t) { std::abort(); }
void rt_trap(NxwGuestCpu*, uint64_t pc, uint32_t why) {
    std::printf("trap at %llx, %x\n", static_cast<unsigned long long>(pc), why);
    std::exit(1);
}
NxwGuestFn rt_miss(NxwGuestCpu*, uint64_t) { std::abort(); }
void rt_yield(NxwGuestCpu*, uint32_t, uint64_t) {}

#define SEMFN(N) uint64_t nxw_sem_##N(nxw::sem::State*, uint32_t, uint64_t);
SEMFN(SHA256H) SEMFN(SHA256H2) SEMFN(SHA256SU0) SEMFN(SHA256SU1) SEMFN(SHA1H) SEMFN(SHA1C) SEMFN(SHA1P) SEMFN(SHA1M)
SEMFN(SHA1SU0) SEMFN(SHA1SU1)
}

namespace {

int failures = 0, checks = 0;
void check(bool ok, const std::string& what) {
    checks++;
    if (!ok) failures++, std::printf("  FAIL %s\n", what.c_str());
}
std::string hx(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%llx", static_cast<unsigned long long>(v));
    return b;
}
#define EQ(a, b, what) check(uint64_t(a) == uint64_t(b), std::string(what) + ": " + hx(uint64_t(a)) + " != " + hx(uint64_t(b)))

alignas(16) uint32_t code[8];
alignas(16) uint8_t mem[256];

struct Cpu : NxwGuestCpu {
    Cpu() { std::memset(static_cast<NxwGuestCpu*>(this), 0, sizeof(NxwGuestCpu)), excl_addr = ~0ull; }
    void run(std::initializer_list<uint32_t> ws) {
        size_t i = 0;
        for (uint32_t w : ws) code[i++] = w;
        code[i] = 0xd65f03c0;   // ret
        nxw_interp_run(this, reinterpret_cast<uint64_t>(code));
    }
    void s(int n, float v) { std::memset(q[n], 0, 16), std::memcpy(q[n], &v, 4); }
    void su(int n, uint32_t v) { std::memset(q[n], 0, 16), std::memcpy(q[n], &v, 4); }
    void d(int n, double v) { std::memset(q[n], 0, 16), std::memcpy(q[n], &v, 8); }
    void du(int n, uint64_t v) { std::memset(q[n], 0, 16), std::memcpy(q[n], &v, 8); }
    void hu(int n, uint16_t v) { std::memset(q[n], 0, 16), std::memcpy(q[n], &v, 2); }
    template <class T> void lanes(int n, std::initializer_list<T> l) {
        std::memset(q[n], 0, 16);
        size_t i = 0;
        for (T v : l) std::memcpy(q[n] + sizeof(T) * i++, &v, sizeof(T));
    }
    template <class T> T lane(int n, int i) const {
        T v;
        std::memcpy(&v, q[n] + sizeof(T) * i, sizeof(T));
        return v;
    }
    uint32_t s32(int n) const { return lane<uint32_t>(n, 0); }
    uint64_t hi64(int n) const { return lane<uint64_t>(n, 1); }
};
uint32_t fb(float f) { return std::bit_cast<uint32_t>(f); }
float fu(uint32_t u) { return std::bit_cast<float>(u); }

// One-operand FP instruction on s1 -> s0 (or d)
uint32_t f1(uint32_t word, uint32_t in) {
    Cpu c;
    c.su(1, in);
    c.run({word});
    check(c.hi64(0) == 0 && c.lane<uint32_t>(0, 1) == 0, "scalar result clears the rest of the register");
    return c.s32(0);
}
uint32_t f2(uint32_t word, uint32_t a, uint32_t b) {
    Cpu c;
    c.su(1, a), c.su(2, b);
    c.run({word});
    return c.s32(0);
}
uint64_t fcvt_w(uint32_t word, float v) {
    Cpu c;
    c.x[0] = ~0ull;
    c.s(1, v);
    c.run({word});
    return c.x[0];
}

void fp_estimates() {
    const uint32_t frecpe = 0x5ea1d820, frsqrte = 0x7ea1d820;
    EQ(f1(frecpe, fb(1.0f)), 0x3f7f8000, "frecpe 1");
    EQ(f1(frecpe, fb(2.0f)), 0x3eff8000, "frecpe 2");
    EQ(f1(frecpe, fb(0.5f)), 0x3fff8000, "frecpe 0.5");
    EQ(f1(frecpe, fb(3.0f)), 0x3eaa8000, "frecpe 3");
    EQ(f1(frecpe, fb(-2.0f)), 0xbeff8000, "frecpe -2");
    EQ(f1(frecpe, 0x00000000), 0x7f800000, "frecpe +0");
    EQ(f1(frecpe, 0x80000000), 0xff800000, "frecpe -0");
    EQ(f1(frecpe, 0x7f800000), 0x00000000, "frecpe +inf");
    EQ(f1(frecpe, 0xff800000), 0x80000000, "frecpe -inf");
    EQ(f1(frecpe, 0x7fc00001), 0x7fc00001, "frecpe qnan");
    EQ(f1(frecpe, 0x7f800001), 0x7fc00001, "frecpe snan quieted");
    EQ(f1(frecpe, 0x00080000), 0x7f800000, "frecpe 2^-130 overflows");
    EQ(f1(frecpe, 0x00200000), 0x7f7f8000, "frecpe 2^-128");
    EQ(f1(frecpe, 0x00400000), 0x7eff8000, "frecpe 2^-127");
    EQ(f1(frecpe, 0x7e800000), 0x007fc000, "frecpe 2^126 gives a denormal");
    EQ(f1(frsqrte, fb(1.0f)), 0x3f7f8000, "frsqrte 1");
    EQ(f1(frsqrte, fb(2.0f)), 0x3f348000, "frsqrte 2");
    EQ(f1(frsqrte, fb(4.0f)), 0x3eff8000, "frsqrte 4");
    EQ(f1(frsqrte, fb(0.25f)), 0x3fff8000, "frsqrte 0.25");
    EQ(f1(frsqrte, 0x00000001), 0x64b48000, "frsqrte smallest denormal");
    EQ(f1(frsqrte, fb(-1.0f)), 0x7fc00000, "frsqrte -1");
    EQ(f1(frsqrte, 0xff800000), 0x7fc00000, "frsqrte -inf");
    EQ(f1(frsqrte, 0x00000000), 0x7f800000, "frsqrte +0");
    EQ(f1(frsqrte, 0x80000000), 0xff800000, "frsqrte -0");
    EQ(f1(frsqrte, 0x7f800000), 0x00000000, "frsqrte +inf");
    {
        Cpu c;
        c.lanes<float>(1, {1.0f, 2.0f, 3.0f, 0.5f});
        c.run({0x4ea1d820});   // frecpe v0.4s
        EQ(c.lane<uint32_t>(0, 0), 0x3f7f8000, "frecpe.4s 0");
        EQ(c.lane<uint32_t>(0, 1), 0x3eff8000, "frecpe.4s 1");
        EQ(c.lane<uint32_t>(0, 2), 0x3eaa8000, "frecpe.4s 2");
        EQ(c.lane<uint32_t>(0, 3), 0x3fff8000, "frecpe.4s 3");
        c.lanes<double>(1, {1.0, 4.0});
        c.run({0x6ee1d820});   // frsqrte v0.2d
        EQ(c.lane<uint64_t>(0, 0), 0x3feff00000000000ull, "frsqrte.2d 1");
        EQ(c.lane<uint64_t>(0, 1), 0x3fdff00000000000ull, "frsqrte.2d 4");
    }
    const uint32_t frecps = 0x5e22fc20, frsqrts = 0x5ea2fc20;
    EQ(f2(frecps, 0x7f800000, 0), fb(2.0f), "frecps inf*0");
    EQ(f2(frecps, 0xff800000, 0), fb(2.0f), "frecps -inf*0");
    EQ(f2(frecps, fb(2.0f), fb(0.5f)), fb(1.0f), "frecps 2, 0.5");
    EQ(f2(frecps, fb(1.0f), fb(3.0f)), fb(-1.0f), "frecps 1, 3");
    EQ(f2(frsqrts, 0x7f800000, 0), fb(1.5f), "frsqrts inf*0");
    EQ(f2(frsqrts, fb(1.0f), fb(1.0f)), fb(1.0f), "frsqrts 1, 1");
    EQ(f2(frsqrts, fb(3.0f), fb(1.0f)), 0, "frsqrts 3, 1");
    // (3 - 2^127 * 2) / 2 = -2^127 + 1.5: the sum overflows, its half does not (one rounding)
    EQ(f2(frsqrts, 0x7f000000, fb(2.0f)), 0xff000000, "frsqrts rounds the halved sum once");
}

void fp_minmax_convert() {
    const uint32_t fmin = 0x1e225820, fmax = 0x1e224820, fminnm = 0x1e227820, fmaxnm = 0x1e226820;
    EQ(f2(fmin, 0, 0x80000000), 0x80000000, "fmin +0 -0");
    EQ(f2(fmax, 0x80000000, 0), 0, "fmax -0 +0");
    EQ(f2(fmin, 0x7fc00001, fb(1.0f)), 0x7fc00001, "fmin qnan");
    EQ(f2(fmax, fb(1.0f), 0x7fc00002), 0x7fc00002, "fmax qnan second");
    EQ(f2(fmin, 0x7fc00001, 0x7f800002), 0x7fc00002, "fmin: the signalling NaN wins");
    EQ(f2(fminnm, 0x7fc00001, fb(1.0f)), fb(1.0f), "fminnm ignores a quiet NaN");
    EQ(f2(fmaxnm, fb(1.0f), 0x7fc00001), fb(1.0f), "fmaxnm ignores a quiet NaN");
    EQ(f2(fminnm, 0x7f800001, fb(1.0f)), 0x7fc00001, "fminnm: a signalling NaN propagates");
    const uint32_t fcvtzs = 0x1e380020, fcvtzu = 0x1e390020, fcvtns = 0x1e200020, fcvtas = 0x1e240020, fcvtms = 0x1e300020,
                   fcvtps = 0x1e280020;
    EQ(fcvt_w(fcvtzs, 3.9f), 3, "fcvtzs 3.9");
    EQ(fcvt_w(fcvtzs, -3.9f), 0xfffffffd, "fcvtzs -3.9 (w, zero-extended)");
    EQ(fcvt_w(fcvtzs, 1e10f), 0x7fffffff, "fcvtzs saturates up");
    EQ(fcvt_w(fcvtzs, -1e10f), 0x80000000, "fcvtzs saturates down");
    EQ(fcvt_w(fcvtzs, NAN), 0, "fcvtzs NaN");
    EQ(fcvt_w(fcvtzs, 2147483520.0f), 0x7fffff80, "fcvtzs largest below 2^31");
    EQ(fcvt_w(fcvtzs, 2147483648.0f), 0x7fffffff, "fcvtzs 2^31");
    EQ(fcvt_w(fcvtzu, -1.0f), 0, "fcvtzu -1");
    EQ(fcvt_w(fcvtzu, 4294967040.0f), 0xffffff00, "fcvtzu large");
    EQ(fcvt_w(fcvtzu, 5e9f), 0xffffffff, "fcvtzu saturates");
    EQ(fcvt_w(fcvtns, 2.5f), 2, "fcvtns 2.5");
    EQ(fcvt_w(fcvtns, 3.5f), 4, "fcvtns 3.5");
    EQ(fcvt_w(fcvtns, -2.5f), 0xfffffffe, "fcvtns -2.5");
    EQ(fcvt_w(fcvtas, 2.5f), 3, "fcvtas 2.5");
    EQ(fcvt_w(fcvtas, -2.5f), 0xfffffffd, "fcvtas -2.5");
    EQ(fcvt_w(fcvtms, -2.5f), 0xfffffffd, "fcvtms -2.5");
    EQ(fcvt_w(fcvtps, -2.5f), 0xfffffffe, "fcvtps -2.5");
    EQ(fcvt_w(fcvtps, 2.1f), 3, "fcvtps 2.1");
    {
        Cpu c;
        c.d(1, 9.3e18);
        c.run({0x9e780020});
        EQ(c.x[0], 0x7fffffffffffffffull, "fcvtzs x 9.3e18");
        c.d(1, -9223372036854775808.0);
        c.run({0x9e780020});
        EQ(c.x[0], 0x8000000000000000ull, "fcvtzs x -2^63");
    }
    // half precision
    const uint32_t s2h = 0x1e23c020, h2s = 0x1ee24020, d2h = 0x1e63c020;
    auto h = [&](float v) { return f1(s2h, fb(v)) & 0xffff; };
    EQ(h(1.0f), 0x3c00, "fcvt h 1");
    EQ(h(65504.0f), 0x7bff, "fcvt h max");
    EQ(h(65519.0f), 0x7bff, "fcvt h below the tie");
    EQ(h(65520.0f), 0x7c00, "fcvt h tie to infinity");
    EQ(h(fu(0x33800000)), 0x0001, "fcvt h 2^-24");
    EQ(h(fu(0x33000000)), 0x0000, "fcvt h 2^-25 ties to even");
    EQ(h(fu(0x33c00000)), 0x0002, "fcvt h 1.5 * 2^-24 ties to even");
    EQ(h(-0.0f), 0x8000, "fcvt h -0");
    EQ(h(fu(0x3f801000)), 0x3c00, "fcvt h 1 + 2^-11 ties to even");
    EQ(h(fu(0x3f803000)), 0x3c02, "fcvt h 1 + 3 * 2^-11 ties to even");
    EQ(h(fu(0x7fc00000)), 0x7e00, "fcvt h NaN");
    EQ(h(fu(0x7f800001)), 0x7e00, "fcvt h signalling NaN quieted");
    {
        Cpu c;
        c.hu(1, 0x0001);
        c.run({h2s});
        EQ(c.s32(0), 0x33800000, "fcvt s from the smallest half denormal");
        c.hu(1, 0x7c01);
        c.run({h2s});
        EQ(c.s32(0), 0x7fc02000, "fcvt s from a signalling half NaN");
        c.hu(1, 0xfc00);
        c.run({h2s});
        EQ(c.s32(0), 0xff800000, "fcvt s from -inf");
        c.d(1, 1.0 + std::ldexp(1.0, -11) + std::ldexp(1.0, -40));
        c.run({d2h});
        EQ(c.s32(0), 0x3c01, "fcvt h from double rounds once (no double rounding)");
    }
    {
        Cpu c;
        c.run({0x1e2e1000});   // fmov s0, #1.0
        EQ(c.s32(0), 0x3f800000, "fmov s #1.0");
        c.run({0x1e701000});   // fmov d0, #-2.0
        EQ(c.lane<uint64_t>(0, 0), 0xc000000000000000ull, "fmov d #-2.0");
        c.su(1, 0x3f800001), c.su(2, 0x3f7fffff), c.s(3, -1.0f);
        c.run({0x1f020c20});   // fmadd s0, s1, s2, s3
        EQ(c.s32(0), 0x337ffffe, "fmadd rounds once");
        c.d(1, 2.0), c.d(2, 3.0);
        c.run({0x1e628820});   // fnmul d0, d1, d2
        EQ(c.lane<uint64_t>(0, 0), 0xc018000000000000ull, "fnmul");
        const auto fcmp = [&](float a, float b) {
            c.s(1, a), c.s(2, b), c.nzcv = 0;
            c.run({0x1e222020});
            return c.nzcv;
        };
        EQ(fcmp(1, 2), 0x80000000, "fcmp less");
        EQ(fcmp(2, 1), 0x20000000, "fcmp greater");
        EQ(fcmp(1, 1), 0x60000000, "fcmp equal");
        EQ(fcmp(NAN, 1), 0x30000000, "fcmp unordered");
    }
}

void integer() {
    Cpu c;
    c.x[1] = 0x7fffffff, c.x[2] = 1;
    c.run({0x2b020020});   // adds w0, w1, w2
    EQ(c.x[0], 0x80000000, "adds w overflow result");
    EQ(c.nzcv, 0x90000000, "adds w overflow flags");
    c.x[1] = 0xffffffff;
    c.run({0x2b020020});
    EQ(c.nzcv, 0x60000000, "adds w carry and zero");
    c.x[1] = 0, c.x[2] = 1;
    c.run({0xeb020020});   // subs x0, x1, x2
    EQ(c.x[0], ~0ull, "subs 0 - 1");
    EQ(c.nzcv, 0x80000000, "subs 0 - 1 flags");
    c.x[1] = 0x8000000000000000ull;
    c.run({0xeb020020});
    EQ(c.nzcv, 0x30000000, "subs signed overflow");
    for (auto [in, out] : {std::pair<uint64_t, uint64_t>{0, 31}, {0xffffffff, 31}, {0x40000000, 0}, {0x00ff0000, 7}}) {
        c.x[1] = in;
        c.run({0x5ac01420});   // cls w0, w1
        EQ(c.x[0], out, "cls " + hx(in));
    }
    c.x[1] = 0;
    c.run({0xdac01020});   // clz x0, x1
    EQ(c.x[0], 64, "clz 0");
    c.x[1] = 0x12345678;
    c.run({0x5ac00020});   // rbit w0, w1
    EQ(c.x[0], 0x1e6a2c48, "rbit");
    c.x[1] = 0x0102030405060708ull;
    c.run({0xdac00c20});   // rev x0, x1
    EQ(c.x[0], 0x0807060504030201ull, "rev");
    c.x[1] = 0x12345678;
    c.run({0xd3442c20});   // ubfx x0, x1, #4, #8
    EQ(c.x[0], 0x67, "ubfx");
    c.x[1] = 0xf8;
    c.run({0x13031c20});   // sbfx w0, w1, #3, #5
    EQ(c.x[0], 0xffffffff, "sbfx");
    c.x[0] = ~0ull, c.x[1] = 0x1234;
    c.run({0xb3783c20});   // bfi x0, x1, #8, #16
    EQ(c.x[0], 0xffffffffff1234ffull, "bfi");
    c.x[1] = 0x0123456789abcdefull, c.x[2] = 0xfedcba9876543210ull;
    c.run({0x93c23020});   // extr x0, x1, x2, #12
    EQ(c.x[0], 0xdeffedcba9876543ull, "extr");
    c.x[1] = 3, c.x[2] = 5, c.nzcv = 0x40000000;
    c.run({0xfa421024});   // ccmp x1, x2, #4, ne
    EQ(c.nzcv, 0x40000000, "ccmp, condition false");
    c.nzcv = 0;
    c.run({0xfa421024});
    EQ(c.nzcv, 0x80000000, "ccmp, condition true");
    c.x[1] = 1ull << 63, c.x[2] = 4;
    c.run({0x9bc27c20});   // umulh
    EQ(c.x[0], 2, "umulh");
    c.x[1] = ~0ull, c.x[2] = ~0ull;
    c.run({0x9b427c20});   // smulh
    EQ(c.x[0], 0, "smulh -1 * -1");
    c.x[1] = 0x80000000, c.x[2] = 0xffffffff;
    c.run({0x1ac20c20});   // sdiv w0, w1, w2
    EQ(c.x[0], 0x80000000, "sdiv INT_MIN / -1");
    c.x[1] = 7, c.x[2] = 0xfffffffe;
    c.run({0x1ac20c20});
    EQ(c.x[0], 0xfffffffd, "sdiv 7 / -2");
    c.x[2] = 0;
    c.run({0x1ac20c20});
    EQ(c.x[0], 0, "sdiv by 0");
    c.x[1] = ~0ull, c.x[2] = 3;
    c.run({0x9ac20820});   // udiv
    EQ(c.x[0], 0x5555555555555555ull, "udiv");
    c.run({0x9200f020});   // and x0, x1, #0x5555555555555555
    EQ(c.x[0], 0x5555555555555555ull, "and bitmask immediate");
    c.x[1] = 0xf;
    c.run({0x32089c20});   // orr w0, w1, #0xff00ff00
    EQ(c.x[0], 0xff00ff0f, "orr bitmask immediate");
    c.x[0] = ~0ull;
    c.run({0xf2e24680});   // movk x0, #0x1234, lsl #48
    EQ(c.x[0], 0x1234ffffffffffffull, "movk");
    for (bool cc : {false, true}) {
        uint32_t crc = 0xffffffff;
        for (const char* p = "123456789"; *p; p++) {
            c.x[0] = crc, c.x[1] = uint8_t(*p);
            c.run({cc ? 0x1ac15000u : 0x1ac14000u});
            crc = uint32_t(c.x[0]);
        }
        EQ(crc ^ 0xffffffff, cc ? 0xe3069283 : 0xcbf43926, cc ? "crc32c check value" : "crc32 check value");
    }
}

void simd() {
    Cpu c;
    c.lanes<int16_t>(1, {0x7fff, -32768, 5, 0, 0, 0, 0, 0});
    c.lanes<int16_t>(2, {1, -1, 7, 0, 0, 0, 0, 0});
    c.run({0x4e620c20});   // sqadd v0.8h
    EQ(c.lane<uint16_t>(0, 0), 0x7fff, "sqadd saturates up");
    EQ(c.lane<uint16_t>(0, 1), 0x8000, "sqadd saturates down");
    EQ(c.lane<uint16_t>(0, 2), 12, "sqadd");
    c.lanes<uint16_t>(1, {0, 1, 2, 3, 4, 5, 6, 7});
    c.lanes<uint16_t>(2, {8, 9, 10, 11, 12, 13, 14, 15});
    c.run({0x4e421820});   // uzp1 v0.8h
    for (int i = 0; i < 8; i++) EQ(c.lane<uint16_t>(0, i), 2 * i, "uzp1 " + std::to_string(i));
    for (int i = 0; i < 16; i++) c.q[1][i] = uint8_t(i), c.q[2][i] = uint8_t(16 + i);
    c.run({0x4e027820});   // zip2 v0.16b
    for (int i = 0; i < 16; i++) EQ(c.q[0][i], i & 1 ? 24 + i / 2 : 8 + i / 2, "zip2 " + std::to_string(i));
    c.run({0x6e021820});   // ext v0.16b, v1, v2, #3
    for (int i = 0; i < 16; i++) EQ(c.q[0][i], i + 3, "ext " + std::to_string(i));
    for (int i = 0; i < 16; i++) c.q[1][i] = uint8_t(100 + i);
    const uint8_t idx[16] = {0, 15, 16, 255, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
    std::memcpy(c.q[2], idx, 16);
    c.run({0x4e020020});   // tbl
    EQ(c.q[0][0], 100, "tbl 0"), EQ(c.q[0][1], 115, "tbl 15"), EQ(c.q[0][2], 0, "tbl 16 -> 0"), EQ(c.q[0][3], 0, "tbl 255 -> 0");
    c.lanes<uint8_t>(1, {0xff, 1, 0, 0x0f, 0x80, 0x7e, 3, 0xaa, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff});
    c.run({0x0e205820});   // cnt v0.8b
    EQ(c.lane<uint64_t>(0, 0), 0x0402060104000108ull, "cnt");
    EQ(c.hi64(0), 0, "cnt.8b clears the upper half");
    c.lanes<uint16_t>(1, {0xffff, 2, 3, 4, 5, 6, 7, 8});
    c.run({0x4e71b820});   // addv h0
    EQ(c.lane<uint64_t>(0, 0), 34, "addv wraps");
    c.lanes<int32_t>(1, {-5, 7, -1, 0});
    c.run({0x4f200420});   // sshr v0.4s, #32
    EQ(c.lane<uint32_t>(0, 0), 0xffffffff, "sshr #32 negative"), EQ(c.lane<uint32_t>(0, 1), 0, "sshr #32 positive");
    c.lanes<uint64_t>(1, {0x8000000000000000ull, 1});
    c.lanes<uint64_t>(2, {0xc1, 64});   // -63, 64
    c.run({0x6ee24420});   // ushl v0.2d
    EQ(c.lane<uint64_t>(0, 0), 1, "ushl by -63"), EQ(c.lane<uint64_t>(0, 1), 0, "ushl by 64");
    c.lanes<int32_t>(1, {70000, -70000, 5, -5});
    c.run({0x0e614820});   // sqxtn v0.4h
    EQ(c.lane<uint64_t>(0, 0), 0xfffb000580007fffull, "sqxtn"), EQ(c.hi64(0), 0, "sqxtn clears the upper half");
    c.lanes<float>(1, {1, 2, 3, 4});
    c.lanes<float>(2, {5, 6, 7, 8});
    c.run({0x6e22d420});   // faddp v0.4s
    check(c.lane<float>(0, 0) == 3 && c.lane<float>(0, 3) == 15, "faddp");
    c.run({0x6f05e540});   // movi v0.2d, #0xff00ff00ff00ff00
    EQ(c.lane<uint64_t>(0, 0), 0xff00ff00ff00ff00ull, "movi.2d"), EQ(c.hi64(0), 0xff00ff00ff00ff00ull, "movi.2d upper");
    c.lanes<uint16_t>(1, {0, 0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666, 0x7777});
    c.run({0x0e163c20});   // umov w0, v1.h[5]
    EQ(c.x[0], 0x5555, "umov");
    c.run({0x4e0e0420});   // dup v0.8h, v1.h[3]
    for (int i = 0; i < 8; i++) EQ(c.lane<uint16_t>(0, i), 0x3333, "dup");
    c.lanes<uint8_t>(1, {1, 2, 3, 0x80});
    c.run({0x4e072c20});   // smov x0, v1.b[3]
    EQ(c.x[0], 0xffffffffffffff80ull, "smov");
    std::memset(c.q[0], 0xaa, 16);
    c.x[1] = 0x12345678;
    c.run({0x4e141c20});   // ins v0.s[2], w1
    EQ(c.lane<uint32_t>(0, 2), 0x12345678, "ins"), EQ(c.lane<uint32_t>(0, 3), 0xaaaaaaaa, "ins keeps the rest");
    c.lanes<float>(1, {1.5f, -2.0f});
    c.run({0x0e617820});   // fcvtl v0.2d
    check(c.lane<double>(0, 0) == 1.5 && c.lane<double>(0, 1) == -2.0, "fcvtl");
    c.lanes<float>(1, {1.0f, 65520.0f, -0.0f, fu(0x33000000)});
    c.run({0x0e216820});   // fcvtn v0.4h
    EQ(c.lane<uint64_t>(0, 0), 0x000080007c003c00ull, "fcvtn"), EQ(c.hi64(0), 0, "fcvtn clears the upper half");
    c.lanes<int16_t>(1, {0x4000, -32768, 0, 0});
    c.lanes<int16_t>(2, {0x4000, -32768, 0, 0});
    c.run({0x2e62b420});   // sqrdmulh v0.4h
    EQ(c.lane<uint16_t>(0, 0), 0x2000, "sqrdmulh"), EQ(c.lane<uint16_t>(0, 1), 0x7fff, "sqrdmulh saturates");
    c.lanes<uint64_t>(1, {3, 0});
    c.lanes<uint64_t>(2, {3, 0});
    c.run({0x0ee2e020});   // pmull v0.1q
    EQ(c.lane<uint64_t>(0, 0), 5, "pmull 3 * 3");
    c.lanes<uint64_t>(1, {0x8000000000000000ull, 0});
    c.lanes<uint64_t>(2, {2, 0});
    c.run({0x0ee2e020});
    EQ(c.lane<uint64_t>(0, 0), 0, "pmull carries"), EQ(c.hi64(0), 1, "pmull high half");
}

void bytes(uint8_t* out, const char* hexs) {
    for (int i = 0; i < 16; i++) out[i] = uint8_t(std::strtoul(std::string(hexs + 2 * i, 2).c_str(), nullptr, 16));
}
bool same16(const uint8_t* a, const char* hexs) {
    uint8_t b[16];
    bytes(b, hexs);
    return std::memcmp(a, b, 16) == 0;
}

void crypto() {
    // FIPS-197 Appendix B, round 1 (bytes in block order)
    Cpu c;
    bytes(c.q[0], "193de3bea0f4e22b9ac68d2ae9f84808");
    std::memset(c.q[1], 0, 16);
    c.run({0x4e284820});   // aese v0, v1: ShiftRows, SubBytes
    check(same16(c.q[0], "d4bf5d30e0b452aeb84111f11e2798e5"), "aese (FIPS-197 round 1)");
    std::memcpy(c.q[1], c.q[0], 16);
    c.run({0x4e286820});   // aesmc v0, v1
    check(same16(c.q[0], "046681e5e0cb199a48f8d37a2806264c"), "aesmc (FIPS-197 round 1)");
    std::memcpy(c.q[1], c.q[0], 16);
    c.run({0x4e287820});   // aesimc v0, v1
    check(same16(c.q[0], "d4bf5d30e0b452aeb84111f11e2798e5"), "aesimc inverts aesmc");
    std::memset(c.q[1], 0, 16);
    c.run({0x4e285820});   // aesd v0, v1
    check(same16(c.q[0], "193de3bea0f4e22b9ac68d2ae9f84808"), "aesd inverts aese");

    // SHA-256 and SHA-1 of "abc", one block, through the semantic functions on a State
    uint8_t block[64] = {'a', 'b', 'c', 0x80};
    block[63] = 24;
    uint32_t w[16];
    for (int i = 0; i < 16; i++) w[i] = uint32_t(block[4 * i]) << 24 | block[4 * i + 1] << 16 | block[4 * i + 2] << 8 | block[4 * i + 3];
    nxw::sem::State s{};
    auto setv = [&](int n, const uint32_t* v) { std::memcpy(&s.q[n], v, 16); };
    auto getv = [&](int n, uint32_t* v) { std::memcpy(v, &s.q[n], 16); };
    {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
            0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
            0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        const uint32_t h0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        setv(0, h0), setv(1, h0 + 4);
        uint32_t m[4][4];
        for (int i = 0; i < 4; i++) std::memcpy(m[i], w + 4 * i, 16);
        for (int g = 0; g < 16; g++) {
            uint32_t wk[4];
            for (int i = 0; i < 4; i++) wk[i] = m[0][i] + K[4 * g + i];
            setv(20, wk);
            s.q[2] = s.q[0];
            nxw_sem_SHA256H(&s, 0x5e144020, 0);    // sha256h q0, q1, v20.4s
            nxw_sem_SHA256H2(&s, 0x5e145041, 0);   // sha256h2 q1, q2, v20.4s
            if (g < 12) {
                setv(4, m[0]), setv(5, m[1]), setv(6, m[2]), setv(7, m[3]);
                nxw_sem_SHA256SU0(&s, 0x5e2828a4, 0);   // sha256su0 v4.4s, v5.4s
                nxw_sem_SHA256SU1(&s, 0x5e0760c4, 0);   // sha256su1 v4.4s, v6.4s, v7.4s
                uint32_t next[4];
                getv(4, next);
                std::memmove(m[0], m[1], sizeof m - sizeof m[0]);
                std::memcpy(m[3], next, 16);
            } else {
                std::memmove(m[0], m[1], sizeof m - sizeof m[0]);
            }
        }
        uint32_t a[4], e[4];
        getv(0, a), getv(1, e);
        const uint32_t want[8] = {0xba7816bf, 0x8f01cfea, 0x414140de, 0x5dae2223, 0xb00361a3, 0x96177a9c, 0xb410ff61, 0xf20015ad};
        bool ok = true;
        for (int i = 0; i < 4; i++) ok &= a[i] + h0[i] == want[i] && e[i] + h0[4 + i] == want[4 + i];
        check(ok, "SHA-256(\"abc\") through sha256h / h2 / su0 / su1");
    }
    {
        const uint32_t h0[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
        setv(0, h0);
        uint32_t e = h0[4];
        uint32_t m[4][4];
        for (int i = 0; i < 4; i++) std::memcpy(m[i], w + 4 * i, 16);
        for (int g = 0; g < 20; g++) {
            const uint32_t k = g < 5 ? 0x5a827999 : g < 10 ? 0x6ed9eba1 : g < 15 ? 0x8f1bbcdc : 0xca62c1d6;
            uint32_t wk[4];
            for (int i = 0; i < 4; i++) wk[i] = m[0][i] + k;
            setv(20, wk);
            nxw_sem_SHA1H(&s, 0x5e280811, 0);   // sha1h s17, s0: the next E
            uint32_t ev[4] = {e, 0, 0, 0};
            setv(16, ev);
            if (g < 5) nxw_sem_SHA1C(&s, 0x5e140200, 0);                   // sha1c q0, s16, v20.4s
            else if (g < 10 || g >= 15) nxw_sem_SHA1P(&s, 0x5e141200, 0);   // sha1p
            else nxw_sem_SHA1M(&s, 0x5e142200, 0);                         // sha1m
            uint32_t nx[4];
            getv(17, nx);
            e = nx[0];
            if (g < 16) {
                setv(4, m[0]), setv(5, m[1]), setv(6, m[2]), setv(7, m[3]);
                nxw_sem_SHA1SU0(&s, 0x5e0630a4, 0);   // sha1su0 v4.4s, v5.4s, v6.4s
                nxw_sem_SHA1SU1(&s, 0x5e2818e4, 0);   // sha1su1 v4.4s, v7.4s
                uint32_t next[4];
                getv(4, next);
                std::memmove(m[0], m[1], sizeof m - sizeof m[0]);
                std::memcpy(m[3], next, 16);
            } else {
                std::memmove(m[0], m[1], sizeof m - sizeof m[0]);
            }
        }
        uint32_t a[4];
        getv(0, a);
        const uint32_t want[5] = {0xa9993e36, 0x4706816a, 0xba3e2571, 0x7850c26c, 0x9cd0d89d};
        bool ok = e + h0[4] == want[4];
        for (int i = 0; i < 4; i++) ok &= a[i] + h0[i] == want[i];
        check(ok, "SHA-1(\"abc\") through sha1c / p / m / h / su0 / su1");
    }
}

void memory() {
    Cpu c;
    const uint64_t b = reinterpret_cast<uint64_t>(mem);
    for (int i = 0; i < 64; i++) mem[i] = uint8_t(i);
    c.x[2] = b;
    c.run({0xa9c10440});   // ldp x0, x1, [x2, #16]!
    EQ(c.x[0], 0x1716151413121110ull, "ldp pre-index 0"), EQ(c.x[1], 0x1f1e1d1c1b1a1918ull, "ldp pre-index 1");
    EQ(c.x[2], b + 16, "ldp writeback");
    const uint32_t neg = 0x80000000;
    std::memcpy(mem + 12, &neg, 4);
    c.x[1] = b + 16, c.x[2] = 0xffffffff;
    c.run({0xb8a2d820});   // ldrsw x0, [x1, w2, sxtw #2]
    EQ(c.x[0], 0xffffffff80000000ull, "ldrsw with a negative extended index");
    for (uint32_t i = 0; i < 8; i++) std::memcpy(mem + 4 * i, &i, 4);
    c.x[1] = b;
    c.run({0x4c408820});   // ld2 {v0.4s, v1.4s}, [x1]
    EQ(c.lane<uint32_t>(0, 1), 2, "ld2 even"), EQ(c.lane<uint32_t>(1, 3), 7, "ld2 odd");
    uint64_t v = 5;
    std::memcpy(mem, &v, 8);
    c.x[0] = 5, c.x[1] = 9, c.x[2] = b;
    c.run({0xc8a07c41});   // cas x0, x1, [x2]
    std::memcpy(&v, mem, 8);
    EQ(v, 9, "cas stores"), EQ(c.x[0], 5, "cas returns the old value");
    c.x[0] = 4, c.x[1] = 1;
    c.run({0xc8a07c41});
    std::memcpy(&v, mem, 8);
    EQ(v, 9, "cas leaves memory alone on a mismatch"), EQ(c.x[0], 9, "cas returns the current value");
    v = 10, std::memcpy(mem, &v, 8);
    c.x[0] = 3;
    c.run({0xf8200041});   // ldadd x0, x1, [x2]
    std::memcpy(&v, mem, 8);
    EQ(v, 13, "ldadd"), EQ(c.x[1], 10, "ldadd returns the old value");
    v = 42, std::memcpy(mem, &v, 8);
    c.x[1] = 99;
    c.run({0xc85ffc40, 0xc803fc41});   // ldaxr x0, [x2]; stlxr w3, x1, [x2]
    std::memcpy(&v, mem, 8);
    EQ(c.x[0], 42, "ldaxr"), EQ(c.x[3], 0, "stlxr succeeds"), EQ(v, 99, "stlxr stores");
    c.excl_addr = ~0ull, c.x[1] = 7;
    c.run({0xc803fc41});
    std::memcpy(&v, mem, 8);
    EQ(c.x[3], 1, "stlxr fails without a monitor"), EQ(v, 99, "a failed stlxr stores nothing");
}

}  // namespace

int main() {
    fp_estimates();
    fp_minmax_convert();
    integer();
    simd();
    crypto();
    memory();
    std::printf("interp-kat: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
