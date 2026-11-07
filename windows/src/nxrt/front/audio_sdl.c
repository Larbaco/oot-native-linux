// The sound card through SDL3 (DESIGN D14): the device backend of audio_out.h behind hle_audio.c (nxnative's CoreAudio AudioUnit on the Mac), next to the
// silent sink (audio_silent.c), which stays the fallback: no SDL3.dll, no device, a device that will not open or start, or one that stops calling back (hle_audio.c's
// watchdog). Written from SDL3's public headers (zlib licence).
// Pacing (audio_out.h): SDL's device thread asks for the bytes the sound card is about to play (the stream's get callback); the callback pulls exactly that many
// whole frames of PCM16 at 48 kHz from hle_audio.c, so blocks are released, and the game's audio threads woken, at the sound card's own rate, as the AudioUnit's
// render callback does on the Mac. The callback never blocks or logs. It runs with the stream's lock held (SDL_audio.h) and pulls only while the device is started:
// stop clears that, pauses the device and takes the lock once, so no pull runs or will run when it returns.
// DM_AUDIO=1 asks hle_audio.c for the device (the default stays the silent sink, as on the Mac); SDL_AUDIO_DRIVER picks SDL's driver (e.g. dummy or disk for tests).
// The reserve: a device whose period is longer than a pull (PipeWire's 1024 frames; WASAPI's 480 is not) asks for the whole period at once, and the game keeps
// fewer frames than that queued (e.g. 3 blocks of 320 frames, 960), so every request ran the queue dry and the rest was padded with silence (crackling on
// PipeWire). There a feeder thread keeps one period (or the largest request seen) queued in the stream from what the owner has ready, never padding, after
// each callback and every 2 ms; the callback then pulls only a shortfall. Releases still follow the sound card: the stream holds at most the reserve.
// NXRT_AUDIO_RESERVE=0 turns it off (pulls as before).
#include "dm.h"
#include "audio_out.h"
#include "front_sdl.h"
#include <SDL3/SDL.h>
#include <stdatomic.h>

enum { RATE = 48000, CHUNK_FRAMES = 512 };
struct audio_dev {
    SDL_AudioStream *st;
    int ch;
    audio_pull_fn pull; audio_ready_fn ready; void *ctx;
    _Atomic int run; int started;
    _Atomic uint64_t pulls;
    int period;                                  // the reserve's least size in frames, 0 = no reserve
    _Atomic int want;                            // the largest request seen, in frames
    SDL_Thread *feeder; SDL_Semaphore *kick; _Atomic int feed_on;
};

// SDL3.dll is delay-loaded (nxrt starts without it under --gfx null): the first call raises an exception when it is missing
#ifdef _WIN32
static int sdl_present(void) { __try { SDL_GetVersion(); return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
#else
static int sdl_present(void) { return 1; }   // linked like any other library
#endif
static pthread_once_t g_up_once = PTHREAD_ONCE_INIT; static int g_up;
static void sdl_audio_start(void) {
    if (!sdl_present()) { LOG("[RED] audio: SDL3.dll is missing next to the executable: no sound card, the silent sink plays"); return; }
    front_sdl_lock(); g_up = SDL_InitSubSystem(SDL_INIT_AUDIO); front_sdl_unlock();
    if (!g_up) LOG("[RED] audio: SDL cannot start its audio subsystem: %s", SDL_GetError());
    else LOG("audio: SDL %d.%d.%d, audio driver %s", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION, SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?");
    return;
}

static void SDLCALL get_cb(void *ud, SDL_AudioStream *st, int additional, int total) {
    audio_dev *d = ud;
    int fb = 2 * d->ch;
    if (d->feeder) {   // the reserve grows to the largest request, and is topped up again now
        if (total / fb > atomic_load_explicit(&d->want, memory_order_relaxed)) atomic_store_explicit(&d->want, total / fb, memory_order_relaxed);
        SDL_SignalSemaphore(d->kick);
    }
    if (additional <= 0) return;
    int frames = (additional + fb - 1) / fb;
    uint8_t buf[CHUNK_FRAMES * 2 * 8];
    while (frames > 0) {
        int n = frames < CHUNK_FRAMES ? frames : CHUNK_FRAMES; uint64_t bytes = (uint64_t)n * (uint64_t)fb;
        if (atomic_load_explicit(&d->run, memory_order_acquire)) { d->pull(d->ctx, buf, bytes); atomic_fetch_add_explicit(&d->pulls, 1, memory_order_relaxed); }
        else memset(buf, 0, (size_t)bytes);
        SDL_PutAudioStreamData(st, buf, (int)bytes);
        frames -= n;
    }
}

// The reserve's feeder: under the stream's lock, as the callback runs, so the two never pull at once; it stops pulling as soon as the device is stopped
static int SDLCALL feed(void *p) {
    audio_dev *d = p; int fb = 2 * d->ch;
    uint8_t buf[CHUNK_FRAMES * 2 * 8];
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH);
    while (atomic_load_explicit(&d->feed_on, memory_order_acquire)) {
        SDL_WaitSemaphoreTimeout(d->kick, 2);
        int target = atomic_load_explicit(&d->want, memory_order_relaxed); if (target < d->period) target = d->period;
        SDL_LockAudioStream(d->st);
        while (atomic_load_explicit(&d->run, memory_order_acquire)) {
            int queued = SDL_GetAudioStreamQueued(d->st), n = (int)d->ready(d->ctx);
            if (queued < 0) break;
            if (n > target - queued / fb) n = target - queued / fb;
            if (n > CHUNK_FRAMES) n = CHUNK_FRAMES;
            if (n <= 0) break;
            d->pull(d->ctx, buf, (uint64_t)n * (uint64_t)fb); atomic_fetch_add_explicit(&d->pulls, 1, memory_order_relaxed);
            SDL_PutAudioStreamData(d->st, buf, n * fb);
        }
        SDL_UnlockAudioStream(d->st);
    }
    return 0;
}

static audio_dev *a_open(int ch, int rate, audio_pull_fn pull, audio_ready_fn ready, void *ctx) {
    if (ch < 1 || ch > 8 || rate != RATE) return NULL;
    pthread_once(&g_up_once, sdl_audio_start);
    if (!g_up) return NULL;
    audio_dev *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->ch = ch; d->pull = pull; d->ready = ready; d->ctx = ctx;
    SDL_AudioSpec spec = {SDL_AUDIO_S16, ch, RATE};
    d->st = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, get_cb, d);   // opens paused
    if (!d->st) { LOG("[RED] audio: the default playback device would not open (%d channels, 48 kHz PCM16): %s", ch, SDL_GetError()); free(d); return NULL; }
    SDL_AudioDeviceID dev = SDL_GetAudioStreamDevice(d->st); SDL_AudioSpec ds; int frames = 0;
    const char *name = SDL_GetAudioDeviceName(dev);
    if (SDL_GetAudioDeviceFormat(dev, &ds, &frames))
        LOG("audio: device '%s' opened: %d Hz, %d channels, %d frames a period (%.1f ms); the game's %d channels at 48 kHz are converted by SDL where they differ",
            name ? name : "?", ds.freq, ds.channels, frames, ds.freq ? 1e3 * frames / ds.freq : 0.0, ch);
    const char *e = getenv("NXRT_AUDIO_RESERVE");
    if (ready && frames > CHUNK_FRAMES && !(e && *e == '0')) {
        d->period = ds.freq > 0 ? (int)((int64_t)frames * RATE / ds.freq) : frames;   // the device's period in the stream's 48 kHz frames
        atomic_init(&d->want, 0); atomic_init(&d->feed_on, 1);
        d->kick = SDL_CreateSemaphore(0);
        if (d->kick) d->feeder = SDL_CreateThread(feed, "audio reserve", d);
        if (d->feeder) LOG("audio: the device asks for %d frames at once, more than a pull: %d frames (%.1f ms) are kept queued ahead of it", frames, d->period,
                           1e3 * d->period / RATE);
        else { LOG("[RED] audio: no thread for the reserve (%s): pulls as they come, with underruns where the game keeps less queued", SDL_GetError());
               if (d->kick) SDL_DestroySemaphore(d->kick); d->kick = NULL; d->period = 0; }
    }
    return d;
}
static int a_start(audio_dev *d) {
    if (d->started) return 0;
    atomic_store_explicit(&d->run, 1, memory_order_release);
    if (!SDL_ResumeAudioStreamDevice(d->st)) { atomic_store(&d->run, 0); return -1; }
    d->started = 1;
    return 0;
}
static void a_stop(audio_dev *d) {
    if (!d->started) return;
    atomic_store_explicit(&d->run, 0, memory_order_release);
    SDL_PauseAudioStreamDevice(d->st);
    SDL_LockAudioStream(d->st); SDL_ClearAudioStream(d->st); SDL_UnlockAudioStream(d->st);   // a callback still running has finished; what it queued is not played later
    d->started = 0;
}
static void a_close(audio_dev *d) {
    if (!d) return;
    a_stop(d);
    if (d->feeder) { atomic_store_explicit(&d->feed_on, 0, memory_order_release); SDL_SignalSemaphore(d->kick); SDL_WaitThread(d->feeder, NULL); SDL_DestroySemaphore(d->kick); }
    SDL_DestroyAudioStream(d->st); free(d);
}
static uint64_t a_pulls(audio_dev *d) { return atomic_load_explicit(&d->pulls, memory_order_relaxed); }

static const audio_out_backend g_sdl = {"SDL3 sound card", a_open, a_start, a_stop, a_close, a_pulls};
const audio_out_backend *audio_out_device(void) { return &g_sdl; }
