/* Audio pacing (hle/hle_audio.c, hle/audio_silent.c): the silent sink consumes 48 kHz in the guest's time, so an audio thread paces exactly as with a sound card.
   1. The sink alone pulls 240 frames per 5 ms (200 pulls a second).
   2. An output through hle_audio.c's queues (the host PCM output the in-process renderer uses): 2 s of 5 ms buffers, appended as they come back, are released at
      48,000 frames a second (±1 %), in order, with no underrun once primed; a six-channel output paces the same.
   3. While the guest clock is frozen (the app in the background) nothing is consumed; afterwards the count starts again without catching up. No game data. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nxrt/runtime.h"
#include "audio_out.h"

NXRT_EARLY_RESERVE()

static int g_fail;
#define T(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

uint64_t now_ns(void);
void guest_clock_freeze(int on);
void *dm_pcmout_open(int ch);
void dm_pcmout_start(void *k);
void dm_pcmout_close(void *k);
int dm_pcmout_append(void *k, void *b);
void *dm_pcmout_released(void *k);
void dm_pcmout_wait(void *k, int ms);

static volatile LONG64 g_pulls;
static void count_pull(void *ctx, uint8_t *dst, uint64_t n) { (void)ctx; memset(dst, 0, n); InterlockedIncrement64(&g_pulls); }

typedef struct { void *ptr; uint64_t cap, size; } gbuf;   /* the front of the guest AudioOutBuffer */
/* Keeps nbuf buffers of 240 frames queued for `secs` seconds; returns the frames released per second over that time */
static double run_output(int ch, int nbuf, double secs, int *order_ok) {
    void *out = dm_pcmout_open(ch);
    gbuf *b = calloc((size_t)nbuf, sizeof *b);
    for (int i = 0; i < nbuf; i++) { b[i].cap = b[i].size = 240u * 2u * (unsigned)ch; b[i].ptr = calloc(1, (size_t)b[i].cap); dm_pcmout_append(out, &b[i]); }
    dm_pcmout_start(out);
    uint64_t t0 = 0, released = 0, end = 0; int next = 0; *order_ok = 1;
    for (;;) {
        gbuf *r = dm_pcmout_released(out);
        if (!r) { dm_pcmout_wait(out, 10); continue; }
        uint64_t now = now_ns();
        if (!t0) t0 = now;                   /* timed from the first release: the start-up latency is not the rate */
        else released++;
        *order_ok &= r == &b[next]; next = (next + 1) % nbuf;
        if (now - t0 >= (uint64_t)(secs * 1e9)) { end = now; break; }
        dm_pcmout_append(out, r);
    }
    dm_pcmout_close(out);
    return released * 240.0 / ((end - t0) / 1e9);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (nxrt_init(&(nxrt_options){.quiet = 1})) { printf("FAIL nxrt_init\n"); return 1; }
    /* 1. the sink on its own */
    const audio_out_backend *s = audio_out_silent();
    T(s);   /* audio_out_device(): the SDL3 sound card when built with SDL3 (test_audio_sdl.c), else none */
    audio_dev *d = s->open(2, 48000, count_pull, NULL);
    T(d && !s->open(2, 44100, count_pull, NULL));   /* 48 kHz only, as hle_audio.c */
    uint64_t t0 = now_ns();
    T(s->start(d) == 0);
    Sleep(1000);
    s->stop(d);
    double secs = (now_ns() - t0) / 1e9; LONG64 n = g_pulls;
    printf("silent sink: %lld pulls of 240 frames in %.3f s (%.1f per s, want 200)\n", (long long)n, secs, n / secs);
    T(n >= 195 && n <= 202);
    T(s->pulls(d) == (uint64_t)n);
    /* 3. a frozen guest clock holds the sink */
    g_pulls = 0; s->start(d); Sleep(200);
    guest_clock_freeze(1); LONG64 at_freeze = g_pulls; Sleep(300); LONG64 frozen = g_pulls - at_freeze;
    guest_clock_freeze(0); Sleep(200); s->stop(d);
    printf("silent sink with the guest clock frozen for 0.3 s: %lld pulls while frozen, %lld in all\n", (long long)frozen, (long long)g_pulls);
    T(frozen <= 1);
    T(g_pulls >= 70 && g_pulls <= 90);   /* about 0.4 s of pulls: the frozen time is not made up */
    s->close(d);
    /* 2. outputs through hle_audio.c */
    int order = 0;
    double r2 = run_output(2, 4, 2.0, &order);
    printf("stereo output, 4 buffers of 5 ms kept queued: %.0f frames/s (want 48000)\n", r2);
    T(r2 > 47520 && r2 < 48480 && order);
    double r6 = run_output(6, 3, 1.0, &order);
    printf("six-channel output, 3 buffers: %.0f frames/s\n", r6);
    T(r6 > 47520 && r6 < 48480 && order);
    printf("nxrt.audio: %s\n", g_fail ? "FAIL" : "ok");
    return g_fail;
}
