// from nxnative dm/host/front.h (MIT, © seedds): the host's interface to its front end. Windows has no front end yet (SDL3 comes with M6, DESIGN D14):
// only the touch and pointer-look entry points the ported hid files define are kept. Dropped: front_run, the Metal layer, screen modes, progress, text input,
// preloading, front_save_failed (hle_fs.c logs it).
#pragma once
#include <stdint.h>

// Touchscreen (hle_touch.c): the whole set of touch points, n = 0 = all lifted; x / y normalized [0,1) on the output frame, origin at top left
typedef struct { uint32_t id; float x, y; } dm_touch;
void dm_touch_set(const dm_touch *t, int n);
void dm_touch_script(const dm_touch *t, int n);   // scripted touches (DM_KEYS, DM_CTL; hle_hid.c): as dm_touch_set, but never recorded by DM_REC
// Pointer look (hle_hid.c): turn the camera by this many degrees (right, up) through the right stick
void dm_look_add(float right_deg, float up_deg);
void dm_look_add_at(float right_deg, float up_deg, double age_s);
int dm_look_on(void);
float dm_look_sens(float def);
