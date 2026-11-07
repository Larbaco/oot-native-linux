// The front end's input mapping (front_sdl.c), pure so tests/test_input.c checks it without a window or a controller. Written from SDL3's public headers
// (scancodes, gamepad buttons, labels and axes; zlib licence) and nxnative's Mac front end (dm/host/front_mac.m, MIT, © seedds) for the layout:
//   keyboard (Nintendo layout, by key position): WASD left stick, arrow keys D-pad, J=A K=B U=X I=Y, Q=L E=R, Z=ZL C=ZR, F=press left stick G=press right stick,
//   Return=+ Tab=−, Space=shake the controller (front_mac.m key_bit; SDL scancodes are positions, as the Mac's key codes are);
//   controllers: face buttons by position as on a Switch (right A, bottom B, top X, left Y: SDL3 names them by position, so a Nintendo pad maps name for name, as
//   pad_apple.h has it), or by printed label with DM_PAD_FACE=labels (an Xbox pad's A is then A); shoulders L R, triggers ZL ZR, Start + Back −, both sticks and
//   their presses, the D-pad. Guide (Home) and Misc1 (Capture) have no function, as on the Mac.
// Sticks: SDL's y grows downward, the Switch's upward. A radial inner dead zone (a resting stick's drift) and an outer one (research/01 #18: a DualSense tops
// out near 0.96, so a full push never reached the game): magnitudes below inner are 0, at or above outer full tilt, linear between, the direction kept.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_gamepad.h>
#include "dm_input.h"

#define FRONT_PAD_INNER 0.08f        // DM_PAD_INNER
#define FRONT_PAD_OUTER 0.92f        // DM_PAD_OUTER
#define FRONT_TRIGGER_PRESS 8192     // a trigger counts as pressed past a quarter of its travel
#define FRONT_LOOK_SENS 0.05f        // degrees per mouse count (raw input, no pointer acceleration); DM_LOOK_SENS / DM_LOOK_SCALE (hle_hid.c dm_look_sens)

// The keyboard: buttons held and the four stick keys
typedef struct { uint64_t btn; int w, a, s, d; } front_kbd;
// The NpadButton (or DM_BTN_SHAKE) a key presses; 0 for none
static inline uint64_t front_key_button(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_J: return DM_BTN_A;      case SDL_SCANCODE_K: return DM_BTN_B;      case SDL_SCANCODE_U: return DM_BTN_X;     case SDL_SCANCODE_I: return DM_BTN_Y;
    case SDL_SCANCODE_F: return DM_BTN_LS;     case SDL_SCANCODE_G: return DM_BTN_RS;
    case SDL_SCANCODE_Q: return DM_BTN_L;      case SDL_SCANCODE_E: return DM_BTN_R;      case SDL_SCANCODE_Z: return DM_BTN_ZL;    case SDL_SCANCODE_C: return DM_BTN_ZR;
    case SDL_SCANCODE_RETURN: return DM_BTN_PLUS;                                         case SDL_SCANCODE_TAB: return DM_BTN_MINUS;
    case SDL_SCANCODE_LEFT: return DM_BTN_LEFT; case SDL_SCANCODE_UP: return DM_BTN_UP;  case SDL_SCANCODE_RIGHT: return DM_BTN_RIGHT; case SDL_SCANCODE_DOWN: return DM_BTN_DOWN;
    case SDL_SCANCODE_SPACE: return DM_BTN_SHAKE;
    default: return 0;
    }
}
// A key event; returns 1 when the keyboard's state changed (it is a key the game takes)
static inline int front_kbd_key(front_kbd *k, SDL_Scancode sc, int down) {
    int *stick = sc == SDL_SCANCODE_W ? &k->w : sc == SDL_SCANCODE_A ? &k->a : sc == SDL_SCANCODE_S ? &k->s : sc == SDL_SCANCODE_D ? &k->d : NULL;
    if (stick) { if (*stick == !!down) return 0; *stick = !!down; return 1; }
    uint64_t m = front_key_button(sc); if (!m) return 0;
    uint64_t b = down ? k->btn | m : k->btn & ~m; if (b == k->btn) return 0;
    k->btn = b; return 1;
}
static inline void front_kbd_axes(const front_kbd *k, int32_t *lx, int32_t *ly) { *lx = (k->d - k->a) * DM_STICK_MAX; *ly = (k->w - k->s) * DM_STICK_MAX; }

// A controller button as NpadButton; by_label: DM_PAD_FACE=labels, label = SDL_GetGamepadButtonLabel's answer for a face button
static inline uint64_t front_pad_button(SDL_GamepadButton b, int by_label, SDL_GamepadButtonLabel label) {
    if (by_label && b <= SDL_GAMEPAD_BUTTON_NORTH) switch (label) {
        case SDL_GAMEPAD_BUTTON_LABEL_A: return DM_BTN_A; case SDL_GAMEPAD_BUTTON_LABEL_B: return DM_BTN_B;
        case SDL_GAMEPAD_BUTTON_LABEL_X: return DM_BTN_X; case SDL_GAMEPAD_BUTTON_LABEL_Y: return DM_BTN_Y;
        default: break;   // shapes (PlayStation): by position
    }
    switch (b) {
    case SDL_GAMEPAD_BUTTON_EAST: return DM_BTN_A;   case SDL_GAMEPAD_BUTTON_SOUTH: return DM_BTN_B;
    case SDL_GAMEPAD_BUTTON_NORTH: return DM_BTN_X;  case SDL_GAMEPAD_BUTTON_WEST: return DM_BTN_Y;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return DM_BTN_L;  case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return DM_BTN_R;
    case SDL_GAMEPAD_BUTTON_START: return DM_BTN_PLUS;       case SDL_GAMEPAD_BUTTON_BACK: return DM_BTN_MINUS;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return DM_BTN_LS;    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return DM_BTN_RS;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return DM_BTN_LEFT;   case SDL_GAMEPAD_BUTTON_DPAD_UP: return DM_BTN_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return DM_BTN_RIGHT; case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return DM_BTN_DOWN;
    default: return 0;
    }
}
// One stick, SDL's axis values (-32768..32767, y down) to the Switch's (±DM_STICK_MAX, y up), through the radial dead zones
static inline void front_stick(int32_t x, int32_t y, float inner, float outer, int32_t *ox, int32_t *oy) {
    double fx = x / 32767.0, fy = -y / 32767.0, m = sqrt(fx * fx + fy * fy);
    if (!(m > inner) || m <= 0) { *ox = *oy = 0; return; }
    double t = outer > inner ? (m - inner) / (outer - inner) : 1; if (t > 1) t = 1;
    double vx = fx * t / m, vy = fy * t / m;
    *ox = (int32_t)lrint(fmax(-1, fmin(1, vx)) * DM_STICK_MAX); *oy = (int32_t)lrint(fmax(-1, fmin(1, vy)) * DM_STICK_MAX);
}
static inline int front_trigger(int32_t v) { return v > FRONT_TRIGGER_PRESS; }
// A pointer position (window pixels) to the touch screen's normalized point on the centred 16:9 picture (front_mac.m mouseDragged; origin top left)
static inline void front_touch_point(double px, double py, double w, double h, float *tx, float *ty) {
    double pw = w, ph = h;
    if (pw <= 0 || ph <= 0) { *tx = *ty = 0; return; }
    if (pw / ph > 16.0 / 9.0) pw = ph * 16.0 / 9.0; else ph = pw * 9.0 / 16.0;
    double x0 = (w - pw) / 2, y0 = (h - ph) / 2;
    *tx = (float)fmin(fmax((px - x0) / pw, 0), 0.999); *ty = (float)fmin(fmax((py - y0) / ph, 0), 0.999);
}

// Rumble (M6 phase B): a player's two actuators (hle_haptics.c: left and right, each a low and a high band) as SDL_RumbleGamepad's strong (low-frequency) and weak
// (high-frequency) motors, mixed as nxnative's DualShock 4 route feeds SDL's hidapi driver (haptics_apple.m ds4_apply): the strong motor follows the low band, mostly
// from the left actuator, the weak one the high band, mostly from the right. gain scales (DM_RUMBLE_GAIN, 0-3); levels at or below min are silent and the rest is
// stretched over the full range (DM_RUMBLE_MIN, 0-0.9: haptics_apple.m controller_level)
static inline float front_rumble_level(float x, float gain, float min) {
    x = isfinite(x) && isfinite(gain) ? x * gain : 0; x = x < 0 ? 0 : x > 1 ? 1 : x;
    return x <= min ? 0 : (x - min) / (1 - min);
}
static inline void front_rumble_mix(float l_low, float l_high, float r_low, float r_high, float gain, float min, uint16_t *strong, uint16_t *weak) {
    *strong = (uint16_t)lrintf(front_rumble_level(0.85f * l_low + 0.15f * r_low, gain, min) * 65535.0f);
    *weak = (uint16_t)lrintf(front_rumble_level(0.15f * l_high + 0.85f * r_high, gain, min) * 65535.0f);
}

// Motion (M6 phase B): SDL's gamepad sensors in the Switch's frame for dm_motion_set_hid. SDL (SDL_sensor.h), a controller held in front of you: x right, y up out of
// its face, z toward the player; acceleration in m/s² as the specific force (+g along y lying flat), rotation in rad/s, counter-clockwise positive. The Switch
// (motion_fusion.h): x right, y away from the player, z up out of the face; acceleration in G as the gravity direction ((0, 0, -1) lying flat), rad/s. The axis map
// (x, -z, y) is a proper rotation, so the rates map as the vectors do
#define FRONT_STANDARD_GRAVITY 9.80665f
static inline void front_motion_frame(const float sdl_acc[3], const float sdl_gyro[3], float acc[3], float rot[3]) {
    acc[0] = -sdl_acc[0] / FRONT_STANDARD_GRAVITY; acc[1] = sdl_acc[2] / FRONT_STANDARD_GRAVITY; acc[2] = -sdl_acc[1] / FRONT_STANDARD_GRAVITY;
    rot[0] = sdl_gyro[0]; rot[1] = -sdl_gyro[2]; rot[2] = sdl_gyro[1];
}
// The Switch button names of an NpadButton mask, for the controller check's log (DM_PAD_CHECK)
static inline int front_button_names(uint64_t m, char *out, int cap) {
    static const char *nm[] = {"A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "+", "-", "Left", "Up", "Right", "Down"};
    int o = 0; out[0] = 0;
    for (int i = 0; i < 16 && o < cap - 8; i++) if (m >> i & 1) o += snprintf(out + o, (size_t)(cap - o), "%s%s", o ? " " : "", nm[i]);
    return o;
}
