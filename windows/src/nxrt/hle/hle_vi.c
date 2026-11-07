// from nxnative dm/gfx/hle_vi.c (MIT, © seedds). Windows: unchanged (no front end: the native window is the host's static object).
// nn::vi (display layer): on a real device, the vi service is used to open the display, create a layer and get the native window handle; the handle is passed to NVN's window builder.
// Host: returns host static objects; when hooking up Metal, the native window corresponds to the front end's CAMetalLayer (front_metal_layer()).
#include "hle.h"
#include <stdatomic.h>

static struct { uint32_t magic; } g_display = {0x50534944}, g_layer = {0x5259414c};
static struct { uint32_t magic; void *layer; } g_native = {0x4e49574e, 0};

HLE_FORCE("_ZN2nn2vi10InitializeEv", "display service init; the host has no vi service", void, h_vi_Init, void) {}
HLE_FORCE("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", "open default display; the host returns a static object", Result, h_vi_OpenDisp, void **out) {
    *out = &g_display; return RES_OK;
}
HLE_FORCE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", "create layer; the host returns a static object", Result, h_vi_CreateLayer, void **out, void *disp) {
    (void)disp; *out = &g_layer; return RES_OK;
}
// The overload with the layer's size (SDL3's Switch video driver, Absolum: 1920x1080 docked); the SDK's version calls through the vi service object the host never made (SIGSEGV at +0xe4).
// An NVN or Vulkan window's textures decide what is presented; an EGL window surface takes the layer's size (dm/gfx/gl/hle_gl.m: Digimon Story Cyber Sleuth sizes its layer
// itself and draws into the surface at that size)
static _Atomic uint32_t g_layer_wh;   // w | h << 16 of the last sized CreateLayer, 0 = none
void dm_vi_layer_size(int *w, int *h) { uint32_t v = g_layer_wh; *w = (int)(v & 0xffff); *h = (int)(v >> 16); }
HLE_FORCE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayEii", "create layer of a given size; the host returns a static object", Result, h_vi_CreateLayerWH, void **out, void *disp, int w, int h) {
    (void)disp; LOG("vi::CreateLayer %dx%d", w, h); *out = &g_layer;
    if (w > 0 && h > 0 && w < 0x10000 && h < 0x10000) g_layer_wh = (uint32_t)w | (uint32_t)h << 16;
    return RES_OK;
}
// SDK 22's overload with a LayerCreationSettings, which its constructor LayerCreationSettings(int w, int h) fills as {w, h, 2} (sdk+0x4526a0 in EA SPORTS FC 27; the SDK's own
// constructor runs). The SDK's CreateLayer goes to am::GetSelfController(), which the host never made (SIGSEGV at the load from it, sdk+0x44ea94)
HLE_FORCE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayEPKNS0_21LayerCreationSettingsE", "create layer from creation settings (SDK 22); the host returns a static object",
          Result, h_vi_CreateLayerSettings, void **out, void *disp, const int32_t *settings) {
    return h_vi_CreateLayerWH(out, disp, settings ? settings[0] : 0, settings ? settings[1] : 0);
}
HLE_FORCE("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", "native window handle, passed to the NVN window builder; the host returns a static object", Result, h_vi_GetNative, void **out, void *layer) {
    (void)layer; *out = &g_native; return RES_OK;
}
// Link's Awakening also crops its layer and tears the display down on exit; the SDK's versions dereference the vi service objects the host never made (SetLayerCrop: SIGSEGV at +0x64).
// The crop is kept for the present: the Vulkan present (hle_vk.m), where Super Mario Galaxy keeps a 1920x1080 swapchain in handheld mode, draws into its top-left 1280x720 and
// crops the layer to that every frame; and the NVN present when the game sets no window crop of its own (nvnWindowSetCrop; present_crop in hle_nvn.c), where Link's Awakening
// draws 1280x720 into its 1920x1080 window textures in handheld mode and crops the layer to that. Logged only when it changes
static _Atomic uint64_t g_crop;   // x | y << 16 | w << 32 | h << 48, 0 = none
void dm_vi_layer_crop(int c[4]) { uint64_t v = g_crop; c[0] = (int)(v & 0xffff); c[1] = (int)(v >> 16 & 0xffff); c[2] = (int)(v >> 32 & 0xffff); c[3] = (int)(v >> 48); }
HLE_FORCE("_ZN2nn2vi12SetLayerCropEPNS0_5LayerEiiii", "layer crop; the host records it and the present crops by it", Result, h_vi_SetCrop, void *layer, int x, int y, int w, int h) {
    (void)layer;
    uint64_t v = x < 0 || y < 0 || w <= 0 || h <= 0 || x > 0xffff || y > 0xffff || w > 0xffff || h > 0xffff ? 0 : (uint64_t)x | (uint64_t)y << 16 | (uint64_t)w << 32 | (uint64_t)h << 48;
    if (atomic_exchange(&g_crop, v) != v) LOG("vi::SetLayerCrop %d,%d %dx%d", x, y, w, h);
    return RES_OK;
}
// Super Mario Odyssey sets its layer's scaling mode; the SDK's version uses the vi service objects the host never made
HLE_FORCE("_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE", "layer scaling mode; the host present does its own scaling", Result, h_vi_SetScaling, void *layer, int mode) {
    (void)layer; LOG("vi::SetLayerScalingMode %d (ignored)", mode); return RES_OK;
}
// Unreal Engine paces its frames by the layer's frame number (Octopath Traveler 0, main+0x10140e8); the SDK's versions read the Android surface of the vi service the host never made
// (SIGSEGV in android::Surface::getQueuedFrameNumber). The host's frame number is the count of frames the game has presented
uint64_t dm_nvn_frame_count(void);
HLE_FORCE("_ZN2nn2vi20GetLatestFrameNumberEPmPKNS0_5LayerE", "the layer's latest frame number: the frames presented so far", Result, h_vi_FrameNo, uint64_t *out, const void *layer) {
    (void)layer; *out = dm_nvn_frame_count(); return RES_OK;
}
HLE_FORCE("_ZN2nn2vi16ListFrameHistoryEPNS0_9FrameInfoEiPKNS0_5LayerE", "the layer's frame history: none kept, 0 entries", int, h_vi_FrameHist, void *out, int n, const void *layer) {
    (void)out; (void)layer; static int once; if (!once++) LOG("vi::ListFrameHistory (%d entries asked): the host keeps no frame history, 0 returned", n);
    return 0;
}
// Rhythm Heaven Groove (SDK 21) asks for the built-in panel's identity at start (main+0xa9a4); the SDK's version calls through the vi service object the host never made (SIGSEGV at +0x20).
//   The game reads a u32 kind and, when it is 2 or more, 0x14 more bytes that it hands on; any other answer makes it hand on zeros. The host has no panel: an all-zero identifier, kind 0
HLE_FORCE("_ZN2nn2vi32GetDefaultDisplayPanelIdentifierEPNS0_15PanelIdentifierE", "built-in panel identity; the host has no panel: all zero", Result, h_vi_PanelId, void *out) {
    memset(out, 0, 0x18); return RES_OK;
}
// The Great Ace Attorney Chronicles (SDK 10) lists up to 10 displays before opening the default one (main+0x808f08) and does not read the list. DisplayInfo is 0x60 bytes:
//   name[0x40], bool has a layer limit at +0x40, then u64 layer count limit, largest layer width and height. One display, "Default", as Horizon lists for an application
HLE_FORCE("_ZN2nn2vi12ListDisplaysEPNS0_11DisplayInfoEi", "list displays; the host has one, \"Default\"", int, h_vi_ListDisp, void *out, int n) {
    if (n <= 0 || !out) return 0;
    uint8_t *p = out; memset(p, 0, 0x60); strcpy((char *)p, "Default");
    p[0x40] = 1; uint64_t lim[3] = {1, 1920, 1080}; memcpy(p + 0x48, lim, sizeof lim);
    return 1;
}
HLE_FORCE("_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", "destroy layer; host static object", void, h_vi_DestroyLayer, void *layer) { (void)layer; }
HLE_FORCE("_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", "close display; host static object", void, h_vi_CloseDisp, void *disp) { (void)disp; }
HLE_FORCE("_ZN2nn2vi8FinalizeEv", "display service shutdown; the host has no vi service", void, h_vi_Fini, void) {}

// ---------- SWITCH2 (private bring-up, Pokopia SDK 20.5.25): display calls that only exist on the Switch 2 vi service ----------
HLE_FORCE("_ZN2nn2vi23SetDisplayVrrCapabilityEPNS0_7DisplayEb", "SWITCH2: variable refresh rate capability of the display; the host ignores it", Result, h_vi_SetVrr, void *disp, unsigned on) { (void)disp; (void)on; return 0; }
HLE_FORCE("_ZN2nn2vi52SetLayerNextHdrExtendedContentLuminanceRangeMetadataEPNS0_5LayerEtt", "SWITCH2: HDR extended luminance range metadata for the next frame; the host presents SDR", Result, h_vi_HdrExt, void *layer, unsigned lo, unsigned hi) { (void)layer; (void)lo; (void)hi; return 0; }
HLE_FORCE("_ZN2nn2vi52SetLayerNextHdrStandardContentLuminanceRangeMetadataEPNS0_5LayerEtt", "SWITCH2: HDR standard luminance range metadata for the next frame; the host presents SDR", Result, h_vi_HdrStd, void *layer, unsigned lo, unsigned hi) { (void)layer; (void)lo; (void)hi; return 0; }

// ---------- SWITCH2 (SDK 22.2.8, Ocarina of Time): the rest of the layer/display extensions. The SDK's versions go through vi service objects the host never made
// (ClearLayerExtendedSwapInterval jumped to a null function pointer); the host presents SDR at its own refresh rate, so they only need to succeed ----------
HLE_FORCE("_ZN2nn2vi28SetLayerExtendedSwapIntervalEPNS0_5LayerEii", "SWITCH2: extended swap interval of the layer; the host paces by its own vsync", Result, h_vi_SetXSwap, void *layer, int a, int b) { (void)layer; (void)a; (void)b; return 0; }
HLE_FORCE("_ZN2nn2vi30ClearLayerExtendedSwapIntervalEPNS0_5LayerE", "SWITCH2: clears the extended swap interval", Result, h_vi_ClrXSwap, void *layer) { (void)layer; return 0; }
HLE_FORCE("_ZN2nn2vi16SetLayerUserDataEPNS0_5LayerEm", "layer user data (kept by the SDK for its frame history); the host records nothing", Result, h_vi_UserData, void *layer, uint64_t v) { (void)layer; (void)v; return 0; }
HLE_FORCE("_ZN2nn2vi35SetLayerNextHdrContentGamutMetadataEPNS0_5LayerENS0_15HdrContentGamutE", "SWITCH2: HDR content gamut for the next frame; the host presents SDR", Result, h_vi_HdrGamut, void *layer, int g) { (void)layer; (void)g; return 0; }
HLE_FORCE("_ZN2nn2vi37ClearLayerNextHdrContentGamutMetadataEPNS0_5LayerE", "SWITCH2: clears the HDR content gamut", Result, h_vi_HdrGamutClr, void *layer) { (void)layer; return 0; }
HLE_FORCE("_ZN2nn2vi41ClearLayerNextHdrAdjustableBezierMetadataEPNS0_5LayerE", "SWITCH2: clears the HDR bezier metadata", Result, h_vi_HdrBezClr, void *layer) { (void)layer; return 0; }
HLE_FORCE("_ZN2nn2vi41ClearLayerNextHdrToSdrToneMappingMetadataEPNS0_5LayerE", "SWITCH2: clears the HDR-to-SDR tone mapping", Result, h_vi_HdrTmClr, void *layer) { (void)layer; return 0; }
HLE_FORCE("_ZN2nn2vi39SetLayerNextHdrToSdrToneMappingMetadataEPNS0_5LayerENS0_14HdrToneMappingE", "SWITCH2: HDR-to-SDR tone mapping for the next frame; the host presents SDR", Result, h_vi_HdrTm, void *layer, int m) { (void)layer; (void)m; return 0; }
HLE_FORCE("_ZN2nn2vi24QueryDisplayVrrSupportedEPbPKNS0_7DisplayE", "SWITCH2: the host display is not queried for VRR: not supported", Result, h_vi_QVrr, uint8_t *out, const void *disp) { (void)disp; if (out) *out = 0; return 0; }
// The game (agl HDR pipeline: HdrColorMappingParam / hdr_color_mapping_compose / HdrEnable) asks the display whether it is in HDR and renders in nits when it is. The host presents SDR,
// so both queries say so: not HDR, output format 0 (SDR), and a standard-dynamic-range paper white. NXRT_HDR_STATUS=<isHdr>,<format>,<luminance> and NXRT_HDR_PAPER=<nits>
// override the answers (experiments); the first few calls are logged by default, then a summary every 600 calls.
// NXRT_LOG_VERBOSE=1 restores per-call logging for HLE diagnosis.
static long hdr_env(const char *name, int idx, long def) {
    const char *e = getenv(name); if (!e) return def;
    for (int i = 0; i < idx; i++) { e = strchr(e, ','); if (!e) return def; e++; }
    return *e ? strtol(e, 0, 0) : def;
}
HLE_FORCE("_ZN2nn2vi21QueryDisplayHdrStatusEPbPNS0_15HdrOutputFormatEPtPKNS0_7DisplayE", "SWITCH2: HDR status of the display; the host presents SDR: not HDR, SDR format",
          Result, h_vi_QHdrStatus, uint8_t *is_hdr, int *fmt, uint16_t *lum, const void *disp) {
    uint8_t h = (uint8_t)hdr_env("NXRT_HDR_STATUS", 0, 0); int f = (int)hdr_env("NXRT_HDR_STATUS", 1, 0); uint16_t l = (uint16_t)hdr_env("NXRT_HDR_STATUS", 2, 0);
    static _Atomic unsigned long long calls;
    static int verbose = -1;
    if (verbose < 0) { const char *e = getenv("NXRT_LOG_VERBOSE"); verbose = e && *e && *e != '0'; }
    unsigned long long n = atomic_fetch_add(&calls, 1) + 1;
    if (verbose || n <= 3) LOG("vi::QueryDisplayHdrStatus(disp=%p) -> isHdr=%u format=%d luminance=%u", disp, h, f, l);
    else if (n % 600 == 0) LOG("vi::QueryDisplayHdrStatus repeated %llu times (last: isHdr=%u format=%d luminance=%u)", n, h, f, l);
    if (is_hdr) *is_hdr = h; if (fmt) *fmt = f; if (lum) *lum = l; return 0;
}
HLE_FORCE("_ZN2nn2vi25QueryDisplayHdrPaperWhiteEPtPKNS0_7DisplayE", "SWITCH2: HDR paper white of the display (nits); the host presents SDR", Result, h_vi_QPaper, uint16_t *out, const void *disp) {
    uint16_t v = (uint16_t)hdr_env("NXRT_HDR_PAPER", 0, 0);
    LOG("vi::QueryDisplayHdrPaperWhite(disp=%p) -> %u", disp, v);
    if (out) *out = v; return 0;
}

#include "hle_vi.adapters.inc"
