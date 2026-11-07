// The SDL3 front end (DESIGN D14): keyboard, mouse look and controllers → hle_hid.c, on the window's thread (vk_window.c calls in; SDL delivers a window's input and
// the controllers' events on the thread that pumps them). Behaviour follows nxnative's Mac front end (dm/host/front_mac.m, MIT, © seedds); written from SDL3's
// public headers (zlib licence). The mapping itself is front_map.h.
// Keyboard: player 1 (dm_input_set), the Mac's layout. Mouse, while the game profile has pointer look (hle_hid.c dm_look_on; Pokopia: direct look through look_cam):
// a click on the picture captures the mouse (relative mode: the pointer hides and stays put), its movement turns the camera by DM_LOOK_SENS degrees per count
// (default FRONT_LOOK_SENS), the left button is A, the right one B and the middle one the right stick's press; Esc, or the window losing focus, lets it go. Without
// pointer look the left button is the touch screen. Controllers: each is a player of its own (pad_slots.h: the first is player 1, with the keyboard); DM_PAD_MERGE=1
// makes every controller player 1, DM_KBD_PLAYER=own keeps player 1 for the keyboard alone. DM_PAD_FACE=labels maps face buttons by their printed letters,
// DM_PAD_INNER / DM_PAD_OUTER set the sticks' dead zones (front_map.h). Out of focus nothing reaches the game: what was held is released (front_mac.m input_release).
// M6 phase B: rumble (hle_haptics.c → SDL_RumbleGamepad, mixed by front_map.h front_rumble_mix; DM_RUMBLE=0 off, DM_RUMBLE_GAIN, DM_RUMBLE_MIN, suspended out of
// focus as front_mac.m does), gyro and accelerometer (SDL's gamepad sensors → dm_motion_set_hid in the Switch's frame, front_motion_frame; DM_MOTION=0 off), the
// player lights (SDL_SetGamepadPlayerIndex), and the controller check (DM_PAD_CHECK=1: each controller's details, presses, sticks, triggers, motion and rumble in the
// log and the window title; F5 / F6 run the strong / weak motor for a second, F7 sends player 1 a vibration through the game's path, hle_haptics.c)
#include "dm.h"
#include "dm_input.h"
#include "front.h"
#include "front_map.h"
#include "hle_haptics.h"
#include "pad_slots.h"
#include "front_sdl.h"
#include <SDL3/SDL.h>
#include <stdatomic.h>

#define NPADS 16
enum { RUMBLE_MS = 1000, CHECK_TEST_NS = 1000000000 };
#define RUMBLE_REFRESH_NS 400000000ull   // a running effect is sent again before its duration ends (a process that dies leaves no motor running past RUMBLE_MS)
#define CHECK_TITLE_NS 200000000ull
#define CHECK_LOG_NS 1000000000ull
typedef struct {
    SDL_Gamepad *g; SDL_JoystickID id; char name[64];
    int rumble, motion, has_acc; float acc[3], rot[3];   // rumble: routed to its player; motion: its sensors on; the latest SDL samples
    uint64_t check_btn; float check_max[6];              // the controller check: the buttons last logged, the largest deflection seen per axis
} fpad;
static struct {
    SDL_Window *win;
    int input_on;                 // the window has focus
    front_kbd kbd;
    uint64_t look_btn;            // the mouse buttons while it looks, ORed into the keyboard's
    int look_cap, mouse_touch;
    float sens, inner, outer;
    int merge, kbd_own, by_label;
    char title[200];
    pad_slots slots;
    fpad pads[NPADS];             // fixed places: an entry's address is its slot owner
    // rumble: the levels of the game's latest snapshot per player (the haptics sink writes them on a game thread), sent from the window thread
    int rumble_on, motion_on; float rumble_gain, rumble_min;
    uint32_t ev_rumble; _Atomic int rumble_pending; plat_rwlock_t rlk;
    struct { uint16_t strong, weak; int dirty; } rum[DM_PLAYERS];
    uint64_t rum_sent[DM_PLAYERS]; uint16_t rum_now[DM_PLAYERS][2];
    // the controller check (DM_PAD_CHECK)
    int check, test_kind; uint64_t check_title_ns, check_log_ns, test_end_ns;
} F;

static plat_rwlock_t g_sdl_lock = PLAT_RWLOCK_INIT;
void front_sdl_lock(void) { plat_rw_wrlock(&g_sdl_lock); }
void front_sdl_unlock(void) { plat_rw_wrunlock(&g_sdl_lock); }

static float env_f(const char *k, float def) { const char *e = getenv(k); return e && *e ? (float)atof(e) : def; }
static void push(void) {
    int32_t lx, ly; front_kbd_axes(&F.kbd, &lx, &ly);
    dm_input_set(F.kbd.btn | F.look_btn, lx, ly, 0, 0);
}

// ---------- Mouse look ----------
static void look_capture(int on) {
    if (on == F.look_cap) return;
    F.look_cap = on;
    SDL_SetWindowRelativeMouseMode(F.win, on);
    char t[300]; snprintf(t, sizeof t, "%s%s", F.title, on ? " - mouse look (Esc releases the mouse)" : "");
    SDL_SetWindowTitle(F.win, t);
    LOG("front end: mouse look %s", on ? "on (Esc releases the mouse)" : "off");
}
static void look_button(uint64_t bit, int down) { F.look_btn = down ? F.look_btn | bit : F.look_btn & ~bit; push(); }
static void touch_at(float x, float y) {
    int w = 0, h = 0; SDL_GetWindowSize(F.win, &w, &h);
    dm_touch t = {0}; front_touch_point(x, y, w, h, &t.x, &t.y);
    dm_touch_set(&t, 1); F.mouse_touch = 1;
}

// ---------- Controllers ----------
static fpad *pad_by_id(SDL_JoystickID id) { for (int i = 0; i < NPADS; i++) if (F.pads[i].g && F.pads[i].id == id) return &F.pads[i]; return NULL; }
static int pad_player(fpad *p) { return F.merge ? 0 : pad_slot_of(&F.slots, p); }
static uint64_t pad_buttons(fpad *p) {
    uint64_t m = 0;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++)
        if (SDL_GetGamepadButton(p->g, (SDL_GamepadButton)b))
            m |= front_pad_button((SDL_GamepadButton)b, F.by_label, b <= SDL_GAMEPAD_BUTTON_NORTH ? SDL_GetGamepadButtonLabel(p->g, (SDL_GamepadButton)b) : SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN);
    if (front_trigger(SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER))) m |= DM_BTN_ZL;
    if (front_trigger(SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))) m |= DM_BTN_ZR;
    return m;
}
static void check_buttons(fpad *p, uint64_t m);
static void pad_read(fpad *p) {
    if (!F.input_on) return;
    int pl = pad_player(p); if (pl < 0) return;
    uint64_t m = pad_buttons(p);
    int32_t lx, ly, rx, ry;
    front_stick(SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_LEFTX), SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_LEFTY), F.inner, F.outer, &lx, &ly);
    front_stick(SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_RIGHTX), SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_RIGHTY), F.inner, F.outer, &rx, &ry);
    dm_pad_set_player(pl, m, lx, ly, rx, ry);
    if (F.check) check_buttons(p, m);
}
static void npad_name(int p, char *out, size_t cap) { uint32_t id = dm_player_npad(p); if (id == 0x20) snprintf(out, cap, "handheld"); else snprintf(out, cap, "%u", id); }
static const char *face_layout(SDL_Gamepad *g) {
    if (F.by_label && SDL_GetGamepadButtonLabel(g, SDL_GAMEPAD_BUTTON_SOUTH) == SDL_GAMEPAD_BUTTON_LABEL_A) return "face buttons by their letters (DM_PAD_FACE=labels)";
    return SDL_GetGamepadButtonLabel(g, SDL_GAMEPAD_BUTTON_EAST) == SDL_GAMEPAD_BUTTON_LABEL_A ? "Nintendo layout, buttons as labelled"
                                                                                              : "face buttons by position (right A, bottom B, top X, left Y)";
}
// Rumble goes to a player while one of its controllers can rumble (hle_haptics.c routes: the game sees vibration devices only then)
static void rumble_route(int pl) {
    if (pl < 0) return;
    int any = 0; for (int i = 0; i < NPADS; i++) any |= F.pads[i].g && F.pads[i].rumble && pad_player(&F.pads[i]) == pl;
    dm_haptics_route(pl, any);
}
static void check_pad_details(fpad *p);
static void pad_added(SDL_JoystickID id) {
    if (pad_by_id(id)) return;
    fpad *p = NULL; for (int i = 0; i < NPADS && !p; i++) if (!F.pads[i].g) p = &F.pads[i];
    if (!p) { LOG("front end: controller %u connected: more than %d controllers, not used", (unsigned)id, NPADS); return; }
    SDL_Gamepad *g = SDL_OpenGamepad(id);
    if (!g) { LOG("[RED] front end: controller %u could not be opened: %s", (unsigned)id, SDL_GetError()); return; }
    p->g = g; p->id = id; snprintf(p->name, sizeof p->name, "%s", SDL_GetGamepadName(g) ? SDL_GetGamepadName(g) : "?");
    int pl = F.merge ? 0 : pad_slot_take(&F.slots, p, p->name); char nb[16];
    const char *type = SDL_GetGamepadStringForType(SDL_GetGamepadType(g));
    if (pl < 0) { LOG("front end: controller '%s' connected: all %d players are taken, not used", p->name, DM_PLAYERS); SDL_CloseGamepad(g); memset(p, 0, sizeof *p); return; }
    p->rumble = F.rumble_on && SDL_GetBooleanProperty(SDL_GetGamepadProperties(g), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
    if (F.motion_on && SDL_GamepadHasSensor(g, SDL_SENSOR_GYRO) && SDL_GamepadHasSensor(g, SDL_SENSOR_ACCEL))
        p->motion = SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_GYRO, true) && SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_ACCEL, true);
    SDL_SetGamepadPlayerIndex(g, pl);   // the player lights (Switch Pro, DualSense, Xbox 360); no effect elsewhere
    npad_name(pl, nb, sizeof nb);
    LOG("front end: controller '%s' (%s) connected: player %d (npad %s), %s; rumble %s, motion %s", p->name, type ? type : "?", pl + 1, nb, face_layout(g),
        p->rumble ? "on" : !F.rumble_on ? "off (DM_RUMBLE=0)" : "none", p->motion ? "on (gyro + accelerometer)" : !F.motion_on ? "off (DM_MOTION=0)" : "none");
    if (!F.merge) dm_pad_players(pad_slot_mask(&F.slots));
    if (p->rumble) rumble_route(pl);
    if (F.check) check_pad_details(p);
    pad_read(p);
}
static void pad_removed(SDL_JoystickID id) {
    fpad *p = pad_by_id(id); if (!p) return;
    int pl = pad_player(p); char nb[16]; npad_name(pl < 0 ? 0 : pl, nb, sizeof nb);
    if (F.merge) LOG("front end: controller '%s' disconnected", p->name);
    else LOG("front end: controller '%s' disconnected: player %d (npad %s) is free", p->name, pl + 1, nb);
    int rumbled = p->rumble, moved = p->motion;
    if (p->rumble) SDL_RumbleGamepad(p->g, 0, 0, 0);
    if (F.merge) {   // as front_mac.m: released when the last controller goes away
        SDL_CloseGamepad(p->g); memset(p, 0, sizeof *p);
        int any = 0; for (int i = 0; i < NPADS; i++) any |= F.pads[i].g != NULL;
        if (!any) dm_pad_set(0, 0, 0, 0, 0);
        if (rumbled) rumble_route(0);
        if (moved && !any) dm_motion_clear_hid(0);
        return;
    }
    pad_slot_free(&F.slots, p);
    if (pl >= 0) dm_pad_set_player(pl, 0, 0, 0, 0, 0);   // nothing stays pressed
    SDL_CloseGamepad(p->g); memset(p, 0, sizeof *p);
    dm_pad_players(pad_slot_mask(&F.slots));
    if (rumbled) rumble_route(pl);
    if (moved && pl >= 0) dm_motion_clear_hid(pl);
}

// ---------- Motion ----------
static void pad_sensor(const SDL_GamepadSensorEvent *e) {
    fpad *p = pad_by_id(e->which); if (!p || !p->motion) return;
    if (e->sensor == SDL_SENSOR_ACCEL) { memcpy(p->acc, e->data, sizeof p->acc); p->has_acc = 1; return; }
    if (e->sensor != SDL_SENSOR_GYRO) return;
    memcpy(p->rot, e->data, sizeof p->rot);
    int pl = pad_player(p);
    if (!F.input_on || !p->has_acc || pl < 0) return;
    float acc[3], rot[3]; front_motion_frame(p->acc, p->rot, acc, rot);
    uint64_t now = now_ns(), sdl_now = SDL_GetTicksNS(), age = sdl_now > e->timestamp ? sdl_now - e->timestamp : 0;   // the sample's time on the host clock
    dm_motion_set_hid(pl, acc, rot, now > age ? now - age : now);
}

// ---------- Rumble ----------
// The haptics sink (hle_haptics.c): called under its lock on a game thread; keeps the levels and wakes the window thread, which sends them (no SDL call here)
static void rumble_sink(dm_haptic_state s) {
    if (s.player < 0 || s.player >= DM_PLAYERS) return;
    uint16_t a, b;
    front_rumble_mix(s.motor[0].amplitude_low, s.motor[0].amplitude_high, s.motor[1].amplitude_low, s.motor[1].amplitude_high, F.rumble_gain, F.rumble_min, &a, &b);
    plat_rw_wrlock(&F.rlk);
    int change = F.rum[s.player].strong != a || F.rum[s.player].weak != b;
    F.rum[s.player].strong = a; F.rum[s.player].weak = b; F.rum[s.player].dirty |= change;
    plat_rw_wrunlock(&F.rlk);
    if (change && F.ev_rumble && !atomic_exchange(&F.rumble_pending, 1)) { SDL_Event e; SDL_zero(e); e.type = F.ev_rumble; SDL_PushEvent(&e); }
}
// On the window thread: changed levels to the player's controllers, and running effects sent again before their duration ends
static void rumble_apply(void) {
    atomic_store(&F.rumble_pending, 0);
    uint64_t now = SDL_GetTicksNS();
    for (int pl = 0; pl < DM_PLAYERS; pl++) {
        plat_rw_wrlock(&F.rlk);
        uint16_t a = F.rum[pl].strong, b = F.rum[pl].weak; int dirty = F.rum[pl].dirty; F.rum[pl].dirty = 0;
        plat_rw_wrunlock(&F.rlk);
        if (!dirty && !((a || b) && now - F.rum_sent[pl] > RUMBLE_REFRESH_NS)) continue;
        F.rum_sent[pl] = now;
        int sent = 0;
        for (int i = 0; i < NPADS; i++) if (F.pads[i].g && F.pads[i].rumble && pad_player(&F.pads[i]) == pl) { SDL_RumbleGamepad(F.pads[i].g, a, b, a || b ? RUMBLE_MS : 0); sent++; }
        if (F.check && dirty && sent && ((a || b) != (F.rum_now[pl][0] || F.rum_now[pl][1])))
            LOG("pad check: rumble for player %d %s (strong %.2f, weak %.2f)", pl + 1, a || b ? "starts" : "stops", a / 65535.0, b / 65535.0);
        F.rum_now[pl][0] = a; F.rum_now[pl][1] = b;
    }
}

// ---------- The controller check (DM_PAD_CHECK) ----------
static void check_pad_details(fpad *p) {
    SDL_Gamepad *g = p->g; SDL_PropertiesID pr = SDL_GetGamepadProperties(g); int pct = -1;
    SDL_PowerState ps = SDL_GetGamepadPowerInfo(g, &pct);
    SDL_JoystickConnectionState cs = SDL_GetGamepadConnectionState(g);
    LOG("pad check: '%s': type %s, USB id %04x:%04x, %s, battery %s%s%d%%; rumble %d, trigger rumble %d, gyro %d (%.0f Hz), accelerometer %d (%.0f Hz), touchpads %d, "
        "light %d, player lights %d", p->name, SDL_GetGamepadStringForType(SDL_GetGamepadType(g)) ? SDL_GetGamepadStringForType(SDL_GetGamepadType(g)) : "?",
        SDL_GetGamepadVendor(g), SDL_GetGamepadProduct(g), cs == SDL_JOYSTICK_CONNECTION_WIRED ? "wired" : cs == SDL_JOYSTICK_CONNECTION_WIRELESS ? "wireless" : "connection unknown",
        ps == SDL_POWERSTATE_ON_BATTERY ? "on battery " : ps == SDL_POWERSTATE_CHARGING ? "charging " : ps == SDL_POWERSTATE_CHARGED ? "charged " : ps == SDL_POWERSTATE_NO_BATTERY ? "none " : "unknown ",
        pct >= 0 ? "" : "?", pct, (int)SDL_GetBooleanProperty(pr, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false), (int)SDL_GetBooleanProperty(pr, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false),
        (int)SDL_GamepadHasSensor(g, SDL_SENSOR_GYRO), SDL_GetGamepadSensorDataRate(g, SDL_SENSOR_GYRO), (int)SDL_GamepadHasSensor(g, SDL_SENSOR_ACCEL), SDL_GetGamepadSensorDataRate(g, SDL_SENSOR_ACCEL),
        SDL_GetNumGamepadTouchpads(g), (int)SDL_GetBooleanProperty(pr, SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false), (int)SDL_GetBooleanProperty(pr, SDL_PROP_GAMEPAD_CAP_PLAYER_LED_BOOLEAN, false));
    char *map = SDL_GetGamepadMapping(g);
    LOG("pad check: '%s' mapping: %s", p->name, map ? map : "(none)");
    SDL_free(map);
    LOG("pad check: press every button once (each press is logged with the Switch button it plays), turn both sticks round to their edges, pull both triggers, tilt and "
        "turn the controller (motion is logged once a second), F5 / F6 run the strong / weak motor for a second, F7 vibrates player 1 through the game's path");
}
// A controller button as the player sees it: a face button by its place and printed label, the others by SDL's name for them
static const char *btn_name(SDL_Gamepad *g, int b, char *buf, size_t n) {
    static const char *pos[4] = {"bottom", "right", "left", "top"};   // SOUTH, EAST, WEST, NORTH
    static const char *lab[9] = {"unlabelled", "A", "B", "X", "Y", "Cross", "Circle", "Square", "Triangle"};
    if (b <= SDL_GAMEPAD_BUTTON_NORTH) { int l = (int)SDL_GetGamepadButtonLabel(g, (SDL_GamepadButton)b); snprintf(buf, n, "%s face button (%s)", pos[b], l >= 0 && l < 9 ? lab[l] : "?"); return buf; }
    const char *s = SDL_GetGamepadStringForButton((SDL_GamepadButton)b);
    snprintf(buf, n, "%s", s ? s : "?"); return buf;
}
static void check_buttons(fpad *p, uint64_t m) {
    uint64_t down = m & ~p->check_btn; p->check_btn = m;
    if (!down) return;
    char held[240] = "", sw[120], b1[48]; int o = 0;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && o < 200; b++)
        if (SDL_GetGamepadButton(p->g, (SDL_GamepadButton)b)) o += snprintf(held + o, sizeof held - (size_t)o, "%s%s", o ? ", " : "", btn_name(p->g, b, b1, sizeof b1));
    front_button_names(down, sw, sizeof sw);
    LOG("pad check: '%s' -> Switch %s (held on the controller: %s)", p->name, sw, o ? held : "a trigger past a quarter of its travel");
}
static void check_axes(fpad *p) {   // the largest deflection seen per axis (sticks after the dead zones, triggers raw)
    int32_t s[4]; front_stick(SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_LEFTX), SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_LEFTY), F.inner, F.outer, &s[0], &s[1]);
    front_stick(SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_RIGHTX), SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_RIGHTY), F.inner, F.outer, &s[2], &s[3]);
    float v[6] = {(float)hypot(s[0], s[1]) / DM_STICK_MAX, 0, (float)hypot(s[2], s[3]) / DM_STICK_MAX, 0,
                  SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 32767.0f, SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 32767.0f};
    for (int i = 0; i < 6; i++) if (v[i] > p->check_max[i]) p->check_max[i] = v[i];
}
static void check_tick(uint64_t now) {
    if (F.test_kind && now >= F.test_end_ns) {   // the end of an F5 / F6 / F7 test
        if (F.test_kind == 3) { dm_vibration_update u[2] = {{0, 0, {0, 160, 0, 320}}, {0, 1, {0, 160, 0, 320}}}; dm_haptics_send(u, 2); }
        else for (int i = 0; i < NPADS; i++) if (F.pads[i].g && F.pads[i].rumble) SDL_RumbleGamepad(F.pads[i].g, 0, 0, 0);
        LOG("pad check: rumble test ends"); F.test_kind = 0;
    }
    for (int i = 0; i < NPADS; i++) if (F.pads[i].g) check_axes(&F.pads[i]);
    int log = now - F.check_log_ns >= CHECK_LOG_NS, title = now - F.check_title_ns >= CHECK_TITLE_NS;
    if (!log && !title) return;
    char t[400]; int o = snprintf(t, sizeof t, "%s | pad check:", F.title);
    for (int i = 0; i < NPADS; i++) {
        fpad *p = &F.pads[i]; if (!p->g) continue;
        char held[120]; front_button_names(pad_buttons(p), held, sizeof held);
        int32_t lx, ly, rx, ry;
        front_stick(SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_LEFTX), SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_LEFTY), F.inner, F.outer, &lx, &ly);
        front_stick(SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_RIGHTX), SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_RIGHTY), F.inner, F.outer, &rx, &ry);
        float acc[3] = {0}, rot[3] = {0}; if (p->motion && p->has_acc) front_motion_frame(p->acc, p->rot, acc, rot);
        int pl = pad_player(p);
        o += snprintf(t + o, sizeof t - o, " [P%d %.20s: %s L%+.2f,%+.2f R%+.2f,%+.2f", pl + 1, p->name, held[0] ? held : "-", lx / (float)DM_STICK_MAX, ly / (float)DM_STICK_MAX,
                      rx / (float)DM_STICK_MAX, ry / (float)DM_STICK_MAX);
        if (p->motion) o += snprintf(t + o, sizeof t - o, " acc %+.1f,%+.1f,%+.1f", acc[0], acc[1], acc[2]);
        o += snprintf(t + o, sizeof t - o, "]");
        if (o >= (int)sizeof t - 2) break;
        if (log) {
            LOG("pad check: '%s' player %d: held %s; left stick %+.2f,%+.2f, right %+.2f,%+.2f (largest so far: left %.2f, right %.2f; triggers %.2f / %.2f)", p->name, pl + 1,
                held[0] ? held : "nothing", lx / (float)DM_STICK_MAX, ly / (float)DM_STICK_MAX, rx / (float)DM_STICK_MAX, ry / (float)DM_STICK_MAX, p->check_max[0], p->check_max[2], p->check_max[4], p->check_max[5]);
            if (p->motion) LOG("pad check: '%s' motion in the Switch frame: acc %+.2f %+.2f %+.2f G (flat and still: 0 0 -1), gyro %+.2f %+.2f %+.2f rad/s (still: about 0)%s", p->name,
                               acc[0], acc[1], acc[2], rot[0], rot[1], rot[2], p->has_acc ? "" : " (no accelerometer sample yet)");
            if (p->rumble && pl >= 0 && (F.rum_now[pl][0] || F.rum_now[pl][1])) LOG("pad check: '%s' rumble now strong %.2f weak %.2f", p->name, F.rum_now[pl][0] / 65535.0, F.rum_now[pl][1] / 65535.0);
        }
    }
    if (title && !F.look_cap) SDL_SetWindowTitle(F.win, t);
    if (log) F.check_log_ns = now;
    if (title) F.check_title_ns = now;
}
static void check_test(int kind) {   // F5 strong motor, F6 weak motor (straight to every controller), F7 player 1 through hle_haptics.c and the mix
    uint64_t now = SDL_GetTicksNS(); int any = 0;
    for (int i = 0; i < NPADS; i++) any |= F.pads[i].g && F.pads[i].rumble;
    if (!any) { LOG("pad check: no controller here can rumble (or DM_RUMBLE=0)"); return; }
    if (kind == 3) {
        if (!(dm_haptics_available() & 1)) { LOG("pad check: player 1 has no controller that rumbles"); return; }
        dm_vibration_update u[2] = {{0, 0, {0.7f, 160, 0.3f, 320}}, {0, 1, {0.3f, 160, 0.7f, 320}}}; dm_haptics_send(u, 2);
        LOG("pad check: rumble test: player 1 through the game's path (left actuator 0.7 low, right 0.7 high) for a second");
    } else {
        for (int i = 0; i < NPADS; i++) if (F.pads[i].g && F.pads[i].rumble) SDL_RumbleGamepad(F.pads[i].g, kind == 1 ? 0xc000 : 0, kind == 2 ? 0xc000 : 0, RUMBLE_MS);
        LOG("pad check: rumble test: the %s motor of every controller for a second", kind == 1 ? "strong (low-frequency, left)" : "weak (high-frequency, right)");
    }
    F.test_kind = kind; F.test_end_ns = now + CHECK_TEST_NS;
}

// ---------- Focus ----------
static void input_release(void) {
    F.input_on = 0;
    look_capture(0); F.look_btn = 0;
    memset(&F.kbd, 0, sizeof F.kbd); push();
    if (F.mouse_touch) { F.mouse_touch = 0; dm_touch_set(NULL, 0); }
    for (int i = 0; i < NPADS; i++) if (F.pads[i].g) { int pl = pad_player(&F.pads[i]); if (pl >= 0) { dm_pad_set_player(pl, 0, 0, 0, 0, 0); if (F.pads[i].motion) dm_motion_clear_hid(pl); } }
    dm_haptics_suspend(DM_HAPTICS_INACTIVE, 1);   // front_mac.m: no rumble while another application has the focus
}
static void input_resume(void) {
    F.input_on = 1;
    dm_haptics_suspend(DM_HAPTICS_INACTIVE, 0);
    for (int i = 0; i < NPADS; i++) if (F.pads[i].g) pad_read(&F.pads[i]);   // a button still held, a stick still deflected
}

void front_sdl_start(SDL_Window *win) {
    F.win = win; F.input_on = 1;
    snprintf(F.title, sizeof F.title, "%s", SDL_GetWindowTitle(win) ? SDL_GetWindowTitle(win) : "");
    const char *e = getenv("DM_PAD_MERGE"); F.merge = e && atoi(e);
    e = getenv("DM_KBD_PLAYER"); F.kbd_own = !F.merge && e && !strcmp(e, "own");
    e = getenv("DM_PAD_FACE"); F.by_label = e && !strcmp(e, "labels");
    F.inner = env_f("DM_PAD_INNER", FRONT_PAD_INNER); F.outer = env_f("DM_PAD_OUTER", FRONT_PAD_OUTER);
    if (F.inner < 0 || F.inner >= 1) F.inner = FRONT_PAD_INNER;
    if (F.outer <= F.inner || F.outer > 1) F.outer = FRONT_PAD_OUTER;
    F.sens = dm_look_sens(FRONT_LOOK_SENS);
    e = getenv("DM_RUMBLE"); F.rumble_on = !(e && *e && !atoi(e));
    F.rumble_gain = env_f("DM_RUMBLE_GAIN", 1); if (!(F.rumble_gain >= 0)) F.rumble_gain = 1; if (F.rumble_gain > 3) F.rumble_gain = 3;
    F.rumble_min = env_f("DM_RUMBLE_MIN", 0); if (!(F.rumble_min >= 0)) F.rumble_min = 0; if (F.rumble_min > 0.9f) F.rumble_min = 0.9f;
    e = getenv("DM_MOTION"); F.motion_on = !(e && *e && !atoi(e));
    e = getenv("DM_PAD_CHECK"); F.check = e && atoi(e);
    plat_rw_init(&F.rlk);
    F.ev_rumble = SDL_RegisterEvents(1);
    dm_haptics_init(rumble_sink, F.rumble_on);   // the game sees vibration devices once a controller that rumbles is a player's (rumble_route)
    static const char kbd = 0;
    if (F.kbd_own) pad_slot_take(&F.slots, &kbd, "keyboard");   // holds player 1 for good
    LOG("front end: keyboard WASD stick, arrows D-pad, J/K/U/I = A/B/X/Y, Q/E = L/R, Z/C = ZL/ZR, F/G = stick presses, Return/Tab = +/-, Space shakes; "
        "F11 or Alt+Enter fullscreen");
    if (dm_look_on()) LOG("front end: mouse look available: click the picture to capture the mouse (left A, right B, middle right-stick press), Esc releases it "
                          "(%.3f degrees per count, DM_LOOK_SENS / DM_LOOK_SCALE)", F.sens);
    LOG("front end: controllers: %s; sticks' dead zones %.2f inner, %.2f outer; rumble %s (gain %.2f, minimum %.2f), motion %s%s", F.merge ? "every controller is player 1 (DM_PAD_MERGE)"
        : F.kbd_own ? "the keyboard is player 1, each controller a player of its own from player 2 on (DM_KBD_PLAYER=own)" : "each controller is a player of its own, the keyboard plays with player 1",
        F.inner, F.outer, F.rumble_on ? "on" : "off", F.rumble_gain, F.rumble_min, F.motion_on ? "on" : "off", F.check ? "; the controller check is on (DM_PAD_CHECK)" : "");
    int n = 0; SDL_JoystickID *ids = SDL_GetGamepads(&n);   // those already connected (an ADDED event follows for each too; pad_added ignores repeats)
    for (int i = 0; ids && i < n; i++) pad_added(ids[i]);
    SDL_free(ids);
    if (F.check && !n) LOG("pad check: no controller connected yet: plug one in (or switch it on) and it is logged here");
}

void front_sdl_tick(void) {
    if (F.ev_rumble) { int run = atomic_load(&F.rumble_pending); for (int p = 0; p < DM_PLAYERS && !run; p++) run = F.rum_now[p][0] || F.rum_now[p][1]; if (run) rumble_apply(); }
    if (F.check) check_tick(SDL_GetTicksNS());
}

void front_sdl_event(const SDL_Event *e) {
    if (F.ev_rumble && e->type == F.ev_rumble) { rumble_apply(); return; }
    switch (e->type) {
    case SDL_EVENT_WINDOW_FOCUS_LOST: input_release(); break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED: input_resume(); break;
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
        if (!F.input_on || e->key.repeat) break;
        int down = e->type == SDL_EVENT_KEY_DOWN;
        if (e->key.scancode == SDL_SCANCODE_ESCAPE) { if (down && F.look_cap) { look_capture(0); F.look_btn = 0; push(); } break; }   // Esc lets the mouse go
        if (down && (e->key.mod & SDL_KMOD_ALT)) break;   // Alt+Enter is the window's (fullscreen), Alt+Tab the system's
        if (F.check && down && e->key.scancode >= SDL_SCANCODE_F5 && e->key.scancode <= SDL_SCANCODE_F7) { check_test(e->key.scancode - SDL_SCANCODE_F5 + 1); break; }
        if (front_kbd_key(&F.kbd, e->key.scancode, down)) push();
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (!F.input_on) break;
        if (dm_look_on()) {   // pointer look: the first click captures, then the buttons play
            if (!F.look_cap) { if (e->button.button == SDL_BUTTON_LEFT) look_capture(1); }
            else look_button(e->button.button == SDL_BUTTON_LEFT ? DM_BTN_A : e->button.button == SDL_BUTTON_RIGHT ? DM_BTN_B : e->button.button == SDL_BUTTON_MIDDLE ? DM_BTN_RS : 0, 1);
        } else if (e->button.button == SDL_BUTTON_LEFT) touch_at(e->button.x, e->button.y);
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        uint64_t bit = e->button.button == SDL_BUTTON_LEFT ? DM_BTN_A : e->button.button == SDL_BUTTON_RIGHT ? DM_BTN_B : e->button.button == SDL_BUTTON_MIDDLE ? DM_BTN_RS : 0;
        if (F.look_btn & bit) look_button(bit, 0);
        if (e->button.button == SDL_BUTTON_LEFT && F.mouse_touch) { F.mouse_touch = 0; dm_touch_set(NULL, 0); }
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (!F.input_on) break;
        if (F.look_cap) {   // SDL's relative deltas are positive to the right and down
            Uint64 now = SDL_GetTicksNS(); double age = now > e->motion.timestamp ? (now - e->motion.timestamp) / 1e9 : 0;
            dm_look_add_at(e->motion.xrel * F.sens, -e->motion.yrel * F.sens, age);
        } else if (F.mouse_touch) touch_at(e->motion.x, e->motion.y);
        break;
    case SDL_EVENT_GAMEPAD_ADDED: pad_added(e->gdevice.which); break;
    case SDL_EVENT_GAMEPAD_REMOVED: pad_removed(e->gdevice.which); break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: { fpad *p = pad_by_id(e->gbutton.which); if (p) pad_read(p); break; }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: { fpad *p = pad_by_id(e->gaxis.which); if (p) pad_read(p); break; }
    case SDL_EVENT_GAMEPAD_REMAPPED: { fpad *p = pad_by_id(e->gdevice.which); if (p) pad_read(p); break; }
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE: pad_sensor(&e->gsensor); break;
    default: break;
    }
}
