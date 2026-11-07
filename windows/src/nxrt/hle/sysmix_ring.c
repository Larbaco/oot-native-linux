#include "sysmix_ring.h"
#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <string.h>

int sysmix_format_supported(uint32_t type, uint32_t channels, uint32_t rate) {
    return (type == SYSMIX_S16 || type == SYSMIX_F32) && channels == 2 && rate == 48000;
}
size_t sysmix_ring_size(uint32_t type, uint32_t capacity) {
    if ((type != SYSMIX_S16 && type != SYSMIX_F32) || !capacity || capacity >= INT_MAX) return 0;
    size_t slots = (size_t)capacity + 1, metadata = (0x40 + (slots < 16 ? slots : 16) * 8 + 63) & ~(size_t)63;
    return (0x1000 + metadata + ((slots * (type == SYSMIX_S16 ? 4 : 8) + 63) & ~(size_t)63) + 4095) & ~(size_t)4095;
}
static uint32_t index_load(sysmix_ring *r, size_t off) {
    return atomic_load_explicit((_Atomic uint32_t *)(r->memory + off), memory_order_acquire);
}
static void index_store(sysmix_ring *r, size_t off, uint32_t v) {
    atomic_store_explicit((_Atomic uint32_t *)(r->memory + off), v, memory_order_release);
}
int sysmix_ring_open(sysmix_ring *r, void *memory, size_t size, uint32_t type, uint32_t channels, uint32_t rate, uint32_t capacity) {
    size_t need = sysmix_ring_size(type, capacity);
    if (!r || !memory || ((uintptr_t)memory & 4095) || (size & 4095) || !need || size < need ||
        !sysmix_format_supported(type, channels, rate)) return -1;
    sysmix_ring candidate = {.memory = memory, .slots = capacity + 1, .sample_type = type,
        .frame_bytes = type == SYSMIX_S16 ? 4 : 8, .gain = 1.0f};
    if (SYSMIX_PAYLOAD + (size_t)candidate.slots * candidate.frame_bytes > size) return -1;
    candidate.consumer = index_load(&candidate, SYSMIX_READ);
    if (candidate.consumer >= candidate.slots || index_load(&candidate, SYSMIX_WRITE) >= candidate.slots) return -1;
    *r = candidate;
    return 0;
}
int sysmix_ring_filled(sysmix_ring *r) {
    if (!r || !r->memory || r->fault) return -1;
    uint32_t read = index_load(r, SYSMIX_READ), write = index_load(r, SYSMIX_WRITE);
    if (read != r->consumer || read >= r->slots || write >= r->slots) { r->fault = 1; return -1; }
    return (int)(((uint64_t)write + r->slots - read) % r->slots);
}
int sysmix_ring_write(sysmix_ring *r, const void *data, int frames) {
    if (frames < 0 || (!data && frames)) return -1;
    int filled = sysmix_ring_filled(r);
    if (filled < 0) return -1;
    uint32_t free = r->slots - 1 - (uint32_t)filled, n = (uint32_t)frames < free ? (uint32_t)frames : free;
    uint32_t write = index_load(r, SYSMIX_WRITE), first = n < r->slots - write ? n : r->slots - write;
    if (first) memcpy(r->memory + SYSMIX_PAYLOAD + (size_t)write * r->frame_bytes, data, (size_t)first * r->frame_bytes);
    if (n > first) memcpy(r->memory + SYSMIX_PAYLOAD, (const uint8_t *)data + (size_t)first * r->frame_bytes, (size_t)(n - first) * r->frame_bytes);
    index_store(r, SYSMIX_WRITE, (write + n) % r->slots);
    return (int)n;
}
int sysmix_ring_render(sysmix_ring *r, int16_t *stereo, uint32_t frames) {
    memset(stereo, 0, (size_t)frames * 4);
    int filled = sysmix_ring_filled(r);
    if (filled < 0) return -1;
    uint32_t taken = (uint32_t)filled < frames ? (uint32_t)filled : frames;
    for (uint32_t i = 0; i < taken; i++) {
        uint32_t at = (uint32_t)(((uint64_t)r->consumer + i) % r->slots);
        const uint8_t *src = r->memory + SYSMIX_PAYLOAD + (size_t)at * r->frame_bytes;
        int nonzero = 0;
        for (int ch = 0; ch < 2; ch++) {
            float v;
            if (r->sample_type == SYSMIX_S16) { int16_t s; memcpy(&s, src + ch * 2, 2); v = s / 32768.0f; }
            else memcpy(&v, src + ch * 4, 4);
            if (!isfinite(v)) continue;
            v = fmaxf(-1.0f, fminf(1.0f, v * r->gain)) * 32768.0f;
            int32_t s = (int32_t)v;
            stereo[i * 2 + ch] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
            nonzero |= stereo[i * 2 + ch] != 0;
        }
        r->nonzero += nonzero;
    }
    r->consumer = (r->consumer + taken) % r->slots;
    index_store(r, SYSMIX_READ, r->consumer);
    r->frames += taken;
    r->underruns += frames > taken;
    return (int)taken;
}
