// LLVM 23.1.4's official windows-msvc libraries were compiled with a newer MSVC STL than the Visual Studio 2022 Build Tools
// on this PC (14.38) link against: they call the STL's vectorized algorithm helpers and _Thrd_sleep_for, which 14.38's
// libcpmt.lib lacks. These are plain implementations with the same declarations (Microsoft STL, Apache-2.0 WITH
// LLVM-exception: stl/inc/xutility, algorithm, xmemory, __msvc_string_view.hpp, xthreads.h). Updating the Build Tools to
// 17.14 or later makes them unnecessary; then delete this file.
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <windows.h>

namespace {

template <class T> size_t mismatch(const void* a, const void* b, size_t n) {
    const T* x = static_cast<const T*>(a);
    const T* y = static_cast<const T*>(b);
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i]) return i;
    return n;
}
template <class T> T min_of(const void* f, const void* l) {
    const T* p = static_cast<const T*>(f);
    const T* e = static_cast<const T*>(l);
    T m = *p;
    for (; p != e; ++p)
        if (*p < m) m = *p;
    return m;
}
template <class T> T max_of(const void* f, const void* l) {
    const T* p = static_cast<const T*>(f);
    const T* e = static_cast<const T*>(l);
    T m = *p;
    for (; p != e; ++p)
        if (*p > m) m = *p;
    return m;
}
template <class T> void* remove_val(void* f, void* l, T v) {
    T* out = static_cast<T*>(f);
    for (T* p = static_cast<T*>(f); p != static_cast<T*>(l); ++p)
        if (*p != v) *out++ = *p;
    return out;
}
template <class T> void replace_val(void* f, void* l, T o, T n) {
    for (T* p = static_cast<T*>(f); p != static_cast<T*>(l); ++p)
        if (*p == o) *p = n;
}

}  // namespace

extern "C" {

size_t __stdcall __std_mismatch_1(const void* a, const void* b, size_t n) noexcept { return mismatch<uint8_t>(a, b, n); }
size_t __stdcall __std_mismatch_2(const void* a, const void* b, size_t n) noexcept { return mismatch<uint16_t>(a, b, n); }
size_t __stdcall __std_mismatch_4(const void* a, const void* b, size_t n) noexcept { return mismatch<uint32_t>(a, b, n); }
size_t __stdcall __std_mismatch_8(const void* a, const void* b, size_t n) noexcept { return mismatch<uint64_t>(a, b, n); }

int32_t __stdcall __std_min_4i(const void* f, const void* l) noexcept { return min_of<int32_t>(f, l); }
uint32_t __stdcall __std_min_4u(const void* f, const void* l) noexcept { return min_of<uint32_t>(f, l); }
uint64_t __stdcall __std_max_8u(const void* f, const void* l) noexcept { return max_of<uint64_t>(f, l); }

const void* __stdcall __std_search_1(const void* f1, const void* l1, const void* f2, size_t n2) noexcept {
    const auto* h = static_cast<const uint8_t*>(f1);
    const auto* e = static_cast<const uint8_t*>(l1);
    if (n2 == 0) return f1;
    for (; size_t(e - h) >= n2; ++h)
        if (std::memcmp(h, f2, n2) == 0) return h;
    return l1;
}
const void* __stdcall __std_find_end_1(const void* f1, const void* l1, const void* f2, size_t n2) noexcept {
    const auto* b = static_cast<const uint8_t*>(f1);
    const auto* e = static_cast<const uint8_t*>(l1);
    if (n2 == 0 || size_t(e - b) < n2) return l1;
    for (const uint8_t* h = e - n2;; --h) {
        if (std::memcmp(h, f2, n2) == 0) return h;
        if (h == b) break;
    }
    return l1;
}
const void* __stdcall __std_find_last_trivial_1(const void* f, const void* l, uint8_t v) noexcept {
    const auto* b = static_cast<const uint8_t*>(f);
    for (const auto* p = static_cast<const uint8_t*>(l); p != b;)
        if (*--p == v) return p;
    return l;
}
size_t __stdcall __std_find_first_of_trivial_pos_1(const void* hay, size_t hn, const void* needle, size_t nn) noexcept {
    const auto* h = static_cast<const uint8_t*>(hay);
    const auto* n = static_cast<const uint8_t*>(needle);
    for (size_t i = 0; i < hn; i++)
        for (size_t j = 0; j < nn; j++)
            if (h[i] == n[j]) return i;
    return size_t(-1);
}

void* __stdcall __std_remove_1(void* f, void* l, uint8_t v) noexcept { return remove_val<uint8_t>(f, l, v); }
void* __stdcall __std_remove_8(void* f, void* l, uint64_t v) noexcept { return remove_val<uint64_t>(f, l, v); }
void __stdcall __std_replace_4(void* f, void* l, uint32_t o, uint32_t n) noexcept { replace_val<uint32_t>(f, l, o, n); }
void __stdcall __std_replace_8(void* f, void* l, uint64_t o, uint64_t n) noexcept { replace_val<uint64_t>(f, l, o, n); }

void __stdcall _Thrd_sleep_for(unsigned long ms) noexcept { Sleep(ms); }

}  // extern "C"
