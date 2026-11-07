// Offline harness for the real movie decoder (hle/movie_real.c): demux a .webm, push its packets through the same mvr_* path the game's decoder stand-in uses,
// and write the first N pictures as PPM (BT.709 limited-range NV12 -> RGB, so the NV12 layout is judged the way the game's shader reads it).
//   nxrt-movie-decode <file.webm> <outdir> [N=10] [step=1]   then e.g. ffmpeg -i frame_0000.ppm frame_0000.png
#include "movie_real.h"
#include <libavformat/avformat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s file.webm outdir [N=10] [step=1]\n", argv[0]); return 2; }
    int N = argc > 3 ? atoi(argv[3]) : 10, step = argc > 4 ? atoi(argv[4]) : 1; if (step < 1) step = 1;
    setenv("NXRT_MOVIE_DECODE", "1", 1);
    AVFormatContext *fc = NULL;
    if (avformat_open_input(&fc, argv[1], NULL, NULL) < 0 || avformat_find_stream_info(fc, NULL) < 0) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    int vs = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0), nsa = 0;
    for (unsigned i = 0; i < fc->nb_streams; i++) if (fc->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) nsa++;
    if (vs < 0) { fprintf(stderr, "no video stream\n"); return 1; }
    AVCodecParameters *cp = fc->streams[vs]->codecpar;
    printf("video: %s %dx%d, extradata %d bytes, %d audio stream(s)\n", avcodec_get_name(cp->codec_id), cp->width, cp->height, cp->extradata_size, nsa);
    mvr *r = mvr_create(cp->extradata, (size_t)cp->extradata_size, NULL, NULL, NULL);
    if (!r) { fprintf(stderr, "mvr_create failed\n"); return 1; }
    AVPacket *pk = av_packet_alloc(); AVRational ns = {1, 1000000000};
    int got = 0, eos = 0, sent = 0, done_in = 0; double t0 = now(); int64_t prev = -1, out_of_order = 0;
    mvr_pic p;
    while (!eos && got < 6000 && now() - t0 < 30.0) {
        if (!done_in && mvr_room(r) > 0) {
            if (av_read_frame(fc, pk) < 0) { mvr_submit(r, NULL, 0, 0, 1); done_in = 1; }
            else { if (pk->stream_index == vs) { mvr_submit(r, pk->data, (size_t)pk->size, av_rescale_q(pk->pts, fc->streams[vs]->time_base, ns), 0); sent++; } av_packet_unref(pk); }
        }
        int any = 0;
        while (mvr_fetch(r, &p)) {
            any = 1;
            if (p.eos) { eos = 1; break; }
            if (p.ts < prev) out_of_order++; prev = p.ts;
            if (got % step == 0 && got / step < N) { char path[512]; snprintf(path, sizeof path, "%s/frame_%04d.ppm", argv[2], got); mvr_write_ppm(path, p.nv12, p.w, p.h); printf("frame %d: ts %lld ms %dx%d (Y %zu + UV %zu bytes) -> %s\n", got, (long long)(p.ts / 1000000), p.w, p.h, p.ysz, p.uvsz, path); }
            got++; mvr_pic_free(&p); if (got >= N * step) break;
        }
        if (!any) usleep(300);
    }
    double dt = now() - t0;
    printf("%d packets sent, %d pictures in %.2f s (%.1f fps), timestamps out of order: %lld, end of stream seen: %d\n", sent, got, dt, got / dt, (long long)out_of_order, eos);
    mvr_destroy(r); av_packet_free(&pk); avformat_close_input(&fc);
    return eos && got > 0 ? 0 : 1;
}
