/* Android HID host: the game-facing controller functions of the macOS
 * runtime (games/totk/runtime/input.c), backed by the Thor's built-in
 * controller through thor_gamepad.c. The state ABI follows libnx HID. The
 * macOS keyboard, scripted-input and screenshot diagnostics are not ported. */
#include "runtime.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int64_t sampling; uint64_t buttons; int32_t lx, ly, rx, ry; uint32_t attributes, pad; } NpadState;
_Static_assert(sizeof(NpadState) == 40, "Npad full-key ABI");
typedef struct { int64_t sampling; int32_t x, y, dx, dy, wx, wy; uint32_t buttons, attributes; } MouseState;
typedef struct { int64_t sampling; uint32_t modifiers, attributes; uint64_t keys[4]; } KeyboardState;
_Static_assert(sizeof(MouseState) == 40 && sizeof(KeyboardState) == 48, "mouse/keyboard ABI");
typedef struct { int8_t players; uint8_t pad[3]; uint32_t selected; } ControllerSupportResult;
_Static_assert(sizeof(ControllerSupportResult) == 8, "controller support result ABI");

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t styles = 1;
static bool enabled, allowed = true, mouse_ready, keyboard_ready;
static int64_t sample;

void runtime_input_clear(void) { runtime_gamepad_clear(); }

static void choose_locked(void);
static uint32_t pad_style(void);
static void initialize(void) {
    runtime_gamepad_start();
    pthread_mutex_lock(&lock); enabled = true; pthread_mutex_unlock(&lock);
    pthread_mutex_lock(&lock); choose_locked(); pthread_mutex_unlock(&lock);
    fprintf(stderr, "[android-input] Thor controller initialized as %s\n", pad_style() == 2 ? "handheld controller" : "player 1 controller");
}
/* The Thor's controller takes the first form the game accepts, as a console assigns an attached controller:
   handheld (id 0x20, style bit 1) when the game supports it in handheld mode, else player 1 (id 0) as a full-key
   controller (style bit 0), else player 1 as dual Joy-Con (style bit 2). Docked prefers player 1. Unreal first allows
   every id and style, then narrows to player 1 with full-key / handheld / dual styles (Double Exposure, 26 Oct 2026). */
static _Atomic uint32_t chosen_id = 0x20, chosen_style = 2;
static uint32_t supported_mask = 0x1ff;   /* bit i: player id i (0..7), bit 8: handheld id 0x20 */
static uint32_t pad_id(void) { return chosen_id; }
static uint32_t pad_style(void) { return chosen_style; }
static void choose_locked(void) {
    bool hh = runtime_handheld_mode(), p1 = supported_mask & 1, hhid = supported_mask & 0x100;
    uint32_t id = 0, st = 0;
    if (hh && hhid && (styles & 2)) { id = 0x20; st = 2; }
    else if (p1 && (styles & 1)) st = 1;
    else if (p1 && (styles & 4)) st = 4;
    else if (hhid && (styles & 2)) { id = 0x20; st = 2; }
    allowed = st != 0;
    if (st && (id != chosen_id || st != chosen_style)) fprintf(stderr, "[android-input] Thor controller now %s\n",
        st == 2 ? "the handheld controller" : st == 1 ? "player 1, full-key controller" : "player 1, dual Joy-Con");
    if (st) { chosen_id = id; chosen_style = st; }
    else fprintf(stderr, "[android-input] the game accepts no form of the Thor controller (ids %#x, styles %#x)\n", supported_mask, styles);
}
static void supported_ids(const uint32_t *ids, uint64_t n) {
    if (n > 10 || (!ids && n)) runtime_fail("invalid controller ids");
    uint32_t mask = 0;
    for (uint64_t i = 0; i < n; i++) { if (ids[i] < 8) mask |= 1u << ids[i]; else if (ids[i] == 0x20) mask |= 0x100; }
    pthread_mutex_lock(&lock); supported_mask = mask; choose_locked(); pthread_mutex_unlock(&lock);
}
static void supported_styles(uint32_t value) { pthread_mutex_lock(&lock); styles = value; choose_locked(); pthread_mutex_unlock(&lock); }
/* One log line per distinct (call, id, style) the game uses: which form it reads the controller in. */
static void note_query(const char *what, uint32_t id, uint32_t st) {
    static uint64_t seen[32]; static unsigned n;
    uint64_t key = (uint64_t)(uintptr_t)what << 16 ^ (uint64_t)id << 8 ^ st;
    for (unsigned i = 0; i < n; i++) if (seen[i] == key) return;
    if (n < 32) seen[n++] = key;
    fprintf(stderr, "[android-input] game reads %s id %#x style %u (Thor: id %#x style %u)\n", what, id, st, pad_id(), pad_style());
}
/* Docked: player 1 answers in whichever style the game reads it (full-key, dual Joy-Con, left/right), as the Thor's
   controller would after the console's controller assignment. */
static bool docked_p1(uint32_t id) { return !runtime_handheld_mode() && (id == 0 || id == 0x20) && pad_id() == 0; }
static uint32_t style(const uint32_t *id) {
    if (!id) runtime_fail("controller id pointer");
    pthread_mutex_lock(&lock);
    uint32_t v = enabled && allowed && *id == pad_id() ? (styles & pad_style()) : 0;
    /* Z-A reads only the handheld id, also docked (31 Oct 2026): the Thor answers there as well. */
    if (!v && enabled && allowed && *id == 0x20 && docked_p1(*id)) v = styles & 2;
    note_query("style set", *id, v);
    pthread_mutex_unlock(&lock);
    return v;
}
/* HID also reports each stick as four direction buttons (NpadButton StickLLeft = bit 16 .. StickRDown = bit 23)
   once it is pushed past half its range; Unreal's menus navigate with those bits, not the axes. */
static uint64_t stick_buttons(int32_t x, int32_t y, int shift) {
    const int32_t half = 0x4000;
    return (uint64_t)((x < -half) | (y > half) << 1 | (x > half) << 2 | (y < -half) << 3) << shift;
}
static int pad_states(NpadState *out, int max, const uint32_t *id, uint32_t want_style) {
    if (max <= 0) return 0;
    if (!out || !id) runtime_fail("controller output pointer");
    pthread_mutex_lock(&lock);
    NpadState state = {.sampling = ++sample};
    note_query("state", *id, want_style);
    if (enabled && allowed && *id == pad_id() && (want_style == pad_style() || docked_p1(*id))) {
        RuntimeGamepadState pad;
        runtime_gamepad_read(&pad);
        state.attributes = 1;   /* connected */
        pad.buttons |= stick_buttons(pad.lx, pad.ly, 16) | stick_buttons(pad.rx, pad.ry, 20);
        state.buttons = pad.buttons;
        static uint64_t last_buttons;
        if (pad.buttons && pad.buttons != last_buttons) fprintf(stderr, "[android-input] buttons %#llx stick %d,%d\n", (unsigned long long)pad.buttons, pad.lx, pad.ly);
        last_buttons = pad.buttons;
        state.lx = pad.lx; state.ly = pad.ly; state.rx = pad.rx; state.ry = pad.ry;
    }
    *out = state;
    pthread_mutex_unlock(&lock);
    return 1;
}
static int full_states(NpadState *out, int max, const uint32_t *id) { return pad_states(out, max, id, 1); }
static int handheld_states(NpadState *out, int max, const uint32_t *id) { return pad_states(out, max, id, 2); }
static int dual_states(NpadState *out, int max, const uint32_t *id) { return pad_states(out, max, id, 4); }
static int disconnected_states(NpadState *out, int max, const uint32_t *id) {
    (void)id;
    if (max <= 0) return 0;
    if (!out) runtime_fail("controller output pointer");
    pthread_mutex_lock(&lock); *out = (NpadState){.sampling = ++sample}; pthread_mutex_unlock(&lock);
    return 1;
}
static int joy_hold = 1;   /* NpadJoyHoldType_Horizontal, the system default */
static void hold_type(int type) { if (type < 0 || type > 1) runtime_fail("invalid joy hold type"); pthread_mutex_lock(&lock); joy_hold = type; pthread_mutex_unlock(&lock); }
static int get_hold_type(void) { pthread_mutex_lock(&lock); int t = joy_hold; pthread_mutex_unlock(&lock); return t; }
/* Unreal reads the latest state with the singular forms; TOTK used GetNpadStates / GetMouseStates. */
static void full_state(NpadState *out, const uint32_t *id) { full_states(out, 1, id); }
static void handheld_state(NpadState *out, const uint32_t *id) { handheld_states(out, 1, id); }
static void disconnected_state(NpadState *out, const uint32_t *id) { disconnected_states(out, 1, id); }
static void dual_state(NpadState *out, const uint32_t *id) { dual_states(out, 1, id); }
static void disconnect(const uint32_t *id) {
    if (!id) runtime_fail("disconnect id");
    if (*id == pad_id()) { pthread_mutex_lock(&lock); allowed = false; pthread_mutex_unlock(&lock); }
}
/* The game sees no mouse and a keyboard without keys, as on macOS. */
static void initialize_mouse(void) { pthread_mutex_lock(&lock); mouse_ready = true; pthread_mutex_unlock(&lock); }
static void initialize_keyboard(void) { pthread_mutex_lock(&lock); keyboard_ready = true; pthread_mutex_unlock(&lock); }
static int mouse_states(MouseState *out, int max) {
    if (max <= 0) return 0;
    if (!out) runtime_fail("mouse output pointer");
    pthread_mutex_lock(&lock);
    if (!mouse_ready) runtime_fail("mouse used before InitializeMouse");
    *out = (MouseState){.sampling = ++sample};
    pthread_mutex_unlock(&lock);
    return 1;
}
static void mouse_state(MouseState *out) { mouse_states(out, 1); }
static void keyboard_state(KeyboardState *out) {
    if (!out) runtime_fail("keyboard output pointer");
    pthread_mutex_lock(&lock);
    if (!keyboard_ready) runtime_fail("keyboard used before InitializeKeyboard");
    *out = (KeyboardState){.sampling = ++sample};
    pthread_mutex_unlock(&lock);
}
/* One controller: the controller-support applet returns at once with it. */
static uint32_t show_controller_support(ControllerSupportResult *out, const void *arg) {
    (void)arg;
    if (out) *out = (ControllerSupportResult){.players = 1, .selected = pad_id()};
    return 0;
}

/* Rumble and motion stay off unless TOTK_RUMBLE / TOTK_GYRO are set, and the
   Android side has no actuator or sensor bridge yet. */
void runtime_rumble_set(unsigned side, float amp_low, float freq_low, float amp_high, float freq_high) {
    (void)side; (void)amp_low; (void)freq_low; (void)amp_high; (void)freq_high;
}
int runtime_motion_read(RuntimeMotionState *out, int max) { (void)out; (void)max; return 0; }
void runtime_motion_start(void) {}

/* Controller configuration a Pokémon title sets up (Legends Z-A 2.0.2). The Thor has one built-in controller that is
   never split into Joy-Cons: the settings are kept and read back, assignment changes have nothing to move, and the
   controller keeps its one player LED and fixed colours. */
static int handheld_activation, communication_mode = 3;   /* NpadHandheldActivationMode_Dual, NpadCommunicationMode_Default */
static void set_handheld_activation(int mode) {
    if (mode < 0 || mode > 2) runtime_fail("invalid handheld activation mode");
    pthread_mutex_lock(&lock); handheld_activation = mode; pthread_mutex_unlock(&lock);
}
static int get_handheld_activation(void) { pthread_mutex_lock(&lock); int m = handheld_activation; pthread_mutex_unlock(&lock); return m; }
static void set_communication_mode(int mode) {
    if (mode < 0 || mode > 3) runtime_fail("invalid npad communication mode");
    pthread_mutex_lock(&lock); communication_mode = mode; pthread_mutex_unlock(&lock);
}
static int get_communication_mode(void) { pthread_mutex_lock(&lock); int m = communication_mode; pthread_mutex_unlock(&lock); return m; }
static uint32_t get_supported_styles(void) { pthread_mutex_lock(&lock); uint32_t s = styles; pthread_mutex_unlock(&lock); return s; }
static void joy_assignment_change(const uint32_t *id) { if (!id) runtime_fail("joy assignment id"); }
static void joy_assignment_single_type(const uint32_t *id, int type) { (void)type; joy_assignment_change(id); }
/* No second Joy-Con is freed by a split: reports false and leaves the output id alone. */
static unsigned char joy_assignment_single_out(uint32_t *out, const uint32_t *id, int type) { (void)out; (void)type; joy_assignment_change(id); return 0; }
static int get_joy_assignment(const uint32_t *id) { joy_assignment_change(id); return 0; }   /* NpadJoyAssignmentMode_Dual */
static uint32_t merge_joy(const uint32_t *a, const uint32_t *b) { joy_assignment_change(a); joy_assignment_change(b); return 0; }
static void swap_assignment(const uint32_t *a, const uint32_t *b) { joy_assignment_change(a); joy_assignment_change(b); }
static void lr_assignment_mode(void) {}
static uint8_t player_led(const uint32_t *id) { joy_assignment_change(id); return 1; }   /* first LED, player 1 */
typedef struct { uint8_t main[4], sub[4]; } ControllerColor;   /* nn::hid::NpadControllerColor, RGBA */
static const ControllerColor thor_color = {{0x2b, 0x2b, 0x2e, 0xff}, {0x10, 0x10, 0x12, 0xff}};
static uint32_t controller_color(ControllerColor *out, const uint32_t *id) {
    if (!out) runtime_fail("controller color output"); joy_assignment_change(id); *out = thor_color; return 0;
}
static uint32_t controller_colors(ControllerColor *left, ControllerColor *right, const uint32_t *id) {
    if (!left || !right) runtime_fail("controller color output"); joy_assignment_change(id); *left = *right = thor_color; return 0;
}
static uint32_t strap_guide(void) { return 0; }   /* the guide applet shows Joy-Con straps; nothing to show */
/* The style set of the Thor controller changes only when the game narrows what it supports; the event is bound
   (not signalled), as for a console whose controller stays attached. */
static void bind_style_update(const uint32_t *id, void *event, int clear_mode) {
    if (!event || clear_mode < 0 || clear_mode > 1) runtime_fail("style set update event");
    joy_assignment_change(id); hle_bind_system_event(event, clear_mode);
}
static void initialize_touch_screen(int a, int b) { (void)a; (void)b; }   /* the game screen's touches are not passed on */

void *runtime_input_override(const char *name) {
    void *feedback = runtime_hid_feedback_override(name);
    if (feedback) return feedback;
    static const struct { const char *name; void *fn; } entries[] = {
        {"_ZN2nn3hid14InitializeNpadEv", initialize},
        {"_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", supported_ids},
        {"_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", supported_styles},
        {"_ZN2nn3hid15GetNpadStyleSetERKj", style},
        {"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", full_states},
        {"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj", dual_states},
        {"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj", disconnected_states},
        {"_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj", disconnected_states},
        {"_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj", handheld_states},
        {"_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", hold_type},
        {"_ZN2nn3hid18GetNpadJoyHoldTypeEv", get_hold_type},
        {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", full_state},
        {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", dual_state},
        {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", disconnected_state},
        {"_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", disconnected_state},
        {"_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", handheld_state},
        {"_ZN2nn3hid13GetMouseStateEPNS0_10MouseStateE", mouse_state},
        {"_ZN2nn3hid14DisconnectNpadERKj", disconnect},
        {"_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE", show_controller_support},
        {"_ZN2nn3hid15InitializeMouseEv", initialize_mouse},
        {"_ZN2nn3hid18InitializeKeyboardEv", initialize_keyboard},
        {"_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi", mouse_states},
        {"_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE", keyboard_state},
        {"_ZN2nn3hid29SetNpadHandheldActivationModeENS0_26NpadHandheldActivationModeE", set_handheld_activation},
        {"_ZN2nn3hid29GetNpadHandheldActivationModeEv", get_handheld_activation},
        {"_ZN2nn3hid24SetNpadCommunicationModeENS0_21NpadCommunicationModeE", set_communication_mode},
        {"_ZN2nn3hid24GetNpadCommunicationModeEv", get_communication_mode},
        {"_ZN2nn3hid24GetSupportedNpadStyleSetEv", get_supported_styles},
        {"_ZN2nn3hid28SetNpadJoyAssignmentModeDualERKj", joy_assignment_change},
        {"_ZN2nn3hid30SetNpadJoyAssignmentModeSingleERKj", joy_assignment_change},
        {"_ZN2nn3hid30SetNpadJoyAssignmentModeSingleERKjNS0_17NpadJoyDeviceTypeE", joy_assignment_single_type},
        {"_ZN2nn3hid30SetNpadJoyAssignmentModeSingleEPjRKjNS0_17NpadJoyDeviceTypeE", joy_assignment_single_out},
        {"_ZN2nn3hid20GetNpadJoyAssignmentERKj", get_joy_assignment},
        {"_ZN2nn3hid23MergeSingleJoyAsDualJoyERKjS2_", merge_joy},
        {"_ZN2nn3hid18SwapNpadAssignmentERKjS2_", swap_assignment},
        {"_ZN2nn3hid21StartLrAssignmentModeEv", lr_assignment_mode},
        {"_ZN2nn3hid20StopLrAssignmentModeEv", lr_assignment_mode},
        {"_ZN2nn3hid19GetPlayerLedPatternERKj", player_led},
        {"_ZN2nn3hid22GetNpadControllerColorEPNS0_19NpadControllerColorERKj", controller_color},
        {"_ZN2nn3hid22GetNpadControllerColorEPNS0_19NpadControllerColorES2_RKj", controller_colors},
        {"_ZN2nn3hid24ShowControllerStrapGuideEv", strap_guide},
        {"_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE", bind_style_update},
        {"_ZN2nn3hid21InitializeTouchScreenEii", initialize_touch_screen}};
    for (unsigned i = 0; i < sizeof entries / sizeof *entries; i++)
        if (!strcmp(name, entries[i].name)) return entries[i].fn;
    return NULL;
}
