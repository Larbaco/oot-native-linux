// struct tm as the game's C library (the SDK's musl) lays it out: 9 ints, then long tm_gmtoff and const char *tm_zone, 56 bytes on AArch64.
// The Windows CRT's struct tm has only the 9 ints, so every host conversion goes through these helpers (research/02 §3.3: struct tm differs).
#pragma once
#include <stdint.h>
#include <time.h>

typedef struct { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; int64_t tm_gmtoff; const char *tm_zone; } sw_tm;
_Static_assert(sizeof(sw_tm) == 56, "musl's struct tm on AArch64");

static inline void sw_tm_to_host(struct tm *o, const sw_tm *t) {
    o->tm_sec = t->tm_sec; o->tm_min = t->tm_min; o->tm_hour = t->tm_hour; o->tm_mday = t->tm_mday; o->tm_mon = t->tm_mon;
    o->tm_year = t->tm_year; o->tm_wday = t->tm_wday; o->tm_yday = t->tm_yday; o->tm_isdst = t->tm_isdst;
}
// The zone's name and offset come from the CRT: the offset as the broken-down local time read as UTC, less the instant
static inline sw_tm *sw_tm_from_host(sw_tm *o, const struct tm *t, time_t v, int local) {
    o->tm_sec = t->tm_sec; o->tm_min = t->tm_min; o->tm_hour = t->tm_hour; o->tm_mday = t->tm_mday; o->tm_mon = t->tm_mon;
    o->tm_year = t->tm_year; o->tm_wday = t->tm_wday; o->tm_yday = t->tm_yday; o->tm_isdst = t->tm_isdst;
    if (local) { struct tm c = *t; o->tm_gmtoff = (int64_t)(timegm(&c) - v); tzset(); o->tm_zone = plat_tzname(t->tm_isdst > 0 ? 1 : 0); }
    else { o->tm_gmtoff = 0; o->tm_zone = "UTC"; }
    return o;
}
static inline sw_tm *sw_localtime(time_t v, sw_tm *o) { struct tm t; return !localtime_r(&v, &t) ? NULL : sw_tm_from_host(o, &t, v, 1); }
static inline sw_tm *sw_gmtime(time_t v, sw_tm *o) { struct tm t; return !gmtime_r(&v, &t) ? NULL : sw_tm_from_host(o, &t, v, 0); }
