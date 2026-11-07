// SDK 22 output ring protocol. Original implementation; no emulator code is linked.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { SYSMIX_READ = 0x44, SYSMIX_WRITE = 0x1004, SYSMIX_PAYLOAD = 0x1040, SYSMIX_BLOCK = 240 };
enum { SYSMIX_S16 = 2, SYSMIX_F32 = 5 };
typedef struct {
    uint8_t *memory;
    uint32_t slots, sample_type, frame_bytes, consumer;
    float gain;
    int fault;
    uint64_t frames, nonzero, underruns;
} sysmix_ring;
int sysmix_format_supported(uint32_t type, uint32_t channels, uint32_t rate);
size_t sysmix_ring_size(uint32_t type, uint32_t capacity);
int sysmix_ring_open(sysmix_ring *r, void *memory, size_t size, uint32_t type, uint32_t channels, uint32_t rate, uint32_t capacity);
int sysmix_ring_filled(sysmix_ring *r); // -1 on corrupt indices
int sysmix_ring_write(sysmix_ring *r, const void *data, int frames); // partial writes, -1 on error
int sysmix_ring_render(sysmix_ring *r, int16_t *stereo, uint32_t frames); // zero pads, -1 on error
