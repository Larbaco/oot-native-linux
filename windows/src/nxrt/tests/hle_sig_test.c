/* The adapters against direct host calls, on real nxnative signatures (copied from its sources, the names prefixed nxrt_test_ so their adapters do not
   clash with the registry's). gen_hle.py treats this file as a port (--extra): the adapters in hle_sig_test.adapters.inc are generated exactly as the
   production ones. Each case fills a register frame the way an AArch64 caller would (narrow arguments with garbage in the upper bits, as Switch
   callers leave them), runs the adapter, and compares what the host function received and what came back with a direct C call. */
#include <math.h>
#include <stdio.h>
#include "hle.h"

typedef struct { uint64_t v[2]; } Uid;
typedef struct { uint64_t value; } NVNseparateTextureHandle;
typedef struct NVNdevice NVNdevice;
typedef struct { uint32_t w[13]; } GLSLCversion;                 /* 0x34 bytes, returned through x8 */
typedef struct { float v[4]; } NxrtTestFloat4;                   /* float aggregates: synthetic, no nxnative signature has one */
typedef struct { double v[2]; } NxrtTestDouble2;

static struct { uint64_t a[12]; double f[8]; int n; } seen;
#define SEEN(...) do { uint64_t v_[] = {__VA_ARGS__}; seen.n = (int)(sizeof v_ / 8); for (int i_ = 0; i_ < seen.n; i_++) seen.a[i_] = v_[i_]; } while (0)

/* integers: hle_offline.c's SendTo (int, ptr, uint64_t (was unsigned long), int, ptr, unsigned) returning int64_t (was long); -1 must sign-extend */
HLE_FORCE("nxrt_test__ZN2nn6socket6SendToEiPKvmiPK8sockaddrj", "test", int64_t, t_sendto, int s, const void *b, uint64_t n, int f, const void *a, unsigned al) {
    SEEN((uint64_t)(int64_t)s, (uint64_t)(uintptr_t)b, n, (uint64_t)(int64_t)f, (uint64_t)(uintptr_t)a, al); return -1; }
/* a narrow argument (bool, declared unsigned and truncated by the host, as nxnative does) and a narrow result (glslcCompile's uint8_t) */
HLE_FORCE("nxrt_test__ZN2nn4pctl26SetStereoVisionRestrictionEb", "test", uint8_t, t_bool, unsigned on) { SEEN(on & 0xff); return (uint8_t)(0x100 + (on & 1)); }
/* pointers: hle_account.c's ListOpenUsers */
HLE_FORCE("nxrt_test__ZN2nn7account13ListOpenUsersEPiPNS0_3UidEi", "test", Result, t_list, int *n, Uid *out, int max) {
    SEEN((uint64_t)(uintptr_t)n, (uint64_t)(uintptr_t)out, (uint64_t)(int64_t)max); if (max > 0) { out->v[0] = 7; *n = 1; } return RES_OK; }
/* floats: hle_audio.c's SetAudioOutVolume / GetAudioOutVolume, hle_hid.c's SetSixAxisSensorFusionParameters (x and v registers counted apart) */
HLE_FORCE("nxrt_test__ZN2nn5audio17SetAudioOutVolumeEPNS0_8AudioOutEf", "test", void, t_setvol, void *g, float v) { SEEN((uint64_t)(uintptr_t)g); seen.f[0] = v; }
HLE_FORCE("nxrt_test__ZN2nn5audio17GetAudioOutVolumeEPKNS0_8AudioOutE", "test", float, t_getvol, const void *g) { SEEN((uint64_t)(uintptr_t)g); return 0.625f; }
HLE_FORCE("nxrt_test__ZN2nn3hid32SetSixAxisSensorFusionParametersERKNS0_19SixAxisSensorHandleEff", "test", void, t_fusion, const uint32_t *h, float a, float b) {
    SEEN((uint64_t)(uintptr_t)h); seen.f[0] = a; seen.f[1] = b; }
/* doubles: hle_table.c's pow */
HLE("nxrt_test_pow", double, t_pow, double x, double y) { seen.f[0] = x; seen.f[1] = y; return pow(x, y); }
/* a small struct result in x0: hle_nvn.c's nvnDeviceGetSeparateTextureHandle */
HLE_FORCE("nxrt_test_nvnDeviceGetSeparateTextureHandle", "test", NVNseparateTextureHandle, t_septex, const NVNdevice *d, int id) {
    SEEN((uint64_t)(uintptr_t)d, (uint64_t)(int64_t)id); return (NVNseparateTextureHandle){(uint64_t)id | 0xabc0000000000000ull}; }
/* a large struct result through x8: glslcGetVersion's GLSLCversion (0x34 bytes) */
HLE_FORCE("nxrt_test_glslcGetVersion", "test", GLSLCversion, t_glslcver, void) { GLSLCversion v; for (int i = 0; i < 13; i++) v.w[i] = 0x1000u + (uint32_t)i; return v; }
/* variadic: hle_net.c's Fcntl(int, FcntlCommand, ...): the Switch passes the variadic argument in the next register, x2 */
HLE_FORCE("nxrt_test__ZN2nn6socket5FcntlEiNS0_12FcntlCommandEz", "test", int, t_fcntl, int s, int c, int arg) { SEEN((uint64_t)(int64_t)s, (uint64_t)(int64_t)c, (uint64_t)(int64_t)arg); return -5; }
/* nine arguments: the ninth on the guest stack (hle_codec.c's InitializeHardwareOpusMultiStreamDecoder) */
HLE_FORCE("nxrt_test__ZN2nn5codec40InitializeHardwareOpusMultiStreamDecoderEPNS0_34HardwareOpusMultiStreamDecoderTypeEiiiiPKhPvmi", "test", int, t_opus,
          void *hw, int rate, int ch, int streams, int stereo, const uint8_t *mapping, void *buf, size_t size, int option) {
    SEEN((uint64_t)(uintptr_t)hw, (uint64_t)(int64_t)rate, (uint64_t)(int64_t)ch, (uint64_t)(int64_t)streams, (uint64_t)(int64_t)stereo, (uint64_t)(uintptr_t)mapping,
         (uint64_t)(uintptr_t)buf, size, (uint64_t)(int64_t)option); return 0; }
/* float aggregates (synthetic): one in v0-v3 as an argument after an integer, one returned in v0/v1 */
HLE_FORCE("nxrt_test_hfa", "test", NxrtTestDouble2, t_hfa, int k, NxrtTestFloat4 v, float w) {
    SEEN((uint64_t)(int64_t)k); for (int i = 0; i < 4; i++) seen.f[i] = v.v[i]; seen.f[4] = w; return (NxrtTestDouble2){{v.v[0] + w, (double)k}}; }

#include "hle_sig_test.adapters.inc"

static int g_fail;
#define T(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)
static NxwGuestCpu cpu;
static void reset(void) { memset(&cpu, 0, sizeof cpu); memset(&seen, 0, sizeof seen); for (int i = 0; i < 31; i++) cpu.x[i] = 0xdead000000000000ull | (uint64_t)i; }
static void setf(int i, float f) { memset(cpu.q[i], 0xee, 16); memcpy(cpu.q[i], &f, 4); }
static void setd(int i, double d) { memset(cpu.q[i], 0xee, 16); memcpy(cpu.q[i], &d, 8); }
static float getf(int i) { float f; memcpy(&f, cpu.q[i], 4); return f; }
static double getd(int i) { double d; memcpy(&d, cpu.q[i], 8); return d; }

int nxrt_sig_tests(void) {
    static uint8_t buf[256]; uint64_t junk = 0xffffffff00000000ull;
    int cases = 0;
    /* integers, sign extension of a narrow negative result */
    reset(); cpu.x[0] = junk | 3; cpu.x[1] = (uint64_t)(uintptr_t)buf; cpu.x[2] = 0x123456789ull; cpu.x[3] = junk | (uint32_t)-2; cpu.x[4] = (uint64_t)(uintptr_t)(buf + 8); cpu.x[5] = junk | 16;
    adapter_nxrt_test__ZN2nn6socket6SendToEiPKvmiPK8sockaddrj(&cpu);
    { uint64_t r = cpu.x[0];
      T(seen.a[0] == 3 && seen.a[1] == (uint64_t)(uintptr_t)buf && seen.a[2] == 0x123456789ull && seen.a[3] == (uint64_t)-2 && seen.a[4] == (uint64_t)(uintptr_t)(buf + 8) && seen.a[5] == 16);
      int64_t want = t_sendto(3, buf, 0x123456789ull, -2, buf + 8, 16);
      T(r == (uint64_t)want && (int64_t)r == -1); } cases++;
    /* bool argument with garbage above bit 7 (Switch callers do not extend), narrow result */
    reset(); cpu.x[0] = 0x12345601;
    adapter_nxrt_test__ZN2nn4pctl26SetStereoVisionRestrictionEb(&cpu);
    { uint64_t r = cpu.x[0]; T(seen.a[0] == 1 && (uint8_t)r == t_bool(0x12345601)); } cases++;
    /* pointers in and out */
    reset(); int n = 0; Uid u[2] = {{{0}}}; cpu.x[0] = (uint64_t)(uintptr_t)&n; cpu.x[1] = (uint64_t)(uintptr_t)u; cpu.x[2] = junk | 2;
    adapter_nxrt_test__ZN2nn7account13ListOpenUsersEPiPNS0_3UidEi(&cpu);
    T(n == 1 && u[0].v[0] == 7 && seen.a[2] == 2 && (uint32_t)cpu.x[0] == RES_OK); cases++;
    /* a float argument after a pointer: x0 and s0 */
    reset(); cpu.x[0] = 0x7100001000ull; setf(0, 0.75f);
    adapter_nxrt_test__ZN2nn5audio17SetAudioOutVolumeEPNS0_8AudioOutEf(&cpu);
    T(seen.a[0] == 0x7100001000ull && seen.f[0] == 0.75f); cases++;
    reset(); cpu.x[0] = 0x7100002000ull;
    adapter_nxrt_test__ZN2nn5audio17GetAudioOutVolumeEPKNS0_8AudioOutE(&cpu);
    { float r = getf(0); T(r == t_getvol((void *)0x7100002000ull) && r == 0.625f); } cases++;
    reset(); cpu.x[0] = 0x7100003000ull; setf(0, 1.5f); setf(1, -2.25f);
    adapter_nxrt_test__ZN2nn3hid32SetSixAxisSensorFusionParametersERKNS0_19SixAxisSensorHandleEff(&cpu);
    T(seen.a[0] == 0x7100003000ull && seen.f[0] == 1.5f && seen.f[1] == -2.25f); cases++;
    /* doubles */
    reset(); setd(0, 2.0); setd(1, 10.0);
    adapter_nxrt_test_pow(&cpu);
    { double r = getd(0); T(seen.f[0] == 2.0 && seen.f[1] == 10.0 && r == t_pow(2.0, 10.0) && r == 1024.0); } cases++;
    /* an 8-byte struct result in x0 */
    reset(); cpu.x[0] = 0x7100004000ull; cpu.x[1] = junk | 42;
    adapter_nxrt_test_nvnDeviceGetSeparateTextureHandle(&cpu);
    { uint64_t r = cpu.x[0]; T(seen.a[1] == 42 && r == t_septex((const NVNdevice *)0x7100004000ull, 42).value); } cases++;
    /* a 0x34-byte struct through x8, the bytes after it untouched */
    reset(); memset(buf, 0x77, sizeof buf); cpu.x[8] = (uint64_t)(uintptr_t)buf;
    adapter_nxrt_test_glslcGetVersion(&cpu);
    { GLSLCversion want = t_glslcver(); T(!memcmp(buf, &want, sizeof want) && buf[0x34] == 0x77); } cases++;
    /* a variadic argument in x2 */
    reset(); cpu.x[0] = 4; cpu.x[1] = 3; cpu.x[2] = junk | 0x800;
    adapter_nxrt_test__ZN2nn6socket5FcntlEiNS0_12FcntlCommandEz(&cpu);
    { uint64_t r = cpu.x[0]; T(seen.a[0] == 4 && seen.a[1] == 3 && seen.a[2] == 0x800 && (int32_t)r == -5 && r == (uint64_t)(int64_t)t_fcntl(4, 3, 0x800)); } cases++;
    /* the ninth argument from the guest stack (8-byte slot at sp) */
    reset(); static uint64_t stack[4]; stack[0] = junk | 7; cpu.sp = (uint64_t)(uintptr_t)stack;
    for (int i = 0; i < 8; i++) cpu.x[i] = 100 + (uint64_t)i;
    adapter_nxrt_test__ZN2nn5codec40InitializeHardwareOpusMultiStreamDecoderEPNS0_34HardwareOpusMultiStreamDecoderTypeEiiiiPKhPvmi(&cpu);
    T(seen.n == 9 && seen.a[0] == 100 && seen.a[7] == 107 && seen.a[8] == 7); cases++;
    /* a float aggregate argument in v0-v3 after an integer in x0, a float after it in v4; a double aggregate result in v0/v1 */
    reset(); cpu.x[0] = junk | 9; setf(0, 1.0f); setf(1, 2.0f); setf(2, 3.0f); setf(3, 4.0f); setf(4, 0.5f);
    adapter_nxrt_test_hfa(&cpu);
    { double r0 = getd(0), r1 = getd(1);
      T(seen.a[0] == 9 && seen.f[0] == 1.0 && seen.f[3] == 4.0 && seen.f[4] == 0.5);
      NxrtTestDouble2 want = t_hfa(9, (NxrtTestFloat4){{1, 2, 3, 4}}, 0.5f);
      T(r0 == want.v[0] && r1 == want.v[1] && r0 == 1.5); } cases++;
    printf("adapters: %d cases against direct calls, %s\n", cases, g_fail ? "FAILED" : "ok");
    return g_fail;
}
