// nxlift-tests: real game functions lifted and run on Windows, three ways. The modules are mapped at their own addresses
// (identity mapping), each test function and every guest function it calls is lifted and JIT-compiled (ORC), and every
// randomised input runs (1) lifted, (2) in the interpreter (rt_code_table emptied, so every call is interpreted too) and
// (3) through a host C reference. (1) and (2) must leave identical register state and memory; (3) must agree on the result
// (exactly, or within 1 ulp for transcendental functions, which no two libms round alike).
// NXW_DUMP names the 2.0.1 ExeFS (exit 77 without it); NXW_PROGRAM_DB its program.db (default _local\analyze\2.0.1);
// NXW_TEST_ITERS the inputs per function (default 3000); NXW_TEST_ONLY a comma-separated list of test names.
#include "nxlift/lift.hpp"

#include "nxw/a64.hpp"
#include "nxw/interp.hpp"
#include "nxw/rt.h"
#include "nxw/sem.hpp"

#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

#include <windows.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <sstream>

using namespace nxlift;
using nxanalyze::none32;

// ---------------------------------------------------------------- the runtime, as far as these functions need it
extern "C" {
const uint32_t* rt_code_table = nullptr;
uint64_t rt_code_base = 0, rt_code_words = 0, rt_host_base = 0;
extern "C" void __chkstk();
int _fltused_dummy = 0;
}

namespace {

const char* g_current = "";   // the test running, for fatal messages
uint64_t g_mod_lo = 0, g_mod_hi = 0;
int64_t g_max_ulp = 0;        // the largest ulp distance an approximate reference saw in the current test

[[noreturn]] void fatal(const char* what, NxwGuestCpu* cpu, uint64_t pc, uint64_t detail) {
    std::printf("\nFATAL in %s: %s at pc %llx (detail %llx)\n", g_current, what, static_cast<unsigned long long>(pc),
                static_cast<unsigned long long>(detail));
    if (cpu)
        for (int i = 0; i < 31; i++) std::printf("  x%d=%llx%s", i, static_cast<unsigned long long>(cpu->x[i]), i % 4 == 3 ? "\n" : "");
    std::printf("\n");
    std::fflush(stdout);
    ExitProcess(3);
}

}  // namespace

extern "C" void rt_svc(NxwGuestCpu* cpu, uint32_t imm) { fatal("svc", cpu, cpu->pc, imm); }
extern "C" uint64_t rt_mrs(NxwGuestCpu* cpu, uint32_t sr) {
    if (sr == nxw::sem::sr_cntpct_el0 || sr == nxw::sem::sr_cntvct_el0) return 0;   // a fixed clock keeps runs comparable
    fatal("mrs", cpu, cpu->pc, sr);
}
extern "C" void rt_msr(NxwGuestCpu* cpu, uint32_t sr, uint64_t v) {
    if (sr == nxw::sem::sr_fpcr) {
        cpu->fpcr = v;
        return;
    }
    fatal("msr", cpu, cpu->pc, sr);
}
extern "C" void rt_trap(NxwGuestCpu* cpu, uint64_t pc, uint32_t why) { fatal("trap", cpu, pc, why); }
extern "C" NxwGuestFn rt_miss(NxwGuestCpu* cpu, uint64_t target) {
    if (target >= g_mod_lo && target < g_mod_hi) {
        cpu->pc = target;
        return nxw_interp_trampoline;
    }
    fatal("indirect branch outside the modules", cpu, cpu->pc, target);
}
extern "C" void rt_yield(NxwGuestCpu*, uint32_t, uint64_t) {}
extern "C" void rt_nonlocal(NxwGuestCpu* cpu) { fatal("nonlocal", cpu, cpu->pc, 0); }
extern "C" void rt_fiber_switch(NxwGuestCpu* cpu) { fatal("fiber", cpu, cpu->pc, 0); }

namespace {

LONG WINAPI on_exception(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || e->ExceptionRecord->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION) {
        std::printf("\nFATAL in %s: exception %lx at host %p, address %llx\n", g_current, e->ExceptionRecord->ExceptionCode,
                    e->ExceptionRecord->ExceptionAddress,
                    static_cast<unsigned long long>(e->ExceptionRecord->NumberParameters > 1 ? e->ExceptionRecord->ExceptionInformation[1] : 0));
        std::fflush(stdout);
        ExitProcess(4);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------- guest memory
constexpr uint64_t stack_base = 0x72'0000'0000, stack_size = 4 << 20;
constexpr uint64_t buf_base = 0x73'0000'0000, buf_size = 256 << 10;   // A at +0, B at +128K
constexpr uint64_t buf_a = buf_base, buf_b = buf_base + (128 << 10);
constexpr uint64_t tls_base = 0x74'0000'0000, tls_size = 64 << 10;

void* map_at(uint64_t addr, uint64_t size) {
    void* p = VirtualAlloc(reinterpret_cast<void*>(addr), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p != reinterpret_cast<void*>(addr)) {
        std::printf("cannot map guest memory at %llx (%llu bytes)\n", static_cast<unsigned long long>(addr), static_cast<unsigned long long>(size));
        std::exit(1);
    }
    return p;
}
uint8_t* gptr(uint64_t a) { return reinterpret_cast<uint8_t*>(uintptr_t(a)); }

// ---------------------------------------------------------------- inputs and runs
struct Input {
    NxwGuestCpu cpu{};
    std::vector<uint8_t> buf = std::vector<uint8_t>(buf_size);
};
struct Output {
    NxwGuestCpu cpu{};
    std::vector<uint8_t> buf;
};

using Rng = std::mt19937_64;

void set_s(NxwGuestCpu& c, int n, float v) {
    std::memset(c.q[n], 0, 16);
    std::memcpy(c.q[n], &v, 4);
}
void set_d(NxwGuestCpu& c, int n, double v) {
    std::memset(c.q[n], 0, 16);
    std::memcpy(c.q[n], &v, 8);
}
float get_s(const NxwGuestCpu& c, int n) {
    float v;
    std::memcpy(&v, c.q[n], 4);
    return v;
}
double get_d(const NxwGuestCpu& c, int n) {
    double v;
    std::memcpy(&v, c.q[n], 8);
    return v;
}

struct Test {
    std::string name;
    uint32_t fn = none32;
    std::function<void(Rng&, Input&)> gen;
    // checks the lifted output against the host; empty: no reference (lifted vs interpreter only)
    std::function<bool(const Input&, const Output&, std::string&)> ref;
    bool approx = false;   // the reference allows 1 ulp
};

// State a run must reproduce: everything in NxwGuestCpu except pc (the runtime's) and the host-private tail
std::string hex(const uint8_t* p, size_t n) {
    std::string s;
    char b[4];
    for (size_t i = n; i-- > 0;) std::snprintf(b, sizeof b, "%02x", p[i]), s += b;
    return s;
}
bool same_state(const NxwGuestCpu& a, const NxwGuestCpu& b, std::string& why) {
    for (int i = 0; i < 31; i++)
        if (a.x[i] != b.x[i]) return why = "x" + std::to_string(i) + " " + hex(reinterpret_cast<const uint8_t*>(&a.x[i]), 8) + " vs " + hex(reinterpret_cast<const uint8_t*>(&b.x[i]), 8), false;
    if (a.sp != b.sp) return why = "sp", false;
    if (a.nzcv != b.nzcv) return why = "nzcv", false;
    if (a.fpcr != b.fpcr || a.fpsr != b.fpsr) return why = "fpcr/fpsr", false;
    for (int i = 0; i < 32; i++)
        if (std::memcmp(a.q[i], b.q[i], 16)) return why = "q" + std::to_string(i) + " " + hex(a.q[i], 16) + " vs " + hex(b.q[i], 16), false;
    if (a.tpidr_el0 != b.tpidr_el0 || a.tpidrro_el0 != b.tpidrro_el0) return why = "tpidr", false;
    if (a.excl_addr != b.excl_addr || std::memcmp(a.excl_val, b.excl_val, 16)) return why = "exclusive monitor", false;
    return true;
}

Output run(NxwGuestFn f, uint64_t entry, const Input& in, bool lifted) {
    std::memcpy(gptr(buf_base), in.buf.data(), buf_size);
    std::memset(gptr(tls_base), 0, tls_size);
    Output o;
    o.cpu = in.cpu;
    if (lifted) {
        f(&o.cpu);
    } else {
        const uint64_t words = rt_code_words;
        rt_code_words = 0;   // every call interpreted
        o.cpu.pc = entry;
        nxw_interp_trampoline(&o.cpu);
        rt_code_words = words;
    }
    o.buf.assign(gptr(buf_base), gptr(buf_base) + buf_size);
    return o;
}

void base_cpu(Input& in, Rng& r) {
    for (int i = 0; i < 31; i++) in.cpu.x[i] = r();   // garbage in every register, as a caller would leave it
    for (int i = 0; i < 32; i++)
        for (int b = 0; b < 16; b++) in.cpu.q[i][b] = uint8_t(r());
    in.cpu.nzcv = (r() & 15) << 28;
    in.cpu.sp = stack_base + stack_size - 0x1000;
    in.cpu.x[29] = 0;
    in.cpu.x[30] = 0x7fff'dead'0000;   // never used: RET returns to the host caller
    in.cpu.tpidrro_el0 = tls_base;
    in.cpu.tpidr_el0 = tls_base + 0x8000;
    in.cpu.excl_addr = ~0ull;
}
void fill(Input& in, Rng& r, uint64_t a, size_t n, bool nonzero = false) {
    for (size_t i = 0; i < n; i++) {
        uint8_t v = uint8_t(r());
        if (nonzero && !v) v = 1;
        in.buf[a - buf_base + i] = v;
    }
}
size_t pick_len(Rng& r, size_t max) {
    switch (r() % 4) {
    case 0: return r() % 17;
    case 1: return r() % 129;
    case 2: return r() % 1025;
    default: return r() % (max + 1);
    }
}
template <class T> int sgn(T v) { return (v > 0) - (v < 0); }

// Reference runs on a host copy of the buffer at its own address; guest addresses are translated
struct HostBuf {
    std::vector<uint8_t> b;
    explicit HostBuf(const Input& in) : b(in.buf) {}
    uint8_t* p(uint64_t ga) { return b.data() + (ga - buf_base); }
    uint64_t g(const void* hp) const { return hp ? buf_base + uint64_t(static_cast<const uint8_t*>(hp) - b.data()) : 0; }
};
bool same_buf(const HostBuf& h, const Output& o, std::string& why) {
    if (std::memcmp(h.b.data(), o.buf.data(), buf_size) == 0) return true;
    for (size_t i = 0; i < buf_size; i++)
        if (h.b[i] != o.buf[i]) {
            why = "memory differs at buffer+" + std::to_string(i);
            break;
        }
    return false;
}

// ---------------------------------------------------------------- float inputs: every class, edges and ordinary values
float any_float(Rng& r) {
    static const float specials[] = {0.0f, -0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 1.5f, 2.5f, -2.5f, INFINITY, -INFINITY, NAN, -NAN,
                                     std::bit_cast<float>(0x7f800001u), std::bit_cast<float>(0xffbfffffu),   // signalling NaNs
                                     std::bit_cast<float>(1u), std::bit_cast<float>(0x807fffffu),            // denormals
                                     std::bit_cast<float>(0x00800000u), 3.4028235e38f, -3.4028235e38f, 1e-38f, 8388608.0f,
                                     8388607.5f, -8388608.5f, 16777216.0f, 3.14159265f, 1.57079633f, -1.57079633f, 6.28318531f,
                                     1e10f, -1e10f, 88.72f, -103.97f, 89.0f, -88.0f, 1e-7f};
    switch (r() % 6) {
    case 0: return specials[r() % std::size(specials)];
    case 1: return std::bit_cast<float>(uint32_t(r()));                      // any bit pattern
    case 2: return float(std::uniform_real_distribution<double>(-10, 10)(r));
    case 3: return float(std::uniform_real_distribution<double>(-1e6, 1e6)(r));
    case 4: return std::bit_cast<float>(uint32_t(r() % 0x00800000u) | (r() & 1 ? 0x80000000u : 0));   // denormals
    default: return float(std::uniform_real_distribution<double>(-200, 200)(r)) * float(r() % 3 + 1);
    }
}
double any_double(Rng& r) {
    static const double specials[] = {0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 2.5, -2.5, INFINITY, -INFINITY, NAN,
                                      std::bit_cast<double>(0x7ff0000000000001ull), std::bit_cast<double>(1ull),
                                      std::bit_cast<double>(0x800fffffffffffffull), 1.7976931348623157e308, 4503599627370496.0,
                                      4503599627370495.5, -4503599627370496.5, 1e300, -1e-300};
    switch (r() % 5) {
    case 0: return specials[r() % std::size(specials)];
    case 1: return std::bit_cast<double>(r());
    case 2: return std::uniform_real_distribution<double>(-10, 10)(r);
    case 3: return std::uniform_real_distribution<double>(-1e15, 1e15)(r);
    default: return std::bit_cast<double>(r() % 0x0010000000000000ull);
    }
}
// Exact agreement (NaN agrees with NaN of any payload: the host libm's NaNs are not Arm's)
template <class T> bool fp_same(T a, T b) {
    if (std::isnan(a) || std::isnan(b)) return std::isnan(a) && std::isnan(b);
    return std::bit_cast<std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>>(a) ==
           std::bit_cast<std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>>(b);
}
int64_t ulps(float a, float b) {
    if (std::isnan(a) || std::isnan(b)) return std::isnan(a) && std::isnan(b) ? 0 : INT64_MAX;
    auto key = [](float f) {
        const int32_t i = std::bit_cast<int32_t>(f);
        return i < 0 ? int64_t(INT32_MIN) - i : int64_t(i);
    };
    return std::llabs(key(a) - key(b));
}

// C11 7.22.1.4 strtoll for the "C" locale. The reference instead of UCRT's, which gives "no conversion" for "0x" without a
// hex digit after it (the subject sequence there is "0", ending before the x).
long long ref_strtoll(const char* s, const char** end, int base) {
    const char* p = s;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
    bool neg = false;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 99; };
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && digit(p[2]) < 16) p += 2, base = 16;
    else if (base == 0) base = p[0] == '0' ? 8 : 10;
    unsigned long long v = 0;
    bool any = false, over = false;
    for (; digit(*p) < base; p++) {
        any = true;
        const unsigned long long d = unsigned(digit(*p));
        if (v > (~0ull - d) / unsigned(base)) over = true;
        else v = v * unsigned(base) + d;
    }
    if (!any) {
        *end = s;
        return 0;
    }
    *end = p;
    if (neg) return over || v > (1ull << 63) ? LLONG_MIN : static_cast<long long>(0 - v);
    return over || v > uint64_t(LLONG_MAX) ? LLONG_MAX : static_cast<long long>(v);
}

// ---------------------------------------------------------------- the tests
std::vector<Test> make_tests(const Inputs& in) {
    auto fn_of = [&](const char* sym) -> uint32_t {
        const auto f = in.prog.find(sym);
        if (!f) return none32;
        const uint64_t a = in.prog.bases[size_t(f->first)] + in.prog.modules[size_t(f->first)].symbols[f->second].value;
        auto it = in.fn_at.find(a);
        return it == in.fn_at.end() ? none32 : it->second;
    };
    std::vector<Test> t;
    auto add = [&](const char* sym, auto gen, auto ref, bool approx = false) {
        Test x;
        x.name = sym, x.fn = fn_of(sym), x.gen = gen, x.ref = ref, x.approx = approx;
        if (x.fn == none32) std::printf("  (no function for %s)\n", sym);
        else t.push_back(std::move(x));
    };

    // memory and strings: alignments 0..63, lengths 0..8K, overlap both ways, bytes >= 0x80
    add("memcpy", [](Rng& r, Input& i) {
            fill(i, r, buf_a, 64 << 10), fill(i, r, buf_b, 64 << 10);
            i.cpu.x[0] = buf_a + r() % 64, i.cpu.x[1] = buf_b + r() % 64, i.cpu.x[2] = pick_len(r, 8192);
        },
        [](const Input& i, const Output& o, std::string& why) {
            HostBuf h(i);
            std::memcpy(h.p(i.cpu.x[0]), h.p(i.cpu.x[1]), i.cpu.x[2]);
            if (o.cpu.x[0] != i.cpu.x[0]) return why = "return value", false;
            return same_buf(h, o, why);
        });
    add("memmove", [](Rng& r, Input& i) {
            fill(i, r, buf_a, 64 << 10);
            const uint64_t n = pick_len(r, 8192), s = r() % 16384, d = r() % 3 ? s + (int64_t(r() % 129) - 64) : r() % 16384;
            i.cpu.x[0] = buf_a + 8192 + d - 8192 + 64, i.cpu.x[1] = buf_a + 64 + s, i.cpu.x[2] = n;
        },
        [](const Input& i, const Output& o, std::string& why) {
            HostBuf h(i);
            std::memmove(h.p(i.cpu.x[0]), h.p(i.cpu.x[1]), i.cpu.x[2]);
            if (o.cpu.x[0] != i.cpu.x[0]) return why = "return value", false;
            return same_buf(h, o, why);
        });
    add("memset", [](Rng& r, Input& i) {
            fill(i, r, buf_a, 16 << 10);
            i.cpu.x[0] = buf_a + r() % 64, i.cpu.x[1] = r() % 3 ? r() % 256 : r(), i.cpu.x[2] = pick_len(r, 8192);
        },
        [](const Input& i, const Output& o, std::string& why) {
            HostBuf h(i);
            std::memset(h.p(i.cpu.x[0]), int(uint8_t(i.cpu.x[1])), i.cpu.x[2]);
            if (o.cpu.x[0] != i.cpu.x[0]) return why = "return value", false;
            return same_buf(h, o, why);
        });
    add("memcmp", [](Rng& r, Input& i) {
            const uint64_t a = buf_a + r() % 64, b = buf_b + r() % 64, n = pick_len(r, 4096);
            fill(i, r, a, n + 64);
            std::memcpy(&i.buf[b - buf_base], &i.buf[a - buf_base], n + 64);
            if (n && r() % 4) i.buf[b - buf_base + r() % n] ^= uint8_t(1 + r() % 255);
            i.cpu.x[0] = a, i.cpu.x[1] = b, i.cpu.x[2] = n;
        },
        [](const Input& i, const Output& o, std::string& why) {
            HostBuf h(i);
            if (sgn(int32_t(o.cpu.x[0])) != sgn(std::memcmp(h.p(i.cpu.x[0]), h.p(i.cpu.x[1]), i.cpu.x[2]))) return why = "sign", false;
            return true;
        });
    auto cstring = [](Rng& r, Input& i, uint64_t a, size_t len) {
        fill(i, r, a, len, true);
        i.buf[a - buf_base + len] = 0;
        fill(i, r, a + len + 1, 64);
    };
    add("strlen", [&](Rng& r, Input& i) {
            const uint64_t a = buf_a + r() % 64;
            cstring(r, i, a, pick_len(r, 4096));
            i.cpu.x[0] = a;
        },
        [](const Input& i, const Output& o, std::string& why) {
            HostBuf h(i);
            if (o.cpu.x[0] != std::strlen(reinterpret_cast<const char*>(h.p(i.cpu.x[0])))) return why = "length", false;
            return true;
        });
    add("strnlen", [&](Rng& r, Input& i) {
            const uint64_t a = buf_a + r() % 64;
            const size_t n = pick_len(r, 2048);
            cstring(r, i, a, n);
            i.cpu.x[0] = a, i.cpu.x[1] = r() % 2 ? pick_len(r, 2048) : n;
        },
        [](const Input& i, const Output& o, std::string& why) {
            HostBuf h(i);
            if (o.cpu.x[0] != strnlen(reinterpret_cast<const char*>(h.p(i.cpu.x[0])), i.cpu.x[1])) return why = "length", false;
            return true;
        });
    auto two_strings = [&](Rng& r, Input& i) {
        const uint64_t a = buf_a + r() % 64, b = buf_b + r() % 64;
        const size_t n = pick_len(r, 2048);
        cstring(r, i, a, n);
        std::memcpy(&i.buf[b - buf_base], &i.buf[a - buf_base], n + 1);
        switch (r() % 4) {
        case 0: break;                                                                           // equal
        case 1: if (n) i.buf[b - buf_base + r() % n] = uint8_t(1 + r() % 255); break;           // one byte differs
        case 2: i.buf[b - buf_base + (n ? r() % (n + 1) : 0)] = 0; break;                        // b shorter
        default: cstring(r, i, b, pick_len(r, 64)); break;                                       // unrelated
        }
        i.cpu.x[0] = a, i.cpu.x[1] = b;
    };
    add("strcmp", two_strings, [](const Input& i, const Output& o, std::string& why) {
        HostBuf h(i);
        if (sgn(int32_t(o.cpu.x[0])) != sgn(std::strcmp(reinterpret_cast<const char*>(h.p(i.cpu.x[0])), reinterpret_cast<const char*>(h.p(i.cpu.x[1])))))
            return why = "sign", false;
        return true;
    });
    add("strncmp", [&](Rng& r, Input& i) {
            two_strings(r, i);
            i.cpu.x[2] = r() % 8 ? pick_len(r, 2100) : r();
        },
        [](const Input& i, const Output& o, std::string& why) {
            HostBuf h(i);
            if (sgn(int32_t(o.cpu.x[0])) != sgn(std::strncmp(reinterpret_cast<const char*>(h.p(i.cpu.x[0])),
                                                             reinterpret_cast<const char*>(h.p(i.cpu.x[1])), i.cpu.x[2])))
                return why = "sign", false;
            return true;
        });
    auto char_in = [&](Rng& r, Input& i, uint64_t a, size_t n) -> uint64_t {
        switch (r() % 4) {
        case 0: return n ? i.buf[a - buf_base + r() % n] : 'x';   // present
        case 1: return 0;                                          // the terminator
        case 2: return (n ? i.buf[a - buf_base + r() % n] : 'x') | 0xffffff00ull;   // only the low byte counts
        default: return r() % 256;
        }
    };
    add("strchr", [&](Rng& r, Input& i) {
            const uint64_t a = buf_a + r() % 64;
            const size_t n = pick_len(r, 2048);
            cstring(r, i, a, n);
            i.cpu.x[0] = a, i.cpu.x[1] = char_in(r, i, a, n);
        },
        [](const Input& i, const Output& o, std::string& why) {
            HostBuf h(i);
            if (o.cpu.x[0] != h.g(std::strchr(reinterpret_cast<const char*>(h.p(i.cpu.x[0])), char(i.cpu.x[1])))) return why = "pointer", false;
            return true;
        });
    add("strrchr", [&](Rng& r, Input& i) {
            const uint64_t a = buf_a + r() % 64;
            const size_t n = pick_len(r, 2048);
            cstring(r, i, a, n);
            i.cpu.x[0] = a, i.cpu.x[1] = char_in(r, i, a, n);
        },
        [](const Input& i, const Output& o, std::string& why) {
            HostBuf h(i);
            if (o.cpu.x[0] != h.g(std::strrchr(reinterpret_cast<const char*>(h.p(i.cpu.x[0])), char(i.cpu.x[1])))) return why = "pointer", false;
            return true;
        });
    add("memchr", [&](Rng& r, Input& i) {
            const uint64_t a = buf_a + r() % 64;
            const size_t n = pick_len(r, 4096);
            fill(i, r, a, n + 64);
            i.cpu.x[0] = a, i.cpu.x[1] = char_in(r, i, a, n), i.cpu.x[2] = n;
        },
        [](const Input& i, const Output& o, std::string& why) {
            HostBuf h(i);
            if (o.cpu.x[0] != h.g(std::memchr(h.p(i.cpu.x[0]), int(uint8_t(i.cpu.x[1])), i.cpu.x[2]))) return why = "pointer", false;
            return true;
        });
    auto number = [](Rng& r, int base) {
        static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
        std::string s(r() % 3, ' ');
        if (r() % 2) s += "\t\n\v\f\r"[r() % 5];
        if (r() % 3 == 0) s += r() % 2 ? '-' : '+';
        const int b = base ? base : 10;
        if ((base == 16 || base == 0) && r() % 2) s += r() % 2 ? "0x" : "0X";
        else if (base == 0 && r() % 3 == 0) s += '0';
        const size_t n = r() % 4 == 0 ? r() % 40 : r() % 12;
        for (size_t k = 0; k < n; k++) {
            const int d = int(r() % b);
            s += d < 10 ? char('0' + d) : char((r() % 2 ? 'a' : 'A') + d - 10);
        }
        if (r() % 2) s += digits[r() % 62];
        if (r() % 2) s += " tail";
        return s;
    };
    add("atoi", [&](Rng& r, Input& i) {
            std::string s = number(r, 10);
            // atoi overflow is undefined: keep at most 9 digits
            size_t digits = 0;
            for (char& c : s)
                if (c >= '0' && c <= '9' && ++digits > 9) c = 'z';
            const uint64_t a = buf_a + r() % 64;
            std::memcpy(&i.buf[a - buf_base], s.c_str(), s.size() + 1);
            i.cpu.x[0] = a;
        },
        [](const Input& i, const Output& o, std::string& why) {
            HostBuf h(i);
            if (int32_t(o.cpu.x[0]) != std::atoi(reinterpret_cast<const char*>(h.p(i.cpu.x[0])))) return why = "value", false;
            return true;
        });
    add("strtol", [&](Rng& r, Input& i) {
            static const int bases[] = {0, 10, 16, 8, 2, 36, 0, 10};
            const int base = r() % 5 ? bases[r() % std::size(bases)] : int(2 + r() % 35);
            const std::string s = number(r, base);
            const uint64_t a = buf_a + r() % 64;
            std::memcpy(&i.buf[a - buf_base], s.c_str(), s.size() + 1);
            i.cpu.x[0] = a, i.cpu.x[1] = r() % 4 ? buf_b : 0, i.cpu.x[2] = uint64_t(base);
        },
        [](const Input& i, const Output& o, std::string& why) {
            HostBuf h(i);
            const char* end = nullptr;
            const long long v = ref_strtoll(reinterpret_cast<const char*>(h.p(i.cpu.x[0])), &end, int(i.cpu.x[2]));
            if (int64_t(o.cpu.x[0]) != v) return why = "value", false;
            if (i.cpu.x[1]) {
                uint64_t ge;
                std::memcpy(&ge, &o.buf[i.cpu.x[1] - buf_base], 8);
                if (ge != h.g(end))
                    return why = "end pointer for \"" + std::string(reinterpret_cast<const char*>(h.p(i.cpu.x[0]))) + "\" base " +
                                 std::to_string(i.cpu.x[2]) + ": guest +" + std::to_string(ge - i.cpu.x[0]) + ", host +" +
                                 std::to_string(h.g(end) - i.cpu.x[0]),
                           false;
            }
            return true;
        });

    // math: every float class, denormals, signalling NaNs, infinities
    auto f1 = [&](const char* sym, float (*host)(float), bool approx) {
        add(sym, [](Rng& r, Input& i) { set_s(i.cpu, 0, any_float(r)); },
            [host, approx](const Input& i, const Output& o, std::string& why) {
                const float x = get_s(i.cpu, 0), want = host(x), got = get_s(o.cpu, 0);
                if (approx) g_max_ulp = std::max(g_max_ulp, ulps(got, want));
                if (approx ? ulps(got, want) > 1 : !fp_same(got, want)) {
                    char b[160];
                    std::snprintf(b, sizeof b, "f(%a [%08x]) = %a [%08x], host %a [%08x]", x, std::bit_cast<uint32_t>(x), got,
                                  std::bit_cast<uint32_t>(got), want, std::bit_cast<uint32_t>(want));
                    return why = b, false;
                }
                return true;
            },
            approx);
    };
    f1("sqrtf", [](float x) { return std::sqrt(x); }, false);
    f1("floorf", [](float x) { return std::floor(x); }, false);
    f1("ceilf", [](float x) { return std::ceil(x); }, false);
    f1("truncf", [](float x) { return std::trunc(x); }, false);
    f1("roundf", [](float x) { return std::round(x); }, false);
    f1("sinf", [](float x) { return float(std::sin(double(x))); }, true);
    f1("cosf", [](float x) { return float(std::cos(double(x))); }, true);
    f1("expf", [](float x) { return float(std::exp(double(x))); }, true);
    f1("logf", [](float x) { return float(std::log(double(x))); }, true);
    add("fmodf", [](Rng& r, Input& i) { set_s(i.cpu, 0, any_float(r)), set_s(i.cpu, 1, any_float(r)); },
        [](const Input& i, const Output& o, std::string& why) {
            const float a = get_s(i.cpu, 0), b = get_s(i.cpu, 1), want = std::fmod(a, b), got = get_s(o.cpu, 0);
            if (!fp_same(got, want)) {
                char s[160];
                std::snprintf(s, sizeof s, "fmodf(%a, %a) = %a, host %a", a, b, got, want);
                return why = s, false;
            }
            return true;
        });
    add("floor", [](Rng& r, Input& i) { set_d(i.cpu, 0, any_double(r)); },
        [](const Input& i, const Output& o, std::string& why) {
            const double x = get_d(i.cpu, 0);
            if (!fp_same(get_d(o.cpu, 0), std::floor(x))) return why = "value", false;
            return true;
        });
    add("fmod", [](Rng& r, Input& i) { set_d(i.cpu, 0, any_double(r)), set_d(i.cpu, 1, any_double(r)); },
        [](const Input& i, const Output& o, std::string& why) {
            if (!fp_same(get_d(o.cpu, 0), std::fmod(get_d(i.cpu, 0), get_d(i.cpu, 1)))) return why = "value", false;
            return true;
        });
    return t;
}

// Leaf functions of main that use only integer registers (no memory, FP, calls or system instructions): the first few by
// address with at least 12 instructions. Random registers in, the whole register file compared.
std::vector<Test> main_leaf_tests(const Inputs& in, size_t count) {
    using nxw::a64::Op;
    static const std::set<Op> ok = {
        Op::ADD_imm, Op::ADDS_imm, Op::SUB_imm, Op::SUBS_imm, Op::AND_imm, Op::ORR_imm, Op::EOR_imm, Op::ANDS_imm, Op::MOVN,
        Op::MOVZ, Op::MOVK, Op::SBFM, Op::BFM, Op::UBFM, Op::EXTR, Op::B_cond, Op::B, Op::CBZ, Op::CBNZ, Op::TBZ, Op::TBNZ,
        Op::RET, Op::RETAA, Op::HINT, Op::AND_reg, Op::BIC_reg, Op::ORR_reg, Op::ORN_reg, Op::EOR_reg, Op::EON_reg,
        Op::ANDS_reg, Op::BICS_reg, Op::ADD_reg, Op::ADDS_reg, Op::SUB_reg, Op::SUBS_reg, Op::ADD_ext, Op::ADDS_ext,
        Op::SUB_ext, Op::SUBS_ext, Op::ADC, Op::ADCS, Op::SBC, Op::SBCS, Op::CCMN_reg, Op::CCMP_reg, Op::CCMN_imm,
        Op::CCMP_imm, Op::CSEL, Op::CSINC, Op::CSINV, Op::CSNEG, Op::MADD, Op::MSUB, Op::SMADDL, Op::SMSUBL, Op::UMADDL,
        Op::UMSUBL, Op::SMULH, Op::UMULH, Op::UDIV, Op::SDIV, Op::LSLV, Op::LSRV, Op::ASRV, Op::RORV, Op::RBIT, Op::REV16,
        Op::REV32, Op::REV_w, Op::REV_x, Op::CLZ, Op::CLS};
    std::vector<Test> t;
    const auto& db = in.db;
    std::set<uint32_t> calling;   // functions with a BL or a branch that leaves them (tail calls)
    for (const auto& c : db.calls) calling.insert(c.function);
    for (uint32_t m = 0; m < db.modules.size() && t.size() < count; m++) {
        if (std::string(db.str(db.modules[m].name)) != "main") continue;
        const auto& mr = db.modules[m];
        for (uint32_t f = mr.first_function; f < mr.first_function + mr.function_count && t.size() < count; f++) {
            const auto& r = db.functions[f];
            if (!(r.flags & nxanalyze::fn_leaf) || calling.count(f) || r.size < 48 || r.size > 600 || r.block_count < 2) continue;
            const uint32_t* w = in.words(f);
            bool good = true;
            std::set<Op> used;
            for (uint32_t k = 0; k < r.size / 4 && good; k++) {
                const Op op = nxw::a64::decode(w[k]);
                good = ok.count(op) != 0;
                used.insert(op);
            }
            if (!good || used.size() < 6) continue;
            Test x;
            x.name = "main:" + in.fn_name(f);
            x.fn = f;
            x.gen = [](Rng& r, Input& i) {
                for (int k = 0; k < 8; k++)
                    switch (r() % 4) {
                    case 0: i.cpu.x[k] = r() % 16; break;
                    case 1: i.cpu.x[k] = uint64_t(-int64_t(r() % 16)); break;
                    case 2: i.cpu.x[k] = r() & 0xffffffff; break;
                    default: break;   // full 64 bits from base_cpu
                    }
            };
            t.push_back(std::move(x));
        }
    }
    return t;
}

// ---------------------------------------------------------------- phase 2: every op the game uses, alone
// Up to per_op instruction words per op, sampled (reservoir, fixed seed) from every function of every module. Each is lifted
// as [word] + return and run on random states, lifted and interpreted; the states and the memory they touch must agree.
// Branches, SVCs, traps and runtime-handled system registers are covered by phase 1 and the lifter's own control flow.
constexpr uint64_t scratch_base = 0x75'0000'0000, scratch_size = 4 << 20;
constexpr uint64_t op_pc = scratch_base + (2 << 20);                     // the word and a RET live here
constexpr uint64_t data_lo = op_pc + 0x800, data_hi = op_pc + 0x14000;   // what memory operations may touch, compared
constexpr uint64_t data_base = op_pc + 0x1000;                           // base registers point here (+ up to 4 KB)

struct OpCase {
    nxw::a64::Op op;
    uint32_t word;
    bool mem;
    unsigned base;   // the base register of a memory operation
};

bool skip_op(nxw::a64::Op op, uint32_t w) {
    using nxw::a64::Op;
    switch (op) {
    case Op::Unallocated: case Op::B: case Op::BL: case Op::BR: case Op::BLR: case Op::RET: case Op::RETAA: case Op::BRAA:
    case Op::BRAAZ: case Op::BLRAA: case Op::BLRAAZ: case Op::B_cond: case Op::CBZ: case Op::CBNZ: case Op::TBZ: case Op::TBNZ:
    case Op::SVC: case Op::HVC: case Op::SMC: case Op::BRK: case Op::HLT: case Op::UDF: case Op::DCPS1: case Op::DCPS2:
    case Op::DCPS3: case Op::ERET: case Op::ERETAA: case Op::DRPS: case Op::MSR_imm: return true;
    case Op::MRS: return !nxw_sem_sysreg_inline(w >> 5 & 0x7fff, 0);
    case Op::MSR_reg: return !nxw_sem_sysreg_inline(w >> 5 & 0x7fff, 1) && (w >> 5 & 0x7fff) != nxw::sem::sr_fpcr;
    default: return false;
    }
}

std::vector<OpCase> sample_ops(const Inputs& in, const Lifter& L, unsigned per_op) {
    std::map<unsigned, std::vector<uint32_t>> pick;
    std::map<unsigned, uint64_t> seen;
    Rng r(42);
    for (uint32_t f = 0; f < in.db.functions.size(); f++) {
        const uint32_t* w = in.words(f);
        for (uint32_t k = 0; k < in.db.functions[f].size / 4; k++) {
            const auto op = nxw::a64::decode(w[k]);
            if (skip_op(op, w[k])) continue;
            auto& v = pick[unsigned(op)];
            const uint64_t n = ++seen[unsigned(op)];
            if (v.size() < per_op) v.push_back(w[k]);
            else if (r() % n < per_op) v[r() % per_op] = w[k];
        }
    }
    std::vector<OpCase> out;
    for (auto& [o, ws] : pick) {
        const auto op = nxw::a64::Op(o);
        const std::string name = nxw::a64::op_name(op);
        if (!L.has_semantics(name)) continue;
        const bool mem = name.starts_with("LD") || name.starts_with("ST") || name.starts_with("CAS") || name.starts_with("SWP") ||
                         name.starts_with("PRF") || op == nxw::a64::Op::SYS;
        std::sort(ws.begin(), ws.end());
        ws.erase(std::unique(ws.begin(), ws.end()), ws.end());
        for (uint32_t w : ws) out.push_back({op, w, mem, op == nxw::a64::Op::SYS ? (w & 31) : (w >> 5 & 31)});
    }
    return out;
}

void random_fp_lanes(NxwGuestCpu& c, Rng& r) {
    for (int q = 0; q < 32; q++) {
        if (r() % 2) {
            for (int l = 0; l < 4; l++) {
                const float v = any_float(r);
                std::memcpy(c.q[q] + 4 * l, &v, 4);
            }
        } else if (r() % 2) {
            for (int l = 0; l < 2; l++) {
                const double v = any_double(r);
                std::memcpy(c.q[q] + 8 * l, &v, 8);
            }
        }
    }
}

}  // namespace

int main() {
    const char* dump = std::getenv("NXW_DUMP");
    if (!dump || !*dump) {
        std::printf("NXW_DUMP not set: skipped\n");
        return 77;
    }
    const char* dbe = std::getenv("NXW_PROGRAM_DB");
    const std::filesystem::path dbp = dbe && *dbe ? std::filesystem::path(dbe) : std::filesystem::path(NXW_SOURCE_DIR) / "_local/analyze/2.0.1/program.db";
    if (!std::filesystem::exists(dbp)) {
        std::printf("no program.db at %s: skipped\n", dbp.string().c_str());
        return 77;
    }
    const char* it = std::getenv("NXW_TEST_ITERS");
    const unsigned iters = it ? unsigned(std::atoi(it)) : 3000;
    std::set<std::string> only;
    if (const char* o = std::getenv("NXW_TEST_ONLY")) {
        std::stringstream ss(o);
        for (std::string s; std::getline(ss, s, ',');) only.insert(s);
    }
    AddVectoredExceptionHandler(1, on_exception);
    SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);

    try {
        using Clock = std::chrono::steady_clock;
        const auto t0 = Clock::now();
        const Inputs in = load_inputs(dump, dbp, true);

        // the modules at their own addresses, then the stack, buffers and TLS
        g_mod_lo = in.prog.bases.front(), g_mod_hi = in.prog.end();
        map_at(g_mod_lo, g_mod_hi - g_mod_lo);
        for (size_t m = 0; m < in.prog.modules.size(); m++)
            std::memcpy(gptr(in.prog.bases[m]), in.prog.memory[m].data(), in.prog.memory[m].size());
        map_at(stack_base, stack_size);
        map_at(buf_base, buf_size);
        map_at(tls_base, tls_size);

        std::vector<Test> tests = make_tests(in);
        for (Test& t : main_leaf_tests(in, 5)) tests.push_back(std::move(t));
        if (!only.empty()) std::erase_if(tests, [&](const Test& t) { return !only.count(t.name); });

        // lift every test function and its callees into one module, JIT it
        std::vector<uint32_t> roots;
        for (const Test& t : tests) roots.push_back(t.fn);
        const std::vector<uint32_t> fns = call_closure(in, roots);
        const char* ow = std::getenv("NXW_TEST_OP_WORDS");
        const unsigned op_words = ow ? unsigned(std::atoi(ow)) : 24;
        const char* orn = std::getenv("NXW_TEST_OP_RUNS");
        const unsigned op_runs = orn ? unsigned(std::atoi(orn)) : 8;
        std::vector<OpCase> ops;
        auto ctx = std::make_unique<llvm::LLVMContext>();
        std::unique_ptr<llvm::Module> M;
        std::string cpu_name, features;
        llvm::Triple triple;
        Options opt;
        opt.jit = true;
        LiftStats stats;
        {
            Lifter L(in, *ctx, opt);
            M = L.new_module("tests");
            for (uint32_t f : fns) L.lift(*M, f);
            if (op_words && only.empty()) {
                ops = sample_ops(in, L, op_words);
                for (size_t k = 0; k < ops.size(); k++) L.lift_words(*M, "fn_op" + std::to_string(k), {ops[k].word}, op_pc);
            }
            // callees bound to host code (none expected here) trap; other undefined guest functions are interpreted
            {
                llvm::IRBuilder<> b(*ctx);
                llvm::PointerType* ptr = llvm::PointerType::getUnqual(*ctx);
                llvm::FunctionType* fty = llvm::FunctionType::get(b.getVoidTy(), {ptr}, false);
                llvm::FunctionCallee tramp = M->getOrInsertFunction("nxw_interp_trampoline", fty);
                llvm::FunctionCallee trap = M->getOrInsertFunction("rt_trap", llvm::FunctionType::get(b.getVoidTy(), {ptr, b.getInt64Ty(), b.getInt32Ty()}, false));
                std::vector<llvm::Function*> undefined;
                for (llvm::Function& f : *M)
                    if (f.isDeclaration() && (f.getName().starts_with("fn_") || f.getName().starts_with("adapter_"))) undefined.push_back(&f);
                for (llvm::Function* f : undefined) {
                    b.SetInsertPoint(llvm::BasicBlock::Create(*ctx, "entry", f));
                    if (f->getName().starts_with("fn_")) {
                        const uint64_t a = std::stoull(f->getName().substr(3).str(), nullptr, 16);
                        b.CreateStore(b.getInt64(a), b.CreateConstInBoundsGEP1_64(b.getInt8Ty(), f->getArg(0), offsetof(NxwGuestCpu, pc)));
                        b.CreateCall(tramp, {f->getArg(0)});
                        b.CreateRetVoid();
                    } else {
                        b.CreateCall(trap, {f->getArg(0), b.getInt64(0), b.getInt32(NXW_TRAP_SYSTEM)});
                        b.CreateUnreachable();
                    }
                }
            }
            L.finish(*M);
            if (const char* ll = std::getenv("NXW_TEST_LL_PRE")) {
                std::error_code ec;
                llvm::raw_fd_ostream os(ll, ec);
                M->print(os, nullptr);
            }
            L.optimize(*M);
            if (const char* ll = std::getenv("NXW_TEST_LL")) {   // the test module's optimised IR, for debugging
                std::error_code ec;
                llvm::raw_fd_ostream os(ll, ec);
                M->print(os, nullptr);
            }
            cpu_name = L.target().getTargetCPU().str();
            features = L.target().getTargetFeatureString().str();
            triple = M->getTargetTriple();
            stats = L.stats;
        }
        const auto t1 = Clock::now();

        auto jtmb = llvm::orc::JITTargetMachineBuilder(triple);
        jtmb.setCPU(cpu_name);
        jtmb.getFeatures().addFeaturesVector(llvm::SubtargetFeatures(features).getFeatures());
        jtmb.setCodeModel(llvm::CodeModel::Large);
        jtmb.setCodeGenOptLevel(llvm::CodeGenOptLevel::Default);
        auto jit = llvm::orc::LLJITBuilder().setJITTargetMachineBuilder(std::move(jtmb)).create();
        if (!jit) {
            llvm::errs() << "cannot create the JIT: " << jit.takeError() << "\n";
            return 1;
        }
        auto& J = **jit;
        llvm::orc::MangleAndInterner mangle(J.getExecutionSession(), J.getDataLayout());
        llvm::orc::SymbolMap syms;
        auto def = [&](const char* name, const void* addr) {
            syms[mangle(name)] = {llvm::orc::ExecutorAddr::fromPtr(addr), llvm::JITSymbolFlags::Exported};
        };
        def("rt_svc", reinterpret_cast<const void*>(&rt_svc));
        def("rt_mrs", reinterpret_cast<const void*>(&rt_mrs));
        def("rt_msr", reinterpret_cast<const void*>(&rt_msr));
        def("rt_trap", reinterpret_cast<const void*>(&rt_trap));
        def("rt_miss", reinterpret_cast<const void*>(&rt_miss));
        def("rt_yield", reinterpret_cast<const void*>(&rt_yield));
        def("rt_nonlocal", reinterpret_cast<const void*>(&rt_nonlocal));
        def("rt_fiber_switch", reinterpret_cast<const void*>(&rt_fiber_switch));
        def("rt_code_table", &rt_code_table);
        def("rt_code_base", &rt_code_base);
        def("rt_code_words", &rt_code_words);
        def("rt_host_base", &rt_host_base);
        def("nxw_interp_trampoline", reinterpret_cast<const void*>(&nxw_interp_trampoline));
        def("memcpy", reinterpret_cast<const void*>(&std::memcpy));
        def("memmove", reinterpret_cast<const void*>(&std::memmove));
        def("memset", reinterpret_cast<const void*>(&std::memset));
        def("__chkstk", reinterpret_cast<const void*>(&__chkstk));
        def("_fltused", &_fltused_dummy);
        if (auto e = J.getMainJITDylib().define(llvm::orc::absoluteSymbols(std::move(syms)))) {
            llvm::errs() << e << "\n";
            return 1;
        }
        if (auto e = J.addIRModule(llvm::orc::ThreadSafeModule(std::move(M), std::move(ctx)))) {
            llvm::errs() << "cannot add the module: " << e << "\n";
            return 1;
        }
        // look every lifted function up (compiles the module) and fill the D4 table
        std::map<uint32_t, NxwGuestFn> host;
        uint64_t lo = ~0ull, hi = 0;
        for (uint32_t f : fns) {
            auto a = J.lookup(in.fn_name(f));
            if (!a) {
                llvm::errs() << "lookup " << in.fn_name(f) << ": " << a.takeError() << "\n";
                return 1;
            }
            host[f] = a->toPtr<NxwGuestFn>();
            lo = std::min(lo, a->getValue()), hi = std::max(hi, a->getValue());
        }
        const auto t2 = Clock::now();
        if (hi - lo > 0xffff0000ull) {
            std::printf("JIT code spans more than 4 GB; the D4 table needs it closer\n");
            return 1;
        }
        rt_code_base = g_mod_lo, rt_code_words = (g_mod_hi - g_mod_lo) / 4, rt_host_base = lo - 16;
        auto* table = static_cast<uint32_t*>(VirtualAlloc(nullptr, rt_code_words * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        for (auto& [f, p] : host) table[(in.fn_address(f) - rt_code_base) / 4] = uint32_t(reinterpret_cast<uint64_t>(p) - rt_host_base);
        rt_code_table = table;

        std::printf("nxlift-tests: %zu tests, %zu functions lifted (with callees, %llu instructions), lift+opt %.1f s, JIT %.1f s, "
                    "%u inputs each\n",
                    tests.size(), fns.size(), static_cast<unsigned long long>(stats.instructions),
                    std::chrono::duration<double>(t1 - t0).count(), std::chrono::duration<double>(t2 - t1).count(), iters);
        if (!stats.unsupported.empty()) {
            std::printf("  instructions without semantics in the lifted set:");
            for (auto& [op, n] : stats.unsupported) std::printf(" %s x%llu", op.c_str(), static_cast<unsigned long long>(n));
            std::printf("\n");
        }

        unsigned failures = 0;
        std::printf("\n| function | module+offset | insns | inputs | lifted vs interp | vs host | max ulp | interp instr/call |\n"
                    "|---|---|---:|---:|---:|---:|---:|---:|\n");
        for (Test& t : tests) {
            g_current = t.name.c_str();
            Rng r(std::hash<std::string>{}(t.name));
            const uint64_t entry = in.fn_address(t.fn);
            unsigned diff = 0, refbad = 0;
            int64_t max_ulp = 0;
            std::string first;
            nxw::interp::stats() = {};
            for (unsigned k = 0; k < iters; k++) {
                Input i;
                base_cpu(i, r);
                t.gen(r, i);
                const Output a = run(host[t.fn], entry, i, true);
                const Output b = run(nullptr, entry, i, false);
                std::string why;
                if (!same_state(a.cpu, b.cpu, why) || (a.buf != b.buf && (why = "memory", true))) {
                    if (!diff++) first = "lifted vs interpreter, input " + std::to_string(k) + ": " + why;
                }
                if (t.ref) {
                    std::string w2;
                    if (!t.ref(i, a, w2) && !refbad++ && first.empty()) first = "vs host, input " + std::to_string(k) + ": " + w2;
                }
            }
            max_ulp = g_max_ulp;
            g_max_ulp = 0;
            const auto& fr = in.db.functions[t.fn];
            const auto& st = nxw::interp::stats();
            std::printf("| %s | %s+0x%x | %u | %u | %u | %s | %s | %.0f |\n", t.name.c_str(), in.module_name(fr.module), fr.start,
                        fr.size / 4, iters, diff, t.ref ? std::to_string(refbad).c_str() : "-",
                        t.approx ? std::to_string(max_ulp).c_str() : "-", double(st.instructions) / double(iters));
            if (!first.empty()) std::printf("  first problem: %s\n", first.c_str());
            failures += diff + refbad;
        }

        // phase 2
        if (!ops.empty()) {
            map_at(scratch_base, scratch_size);
            Rng r(7);
            for (uint64_t a = scratch_base; a < scratch_base + scratch_size; a += 8) *reinterpret_cast<uint64_t*>(gptr(a)) = r();
            std::map<std::string, std::pair<unsigned, unsigned>> per_op;   // op -> words, failing words
            std::map<std::string, std::string> first_bad;
            unsigned bad_words = 0, runs = 0;
            std::vector<uint8_t> window(data_hi - data_lo), out_a(window.size()), out_b(window.size());
            Input in0;
            const auto t3 = Clock::now();
            for (size_t k = 0; k < ops.size(); k++) {
                const OpCase& oc = ops[k];
                const std::string name = nxw::a64::op_name(oc.op);
                g_current = name.c_str();
                auto a = J.lookup("fn_op" + std::to_string(k));
                if (!a) {
                    llvm::errs() << a.takeError() << "\n";
                    return 1;
                }
                const NxwGuestFn f = a->toPtr<NxwGuestFn>();
                reinterpret_cast<uint32_t*>(gptr(op_pc))[0] = oc.word;
                reinterpret_cast<uint32_t*>(gptr(op_pc))[1] = 0xd65f03c0;   // ret
                bool bad = false;
                for (unsigned j = 0; j < op_runs && !bad; j++, runs++) {
                    in0.cpu = {};
                    base_cpu(in0, r);
                    if (r() % 2) random_fp_lanes(in0.cpu, r);
                    for (auto& b : window) b = uint8_t(r());
                    if (oc.mem) {
                        for (int i = 0; i < 31; i++) in0.cpu.x[i] = r() % 4 ? r() % 64 : (r() % 256) * 8;
                        const uint64_t addr = data_base + (r() % 256) * 16;
                        if (oc.base == 31) in0.cpu.sp = addr;
                        else in0.cpu.x[oc.base] = addr;
                        if (r() % 3 == 0) {   // a monitor that matches, so store-exclusives can succeed
                            in0.cpu.excl_addr = addr;
                            std::memcpy(in0.cpu.excl_val, &window[addr - data_lo], 16);
                        }
                    }
                    NxwGuestCpu ca = in0.cpu, cb = in0.cpu;
                    std::memcpy(gptr(data_lo), window.data(), window.size());
                    f(&ca);
                    std::memcpy(out_a.data(), gptr(data_lo), window.size());
                    std::memcpy(gptr(data_lo), window.data(), window.size());
                    cb.pc = op_pc;
                    nxw_interp_trampoline(&cb);
                    std::memcpy(out_b.data(), gptr(data_lo), window.size());
                    std::string why;
                    if (!same_state(ca, cb, why) || (out_a != out_b && (why = "memory", true))) {
                        bad = true;
                        if (!first_bad.count(name)) {
                            char b[200];
                            std::snprintf(b, sizeof b, "fn_op%zu %08x %s: %s", k, oc.word, nxw::a64::print(oc.word, op_pc).c_str(), why.c_str());
                            first_bad[name] = b;
                        }
                    }
                }
                auto& po = per_op[name];
                po.first++;
                if (bad) po.second++, bad_words++;
            }
            const double secs = std::chrono::duration<double>(Clock::now() - t3).count();
            std::printf("\nPer-op check: %zu ops, %zu instruction words from the game, %u random states each (%u runs, %.1f s): "
                        "%u words where lifted and interpreted differ\n",
                        per_op.size(), ops.size(), op_runs, runs, secs, bad_words);
            for (auto& [op, why] : first_bad) std::printf("  %s (%u of %u words): %s\n", op.c_str(), per_op[op].second, per_op[op].first, why.c_str());
            failures += bad_words;
        }
        std::printf("\n%s: %u mismatches\n", failures ? "FAILED" : "PASSED", failures);
        std::fflush(stdout);
        ExitProcess(failures ? 1 : 0);   // skip LLVM's teardown of the JIT
    } catch (const std::exception& e) {
        std::printf("nxlift-tests: %s\n", e.what());
        return 1;
    }
}
