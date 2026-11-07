// Synthetic SDK 22 audio contracts; dummy SDL only, no game data or real devices.
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nxrt/runtime.h"
#include "hle_adapters.h"
#include "plat.h"
#include "sysmix_ring.h"
#include "audio_revision.h"
NXRT_EARLY_RESERVE()
static int failures;
#define T(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)
size_t dm_audren_service_work_size(const uint8_t *p);
uint64_t now_ns(void);
uint64_t dm_audio_render_calls(void);

static void revisions(void) {
    uint8_t p[0x40] = {0};
    uint32_t values[] = {48000, 240, 72, 11, 96, 2, 16, 0, 0, 96, 96};
    memcpy(p, values, sizeof values);
    uint32_t upper = 0x3f564552, lower = 0x3f764552;
    memcpy(p + 0x34, &upper, 4); size_t a = dm_audren_service_work_size(p);
    memcpy(p + 0x34, &lower, 4); T(a && a == dm_audren_service_work_size(p));
    T(audio_revision_number(upper) == 15 && audio_revision_number(lower) == 15);
    memset(p + 0x34, 0, 4); memcpy(p + 0x38, &lower, 4);
    T(a == dm_audren_service_work_size(p));
    T(audio_revision_number(0x40764552) == 16);
    T(audio_revision_number(0x30564552) == 0 && audio_revision_number(0x41564552) < 0);
    T(audio_revision_number(0x3f784552) < 0);
    uint32_t native16 = 0x40764552;
    memcpy(p + 0x38, &native16, 4); T(dm_audren_service_work_size(p) == 0);
    printf("revision: REV?/REv?, parameter offsets 0x34/0x38, budget %#zx\n", a);
}
static void ring_tests(void) {
    void *mem = plat_aligned_alloc(0x4000, 4096); memset(mem, 0, 0x4000);
    sysmix_ring r; int16_t out[16];
    T(sysmix_format_supported(2, 2, 48000) && sysmix_format_supported(5, 2, 48000));
    T(!sysmix_format_supported(3, 2, 48000) && !sysmix_format_supported(5, 6, 48000) && !sysmix_format_supported(2, 2, 44100));
    T(sysmix_ring_open(&r, mem, 0x4000, 2, 2, 48000, 3) == 0);
    int16_t input[] = {100,-200,300,-400,500,-600,700,-800};
    T(sysmix_ring_write(&r, input, 4) == 3); T(sysmix_ring_write(&r, input, 1) == 0);
    T(sysmix_ring_render(&r, out, 2) == 2 && !memcmp(out, input, 8));
    T(sysmix_ring_write(&r, input + 6, 1) == 1); T(sysmix_ring_write(&r, input, 1) == 1);
    T(sysmix_ring_render(&r, out, 4) == 3);
    T(out[0] == 500 && out[2] == 700 && out[4] == 100 && out[6] == 0 && out[7] == 0);
    T(r.frames == 5 && r.nonzero == 5 && r.underruns == 1);
    atomic_store((_Atomic uint32_t *)((uint8_t *)mem + SYSMIX_WRITE), r.slots);
    T(sysmix_ring_render(&r, out, 1) == -1 && r.fault && out[0] == 0);
    memset(mem, 0, 0x4000);
    T(sysmix_ring_open(&r, mem, 0x4000, 5, 2, 48000, 8) == 0);
    float floats[] = {1,-1,2,-2,NAN,INFINITY,0.25f,-0.5f};
    r.gain = 2;
    T(sysmix_ring_write(&r, floats, 4) == 4 && sysmix_ring_render(&r, out, 4) == 4);
    T(out[0] == 32767 && out[1] == -32768 && out[2] == 32767 && out[3] == -32768);
    T(out[4] == 0 && out[5] == 0 && out[6] == 16384 && out[7] == -32768);
    T(sysmix_ring_open(&r, (uint8_t *)mem + 1, 0x2000, 2, 2, 48000, 3) == -1);
    T(!sysmix_ring_size(2, 0) && !sysmix_ring_size(5, 0x7fffffff));
    T(sysmix_ring_open(&r, mem, 0x2000, 5, 2, 48000, 2000) == -1);
    atomic_store((_Atomic uint32_t *)((uint8_t *)mem + SYSMIX_READ), 99);
    T(sysmix_ring_filled(&r) == -1);
    plat_aligned_free(mem);
    puts("ring: partial/full/wrap, S16/F32, gain, clamp, NaN/Inf, bounds and index faults checked");
}
#define CALL(name, ...) call(adapter_##name, (uint64_t[]){__VA_ARGS__}, sizeof((uint64_t[]){__VA_ARGS__})/8)
#define P(p) ((uint64_t)(uintptr_t)(p))
static uint64_t call(void (*adapter)(NxwGuestCpu *), const uint64_t *args, size_t count) {
    NxwGuestCpu cpu = {0}; for (size_t i = 0; i < count; i++) cpu.x[i] = args[i]; adapter(&cpu); return cpu.x[0];
}
static void hle_output(int type) {
    void *mem = plat_aligned_alloc(0x8000, 4096); memset(mem, 0, 0x8000);
    uint8_t key[0x200] = {0}; uint32_t format[] = {(uint32_t)type, 2, 48000}; const void *keys[] = {key};
    T(CALL(_ZN2nn5audio42IsVirtualOutputDeviceStreamFormatSupportedERKNS0_15AudioDeviceNameERKNS0_12StreamFormatE, 0, P(format)) == 1);
    T(CALL(_ZN2nn5audio6sysmix12CreateStreamEPNS1_13StreamContextEjNS_4util17basic_string_viewIcNSt3__111char_traitsIcEEEENS4_10BitFlagSetILi32ENS1_24StreamCreationOptionFlagEEEPvm, P(key),1,0,0,0x24,P(mem),0x8000) == 0);
    T(CALL(_ZN2nn5audio6sysmix10OpenStreamEPNS1_13StreamContextERKNS0_12StreamFormatEiiNS_4util10BitFlagSetILi32ENS1_20StreamOpenOptionFlagEEE, P(key),P(format),2400,0,0) == 0);
    T(CALL(_ZN2nn5audio6sysmix10OpenStreamEPNS1_13StreamContextERKNS0_12StreamFormatEiiNS_4util10BitFlagSetILi32ENS1_20StreamOpenOptionFlagEEE, P(key),P(format),2400,0,0) != 0);
    int16_t pcm[960*2]; float fp[960*2];
    for (int i = 0; i < 960*2; i++) { pcm[i] = (int16_t)((i % 96 - 48) * 500); fp[i] = pcm[i] / 32768.0f; }
    int written = 0, total = 0;
    T(CALL(_ZN2nn5audio6sysmix11WriteStreamEPiPNS1_13StreamContextEPKvi, P(&written),P(key),P(type == 2 ? (void *)pcm : (void *)fp),960) == 0);
    total += written;
    uint64_t callbacks = dm_audio_render_calls(), t0 = now_ns();
    T(CALL(_ZN2nn5audio6sysmix13EngageStreamsEPKPNS1_13StreamContextEi, P(keys),1) == 0);
    while (total < 48000 && now_ns() - t0 < 3000000000ull) {
        int n = 48000 - total; if (n > 960) n = 960;
        T(CALL(_ZN2nn5audio6sysmix11WriteStreamEPiPNS1_13StreamContextEPKvi, P(&written),P(key),P(type == 2 ? (void *)pcm : (void *)fp),n) == 0);
        total += written; if (!written) plat_sleep_ms(2);
    }
    int filled = 1;
    while (filled && now_ns() - t0 < 4000000000ull) {
        T(CALL(_ZN2nn5audio6sysmix25GetStreamFilledFrameCountEPiPKNS1_13StreamContextE, P(&filled),P(key)) == 0); plat_sleep_ms(2);
    }
    double elapsed = (now_ns() - t0) / 1e9;
    T(total == 48000 && !filled && elapsed > 0.8 && elapsed < 1.5);
    T(dm_audio_render_calls() > callbacks + 50);
    T(CALL(_ZN2nn5audio6sysmix16DisengageStreamsEPKPNS1_13StreamContextEi, P(keys),1) == 0);
    uint32_t state = 99;
    T(CALL(_ZN2nn5audio6sysmix14GetStreamStateEPNS1_11StreamStateEPKNS1_13StreamContextE, P(&state),P(key)) == 0 && state == 1);
    T(CALL(_ZN2nn5audio6sysmix13EngageStreamsEPKPNS1_13StreamContextEi, P(keys),1) == 0);
    plat_sleep_ms(30);
    T(CALL(_ZN2nn5audio6sysmix11CloseStreamEPNS1_13StreamContextE, P(key)) == 0);
    T(CALL(_ZN2nn5audio6sysmix13DestroyStreamEPNS1_13StreamContextE, P(key)) == 0);
    printf("HLE sample type %d: 48000 synthetic frames drained in %.3f s, %llu SDL callbacks; stop/restart/close checked\n", type, elapsed, (unsigned long long)(dm_audio_render_calls() - callbacks));
    plat_aligned_free(mem);
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
#ifdef _WIN32
    _putenv_s("SDL_AUDIO_DRIVER", "dummy"); _putenv_s("SDL_AUDIODRIVER", "dummy"); _putenv_s("DM_AUDIO", "1");
#else
    setenv("SDL_AUDIO_DRIVER", "dummy", 1); setenv("SDL_AUDIODRIVER", "dummy", 1); setenv("DM_AUDIO", "1", 1);
#endif
    revisions(); ring_tests();
    if (nxrt_init(&(nxrt_options){.quiet = 1})) return 1;
    hle_output(2); hle_output(5);
    printf("nxrt.sysmix: %s\n", failures ? "FAIL" : "ok"); return failures != 0;
}
