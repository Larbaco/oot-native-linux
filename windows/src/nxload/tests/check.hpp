// A minimal test harness: TEST(name) registers a test, CHECK / CHECK_EQ record failures, run() returns the exit code.
#pragma once
#include <cstdio>
#include <exception>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace check {

inline int failures = 0;

struct Test { const char* name; void (*fn)(); };
inline std::vector<Test>& registry() {
    static std::vector<Test> r;
    return r;
}
struct Reg {
    Reg(const char* n, void (*f)()) { registry().push_back({n, f}); }
};

inline bool report(bool ok, const char* expr, const char* file, int line) {
    if (!ok) {
        failures++;
        std::printf("  FAIL %s:%d: %s\n", file, line, expr);
    }
    return ok;
}

template <class T> std::string show(const T& v) {
    if constexpr (std::is_convertible_v<T, std::string>) return "\"" + std::string(v) + "\"";
    else if constexpr (std::is_enum_v<T>) return std::to_string(static_cast<long long>(v));
    else return std::to_string(v);
}

template <class T> constexpr bool is_number = std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char>;

template <class A, class B> bool eq(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
    bool ok;
    if constexpr (is_number<A> && is_number<B>) ok = std::cmp_equal(a, b);
    else ok = a == b;
    if (!ok) {
        failures++;
        std::printf("  FAIL %s:%d: %s == %s: %s vs %s\n", file, line, ea, eb, show(a).c_str(), show(b).c_str());
    }
    return ok;
}

template <class F> bool throws(F f) {
    try {
        f();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

inline int run() {
    for (const Test& t : registry()) {
        const int before = failures;
        try {
            t.fn();
        } catch (const std::exception& e) {
            failures++;
            std::printf("  FAIL: threw %s\n", e.what());
        }
        std::printf("%s %s\n", failures == before ? "ok  " : "FAIL", t.name);
    }
    std::printf("%zu tests, %d failed checks\n", registry().size(), failures);
    return failures ? 1 : 0;
}

}  // namespace check

#define CHECK(x) ::check::report(bool(x), #x, __FILE__, __LINE__)
#define CHECK_EQ(a, b) ::check::eq((a), (b), #a, #b, __FILE__, __LINE__)
#define CHECK_THROWS(x) ::check::report(::check::throws([&] { (void)(x); }), "throws: " #x, __FILE__, __LINE__)
#define TEST(name)                                     \
    static void name();                                \
    static const ::check::Reg reg_##name(#name, name); \
    static void name()
