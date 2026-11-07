// Screenshots (docs/m5-replay.md): the game's presented image read back from the GPU at its own resolution (the window's crop applied, GL's bottom-up
// rows turned over), written as PNG. Triggered by present number or by a control file:
//   NXRT_VK_SHOT=<n>[,<n>...]   those presents (counted from 1)
//   NXRT_VK_SHOT_EVERY=<n>      every n-th present
//   NXRT_VK_SHOT_FILE=<path>    when the file exists, the next present; the file is then deleted (a path written in it is where the PNG goes)
//   NXRT_VK_SHOT_DIR=<dir>      where they go (default: beside the log, <log>.shot-<n>.png)
//   NXRT_VK_SHOT_WINDOW=1       also the window as shown (scaled, letterboxed), <...>.window.png
#pragma once
#include "vk_internal.h"
#include "nvn_obj.h"

#ifdef __cplusplus
extern "C" {
#endif

void vks_shot_init(void);
void vks_shot_present(const tex_t *t, uint64_t present_number, const int crop[4]);
// The presented texture as 8-bit RGBA rows, top row first (the caller frees *rgba); 0 on success
int vks_shot_capture(const tex_t *t, const int crop[4], int flip, uint8_t **rgba, int *w, int *h);
int vks_png_write(const char *path, const uint8_t *rgba, int w, int h);           // 8-bit RGBA, no compression (stored deflate blocks)
int vks_to_rgba8(VkFormat f, const uint8_t *src, size_t texels, uint8_t *dst);   // 0 when f is one the screenshots know
uint64_t vks_shot_count(void);
void vks_shot_request(const char *path);   // the next present to path (DM_CTL "shot <path>"); any thread
struct vkt_image;
int vks_stat_record(VkCommandBuffer cb, struct vkt_image *im, const char *tag);   // NXRT_VK_DRAWSTATS: copy of the image's centre block, recorded in cb
int vks_stat_pending(void);
void vks_stat_report(void);
int vks_scan_record(VkCommandBuffer cb, struct vkt_image *im, const char *tag);   // NXRT_VK_NANSCAN: full copy of the target
int vks_scan_pending(void);
void vks_scan_report(void);
void vks_scan_summary(void);   // after the GPU finished: one log line a recorded copy
int vks_dump_image(struct vkt_image *im, unsigned layer, const char *path);   // level 0 of one layer as PNG (diagnostics); 0 on success

#ifdef __cplusplus
}
#endif
