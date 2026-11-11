// Korvid touchpad: a gamepad on a touch screen. The layout, the logic that turns touches
// into a pad's stick and buttons, and the module's calls.
//
// The layout (JSON; lengths in points, which the screen's scale makes pixels; positions
// inside the screen's safe area):
//   {
//     "stick": {
//       "region": [x, y, w, h],    the part of the safe area a stick may start in, as
//                                  fractions of it from its top-left (default: the left
//                                  half, below the top quarter)
//       "margin": 72,              how far inside the region the stick's centre stays
//       "radius": 56,              the knob's travel: full deflection
//       "mode": "drag"             past the radius the centre follows the thumb ("drag")
//                                  or stays where the thumb landed ("clamp")
//     },
//     "buttons": [                 up to 8
//       {"name": "A", "pad": "South", "from": "bottom_right", "at": [128, 64], "radius": 38}
//                                  `pad` is a PadButton's name; the centre is `at` points in
//                                  from the corner `from` (top_left, top_right, bottom_left,
//                                  bottom_right)
//     ],
//     "opacity": 0.35              the controls' alpha at rest
//   }
// What is missing keeps its default (assets/default.json's values). Textures and layout
// rules beyond this are to come.
#pragma once
#include "korvid/core/types.h"
#include "korvid/math/vec2.h"
#include "korvid/platform_hid/input.h"

namespace kv {

struct Engine;

constexpr u32 KV_TOUCHPAD_BUTTONS = 8;

enum class TouchCorner : u8 { TopLeft, TopRight, BottomLeft, BottomRight };

struct TouchpadButton {
    char        name[16] = {};
    PadButton   pad      = PadButton::South;
    TouchCorner from     = TouchCorner::BottomRight;
    vec2        at{0.0f, 0.0f}; // points in from `from`
    f32         radius   = 38.0f;
};

struct TouchpadConfig {
    f32  region[4] = {0.0f, 0.25f, 0.5f, 0.75f}; // x, y, w, h: fractions of the safe area
    f32  margin    = 72.0f;
    f32  radius    = 56.0f;
    bool drag      = true;
    TouchpadButton buttons[KV_TOUCHPAD_BUTTONS];
    u32  button_count = 0;
    f32  opacity   = 0.35f;
    f32  hit       = 1.15f;  // a button takes a finger this far past its drawn radius
    f64  min_press = 0.06;   // seconds a tap holds its button, so a tick samples it
};

// The built-in layout (assets/default.json's).
TouchpadConfig touchpad_default();
// Reads a layout over `out` (which keeps what the text does not say); false with a reason.
bool touchpad_parse(const char* json, usize len, TouchpadConfig& out, const char** why);

// The screen the layout is placed on: the framebuffer in pixels, the safe area's insets
// (left, top, right, bottom) and the pixels per point.
struct TouchpadScreen {
    f32 width = 0.0f, height = 0.0f;
    f32 insets[4] = {};
    f32 scale = 1.0f;
};

// A button's centre and radius on the screen, pixels, origin top-left.
vec2 touchpad_button_center(const TouchpadConfig& c, u32 index, const TouchpadScreen& s);
f32  touchpad_button_radius(const TouchpadConfig& c, u32 index, const TouchpadScreen& s);

// What the touches have made of the pad, and what the logic remembers between updates.
struct TouchpadState {
    u32  stick_touch = 0;      // the finger on the stick, 0 for none
    vec2 center{0, 0};         // the stick's centre and knob, pixels (while stick_touch)
    vec2 knob{0, 0};
    vec2 axis{0, 0};           // [-1, 1], +y down as a pad reports it
    u32  down = 0;             // bit per layout button: held now
    f64  held_until[KV_TOUCHPAD_BUTTONS] = {};
    u32  pad_buttons = 0;      // bit per PadButton: what the pad reports
    u32  seen[KV_MAX_TOUCHES] = {}; // the touches the last update knew
};

// One update from the touches as they are now (a frame's; `now` in seconds).
void touchpad_update(const TouchpadConfig& c, TouchpadState& st, const TouchPoint* touches, u32 count, const TouchpadScreen& s, f64 now);

// The module. It turns itself on when the window's screen is touched; on a desktop,
// touchpad_enable(e, true) makes the left mouse button a finger. touchpad_layout replaces
// the layout with a JSON file read through the mounts (the default: touchpad/default.json).
void touchpad_enable(Engine& e, bool mouse);
bool touchpad_enabled(Engine& e);
void touchpad_layout(Engine& e, const char* path);
const TouchpadState* touchpad_state(Engine& e);

} // namespace kv
