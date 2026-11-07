// The silent sink of hle_audio.c as an audio_out backend: nxnative's null_sink (dm/host/hle_audio.c, MIT, © seedds), paced by the guest's own clock.
// Every 5 ms of guest time (156,250 ticks of the 31.25 MHz counter) it pulls 240 frames, so buffers are consumed at 48 kHz in the game's time, the
// release events fire as a sound card's would, and the game's audio threads pace exactly as they would with sound. More than 100 ms behind (the process
// was stopped), the count restarts from now instead of pulling the missed time in one go; while the guest clock is frozen (background) nothing is pulled.
#include "dm.h"
#include "audio_out.h"
#include <stdatomic.h>

enum { RATE = 48000, FRAMES = RATE / 200, TICKS_PER_PULL = 156250, TICKS_RESYNC = 3125000 };   // 5 ms; 100 ms
struct audio_dev {
    int ch;
    audio_pull_fn pull; void *ctx;
    _Atomic int run; pthread_t th; int started;
    _Atomic uint64_t pulls;
};

static void *sink_main(void *p) {
    audio_dev *d = p; uint8_t scratch[FRAMES * 2 * 8];
    plat_thread_nice(PLAT_PRIO_ABOVE_NORMAL);   // paces the game's audio in real time (nxnative: QOS_CLASS_USER_INTERACTIVE)
    plat_thread_name("audio silent sink");
    uint64_t bytes = (uint64_t)FRAMES * 2 * (uint64_t)d->ch, t = guest_ticks();
    while (atomic_load_explicit(&d->run, memory_order_acquire)) {
        t += TICKS_PER_PULL;
        uint64_t now;
        for (;;) {   // bounded sleeps: a stop is seen within 10 ms, a frozen guest clock simply holds the next pull
            now = guest_ticks();
            if (now >= t || !atomic_load_explicit(&d->run, memory_order_acquire)) break;
            uint64_t ns = (t - now) * 32;
            plat_sleep_ns(ns < 10000000 ? ns : 10000000);
        }
        if (!atomic_load_explicit(&d->run, memory_order_acquire)) break;
        if (now - t > TICKS_RESYNC) t = now;
        d->pull(d->ctx, scratch, bytes);
        atomic_fetch_add_explicit(&d->pulls, 1, memory_order_relaxed);
    }
    return NULL;
}
static audio_dev *s_open(int ch, int rate, audio_pull_fn pull, audio_ready_fn ready, void *ctx) {
    (void)ready;   // no sound, so nothing to hold in reserve
    if (ch < 1 || ch > 8 || rate != RATE) return NULL;
    audio_dev *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->ch = ch; d->pull = pull; d->ctx = ctx;
    return d;
}
static int s_start(audio_dev *d) {
    if (d->started) return 0;
    atomic_store_explicit(&d->run, 1, memory_order_release);
    if (pthread_create(&d->th, NULL, sink_main, d)) { atomic_store(&d->run, 0); return -1; }
    d->started = 1;
    return 0;
}
static void s_stop(audio_dev *d) {
    if (!d->started) return;
    atomic_store_explicit(&d->run, 0, memory_order_release);
    pthread_join(d->th, NULL);
    d->started = 0;
}
static void s_close(audio_dev *d) { if (d) { s_stop(d); free(d); } }
static uint64_t s_pulls(audio_dev *d) { return atomic_load_explicit(&d->pulls, memory_order_relaxed); }

static const audio_out_backend g_silent = {"silent sink", s_open, s_start, s_stop, s_close, s_pulls};
const audio_out_backend *audio_out_silent(void) { return &g_silent; }
#ifndef NXRT_GFX_VK
const audio_out_backend *audio_out_device(void) { return NULL; }   // built without SDL3: no sound card (front/audio_sdl.c is the device backend)
#endif
