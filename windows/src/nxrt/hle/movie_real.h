// Real video decoding for the nn::movie stand-ins (hle_nv.c). AV1 through libavcodec (libdav1d), linked dynamically only when pkg-config finds it
// (NXRT_HAVE_FFMPEG); without it mvr_create() returns NULL and the timed stand-in stays in charge. The decoder runs on its own thread so the game's threads never wait on it.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct mvr mvr;
// One decoded picture in the layout the game expects: NV12 (Y plane, then interleaved UV), tightly packed (pitch = width), Y = ysz bytes, UV = uvsz bytes.
// eos pictures carry no pixels (nv12 == NULL). The caller owns nv12 and releases it with mvr_pic_free.
typedef struct { int64_t ts; int eos, w, h; size_t ysz, uvsz; uint8_t *nv12; } mvr_pic;
void mvr_pic_free(mvr_pic *p);

// Debug: BT.709 limited-range NV12 -> RGB PPM (what the game's shader makes of the buffer). Used by the harness and by NXRT_MOVIE_DUMP.
int mvr_write_ppm(const char *path, const uint8_t *nv12, int w, int h);
int mvr_available(void);   // 1 when built with FFmpeg and not disabled by NXRT_MOVIE_DECODE=0
int mvr_build_available(void); // 1 when FFmpeg support is linked into nxrt
// cfg/cfgsz: the track configuration (av1C). on_input is called from the worker thread when it takes a packet (the input queue has room), on_output when a
// picture is ready. Both may be NULL. Returns NULL when decoding is off or unavailable.
mvr *mvr_create(const void *cfg, size_t cfgsz, void (*on_input)(void *), void (*on_output)(void *), void *user);
// Queue one packet (data may be NULL/0). eos: after the data the decoder is drained and an end-of-stream picture follows the last frame.
// Returns 0 when accepted, 1 when the input queue is full (the caller waits for on_input).
int mvr_submit(mvr *r, const uint8_t *data, size_t n, int64_t ts_ns, int eos);
int mvr_room(mvr *r);                  // free slots in the input queue
int mvr_fetch(mvr *r, mvr_pic *out);   // pops the oldest finished picture: 1 when there was one
void mvr_flush(mvr *r);                // drop everything queued and everything inside the decoder
void mvr_destroy(mvr *r);              // joins the worker; nothing is called back afterwards
