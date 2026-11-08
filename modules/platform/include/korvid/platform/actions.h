// Korvid platform: input actions. A game names what the player can do ("move", "confirm",
// "menu") and binds each to keys, pad buttons, sticks and the d-pad as data; systems ask
// about actions, never devices, so a keyboard and a pad are one code path.
//
//   constinit const InputActionDesc k_actions[] = {
//       {"move",    ActionKind::Axis2,  "keys W S A D, keys Up Down Left Right, stick left, dpad"},
//       {"confirm", ActionKind::Button, "key Enter, key Space, pad South"},
//       {"cancel",  ActionKind::Button, "key Backspace, pad East"},
//       {"menu",    ActionKind::Button, "key Escape, pad Start"},
//   };
//   input_actions(e, k_actions);     // at startup
//
//   void walk(TickContext& ctx) {    // reads "input.actions"
//       const InputActions& a = *ctx.resource<const InputActions>(k_actions_id);
//       vec2 dir = action_axis(a, a.find("move"));
//   }
//
// Bindings, comma-separated: `key <Key>`, `pad <PadButton>` (South, East, Start, ...),
// `keys <up> <down> <left> <right>` (an Axis2 from four keys), `stick left|right`, `dpad`.
// Key names are key_name()'s: A..Z, Num0..Num9, F1..F12, Escape, Space, Enter, Up, ...
//
// Actions are computed once per tick from the sampled input ("input.actions", phase Input,
// every tick paused or not), so they are as deterministic as the input log. An Axis2 is
// +x right, +y up, the strongest binding wins, clamped to the unit circle; a stick inside
// its dead zone (0.2) reads zero. A demo or a test drives actions directly with
// input_action_override, which stands in for the devices on the next sample.
#pragma once
#include "korvid/core/types.h"
#include "korvid/math/math.h"

namespace kv {

struct Engine;

constexpr u32 KV_ACTIONS          = 32;
constexpr u32 KV_ACTION_BINDINGS  = 8;
constexpr u32 KV_ACTION_NONE      = 0xFFFFFFFFu;

enum class ActionKind : u8 { Button, Axis2 };

struct InputActionDesc {
    const char* name;
    ActionKind  kind;
    const char* bindings;
};

enum class BindingKind : u8 { Key, PadButton, Keys4, Stick, Dpad };

struct ActionBinding {
    BindingKind kind;
    u16         keys[4];  // Key: keys[0]; Keys4: up, down, left, right
    u8          button;   // PadButton
    u8          stick;    // 0 left, 1 right
};

struct ActionState {
    char          name[24];
    ActionKind    kind;
    ActionBinding bindings[KV_ACTION_BINDINGS];
    u32           binding_count;
    bool          down, was_down;   // this tick, the last; an Axis2 is down while it is off center
    vec2          axis;             // Axis2: the direction; Button: (1, 0) while down
    u32           held;             // ticks down so far (1 on the tick it was pressed)
    bool          override_set;     // input_action_override's value replaces the devices next sample
    bool          override_down;
    vec2          override_axis;
};

struct InputActions {
    ActionState actions[KV_ACTIONS];
    u32         count = 0;
    f32         dead_zone = 0.2f;

    u32 find(const char* name) const;
};

// Replaces the action set (parsed and copied). False, with a log line naming the binding,
// if one does not parse; the set is then unchanged.
bool input_actions(Engine& e, Span<const InputActionDesc> actions);
// The actions as of the last tick: for code outside the schedule. Systems read the
// resource "input.actions".
const InputActions* input_actions_get(Engine& e);
// Stands in for the devices on the next sample: down and (for an Axis2) the direction.
void input_action_override(Engine& e, const char* name, bool down, vec2 axis = vec2{0, 0});

// Queries. An id of KV_ACTION_NONE answers false and zero.
inline bool action_down(const InputActions& a, u32 id)     { return id < a.count && a.actions[id].down; }
inline bool action_pressed(const InputActions& a, u32 id)  { return id < a.count && a.actions[id].down && !a.actions[id].was_down; }
inline bool action_released(const InputActions& a, u32 id) { return id < a.count && !a.actions[id].down && a.actions[id].was_down; }
inline vec2 action_axis(const InputActions& a, u32 id)     { return id < a.count ? a.actions[id].axis : vec2{0, 0}; }
// True on the tick the action is pressed, then every `interval` ticks once it has been held
// for `delay` ticks: menu navigation.
bool action_repeat(const InputActions& a, u32 id, u32 delay, u32 interval);

// The engine's own: the actions from the sampled input (the "input.actions" system).
void input_actions_sample(InputActions& a, const struct InputState& in);

} // namespace kv
