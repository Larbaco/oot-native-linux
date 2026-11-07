// Real video decoding for the nn::movie stand-ins. See movie_real.h.
// The game's movie player feeds the decoder AV1 temporal units (WebM blocks, "Section 5" low-overhead OBUs) and the track configuration (av1C) and expects
// NV12 pictures with the packet's timestamp back; libavcodec's libdav1d does exactly that.
#include "movie_real.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#ifdef NXRT_BUILDING
#include "dm.h"
#else
#define LOG(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#endif

static uint8_t clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }
int mvr_write_ppm(const char *path, const uint8_t *nv12, int w, int h) {
    FILE *f = fopen(path, "wb"); if (!f) return 0;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    const uint8_t *y = nv12, *uv = nv12 + (size_t)w * h; int cw = (w + 1) / 2;
    uint8_t *row = malloc((size_t)w * 3); if (!row) { fclose(f); return 0; }
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int Y = y[(size_t)j * w + i] - 16, U = uv[((size_t)(j / 2) * cw + i / 2) * 2] - 128, V = uv[((size_t)(j / 2) * cw + i / 2) * 2 + 1] - 128, c = 298 * Y;
            row[i * 3] = clamp8((c + 459 * V + 128) >> 8); row[i * 3 + 1] = clamp8((c - 55 * U - 136 * V + 128) >> 8); row[i * 3 + 2] = clamp8((c + 541 * U + 128) >> 8);   // BT.709
        }
        fwrite(row, 3, (size_t)w, f);
    }
    free(row); fclose(f); return 1;
}
void mvr_pic_free(mvr_pic *p) { if (p) { free(p->nv12); p->nv12 = NULL; } }

#ifndef NXRT_HAVE_FFMPEG
int mvr_available(void) { return 0; }
mvr *mvr_create(const void *c, size_t n, void (*i)(void *), void (*o)(void *), void *u) { (void)c; (void)n; (void)i; (void)o; (void)u; return NULL; }
int mvr_submit(mvr *r, const uint8_t *d, size_t n, int64_t t, int e) { (void)r; (void)d; (void)n; (void)t; (void)e; return 0; }
int mvr_room(mvr *r) { (void)r; return 0; }
int mvr_fetch(mvr *r, mvr_pic *o) { (void)r; (void)o; return 0; }
void mvr_flush(mvr *r) { (void)r; }
void mvr_destroy(mvr *r) { (void)r; }
#else
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>

#define MVR_QIN 8     // packets waiting for the decoder (matches the stand-in's MOVIE_MAXIN: the game then waits for InputReady)
#define MVR_QOUT 4    // finished pictures held before the worker pauses (3 MB each at 1080p)
typedef struct { uint8_t *data; size_t n; int64_t ts; int eos; } mvr_pkt;
struct mvr {
    AVCodecContext *ctx; AVPacket *pkt; AVFrame *fr;
    pthread_t th; pthread_mutex_t mu; pthread_cond_t cv;
    mvr_pkt in[MVR_QIN]; int nin;
    mvr_pic out[MVR_QOUT + 1]; int nout;
    int quit, gen, flush_req, busy; unsigned long long n_pkt, n_pic;
    void (*on_in)(void *), (*on_out)(void *); void *user;
};

int mvr_available(void) { const char *e = getenv("NXRT_MOVIE_DECODE"); return e && *e && *e != '0'; }

// Planar/semi-planar 8-bit (and 10-bit, truncated) 4:2:0 -> packed NV12. Unknown formats come out black rather than failing the movie.
static int to_nv12(const AVFrame *f, mvr_pic *p) {
    int w = f->width, h = f->height, cw = (w + 1) / 2, ch = (h + 1) / 2;
    p->w = w; p->h = h; p->ysz = (size_t)w * h; p->uvsz = (size_t)cw * 2 * ch;
    uint8_t *o = malloc(p->ysz + p->uvsz); if (!o) return 0;
    uint8_t *uv = o + p->ysz; p->nv12 = o;
    switch (f->format) {
    case AV_PIX_FMT_YUV420P: case AV_PIX_FMT_YUVJ420P:
        for (int y = 0; y < h; y++) memcpy(o + (size_t)y * w, f->data[0] + (size_t)y * f->linesize[0], w);
        for (int y = 0; y < ch; y++) {
            const uint8_t *u = f->data[1] + (size_t)y * f->linesize[1], *v = f->data[2] + (size_t)y * f->linesize[2]; uint8_t *d = uv + (size_t)y * cw * 2;
            for (int x = 0; x < cw; x++) { d[2 * x] = u[x]; d[2 * x + 1] = v[x]; }
        }
        break;
    case AV_PIX_FMT_NV12:
        for (int y = 0; y < h; y++) memcpy(o + (size_t)y * w, f->data[0] + (size_t)y * f->linesize[0], w);
        for (int y = 0; y < ch; y++) memcpy(uv + (size_t)y * cw * 2, f->data[1] + (size_t)y * f->linesize[1], (size_t)cw * 2);
        break;
    case AV_PIX_FMT_YUV420P10LE:
        for (int y = 0; y < h; y++) { const uint16_t *s = (const uint16_t *)(f->data[0] + (size_t)y * f->linesize[0]); uint8_t *d = o + (size_t)y * w; for (int x = 0; x < w; x++) d[x] = (uint8_t)(s[x] >> 2); }
        for (int y = 0; y < ch; y++) {
            const uint16_t *u = (const uint16_t *)(f->data[1] + (size_t)y * f->linesize[1]), *v = (const uint16_t *)(f->data[2] + (size_t)y * f->linesize[2]); uint8_t *d = uv + (size_t)y * cw * 2;
            for (int x = 0; x < cw; x++) { d[2 * x] = (uint8_t)(u[x] >> 2); d[2 * x + 1] = (uint8_t)(v[x] >> 2); }
        }
        break;
    default: {
        static int warned; if (!warned++) LOG("[movie] real decode: unsupported pixel format %d, output is black", f->format);
        memset(o, 16, p->ysz); memset(uv, 128, p->uvsz);
    }
    }
    return 1;
}

static void full_to_limited(mvr_pic *p) {
    for (size_t i = 0; i < p->ysz; i++) p->nv12[i] = (uint8_t)(16 + (p->nv12[i] * 219 + 127) / 255);
    for (size_t i = 0; i < p->uvsz; i++) p->nv12[p->ysz + i] = (uint8_t)(16 + (p->nv12[p->ysz + i] * 224 + 127) / 255);
}
// Worker side: receive every picture the decoder has ready and queue it (drops them when a flush happened meanwhile)
static void drain_frames(mvr *r, int gen) {
    for (;;) {
        int e = avcodec_receive_frame(r->ctx, r->fr);
        if (e < 0) return;   // EAGAIN (needs more input) or EOF
        int64_t st = r->fr->best_effort_timestamp != AV_NOPTS_VALUE ? r->fr->best_effort_timestamp : r->fr->pts;   // as Eglantine's Nx2VideoDecoder::Receive
        mvr_pic p = {.ts = st == AV_NOPTS_VALUE ? 0 : st};
        int ok = to_nv12(r->fr, &p);
        if (ok && r->fr->color_range == AVCOL_RANGE_JPEG) full_to_limited(&p);   // the game's shader expects limited range (Eglantine 5239895160)
        av_frame_unref(r->fr);
        if (!ok) continue;
        pthread_mutex_lock(&r->mu);
        // Pause while the game does not take pictures (bounded memory); a flush or quit ends the wait
        while (r->nout >= MVR_QOUT && !r->quit && gen == r->gen) pthread_cond_wait(&r->cv, &r->mu);
        if (r->quit || gen != r->gen) { pthread_mutex_unlock(&r->mu); mvr_pic_free(&p); if (r->quit) return; continue; }
        r->out[r->nout++] = p; r->n_pic++;
        pthread_mutex_unlock(&r->mu);
        if (r->on_out) r->on_out(r->user);
    }
}
static void *worker(void *arg) {
    mvr *r = arg;
    pthread_mutex_lock(&r->mu);
    for (;;) {
        while (!r->quit && !r->nin && !r->flush_req) pthread_cond_wait(&r->cv, &r->mu);
        if (r->quit) break;
        if (r->flush_req) { avcodec_flush_buffers(r->ctx); r->flush_req = 0; continue; }
        mvr_pkt k = r->in[0]; memmove(r->in, r->in + 1, (size_t)--r->nin * sizeof k);
        int gen = r->gen; r->busy = 1;
        pthread_mutex_unlock(&r->mu);
        if (r->on_in) r->on_in(r->user);
        if (k.n && k.data) {
            if (av_new_packet(r->pkt, (int)k.n) == 0) {
                memcpy(r->pkt->data, k.data, k.n); r->pkt->pts = r->pkt->dts = k.ts;
                int e = avcodec_send_packet(r->ctx, r->pkt);
                if (e == AVERROR(EAGAIN)) { drain_frames(r, gen); e = avcodec_send_packet(r->ctx, r->pkt); }
                if (e < 0) LOG("[movie] real decode: avcodec_send_packet failed (%d) on packet %llu", e, r->n_pkt);
                av_packet_unref(r->pkt); r->n_pkt++;
            }
            drain_frames(r, gen);
        }
        if (k.eos) {
            avcodec_send_packet(r->ctx, NULL); drain_frames(r, gen);
            avcodec_flush_buffers(r->ctx);   // the decoder is reusable (a later movie on the same object starts at a key frame anyway)
            pthread_mutex_lock(&r->mu);
            if (!r->quit && gen == r->gen && r->nout < MVR_QOUT + 1) {
                r->out[r->nout++] = (mvr_pic){.ts = k.ts, .eos = 1}; pthread_mutex_unlock(&r->mu);
                if (r->on_out) r->on_out(r->user);
            } else pthread_mutex_unlock(&r->mu);
        }
        free(k.data);
        pthread_mutex_lock(&r->mu); r->busy = 0; pthread_cond_broadcast(&r->cv);
    }
    pthread_mutex_unlock(&r->mu);
    return NULL;
}

mvr *mvr_create(const void *cfg, size_t cfgsz, void (*on_input)(void *), void (*on_output)(void *), void *user) {
    if (!mvr_available()) return NULL;
    const AVCodec *c = avcodec_find_decoder_by_name("libdav1d"); if (!c) c = avcodec_find_decoder(AV_CODEC_ID_AV1);
    if (!c) { LOG("[movie] real decode: no AV1 decoder in libavcodec"); return NULL; }
    mvr *r = calloc(1, sizeof *r); if (!r) return NULL;
    r->ctx = avcodec_alloc_context3(c); r->pkt = av_packet_alloc(); r->fr = av_frame_alloc();
    if (!r->ctx || !r->pkt || !r->fr) goto bad;
    const char *t = getenv("NXRT_MOVIE_THREADS"); int nt = t ? atoi(t) : 4; r->ctx->thread_count = nt > 0 ? nt : 4;
    r->ctx->pkt_timebase = (AVRational){1, 1000000000};
    r->ctx->flags &= ~AV_CODEC_FLAG_LOW_DELAY; r->ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;   // frame threads, as Eglantine (22f947f5e2)
    if (cfg && cfgsz) {
        r->ctx->extradata = av_mallocz(cfgsz + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!r->ctx->extradata) goto bad;
        memcpy(r->ctx->extradata, cfg, cfgsz); r->ctx->extradata_size = (int)cfgsz;
    }
    int e = avcodec_open2(r->ctx, c, NULL); if (e < 0) { LOG("[movie] real decode: avcodec_open2(%s) failed (%d)", c->name, e); goto bad; }
    pthread_mutex_init(&r->mu, NULL); pthread_cond_init(&r->cv, NULL);
    r->on_in = on_input; r->on_out = on_output; r->user = user;
    if (pthread_create(&r->th, NULL, worker, r)) goto bad2;
    LOG("[movie] real decode: %s (%s), %d threads, av1C %zu bytes", c->name, c->long_name, r->ctx->thread_count, cfgsz);
    return r;
bad2: pthread_mutex_destroy(&r->mu); pthread_cond_destroy(&r->cv);
bad:  avcodec_free_context(&r->ctx); av_packet_free(&r->pkt); av_frame_free(&r->fr); free(r); return NULL;
}
int mvr_submit(mvr *r, const uint8_t *data, size_t n, int64_t ts, int eos) {
    uint8_t *cp = NULL; if (data && n) { cp = malloc(n); if (!cp) return 1; memcpy(cp, data, n); } else n = 0;
    pthread_mutex_lock(&r->mu);
    if (r->nin >= MVR_QIN) { pthread_mutex_unlock(&r->mu); free(cp); return 1; }
    r->in[r->nin++] = (mvr_pkt){cp, n, ts, eos};
    pthread_cond_broadcast(&r->cv); pthread_mutex_unlock(&r->mu);
    return 0;
}
int mvr_room(mvr *r) { pthread_mutex_lock(&r->mu); int n = MVR_QIN - r->nin; pthread_mutex_unlock(&r->mu); return n; }
int mvr_fetch(mvr *r, mvr_pic *out) {
    pthread_mutex_lock(&r->mu); int got = r->nout > 0;
    if (got) { *out = r->out[0]; memmove(r->out, r->out + 1, (size_t)--r->nout * sizeof *out); pthread_cond_broadcast(&r->cv); }
    pthread_mutex_unlock(&r->mu); return got;
}
void mvr_flush(mvr *r) {
    pthread_mutex_lock(&r->mu);
    r->gen++; r->flush_req = 1;
    for (int i = 0; i < r->nin; i++) free(r->in[i].data);
    r->nin = 0;
    for (int i = 0; i < r->nout; i++) mvr_pic_free(&r->out[i]);
    r->nout = 0;
    pthread_cond_broadcast(&r->cv);
    while (r->busy) pthread_cond_wait(&r->cv, &r->mu);   // the packet in flight finishes (its pictures are dropped by the generation check)
    pthread_mutex_unlock(&r->mu);
}
void mvr_destroy(mvr *r) {
    if (!r) return;
    pthread_mutex_lock(&r->mu); r->quit = 1; pthread_cond_broadcast(&r->cv); pthread_mutex_unlock(&r->mu);
    pthread_join(r->th, NULL);
    for (int i = 0; i < r->nin; i++) free(r->in[i].data);
    for (int i = 0; i < r->nout; i++) mvr_pic_free(&r->out[i]);
    LOG("[movie] real decode: closed after %llu packets, %llu pictures", r->n_pkt, r->n_pic);
    avcodec_free_context(&r->ctx); av_packet_free(&r->pkt); av_frame_free(&r->fr);
    pthread_mutex_destroy(&r->mu); pthread_cond_destroy(&r->cv); free(r);
}
#endif
