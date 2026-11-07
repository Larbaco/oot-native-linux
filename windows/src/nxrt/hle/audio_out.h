// The host's sound output behind hle_audio.c. nxnative plays through a CoreAudio AudioUnit and keeps a silent sink beside it; here both are backends of one
// interface: the silent sink (audio_silent.c, always there) and a device backend (SDL3, front/audio_sdl.c, DESIGN D14; none without SDL3).
// A backend pulls interleaved PCM16 at 48 kHz from its owner on a thread of its own, at the rate a sound card would.
#pragma once
#include <stdint.h>

typedef struct audio_dev audio_dev;
typedef void (*audio_pull_fn)(void *ctx, uint8_t *dst, uint64_t bytes);   // fills bytes of interleaved PCM16; never blocks, never logs
typedef uint32_t (*audio_ready_fn)(void *ctx);   // frames the owner has queued right now: a pull of up to that many pads nothing; never blocks
typedef struct audio_out_backend {
    const char *name;
    // NULL when it cannot open. ready may be NULL; with it, a device that asks for more at once than the owner keeps queued can hold a reserve (audio_sdl.c)
    audio_dev *(*open)(int channels, int rate, audio_pull_fn pull, audio_ready_fn ready, void *ctx);
    int (*start)(audio_dev *d);                                                  // 0 = pulling
    void (*stop)(audio_dev *d);                                                  // returns once no pull is running or will run
    void (*close)(audio_dev *d);
    uint64_t (*pulls)(audio_dev *d);                                             // pulls so far (the watchdog's evidence that the device still plays)
} audio_out_backend;

// Consumes in real time on the guest's clock: 5 ms (240 frames) every 156,250 ticks of the 31.25 MHz counter, so the game's audio threads pace exactly as
// with a sound card; no sound
const audio_out_backend *audio_out_silent(void);
// The sound card (SDL3), NULL when nxrt is built without SDL3
const audio_out_backend *audio_out_device(void);
