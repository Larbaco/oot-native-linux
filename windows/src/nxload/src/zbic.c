// ZBIC (Nintendo SDK >= 22): a zstd frame with magic "ZBIC" and binary-interpolative-coded entropy tables.
// The vendored zstd amalgamation is compiled into this one translation unit with every symbol static, so it cannot clash
// with the zstd stand-ins that nxlift links for LLVM (llvm_stubs.c) or with a real libzstd.
#include <stddef.h>

#define ZSTD_ZBIC_SUPPORT 1
#define ZSTDLIB_VISIBLE static
#define ZSTDLIB_HIDDEN static
#define ZSTDERRORLIB_VISIBLE static
#define ZSTDERRORLIB_HIDDEN static
#undef ZSTD_MULTITHREAD
#include "../third_party/zbic/zstd.h"
#include "../third_party/zbic/zstd.c"

// Returns ZSTD_decompress's result; interpret it with nxload_zbic_is_error/error_name.
size_t nxload_zbic_decompress(void* dst, size_t dst_cap, const void* src, size_t src_size) {
    return ZSTD_decompress(dst, dst_cap, src, src_size);
}

int nxload_zbic_is_error(size_t result) { return ZSTD_isError(result); }
const char* nxload_zbic_error_name(size_t result) {
    return ZSTD_isError(result) ? ZSTD_getErrorName(result) : "not a ZSTD error";
}

// Returns ZSTD_CONTENTSIZE_UNKNOWN / ZSTD_CONTENTSIZE_ERROR when the frame
// cannot be safely decoded as one bounded stream.
unsigned long long nxload_zbic_content_size(const void* src, size_t src_size) {
    return ZSTD_getFrameContentSize(src, src_size);
}
