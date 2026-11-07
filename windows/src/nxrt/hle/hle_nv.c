// from nxnative dm/gfx/hle_nv.c (MIT, © seedds). Windows: unsigned long is uint64_t (LLP64).
// nv:: (the GPU driver glue layer beneath NVN: creates transfer memory, connects to the nvdrv service). NVN has been replaced wholesale with a host null device, so this layer has nothing to do.
// The allocator callbacks are recorded in the host global g_nv_alloc; the graphics layer takes them from here later when it needs to allocate memory with the game's allocator.
#include "hle.h"
#include "movie_real.h"
#include <stdatomic.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>

typedef void *(*nv_alloc_fn)(uint64_t size, uint64_t align, void *user);
typedef void (*nv_free_fn)(void *p, void *user);
typedef void *(*nv_realloc_fn)(void *p, uint64_t size, void *user);
struct nv_allocator { nv_alloc_fn alloc; nv_free_fn free; nv_realloc_fn realloc; void *user; };
struct nv_allocator g_nv_alloc, g_nv_devtools_alloc;
// The graphics pool the game installs (nv::MemoryInfo reports on it). Nothing is allocated from it on the host: the graphics layer allocates its own Metal objects
static struct { void *mem; uint64_t size; int inited; } g_nv_pool;

#define WHY "NVN has been replaced wholesale with a host null device, so the driver glue layer has nothing to do"
HLE_FORCE("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", WHY "; allocator recorded in g_nv_alloc",
          void, h_nv_SetAlloc, nv_alloc_fn a, nv_free_fn f, nv_realloc_fn r, void *u) { g_nv_alloc = (struct nv_allocator){a, f, r, u}; }
HLE_FORCE("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", WHY "; allocator recorded in g_nv_devtools_alloc",
          void, h_nv_SetDevAlloc, nv_alloc_fn a, nv_free_fn f, nv_realloc_fn r, void *u) { g_nv_devtools_alloc = (struct nv_allocator){a, f, r, u}; }
HLE_FORCE("_ZN2nv18InitializeGraphicsEPvm", WHY "; pool recorded for nv::GetGraphicsMemoryInfo", void, h_nv_Init, void *mem, uint64_t size) {
    g_nv_pool = (typeof(g_nv_pool)){mem, size, 1};
    LOG("nv::InitializeGraphics: pool %.2f MB, recorded (the host's graphics layer allocates its own)", size / 1048576.0);
}
HLE_FORCE("_ZN2nv16FinalizeGraphicsEv", WHY, void, h_nv_Fin, void) { g_nv_pool = (typeof(g_nv_pool)){0}; }
// nv::MemoryInfo is 64-bit counters, the SDK fills them from sign-extended 32-bit driver counters. Its size follows the SDK: 0x20 bytes (four counters) on SDK 10 and older,
// 0x40 bytes from SDK 11 on (where the SDK zeroes all 0x40 when nv is not initialised). Writing 0x40 on an old SDK overran the caller's record by 32 bytes (Hyrule Warriors:
// Definitive Edition, SDK 3.5.1). Its own implementation reads the NV driver object, which the host never creates, so it faults: Well Dweller crashed there (sdk+0x4656c8,
// x0 = 0) while reporting video memory at startup. Report the pool the game installed, with nothing used from it
HLE_FORCE("_ZN2nv21GetGraphicsMemoryInfoEPNS_10MemoryInfoE", WHY "; pool figures from nv::InitializeGraphics, no driver statistics", void, h_nv_MemInfo, uint64_t *out) {
    memset(out, 0, DM_SDK_OLD(dm_sdk_major()) ? 0x20 : 0x40);
    if (g_nv_pool.inited) { out[0] = g_nv_pool.size; out[2] = g_nv_pool.size; }   // [0] total, [1] used (0), [2] free: within the four counters of both sizes
}
HLE_FORCE("_ZN2nv22DumpGraphicsMemoryInfoEv", WHY "; the dump goes to the host log", void, h_nv_Dump, void) {
    LOG("nv::DumpGraphicsMemoryInfo: pool %.2f MB, 0 used (host graphics layer)", g_nv_pool.size / 1048576.0);
}


// SDK 22 (Ocarina of Time): subsdk0's movie::VideoDecoder / AudioDecoder talk to the GPU video service (nn::gsv / nn::codec::video, a HIPC server the host does not have;
// nv::InitCheck aborts, then nn::os::InternalCriticalSection::Enter faults on its null server context). The host has no hardware decoder and returning an error makes the
// game abort (main+0x20e62fc), so these are *stand-in decoders* that follow the protocol the game's movie player (main+0x3b5d200..0x3b62a00) relies on, without decoding:
//   - Initialize/Configure/Start/Stop/Finalize accept. RegisterVideoFrame/RegisterAudioFrame hand the decoder its output pool (VideoFrame is 0x90 bytes, AudioFrame 0x60).
//   - DecodeAsync(Buffer) takes the input at once (nn::movie::Buffer: ptr@0 cap@8 filled@0x10 timestamp@0x18 flags@0x20; flag 8 = end of stream, set by the game when the
//     extractor's ReadSamples reports end of stream) and signals InputReady, so the game keeps feeding. The input becomes one output frame taken from the registered pool
//     (or waits until ReleaseVideoFrame/ReleaseAudioFrame returns one), carrying the timestamp and the end-of-stream state (VideoFrame ts@0x78 eos@0x88, planes
//     filled size@0x28/0x30, buffer@8 size@0x48; AudioFrame buffer*@0 eos@0x10 channels@0x14 rate@0x18 format@0x1c) and OutputReady is signalled.
//   - AcquireXFrame pops the oldest output frame. The pixels are a flat grey, the audio is silence: only the movie's timing and its end of stream are reproduced.
// Each event (InputReady/OutputReady/FormatChanged) of each decoder is its own SystemEvent; the decoder state is keyed by the game's object pointer.
#define MOVIE_MAXF 64
#define MOVIE_MAXIN 8   // inputs the decoder holds before DecodeAsync reports it busy (the game then waits for InputReady)
typedef struct { uint32_t flags; int64_t ts; } movie_in;
typedef struct movie_dec {
    void *self; int audio; int started;
    _Alignas(16) uint8_t ev[3][256]; int ev_bound[3];   // 0 InputReady, 1 OutputReady, 2 FormatChanged
    void *fre[MOVIE_MAXF]; int nfre;     // registered output frames not holding a decoded frame
    void *out[MOVIE_MAXF]; int nout;     // decoded frames for Acquire
    movie_in in[MOVIE_MAXF]; int nin;    // inputs waiting for a free output frame
    uint64_t n_dec, n_eos, n_in;
    mvr *r;   // NXRT_MOVIE_DECODE=1: real AV1 decoder (movie_real.c) behind a video decoder; NULL = stand-in
} movie_dec;
static pthread_mutex_t g_mv_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_mv_trace = -1;   // NXRT_MOVIE_TRACE=1 logs every stand-in decoder call
#define MVT(...) do { if (g_mv_trace < 0) g_mv_trace = getenv("NXRT_MOVIE_TRACE") != NULL; if (g_mv_trace) LOG("[movie] " __VA_ARGS__); } while (0)
static movie_dec *g_mv[16]; static int g_mv_n;
static movie_dec *movie_get(void *self, int audio, int create) {
    for (int i = 0; i < g_mv_n; i++) if (g_mv[i]->self == self) return g_mv[i];
    if (!create || g_mv_n >= 16) return NULL;
    movie_dec *d = aligned_alloc(16, (sizeof *d + 15) & ~15ull); memset(d, 0, sizeof *d); d->self = self; d->audio = audio; return g_mv[g_mv_n++] = d;
}
static void *movie_ev(movie_dec *d, int k) {
    if (!d->ev_bound[k]) { d->ev_bound[k] = 1; hle_bind_system_event(d->ev[k], 1); }
    return d->ev[k];
}
// NXRT_MOVIE_SPEED=<n>: the stand-in decoders stamp their frames with the input's timestamp divided by n, so the game's presentation clock plays the movie n times faster
// (headless tests do not need to sit through the intro; the end of stream still comes in order)
static int64_t movie_ts(int64_t ts) {
    static double speed; if (speed == 0) { const char *e = getenv("NXRT_MOVIE_SPEED"); speed = e ? atof(e) : 1; if (speed < 1) speed = 1; }
    return (int64_t)(ts / speed);
}
static void movie_fill_video(void *f, const movie_in *in) {
    uint8_t *p = *(uint8_t **)((uint8_t *)f + 8); uint64_t sz = *(uint64_t *)((uint8_t *)f + 0x48);
    uint64_t y = sz * 2 / 3;
    if (p && sz) { memset(p, 0x50, y); memset(p + y, 0x80, sz - y); }
    *(uint64_t *)((uint8_t *)f + 0x28) = y; *(uint64_t *)((uint8_t *)f + 0x30) = sz - y;
    *(int64_t *)((uint8_t *)f + 0x78) = movie_ts(in->ts); *(uint8_t *)((uint8_t *)f + 0x88) = (in->flags & 8) != 0;
}
static void movie_fill_audio(void *f, const movie_in *in) {
    uint8_t *buf = *(uint8_t **)f; if (!buf) return;
    static const int bps[5] = {1, 2, 3, 4, 4};
    uint64_t cap = *(uint64_t *)(buf + 8); int ch = *(int32_t *)((uint8_t *)f + 0x14), fm = *(int32_t *)((uint8_t *)f + 0x1c);
    uint64_t n = 1024ull * (ch > 0 ? ch : 2) * bps[(unsigned)fm < 5 ? fm : 1]; if (n > cap) n = cap;
    if (*(void **)buf) memset(*(void **)buf, 0, n);
    *(uint64_t *)(buf + 0x10) = (in->flags & 8) ? 0 : n; *(int64_t *)(buf + 0x18) = movie_ts(in->ts); *(uint32_t *)(buf + 0x20) = in->flags;
    *(uint8_t *)((uint8_t *)f + 0x10) = (in->flags & 8) != 0;
}
// Called with g_mv_mu held: turn waiting inputs into output frames; returns whether any frame became available
static int movie_pump(movie_dec *d) {
    int any = 0;
    static int speed; if (!speed) { const char *e = getenv("NXRT_MOVIE_SPEED"); speed = e ? atoi(e) : 1; if (speed < 1) speed = 1; }
    while (d->nin > 0) {
        // The game can retain every registered video frame while it drains
        // its presentation queue. If EOS is next and that queue already has
        // frames, attach EOS to the last queued frame instead of requiring an
        // additional free frame that may never be returned before the wait.
        if (!d->audio && d->nin == 1 && (d->in[0].flags & 8) && d->nfre == 0 && d->nout > 0) {
            *(uint8_t *)((uint8_t *)d->out[d->nout - 1] + 0x88) = 1;
            d->nin = 0; d->n_eos++; any |= 2;
            LOG("[movie] video decoder %p: end of stream attached to final queued frame", d->self);
            break;
        }
        if (d->nout >= MOVIE_MAXF) break;
        if (!d->audio && speed > 1 && !(d->in[0].flags & 8) && (d->n_in++ % (uint64_t)speed) != 0) {   // fast-forward: the input is consumed, no frame comes out of it
            memmove(d->in, d->in + 1, (size_t)--d->nin * sizeof(movie_in)); any |= 2; continue; }
        if (d->nfre <= 0) break;
        movie_in in = d->in[0]; memmove(d->in, d->in + 1, (size_t)--d->nin * sizeof in);
        void *f = d->fre[0]; memmove(d->fre, d->fre + 1, (size_t)--d->nfre * sizeof f);
        if (d->audio) movie_fill_audio(f, &in); else movie_fill_video(f, &in);
        d->out[d->nout++] = f; any |= 3; d->n_dec++; d->n_eos += (in.flags & 8) != 0;
        if (d->n_dec == 1 || (in.flags & 8)) LOG("[movie] %s decoder %p: output #%llu ts %lld ms%s", d->audio ? "audio" : "video", d->self, (unsigned long long)d->n_dec, (long long)(in.ts / 1000000), (in.flags & 8) ? " END OF STREAM" : "");
    }
    return any;
}
// Real decoding (movie_real.c): the worker thread decodes AV1 into NV12 pictures; they are copied into the game's output frames here, on guest threads only (the frame
// buffers are guest memory). Layout as the game's player reads it: VideoFrame buffer@8/size@0x48 holds Y (w*h) then interleaved UV, tightly packed; plane sizes @0x28/@0x30.
static void movie_real_on_in(void *u) { hle_signal_system_event(((movie_dec *)u)->ev[0]); }
static void movie_real_on_out(void *u) { hle_signal_system_event(((movie_dec *)u)->ev[1]); }
// Called with g_mv_mu held; returns 1 when a frame became available
static int movie_pump_real(movie_dec *d) {
    int any = 0; mvr_pic p;
    while (d->nfre > 0 && d->nout < MOVIE_MAXF && mvr_fetch(d->r, &p)) {
        void *f = d->fre[0]; memmove(d->fre, d->fre + 1, (size_t)--d->nfre * sizeof f);
        uint8_t *buf = *(uint8_t **)((uint8_t *)f + 8); uint64_t sz = *(uint64_t *)((uint8_t *)f + 0x48);
        uint64_t want = p.ysz + p.uvsz, n = want < sz ? want : sz;
        if (p.nv12 && buf) {
            dm_guest_write_prepare(buf, n); memcpy(buf, p.nv12, n);
            if (want > sz) { static _Atomic int warned; if (!atomic_exchange(&warned, 1)) LOG("[movie] real decode: picture %dx%d (%llu bytes) does not fit the game's frame (%llu bytes), cropped", p.w, p.h, (unsigned long long)want, (unsigned long long)sz); }
        }
        { static const char *dump; static int every, init; if (!init) { init = 1; dump = getenv("NXRT_MOVIE_DUMP"); const char *e = getenv("NXRT_MOVIE_DUMP_EVERY"); every = e ? atoi(e) : 60; if (every < 1) every = 60; }
          if (dump && p.nv12 && buf && (d->n_dec % (uint64_t)every) == 0 && want <= sz) { char path[600]; snprintf(path, sizeof path, "%s/game_frame_%06llu.ppm", dump, (unsigned long long)d->n_dec); mvr_write_ppm(path, buf, p.w, p.h); } }   // NXRT_MOVIE_DUMP=<dir>: what the game's frame buffer holds
        *(uint64_t *)((uint8_t *)f + 0x28) = p.ysz; *(uint64_t *)((uint8_t *)f + 0x30) = p.uvsz;
        *(int64_t *)((uint8_t *)f + 0x78) = movie_ts(p.ts); *(uint8_t *)((uint8_t *)f + 0x88) = (uint8_t)p.eos;
        d->out[d->nout++] = f; any = 1; d->n_dec++; d->n_eos += p.eos;
        if (d->n_dec == 1 || p.eos) LOG("[movie] video decoder %p: real output #%llu %dx%d ts %lld ms%s", d->self, (unsigned long long)d->n_dec, p.w, p.h, (long long)(p.ts / 1000000), p.eos ? " END OF STREAM" : "");
        mvr_pic_free(&p);
    }
    return any;
}
static int movie_init(void *self, int audio) {
    static _Atomic int once[2];
    if (audio && !atomic_exchange(&once[audio], 1)) LOG("[movie] audio decoder: timed stand-in initialized (silence)");
    pthread_mutex_lock(&g_mv_mu);
    movie_dec *d = movie_get(self, audio, 1); mvr *old = d->r; d->r = NULL; d->started = 0; d->nfre = d->nout = d->nin = 0;
    for (int k = 0; k < 3; k++) movie_ev(d, k);
    pthread_mutex_unlock(&g_mv_mu);
    if (old) mvr_destroy(old);
    if (self) *(void **)self = NULL;
    return 0;
}
HLE_FORCE("_ZN5movie12VideoDecoder10InitializeEPvmN2nn5movie13MediaMimeTypeENS_24VideoDecoderOutputFormatENS_14VideoCodecModeE",
          "SWITCH2: the movie decoders need the GPU video service; stand-in decoder", int, h_movie_vdec_Init, void *self, void *buf, uint64_t size, int mime, int fmt, int mode) {
    (void)buf; (void)size; (void)mime; (void)fmt; (void)mode; return movie_init(self, 0);
}
HLE_FORCE("_ZN5movie12AudioDecoder10InitializeEPvmN2nn5movie13MediaMimeTypeENS_24AudioDecoderOutputFormatENS_14AudioCodecModeE",
          "SWITCH2: the movie decoders need the GPU video service; stand-in decoder", int, h_movie_adec_Init, void *self, void *buf, uint64_t size, int mime, int fmt, int mode) {
    (void)buf; (void)size; (void)mime; (void)fmt; (void)mode; return movie_init(self, 1);
}
// Register*Frame / Acquire*Frame / Release*Frame / DecodeAsync / Start / Stop / Flush / Finalize / events
static int movie_register(void *self, int audio, void **frames, int n) {
    pthread_mutex_lock(&g_mv_mu); movie_dec *d = movie_get(self, audio, 0);
    MVT("%s register %d frames", audio ? "audio" : "video", n);
    if (d) for (int i = 0; i < n && d->nfre + d->nout < MOVIE_MAXF; i++) d->fre[d->nfre++] = frames[i];
    int any = d ? (d->r ? movie_pump_real(d) : movie_pump(d)) : 0; pthread_mutex_unlock(&g_mv_mu);
    if (any & 1) hle_signal_system_event(movie_ev(d, 1));
    return 0;
}
static int movie_decode(void *self, int audio, const uint8_t *b) {
    pthread_mutex_lock(&g_mv_mu); movie_dec *d = movie_get(self, audio, 0);
    if (!d) { pthread_mutex_unlock(&g_mv_mu); return 0; }
    if (d->r) {   // real decoder: hand the packet (Buffer: ptr@0 filled@0x10 timestamp@0x18 flags@0x20) to the worker; InputReady comes when it takes it
        const uint8_t *data = *(uint8_t **)b; uint64_t n = *(uint64_t *)(b + 0x10); uint32_t fl = *(uint32_t *)(b + 0x20);
        if (mvr_submit(d->r, data, n, *(int64_t *)(b + 0x18), (fl & 8) != 0)) { pthread_mutex_unlock(&g_mv_mu); return 0x1f0100; }
        MVT("video real decode: flags %#x ts %lld ms filled %llu", fl, (long long)(*(int64_t *)(b + 0x18) / 1000000), (unsigned long long)n);
        int any = movie_pump_real(d), room = mvr_room(d->r) > 0; pthread_mutex_unlock(&g_mv_mu);
        if (any) hle_signal_system_event(movie_ev(d, 1));
        if (room) hle_signal_system_event(movie_ev(d, 0));
        return 0;
    }
    if (d->nin >= MOVIE_MAXIN) { pthread_mutex_unlock(&g_mv_mu); return 0x1f0100; }
    d->in[d->nin++] = (movie_in){*(uint32_t *)(b + 0x20), *(int64_t *)(b + 0x18)};
    MVT("%s decode: flags %#x ts %lld ms filled %llu (waiting %d, free frames %d)", audio ? "audio" : "video", d->in[d->nin - 1].flags, (long long)(d->in[d->nin - 1].ts / 1000000), (unsigned long long)*(uint64_t *)(b + 0x10), d->nin, d->nfre);
    int any = movie_pump(d); pthread_mutex_unlock(&g_mv_mu);
    if (any & 1) hle_signal_system_event(movie_ev(d, 1));
    hle_signal_system_event(movie_ev(d, 0));   // the input buffer is consumed: the decoder wants more
    return 0;
}
static int movie_acquire(void *self, int audio, void **out, int *remaining) {
    pthread_mutex_lock(&g_mv_mu); movie_dec *d = movie_get(self, audio, 0); int r = 0x1e0100;
    if (out) *out = NULL;
    if (d && d->r) movie_pump_real(d);
    if (d && d->nout > 0) { if (out) *out = d->out[0]; memmove(d->out, d->out + 1, (size_t)--d->nout * sizeof(void *)); r = 0; }
    if (remaining) *remaining = d ? d->nout : 0;
    MVT("%s acquire -> %p (%d left)", audio ? "audio" : "video", out ? *out : NULL, d ? d->nout : -1);
    pthread_mutex_unlock(&g_mv_mu);
    return audio ? r : 0;   // the video caller tests the pointer, the audio caller the result
}
static int movie_release(void *self, int audio, void *f) {
    pthread_mutex_lock(&g_mv_mu); movie_dec *d = movie_get(self, audio, 0); int any = 0; const int real = d && d->r;
    MVT("%s release %p", audio ? "audio" : "video", f);
    if (d && f && d->nfre + d->nout < MOVIE_MAXF) { d->fre[d->nfre++] = f; any = d->r ? movie_pump_real(d) : movie_pump(d); }
    pthread_mutex_unlock(&g_mv_mu);
    if (any & 1) hle_signal_system_event(movie_ev(d, 1));
    if (any && !real) hle_signal_system_event(movie_ev(d, 0));   // a waiting input was taken: room for more (the real decoder signals it itself)
    return 0;
}
static int movie_start(void *self, int audio, int on) {
    MVT("%s %s", audio ? "audio" : "video", on ? "start" : "stop");
    pthread_mutex_lock(&g_mv_mu); movie_dec *d = movie_get(self, audio, 0); if (d) d->started = on; pthread_mutex_unlock(&g_mv_mu);
    if (d && on) hle_signal_system_event(movie_ev(d, 0));
    return 0;
}
static int movie_flush(void *self, int audio, int fin) {
    MVT("%s flush/finalize", audio ? "audio" : "video");
    pthread_mutex_lock(&g_mv_mu); movie_dec *d = movie_get(self, audio, 0);
    if (d && d->r && !fin) mvr_flush(d->r);   // drops queued packets and pictures; the in-flight packet finishes first
    mvr *dead = NULL; if (d && d->r && fin) { dead = d->r; d->r = NULL; }
    if (d && fin) d->nfre = d->nout = d->nin = 0;   // Finalize: the game frees its frames and buffers next, the decoder must never touch them again
    else if (d) { while (d->nout > 0) d->fre[d->nfre++] = d->out[--d->nout]; d->nin = 0; }
    pthread_mutex_unlock(&g_mv_mu);
    if (dead) mvr_destroy(dead);   // joins the worker, which may be waiting for g_mv_mu: released above
    if (d) hle_signal_system_event(movie_ev(d, 0));
    return 0;
}
static int movie_event(void *self, int audio, int k, void **out) {
    pthread_mutex_lock(&g_mv_mu); movie_dec *d = movie_get(self, audio, 0); void *e = d ? movie_ev(d, k) : NULL; pthread_mutex_unlock(&g_mv_mu);
    if (out) *out = e; return d ? 0 : 0x1d0100;
}
#define MV "SWITCH2: movie stand-in decoder"
HLE_FORCE("_ZN5movie12VideoDecoder18RegisterVideoFrameEPPNS_10VideoFrameEi", MV, int, h_mv_vreg, void *self, void **f, int n) { return movie_register(self, 0, f, n); }
HLE_FORCE("_ZN5movie12AudioDecoder18RegisterAudioFrameEPPNS_10AudioFrameEi", MV, int, h_mv_areg, void *self, void **f, int n) { return movie_register(self, 1, f, n); }
HLE_FORCE("_ZN5movie12VideoDecoder11DecodeAsyncEPKN2nn5movie6BufferE", MV, int, h_mv_vdec, void *self, const uint8_t *b) { return movie_decode(self, 0, b); }
HLE_FORCE("_ZN5movie12AudioDecoder11DecodeAsyncEPKN2nn5movie6BufferE", MV, int, h_mv_adec, void *self, const uint8_t *b) { return movie_decode(self, 1, b); }
HLE_FORCE("_ZN5movie12VideoDecoder17AcquireVideoFrameEPPNS_10VideoFrameE", MV, int, h_mv_vacq, void *self, void **f) { return movie_acquire(self, 0, f, NULL); }
HLE_FORCE("_ZN5movie12VideoDecoder17AcquireVideoFrameEPPNS_10VideoFrameEPi", MV, int, h_mv_vacq2, void *self, void **f, int *rem) { return movie_acquire(self, 0, f, rem); }
HLE_FORCE("_ZN5movie12AudioDecoder17AcquireAudioFrameEPPNS_10AudioFrameEPi", MV, int, h_mv_aacq, void *self, void **f, int *rem) { return movie_acquire(self, 1, f, rem); }
HLE_FORCE("_ZN5movie12VideoDecoder17ReleaseVideoFrameEPNS_10VideoFrameE", MV, int, h_mv_vrel, void *self, void *f) { return movie_release(self, 0, f); }
HLE_FORCE("_ZN5movie12AudioDecoder17ReleaseAudioFrameEPNS_10AudioFrameE", MV, int, h_mv_arel, void *self, void *f) { return movie_release(self, 1, f); }
HLE_FORCE("_ZN5movie12VideoDecoder5StartEv", MV, int, h_mv_vstart, void *self) { return movie_start(self, 0, 1); }
HLE_FORCE("_ZN5movie12AudioDecoder5StartEv", MV, int, h_mv_astart, void *self) { return movie_start(self, 1, 1); }
HLE_FORCE("_ZN5movie12VideoDecoder4StopEv", MV, int, h_mv_vstop, void *self) { return movie_start(self, 0, 0); }
HLE_FORCE("_ZN5movie12AudioDecoder4StopEv", MV, int, h_mv_astop, void *self) { return movie_start(self, 1, 0); }
HLE_FORCE("_ZN5movie12VideoDecoder5FlushEv", MV, int, h_mv_vflush, void *self) { return movie_flush(self, 0, 0); }
HLE_FORCE("_ZN5movie12AudioDecoder5FlushEv", MV, int, h_mv_aflush, void *self) { return movie_flush(self, 1, 0); }
HLE_FORCE("_ZN5movie12VideoDecoder8FinalizeEv", MV, int, h_mv_vfin, void *self) { return movie_flush(self, 0, 1); }
HLE_FORCE("_ZN5movie12AudioDecoder8FinalizeEv", MV, int, h_mv_afin, void *self) { return movie_flush(self, 1, 1); }
HLE_FORCE("_ZN5movie12VideoDecoder18GetInputReadyEventEPPN2nn2os15SystemEventTypeE", MV, int, h_mv_vev0, void *self, void **o) { return movie_event(self, 0, 0, o); }
HLE_FORCE("_ZN5movie12VideoDecoder19GetOutputReadyEventEPPN2nn2os15SystemEventTypeE", MV, int, h_mv_vev1, void *self, void **o) { return movie_event(self, 0, 1, o); }
HLE_FORCE("_ZN5movie12VideoDecoder21GetFormatChangedEventEPPN2nn2os15SystemEventTypeE", MV, int, h_mv_vev2, void *self, void **o) { return movie_event(self, 0, 2, o); }
HLE_FORCE("_ZN5movie12AudioDecoder18GetInputReadyEventEPPN2nn2os15SystemEventTypeE", MV, int, h_mv_aev0, void *self, void **o) { return movie_event(self, 1, 0, o); }
HLE_FORCE("_ZN5movie12AudioDecoder19GetOutputReadyEventEPPN2nn2os15SystemEventTypeE", MV, int, h_mv_aev1, void *self, void **o) { return movie_event(self, 1, 1, o); }
HLE_FORCE("_ZN5movie12AudioDecoder21GetFormatChangedEventEPPN2nn2os15SystemEventTypeE", MV, int, h_mv_aev2, void *self, void **o) { return movie_event(self, 1, 2, o); }
// Configure and the thread settings accept; the audio requirements name the size of an output frame buffer (the game allocates it from "output-buf-size")
HLE_FORCE("_ZN5movie12VideoDecoder9ConfigureEPKN2nn5movie13MediaPropertyEPKvm", MV, int, h_mv_vcfg, void *self, void *p, void *c, uint64_t n) {
    (void)p;   // the track configuration (av1C) is what the real decoder needs; the dimensions come out of the stream
    pthread_mutex_lock(&g_mv_mu); movie_dec *d = movie_get(self, 0, 0); mvr *old = d ? d->r : NULL; if (d) d->r = NULL; pthread_mutex_unlock(&g_mv_mu);
    if (old) mvr_destroy(old);
    if (d && mvr_available()) { mvr *r = mvr_create(c, n, movie_real_on_in, movie_real_on_out, d); pthread_mutex_lock(&g_mv_mu); d->r = r; pthread_mutex_unlock(&g_mv_mu); if (r) LOG("[movie] video decoder %p: real AV1 decoding available", self); else LOG("[movie] real AV1 decoder unavailable; using timed no-image stand-in"); }
    else if (d) {
        const char *setting = getenv("NXRT_MOVIE_DECODE");
        if (setting && *setting == '0') LOG("[movie] NXRT_MOVIE_DECODE=0; using timed no-image stand-in");
        else if (!mvr_build_available()) LOG("[movie] FFmpeg/libdav1d support was not built; using timed no-image stand-in");
    }
    return 0;
}
HLE_FORCE("_ZN5movie12AudioDecoder9ConfigureEPKN2nn5movie13MediaPropertyEPKvm", MV, int, h_mv_acfg, void *self, void *p, void *c, uint64_t n) { (void)self; (void)p; (void)c; (void)n; return 0; }
HLE_FORCE("_ZN5movie12AudioDecoder25GetAudioFrameRequirementsEPN2nn5movie13MediaPropertyE", MV, int, h_mv_areq, void *self, void *prop) {
    (void)self; static void *setsize; if (!setsize) setsize = guest_sym("_ZN2nn5movie13MediaProperty7SetSizeEPKcm");
    if (setsize && prop) GUEST_CALL(setsize, (uint64_t)(uintptr_t)prop, (uint64_t)(uintptr_t)"output-buf-size", 65536);
    return 0;
}
HLE_FORCE("_ZN5movie12VideoDecoder25GetVideoFrameRequirementsEPN2nn5movie13MediaPropertyE", MV, int, h_mv_vreq, void *self, void *prop) { (void)self; (void)prop; return 0; }
HLE_FORCE("_ZN5movie12VideoDecoder24GetOutputFramePropertiesEPN2nn5movie13MediaPropertyE", MV, int, h_mv_vprop, void *self, void *prop) { (void)self; (void)prop; return 0; }
HLE_FORCE("_ZN5movie12AudioDecoder24GetOutputFramePropertiesEPN2nn5movie13MediaPropertyE", MV, int, h_mv_aprop, void *self, void *prop) { (void)self; (void)prop; return 0; }
HLE_FORCE("_ZN5movie12AudioDecoder17GetThreadCoreMaskEv", MV, int, h_mv_a_gcm, void *self) { (void)self; return 0; }
HLE_FORCE("_ZN5movie12AudioDecoder17GetThreadPriorityEv", MV, int, h_mv_a_gtp, void *self) { (void)self; return 0; }
HLE_FORCE("_ZN5movie12AudioDecoder17SetThreadCoreMaskEm", MV, int, h_mv_a_scm, void *self, uint64_t m) { (void)self; (void)m; return 0; }
HLE_FORCE("_ZN5movie12AudioDecoder17SetThreadPriorityEi", MV, int, h_mv_a_stp, void *self, int p) { (void)self; (void)p; return 0; }
HLE_FORCE("_ZN5movie12VideoDecoder17GetThreadCoreMaskEv", MV, int, h_mv_v_gcm, void *self) { (void)self; return 0; }
HLE_FORCE("_ZN5movie12VideoDecoder17GetThreadPriorityEv", MV, int, h_mv_v_gtp, void *self) { (void)self; return 0; }
HLE_FORCE("_ZN5movie12VideoDecoder17SetThreadCoreMaskEm", MV, int, h_mv_v_scm, void *self, uint64_t m) { (void)self; (void)m; return 0; }
HLE_FORCE("_ZN5movie12VideoDecoder17SetThreadPriorityEi", MV, int, h_mv_v_stp, void *self, int p) { (void)self; (void)p; return 0; }

#include "hle_nv.adapters.inc"
