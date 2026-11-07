/* LLVM's official windows-msvc build links zlib and zstd into LLVMSupport but does not ship them. nxlift never compresses
   anything (no compressed debug sections), so these stand-ins report "not available"; crc32 is real, in case it is used. */
#include <stddef.h>

typedef unsigned long uLong;
typedef unsigned int uInt;
typedef unsigned char Bytef;

int compress2(Bytef *dest, uLong *destLen, const Bytef *source, uLong sourceLen, int level) {
    (void)dest, (void)destLen, (void)source, (void)sourceLen, (void)level;
    return -6; /* Z_VERSION_ERROR */
}
uLong compressBound(uLong sourceLen) { return sourceLen + (sourceLen >> 12) + (sourceLen >> 14) + (sourceLen >> 25) + 13; }
int uncompress(Bytef *dest, uLong *destLen, const Bytef *source, uLong sourceLen) {
    (void)dest, (void)destLen, (void)source, (void)sourceLen;
    return -3; /* Z_DATA_ERROR */
}
uLong crc32(uLong crc, const Bytef *buf, uInt len) {
    if (!buf) return 0;
    unsigned c = (unsigned)crc ^ 0xffffffffu;
    for (uInt i = 0; i < len; i++) {
        c ^= buf[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (c & 1 ? 0xedb88320u : 0);
    }
    return c ^ 0xffffffffu;
}

typedef struct ZSTD_CCtx_s ZSTD_CCtx;
#define ZSTD_ERROR_GENERIC ((size_t)-1)
size_t ZSTD_compressBound(size_t srcSize) { return srcSize + (srcSize >> 8) + 64; }
ZSTD_CCtx *ZSTD_createCCtx(void) { return NULL; }
size_t ZSTD_freeCCtx(ZSTD_CCtx *cctx) {
    (void)cctx;
    return 0;
}
size_t ZSTD_CCtx_setParameter(ZSTD_CCtx *cctx, int param, int value) {
    (void)cctx, (void)param, (void)value;
    return ZSTD_ERROR_GENERIC;
}
size_t ZSTD_compress2(ZSTD_CCtx *cctx, void *dst, size_t cap, const void *src, size_t size) {
    (void)cctx, (void)dst, (void)cap, (void)src, (void)size;
    return ZSTD_ERROR_GENERIC;
}
size_t ZSTD_decompress(void *dst, size_t cap, const void *src, size_t size) {
    (void)dst, (void)cap, (void)src, (void)size;
    return ZSTD_ERROR_GENERIC;
}
unsigned ZSTD_isError(size_t code) { return code > (size_t)-120; }
const char *ZSTD_getErrorName(size_t code) {
    (void)code;
    return "zstd is not available in nxlift";
}
