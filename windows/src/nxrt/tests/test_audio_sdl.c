/* The SDL3 sound card (front/audio_sdl.c) against a fake device: SDL's "dummy" audio driver, which plays nothing and consumes in real time, so the test runs
   without a sound card and checks the backend's pacing contract (audio_out.h):
   1. it opens 48 kHz PCM16 only, pulls whole frames at about 48,000 frames a second on SDL's thread, and none after stop returns; it starts again after a stop;
   2. hle_audio.c with DM_AUDIO=1 plays a host output through it: 5 ms blocks kept queued are released at the sound card's rate (48,000 frames a second within
      2 %), in order; the device's callbacks are counted (dm_audio_render_calls), so the silent sink did not stand in.
   3. a device that asks for a whole period of 1024 frames at once (as PipeWire does), with fewer frames than that kept queued (e.g. 3 blocks of 320 frames):
      without the reserve (NXRT_AUDIO_RESERVE=0) a large share of the pulls finds the queue empty; with it none does once playing, at the same rate.
   SDL_AUDIO_DRIVER=dummy is set here; no game data. */
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
void *dm_pcmout_open(int ch);
void dm_pcmout_start(void *k);
void dm_pcmout_close(void *k);
int dm_pcmout_append(void *k, void *b);
void *dm_pcmout_released(void *k);
void dm_pcmout_wait(void *k, int ms);
uint64_t dm_audio_render_calls(void);
uint64_t dm_pcmout_underruns(void *k);

static volatile LONG64 g_bytes, g_calls; static volatile LONG g_bad;
static void count_pull(void *ctx, uint8_t *dst, uint64_t n) {
    if (n % (2 * 2)) InterlockedIncrement(&g_bad);   /* whole stereo frames */
    memset(dst, 0, n); InterlockedAdd64(&g_bytes, (LONG64)n); InterlockedIncrement64(&g_calls); (void)ctx;
}

typedef struct { void *ptr; uint64_t cap, size; } gbuf;
/* As test_audio.c: frames released per second with nbuf blocks of `frames` frames kept queued; *under: pulls that found the queue empty */
static double run_output(int ch, int nbuf, unsigned frames, double secs, int *order_ok, uint64_t *under) {
    void *out = dm_pcmout_open(ch);
    gbuf *b = calloc((size_t)nbuf, sizeof *b);
    for (int i = 0; i < nbuf; i++) { b[i].cap = b[i].size = frames * 2u * (unsigned)ch; b[i].ptr = calloc(1, (size_t)b[i].cap); dm_pcmout_append(out, &b[i]); }
    dm_pcmout_start(out);
    uint64_t t0 = 0, released = 0, end = 0, tw = now_ns(); int next = 0; *order_ok = 1;
    for (;;) {
        gbuf *r = dm_pcmout_released(out);
        if (!r) { if (now_ns() - tw > 5000000000ull) break; dm_pcmout_wait(out, 10); continue; }
        uint64_t now = now_ns();
        if (!t0) t0 = now; else released++;
        *order_ok &= r == &b[next]; next = (next + 1) % nbuf;
        if (now - t0 >= (uint64_t)(secs * 1e9)) { end = now; break; }
        dm_pcmout_append(out, r);
    }
    if (under) *under = dm_pcmout_underruns(out);
    dm_pcmout_close(out);
    return end > t0 ? released * (double)frames / ((end - t0) / 1e9) : 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    _putenv_s("SDL_AUDIO_DRIVER", "dummy");
    if (nxrt_init(&(nxrt_options){.quiet = 1})) { printf("FAIL nxrt_init\n"); return 1; }
    const audio_out_backend *be = audio_out_device();
    T(be && !strcmp(be->name, "SDL3 sound card"));
    if (!be) return 1;
    T(!be->open(2, 44100, count_pull, NULL, NULL));   /* 48 kHz only, as hle_audio.c */
    audio_dev *d = be->open(2, 48000, count_pull, NULL, NULL);
    T(d != NULL);
    if (!d) { printf("nxrt.audio_sdl: FAIL (no dummy device)\n"); return 1; }
    /* 1. the backend alone */
    T(be->start(d) == 0);
    Sleep(300);   /* SDL fills its buffer first: measured after that */
    LONG64 b0 = g_bytes; uint64_t t0 = now_ns();
    Sleep(1500);
    LONG64 b1 = g_bytes; uint64_t t1 = now_ns();
    be->stop(d);
    LONG64 after_stop = g_bytes; Sleep(300);
    double rate = (b1 - b0) / 4.0 / ((t1 - t0) / 1e9);
    printf("SDL3 backend on the dummy driver: %.0f frames/s pulled (want 48000) in %lld pulls; %lld bytes pulled after stop returned\n", rate, (long long)g_calls,
           (long long)(g_bytes - after_stop));
    T(rate > 45600 && rate < 50400);   /* the dummy driver sleeps in whole milliseconds: 5 % */
    T(g_bytes == after_stop && !g_bad);
    T(be->pulls(d) > 0);
    LONG64 c0 = g_calls; T(be->start(d) == 0); Sleep(300); be->stop(d);
    T(g_calls > c0);   /* starts again */
    be->close(d);
    /* 2. through hle_audio.c */
    _putenv_s("DM_AUDIO", "1");
    uint64_t rc0 = dm_audio_render_calls(); int order = 0;
    double r2 = run_output(2, 6, 240, 2.0, &order, NULL);
    printf("stereo output through hle_audio.c on the device backend, 6 blocks of 5 ms kept queued: %.0f frames/s (want 48000), %llu device callbacks\n", r2,
           (unsigned long long)(dm_audio_render_calls() - rc0));
    T(r2 > 47040 && r2 < 48960 && order);
    T(dm_audio_render_calls() - rc0 > 50);
    /* 3. whole periods of 1024 frames asked at once, 960 frames kept queued */
    _putenv_s("SDL_AUDIO_DEVICE_SAMPLE_FRAMES", "1024");
    for (int reserve = 0; reserve < 2; reserve++) {
        _putenv_s("NXRT_AUDIO_RESERVE", reserve ? "1" : "0");
        uint64_t under = 0; rc0 = dm_audio_render_calls();
        double r3 = run_output(2, 3, 320, 2.0, &order, &under);
        uint64_t pulls = dm_audio_render_calls() - rc0;
        printf("1024-frame periods, 3 blocks of 320 frames kept queued, %s: %.0f frames/s, %llu pulls, %llu of them found the queue empty\n",
               reserve ? "with the reserve" : "without the reserve", r3, (unsigned long long)pulls, (unsigned long long)under);
        T(order && pulls > 50);
        if (reserve) T(under <= 2 && r3 > 47040 && r3 < 48960);   /* at most the first request runs dry, before the reserve has filled */
        else T(under * 4 > pulls);   /* the case reproduces: a large share of the pulls ran dry (and the padding slows the releases) */
    }
    printf("nxrt.audio_sdl: %s\n", g_fail ? "FAIL" : "ok");
    return g_fail;
}
