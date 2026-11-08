// Actions: bindings parse (and bad ones are refused with the set unchanged); a button's
// edges across ticks; an Axis2 from keys, a stick past its dead zone and the d-pad, the
// strongest winning, clamped to the unit circle; repeat for menus; overrides.
#include "korvid/platform_hid/actions.h"
#include "korvid/platform_hid/input.h"
#include "korvid/platform/module.h"
#include "korvid/platform_hid/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/test.h"
#include <math.h>
#include <stdlib.h>

namespace {
using namespace kv;
bool near(f32 a, f32 b) { return fabsf(a - b) < 1e-4f; }
}

void test_actions(kv::TestCtx& c) {
    using namespace kv;
    c.begin("actions");
    setenv("SDL_VIDEODRIVER", "dummy", 0);
    const ModuleDesc* const list[] = {&platform_module, &platform_hid_module};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    const InputActionDesc acts[] = {
        {"move", ActionKind::Axis2, "keys W S A D, keys Up Down Left Right, stick left, dpad"},
        {"confirm", ActionKind::Button, "key Enter, key Space, pad South"},
        {"menu", ActionKind::Button, "key Escape, pad Start"},
    };
    KV_CHECK(c, input_actions(e, acts));
    const InputActions& a = *input_actions_get(e);
    u32 move = a.find("move"), confirm = a.find("confirm"), menu = a.find("menu");
    KV_CHECK(c, a.count == 3 && move == 0 && confirm == 1 && menu == 2 && a.find("jump") == KV_ACTION_NONE);
    KV_CHECK(c, a.actions[0].binding_count == 4 && a.actions[1].binding_count == 3);

    // Refusals leave the set alone.
    const InputActionDesc bad1[] = {{"x", ActionKind::Button, "key Nope"}};
    const InputActionDesc bad2[] = {{"x", ActionKind::Button, "stick left"}};
    const InputActionDesc bad3[] = {{"x", ActionKind::Axis2, "keys W S A"}};
    KV_CHECK(c, !input_actions(e, bad1) && !input_actions(e, bad2) && !input_actions(e, bad3) && a.count == 3);

    InputActions& w = *const_cast<InputActions*>(&a);
    InputState in;
    // A button: pressed, held, released.
    in.set(Key::Enter, true);
    input_actions_sample(w, in);
    KV_CHECK(c, action_down(a, confirm) && action_pressed(a, confirm) && a.actions[confirm].held == 1);
    input_actions_sample(w, in);
    KV_CHECK(c, action_down(a, confirm) && !action_pressed(a, confirm) && a.actions[confirm].held == 2);
    in.set(Key::Enter, false);
    input_actions_sample(w, in);
    KV_CHECK(c, !action_down(a, confirm) && action_released(a, confirm) && a.actions[confirm].held == 0);
    input_actions_sample(w, in);
    KV_CHECK(c, !action_released(a, confirm));

    // An axis from keys: up-right is the diagonal, clamped to length 1; +y is up.
    in.set(Key::W, true); in.set(Key::D, true);
    input_actions_sample(w, in);
    vec2 v = action_axis(a, move);
    KV_CHECK(c, action_down(a, move) && near(v.x, 0.70710678f) && near(v.y, 0.70710678f));
    in.set(Key::W, false); in.set(Key::D, false);
    in.set(Key::Down, true);
    input_actions_sample(w, in);
    KV_CHECK(c, near(action_axis(a, move).y, -1) && near(action_axis(a, move).x, 0));
    in.set(Key::Down, false);

    // A pad: the stick inside its dead zone is nothing; past it, rescaled; +y up from SDL's
    // +y down; the d-pad; the strongest binding wins.
    GamepadState& pad = in.pads[0];
    pad.connected = true;
    pad.axes[(u32)PadAxis::LeftX] = 0.15f;
    input_actions_sample(w, in);
    KV_CHECK(c, !action_down(a, move));
    pad.axes[(u32)PadAxis::LeftX] = 0.0f; pad.axes[(u32)PadAxis::LeftY] = -0.6f; // SDL: up
    input_actions_sample(w, in);
    KV_CHECK(c, action_down(a, move) && near(action_axis(a, move).y, 0.5f) && near(action_axis(a, move).x, 0));
    pad.set(PadButton::DpadLeft, true);
    input_actions_sample(w, in);
    KV_CHECK(c, near(action_axis(a, move).x, -1) && near(action_axis(a, move).y, 0)); // the d-pad is stronger
    pad.set(PadButton::DpadLeft, false);
    pad.axes[(u32)PadAxis::LeftY] = 0.0f;
    pad.set(PadButton::Start, true);
    input_actions_sample(w, in);
    KV_CHECK(c, action_pressed(a, menu) && !action_down(a, move));
    pad.set(PadButton::Start, false);

    // Repeat: the press, nothing for the delay, then every interval.
    in.set(Key::S, true);
    u32 fires = 0, first_repeat = 0;
    for (u32 t = 1; t <= 40; ++t) {
        input_actions_sample(w, in);
        if (action_repeat(a, move, 20, 6)) { ++fires; if (fires == 2) first_repeat = t; }
    }
    KV_CHECK(c, fires == 1 + 4 && first_repeat == 21); // t = 1, 21, 27, 33, 39
    in.set(Key::S, false);

    // Overrides stand in for one sample.
    input_action_override(e, "confirm", true);
    input_action_override(e, "move", true, vec2{3, 0});
    input_actions_sample(w, in);
    KV_CHECK(c, action_pressed(a, confirm) && near(action_axis(a, move).x, 1) && action_down(a, move));
    input_actions_sample(w, in);
    KV_CHECK(c, !action_down(a, confirm) && !action_down(a, move));
    engine_shutdown(e);
}
