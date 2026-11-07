// SDK 22 accepts REV? and REv?; lowercase v also marks the native platform.
// Decode for comparisons, but preserve the original tag when calling SDK code.
#pragma once
#include <stdint.h>
static inline int audio_revision_number(uint32_t r) {
    uint32_t tag = r & 0xffffffu, n = r >> 24;
    return (tag == 0x564552u || tag == 0x764552u) && n >= '0' && n <= '@' ? (int)n - '0' : -1;
}
