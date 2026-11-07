// SDK 22 system mixer output, implemented at the SDK boundary (as AudioOut/audren are).
// Protocol reference: Lichii 4f9b9624; independently implemented, no GPL code copied.
#include "hle.h"
#include "sysmix_ring.h"
#include <math.h>
#include <stdbool.h>
#include <stdatomic.h>

void *dm_pcmout_open(int ch);
void dm_pcmout_start(void *out);
void dm_pcmout_close(void *out);
int dm_pcmout_append(void *out, void *buffer);
void *dm_pcmout_released(void *out);
void dm_pcmout_wait(void *out, int ms);

enum { SM_INVALID = 0xc8899, SM_STATE = 0xc8699, SM_UNSUPPORTED = 0x40699, SM_BUFFER = 0xa99, SM_BUFFERS = 8 };
typedef struct sm_stream {
    const void *key;
    void *memory; size_t size;
    sysmix_ring ring;
    pthread_mutex_t mu;
    plat_thr_t thread;
    _Atomic int running;
    int open, mode, threshold;
    uint64_t state_event, data_event;
    struct sm_stream *next;
} sm_stream;
static sm_stream *g_streams;
static pthread_mutex_t g_sm_mu = PTHREAD_MUTEX_INITIALIZER;
static sm_stream *sm_find(const void *key) { for (sm_stream *s = g_streams; s; s = s->next) if (s->key == key) return s; return NULL; }
static void sm_notify(sm_stream *s) {
    int filled = sysmix_ring_filled(&s->ring);
    if (filled < 0) { if (s->state_event) hle_signal_system_event_binding(s->state_event); }
    else if (s->mode == 2 && s->ring.slots - 1 - (uint32_t)filled >= (uint32_t)s->threshold && s->data_event)
        hle_signal_system_event_binding(s->data_event);
}
typedef struct { void *ptr; uint64_t cap, size; } sm_buffer;
static unsigned sm_play(void *arg) {
    sm_stream *s = arg;
    plat_thread_name("SDK22 system mixer");
    void *out = dm_pcmout_open(2);
    int16_t data[SM_BUFFERS][SYSMIX_BLOCK * 2]; sm_buffer buffers[SM_BUFFERS]; int nfree = SM_BUFFERS; sm_buffer *free_list[SM_BUFFERS];
    for (int i = 0; i < SM_BUFFERS; i++) { buffers[i] = (sm_buffer){data[i], sizeof data[i], sizeof data[i]}; free_list[i] = &buffers[i]; }
    dm_pcmout_start(out);
    while (atomic_load(&s->running)) {
        if (!nfree) { sm_buffer *b = dm_pcmout_released(out); if (!b) { dm_pcmout_wait(out, 10); continue; } free_list[nfree++] = b; }
        sm_buffer *b = free_list[--nfree];
        pthread_mutex_lock(&s->mu);
        int taken = sysmix_ring_render(&s->ring, b->ptr, SYSMIX_BLOCK);
        sm_notify(s);
        pthread_mutex_unlock(&s->mu);
        if (taken < 0 || !dm_pcmout_append(out, b)) break;
    }
    dm_pcmout_close(out);
    return 0;
}
static void sm_stop(sm_stream *s) {
    if (atomic_exchange(&s->running, 0)) {
#ifdef _WIN32
        plat_thr_join(s->thread, PLAT_FOREVER);
#else
        pthread_join((pthread_t)s->thread, NULL);
#endif
    }
}
static void sm_destroy_event(void *ev) {
    if (!ev) return;
    void *destroy = guest_sym("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE");
    if (destroy) GUEST_CALL(destroy, (uint64_t)(uintptr_t)ev);
}

HLE_FORCE("_ZN2nn5audio42IsVirtualOutputDeviceStreamFormatSupportedERKNS0_15AudioDeviceNameERKNS0_12StreamFormatE", "SDK 22 system mixer: stereo S16 or F32 at 48 kHz", bool, h_sm_Format, const void *name, const uint32_t *format) {
    (void)name; return format && sysmix_format_supported(format[0], format[1], format[2]);
}
HLE_FORCE("_ZN2nn5audio6sysmix12CreateStreamEPNS1_13StreamContextEjNS_4util17basic_string_viewIcNSt3__111char_traitsIcEEEENS4_10BitFlagSetILi32ENS1_24StreamCreationOptionFlagEEEPvm", "create an SDK 22 system mixer output stream over caller-owned memory", Result, h_sm_Create, void *key, uint32_t device, const char *name, size_t name_size, uint32_t flags, void *memory, size_t size) {
    (void)name; (void)name_size;
    if (!key || !device || !memory || ((uintptr_t)memory & 4095) || (size & 4095) || size < 0x2000) return SM_INVALID;
    if (flags != 0x24) return SM_UNSUPPORTED;
    pthread_mutex_lock(&g_sm_mu);
    if (sm_find(key)) { pthread_mutex_unlock(&g_sm_mu); return SM_STATE; }
    sm_stream *s = calloc(1, sizeof *s);
    if (!s) { pthread_mutex_unlock(&g_sm_mu); return SM_BUFFER; }
    s->key = key; s->memory = memory; s->size = size; pthread_mutex_init(&s->mu, NULL);
    s->next = g_streams; g_streams = s;
    pthread_mutex_unlock(&g_sm_mu);
    return 0;
}
HLE_FORCE("_ZN2nn5audio6sysmix13DestroyStreamEPNS1_13StreamContextE", "destroy the system mixer stream after joining its output worker", Result, h_sm_Destroy, void *key) {
    pthread_mutex_lock(&g_sm_mu);
    sm_stream **at = &g_streams; while (*at && (*at)->key != key) at = &(*at)->next;
    sm_stream *s = *at;
    if (!s) { pthread_mutex_unlock(&g_sm_mu); return SM_STATE; }
    *at = s->next; sm_stop(s); pthread_mutex_destroy(&s->mu); free(s);
    pthread_mutex_unlock(&g_sm_mu); return 0;
}
HLE_FORCE("_ZN2nn5audio6sysmix10OpenStreamEPNS1_13StreamContextERKNS0_12StreamFormatEiiNS_4util10BitFlagSetILi32ENS1_20StreamOpenOptionFlagEEE", "open S16/F32 stereo system mixer output, no prefill or flags", Result, h_sm_Open, void *key, const uint32_t *format, int frames, int prefill, uint32_t flags) {
    if (!format || frames <= 0) return SM_INVALID;
    if (prefill || flags || !sysmix_format_supported(format[0], format[1], format[2])) return SM_UNSUPPORTED;
    pthread_mutex_lock(&g_sm_mu); sm_stream *s = sm_find(key); Result result = SM_STATE;
    if (s && !s->open) {
        size_t need = sysmix_ring_size(format[0], (uint32_t)frames);
        if (!need || need > s->size) result = SM_BUFFER;
        else {
            memset((uint8_t *)s->memory + SYSMIX_READ, 0, 4); memset((uint8_t *)s->memory + SYSMIX_WRITE, 0, 4);
            result = sysmix_ring_open(&s->ring, s->memory, s->size, format[0], format[1], format[2], (uint32_t)frames) ? SM_INVALID : 0;
            if (!result) { s->open = 1; if (s->state_event) hle_signal_system_event_binding(s->state_event);
                LOG("sysmix: opened sample type %u, stereo 48000 Hz, capacity %d frames", format[0], frames); }
        }
    }
    pthread_mutex_unlock(&g_sm_mu); return result;
}
HLE_FORCE("_ZN2nn5audio6sysmix11CloseStreamEPNS1_13StreamContextE", "stop the system mixer and release its PCM output", Result, h_sm_Close, void *key) {
    pthread_mutex_lock(&g_sm_mu); sm_stream *s = sm_find(key);
    if (!s || !s->open) { pthread_mutex_unlock(&g_sm_mu); return SM_STATE; }
    sm_stop(s); s->open = 0; s->mode = 0;
    LOG("sysmix: closed, %llu frames consumed, %llu nonzero, %llu padded blocks",
        (unsigned long long)s->ring.frames, (unsigned long long)s->ring.nonzero, (unsigned long long)s->ring.underruns);
    if (s->state_event) hle_signal_system_event_binding(s->state_event);
    pthread_mutex_unlock(&g_sm_mu); return 0;
}
static Result sm_engage(const void *const *keys, int count, int engage) {
    if (!keys || count < 1 || count > 16) return SM_INVALID;
    pthread_mutex_lock(&g_sm_mu);
    for (int i = 0; i < count; i++) {
        sm_stream *s = sm_find(keys[i]);
        if (!s || !s->open) { pthread_mutex_unlock(&g_sm_mu); return SM_STATE; }
        pthread_mutex_lock(&s->mu); int fault = s->ring.fault; pthread_mutex_unlock(&s->mu);
        if (fault) { pthread_mutex_unlock(&g_sm_mu); return SM_STATE; }
    }
    Result result = 0;
    for (int i = 0; i < count; i++) {
        sm_stream *s = sm_find(keys[i]);
        if (engage && !atomic_load(&s->running)) {
            atomic_store(&s->running, 1);
            if (plat_thr_start(&s->thread, sm_play, s)) { atomic_store(&s->running, 0); result = SM_BUFFER; break; }
        } else if (!engage) sm_stop(s);
        if (s->state_event) hle_signal_system_event_binding(s->state_event);
    }
    pthread_mutex_unlock(&g_sm_mu); return result;
}
HLE_FORCE("_ZN2nn5audio6sysmix13EngageStreamsEPKPNS1_13StreamContextEi", "start system mixer outputs paced by the host PCM backend", Result, h_sm_Engage, const void *const *keys, int count) { return sm_engage(keys, count, 1); }
HLE_FORCE("_ZN2nn5audio6sysmix16DisengageStreamsEPKPNS1_13StreamContextEi", "stop system mixer outputs without consuming pending ring frames", Result, h_sm_Disengage, const void *const *keys, int count) { return sm_engage(keys, count, 0); }
HLE_FORCE("_ZN2nn5audio6sysmix14GetStreamStateEPNS1_11StreamStateEPKNS1_13StreamContextE", "system mixer state: closed, open, or engaged", Result, h_sm_State, uint32_t *state, const void *key) {
    if (!state) return SM_INVALID;
    pthread_mutex_lock(&g_sm_mu); sm_stream *s = sm_find(key);
    if (s) { pthread_mutex_lock(&s->mu); *state = s->open && !s->ring.fault ? atomic_load(&s->running) ? 2 : 1 : 0; pthread_mutex_unlock(&s->mu); }
    pthread_mutex_unlock(&g_sm_mu); return s ? 0 : SM_STATE;
}
static Result sm_count(int *out, const void *key, int free_frames) {
    if (!out) return SM_INVALID;
    *out = 0;
    pthread_mutex_lock(&g_sm_mu); sm_stream *s = sm_find(key); Result result = SM_STATE;
    if (s && s->open) { pthread_mutex_lock(&s->mu); int n = sysmix_ring_filled(&s->ring); if (n >= 0) { *out = free_frames ? (int)s->ring.slots - 1 - n : n; result = 0; } pthread_mutex_unlock(&s->mu); }
    pthread_mutex_unlock(&g_sm_mu); return result;
}
HLE_FORCE("_ZN2nn5audio6sysmix23GetStreamFreeFrameCountEPiPKNS1_13StreamContextE", "system mixer ring free frames", Result, h_sm_Free, int *out, const void *key) { return sm_count(out, key, 1); }
HLE_FORCE("_ZN2nn5audio6sysmix25GetStreamFilledFrameCountEPiPKNS1_13StreamContextE", "system mixer ring filled frames", Result, h_sm_Filled, int *out, const void *key) { return sm_count(out, key, 0); }
HLE_FORCE("_ZN2nn5audio6sysmix11WriteStreamEPiPNS1_13StreamContextEPKvi", "write as many complete system mixer frames as fit, with backpressure", Result, h_sm_Write, int *written, void *key, const void *data, int frames) {
    if (!written) return SM_INVALID;
    *written = 0;
    pthread_mutex_lock(&g_sm_mu); sm_stream *s = sm_find(key); Result result = SM_STATE;
    if (s && s->open) { pthread_mutex_lock(&s->mu); int n = sysmix_ring_write(&s->ring, data, frames); if (n >= 0) { *written = n; result = 0; } else result = SM_INVALID; pthread_mutex_unlock(&s->mu); }
    pthread_mutex_unlock(&g_sm_mu); return result;
}
HLE_FORCE("_ZN2nn5audio6sysmix13SetStreamGainEfPKNS1_13StreamContextENS_8TimeSpanE", "system mixer gain, immediate finite 0..128 only", Result, h_sm_SetGain, float gain, const void *key, int64_t ramp_ns) {
    if (ramp_ns) return SM_UNSUPPORTED;
    if (!isfinite(gain) || gain < 0 || gain > 128) return SM_INVALID;
    pthread_mutex_lock(&g_sm_mu); sm_stream *s = sm_find(key);
    if (s && s->open) { pthread_mutex_lock(&s->mu); s->ring.gain = gain; pthread_mutex_unlock(&s->mu); }
    int ok = s && s->open; pthread_mutex_unlock(&g_sm_mu); return ok ? 0 : SM_STATE;
}
HLE_FORCE("_ZN2nn5audio6sysmix13GetStreamGainEPfPKNS1_13StreamContextE", "system mixer gain", Result, h_sm_GetGain, float *gain, const void *key) {
    if (!gain) return SM_INVALID;
    pthread_mutex_lock(&g_sm_mu); sm_stream *s = sm_find(key);
    if (s && s->open) { pthread_mutex_lock(&s->mu); *gain = s->ring.gain; pthread_mutex_unlock(&s->mu); }
    int ok = s && s->open; pthread_mutex_unlock(&g_sm_mu); return ok ? 0 : SM_STATE;
}
static Result sm_event(void *ev, const void *key, int data, int mode, int threshold, int release) {
    if (!ev || (data && (threshold < 0 || (mode != 0 && mode != 2)))) return SM_INVALID;
    pthread_mutex_lock(&g_sm_mu); sm_stream *s = sm_find(key);
    if (!s || (data && (!s->open || (uint32_t)threshold >= s->ring.slots))) { pthread_mutex_unlock(&g_sm_mu); return SM_STATE; }
    // Bind outside the ring lock: this runs SDK code on the guest thread.
    if (!release) hle_bind_system_event(ev, 1);
    pthread_mutex_lock(&s->mu);
    if (data) { s->mode = release ? 0 : mode; s->threshold = threshold; s->data_event = release ? 0 : hle_system_event_binding(ev); if (!release) sm_notify(s); }
    else { s->state_event = release ? 0 : hle_system_event_binding(ev); if (!release) hle_signal_system_event_binding(s->state_event); }
    pthread_mutex_unlock(&s->mu);
    pthread_mutex_unlock(&g_sm_mu);
    if (release) sm_destroy_event(ev);
    return 0;
}
HLE_FORCE("_ZN2nn5audio6sysmix29AcquireStreamStateChangeEventEPNS_2os11SystemEventEPKNS1_13StreamContextE", "bind a system mixer state event", Result, h_sm_StateEvent, void *ev, const void *key) { return sm_event(ev, key, 0, 0, 0, 0); }
HLE_FORCE("_ZN2nn5audio6sysmix29ReleaseStreamStateChangeEventEPNS_2os11SystemEventEPKNS1_13StreamContextE", "release a system mixer state event", Result, h_sm_ReleaseState, void *ev, const void *key) { return sm_event(ev, key, 0, 0, 0, 1); }
HLE_FORCE("_ZN2nn5audio6sysmix22AcquireStreamDataEventEPNS_2os11SystemEventEPKNS1_13StreamContextENS1_26StreamDataNotificationModeEi", "notify when enough ring frames are free (mode 0 or 2)", Result, h_sm_DataEvent, void *ev, const void *key, int mode, int threshold) { return sm_event(ev, key, 1, mode, threshold, 0); }
HLE_FORCE("_ZN2nn5audio6sysmix22ReleaseStreamDataEventEPNS_2os11SystemEventEPKNS1_13StreamContextE", "release a system mixer data event", Result, h_sm_ReleaseData, void *ev, const void *key) { return sm_event(ev, key, 1, 0, 0, 1); }
HLE_FORCE("_ZN2nn5audio6sysmix12RenameStreamEPNS1_13StreamContextENS_4util17basic_string_viewIcNSt3__111char_traitsIcEEEE", "system mixer stream name has no routing effect on Linux", Result, h_sm_Rename, void *key, const char *name, size_t size) {
    (void)name; (void)size; pthread_mutex_lock(&g_sm_mu); int found = sm_find(key) != NULL; pthread_mutex_unlock(&g_sm_mu); return found ? 0 : SM_STATE;
}

HLE_FORCE("_ZN2nn5audio6sysmix19GetStreamStatisticsEPNS1_16StreamStatisticsEPNS1_13StreamContextEb", "system mixer detailed device statistics are unsupported", Result, h_sm_Statistics, void *out, void *key, bool reset) {
    (void)out; (void)key; (void)reset; return SM_UNSUPPORTED;
}
HLE_FORCE("_ZN2nn5audio6sysmix10ReadStreamEPiPvPNS_8TimeSpanEPNS1_13StreamContextEi", "system mixer output has no capture path", Result, h_sm_Read, int *read, void *data, void *time, void *key, int frames) {
    (void)data; (void)time; (void)key; (void)frames; if (read) *read = 0; return SM_UNSUPPORTED;
}
HLE_FORCE("_ZN2nn5audio6sysmix11PrimeStreamEPiPNS1_13StreamContextEi", "system mixer priming is unsupported", Result, h_sm_Prime, int *written, void *key, int frames) {
    (void)key; (void)frames; if (written) *written = 0; return SM_UNSUPPORTED;
}
HLE_FORCE("_ZN2nn5audio6sysmix11PurgeStreamEPiPNS1_13StreamContextEi", "system mixer purging is unsupported", Result, h_sm_Purge, int *removed, void *key, int frames) {
    (void)key; (void)frames; if (removed) *removed = 0; return SM_UNSUPPORTED;
}
HLE_FORCE("_ZN2nn5audio6sysmix11ClockStreamEPNS1_13StreamContextENS_8TimeSpanE", "system mixer external clock is unsupported", Result, h_sm_Clock, void *key, int64_t time) {
    (void)key; (void)time; return SM_UNSUPPORTED;
}
#include "hle_sysmix.adapters.inc"
