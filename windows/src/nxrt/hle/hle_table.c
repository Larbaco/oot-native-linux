// from nxnative dm/host/hle_table.c (MIT, © seedds): the C library passthroughs (the swhle table) for imports no module defines.
// nxnative wired these straight to Apple's libc, whose AArch64 ABI matches the Switch's (LP64, the same struct tm). On Windows each gets an explicit Switch
// signature instead (L(name) became HLE(name, ...)): long is 64-bit on the Switch, so strtol / strtoul / lroundf return int64_t; struct tm is musl's
// (sw_tm). mktime and localtime_r are forced in hle_time.c, which shadows them here (their entries are left out). Pokopia binds none of these (its sdk defines its own C library).
#include "hle.h"
#include <ctype.h>
#include <math.h>
#include "sw_time.h"

HLE("memcpy", void *, sw_memcpy, void *d, const void *s, size_t n) { return memcpy(d, s, n); }
HLE("memset", void *, sw_memset, void *d, int c, size_t n) { return memset(d, c, n); }
HLE("memmove", void *, sw_memmove, void *d, const void *s, size_t n) { return memmove(d, s, n); }
HLE("memcmp", int, sw_memcmp, const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
HLE("memchr", void *, sw_memchr, const void *s, int c, size_t n) { return (void *)memchr(s, c, n); }
HLE("strlen", size_t, sw_strlen, const char *s) { return strlen(s); }
HLE("strnlen", size_t, sw_strnlen, const char *s, size_t n) { return strnlen(s, n); }
HLE("strcmp", int, sw_strcmp, const char *a, const char *b) { return strcmp(a, b); }
HLE("strncmp", int, sw_strncmp, const char *a, const char *b, size_t n) { return strncmp(a, b, n); }
HLE("strcpy", char *, sw_strcpy, char *d, const char *s) { return strcpy(d, s); }
HLE("strncpy", char *, sw_strncpy, char *d, const char *s, size_t n) { return strncpy(d, s, n); }
HLE("strcat", char *, sw_strcat, char *d, const char *s) { return strcat(d, s); }
HLE("strncat", char *, sw_strncat, char *d, const char *s, size_t n) { return strncat(d, s, n); }
HLE("strchr", char *, sw_strchr, const char *s, int c) { return (char *)strchr(s, c); }
HLE("strrchr", char *, sw_strrchr, const char *s, int c) { return (char *)strrchr(s, c); }
HLE("strstr", char *, sw_strstr, const char *h, const char *n) { return (char *)strstr(h, n); }
HLE("strspn", size_t, sw_strspn, const char *s, const char *a) { return strspn(s, a); }
HLE("strcspn", size_t, sw_strcspn, const char *s, const char *a) { return strcspn(s, a); }
HLE("strpbrk", char *, sw_strpbrk, const char *s, const char *a) { return (char *)strpbrk(s, a); }
HLE("strcasecmp", int, sw_strcasecmp, const char *a, const char *b) { return strcasecmp(a, b); }
HLE("strncasecmp", int, sw_strncasecmp, const char *a, const char *b, size_t n) { return strncasecmp(a, b, n); }
HLE("atoi", int, sw_atoi, const char *s) { return atoi(s); }
HLE("atof", double, sw_atof, const char *s) { return atof(s); }
HLE("strtol", int64_t, sw_strtol, const char *s, char **e, int b) { return strtoll(s, e, b); }      // the Switch's long
HLE("strtoul", uint64_t, sw_strtoul, const char *s, char **e, int b) { return strtoull(s, e, b); }
HLE("strtoll", int64_t, sw_strtoll, const char *s, char **e, int b) { return strtoll(s, e, b); }
HLE("strtoull", uint64_t, sw_strtoull, const char *s, char **e, int b) { return strtoull(s, e, b); }
HLE("strtod", double, sw_strtod, const char *s, char **e) { return strtod(s, e); }
HLE("strtof", float, sw_strtof, const char *s, char **e) { return strtof(s, e); }
HLE("sqrtf", float, sw_sqrtf, float x) { return sqrtf(x); }
HLE("cbrtf", float, sw_cbrtf, float x) { return cbrtf(x); }
HLE("expf", float, sw_expf, float x) { return expf(x); }
HLE("acosf", float, sw_acosf, float x) { return acosf(x); }
HLE("cosf", float, sw_cosf, float x) { return cosf(x); }
HLE("sinf", float, sw_sinf, float x) { return sinf(x); }
HLE("atan2f", float, sw_atan2f, float y, float x) { return atan2f(y, x); }
HLE("fmodf", float, sw_fmodf, float x, float y) { return fmodf(x, y); }
HLE("cos", double, sw_cos, double x) { return cos(x); }
HLE("sin", double, sw_sin, double x) { return sin(x); }
HLE("powf", float, sw_powf, float x, float y) { return powf(x, y); }
HLE("atanf", float, sw_atanf, float x) { return atanf(x); }
HLE("tanf", float, sw_tanf, float x) { return tanf(x); }
HLE("asinf", float, sw_asinf, float x) { return asinf(x); }
HLE("log10", double, sw_log10, double x) { return log10(x); }
HLE("pow", double, sw_pow, double x, double y) { return pow(x, y); }
HLE("logf", float, sw_logf, float x) { return logf(x); }
HLE("ldexpf", float, sw_ldexpf, float x, int e) { return ldexpf(x, e); }
HLE("log10f", float, sw_log10f, float x) { return log10f(x); }
HLE("log", double, sw_log, double x) { return log(x); }
HLE("ldexp", double, sw_ldexp, double x, int e) { return ldexp(x, e); }
HLE("frexp", double, sw_frexp, double x, int *e) { return frexp(x, e); }
HLE("atan2", double, sw_atan2, double y, double x) { return atan2(y, x); }
HLE("modff", float, sw_modff, float x, float *i) { return modff(x, i); }
HLE("exp2f", float, sw_exp2f, float x) { return exp2f(x); }
HLE("exp2", double, sw_exp2, double x) { return exp2(x); }
HLE("log2f", float, sw_log2f, float x) { return log2f(x); }
HLE("log2", double, sw_log2, double x) { return log2(x); }
HLE("lroundf", int64_t, sw_lroundf, float x) { return llroundf(x); }   // the Switch's long
HLE("timegm", int64_t, sw_timegm, sw_tm *tm) {
    struct tm h; sw_tm_to_host(&h, tm);
    time_t v = timegm(&h);
    if (v != (time_t)-1) sw_tm_from_host(tm, &h, v, 0);
    return (int64_t)v;
}
HLE("gmtime_r", sw_tm *, sw_gmtime_r, const int64_t *t, sw_tm *out) { return sw_gmtime((time_t)*t, out); }
HLE("strftime", size_t, sw_strftime, char *s, size_t n, const char *fmt, const sw_tm *tm) { struct tm h; sw_tm_to_host(&h, tm); return strftime(s, n, fmt, &h); }

#include "hle_table.adapters.inc"
