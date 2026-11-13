// The touchpad's layout and logic, headless: touches in, a pad's stick and buttons out, on
// a phone held landscape (2796x1290 pixels at 3 per point, a camera housing on the left).
#include "korvid/core/test.h"
#include "korvid/touchpad/touchpad.h"
#include "korvid/math/scalar.h"
#include <string.h>

namespace {

using namespace kv;

TouchpadScreen phone() {
    TouchpadScreen s;
    s.width = 2796; s.height = 1290; s.scale = 3;
    s.insets[0] = 177; s.insets[1] = 0; s.insets[2] = 177; s.insets[3] = 63;
    return s;
}

bool near(f32 a, f32 b, f32 eps = 1e-3f) { return kv::fabs(a - b) <= eps; }

TouchPoint at(u32 id, f32 x, f32 y, bool ended = false) { return TouchPoint{id, x, y, ended}; }

// Field by field: the structs have padding, which copies need not keep.
bool same(const TouchpadConfig& a, const TouchpadConfig& b) {
    if (memcmp(a.region, b.region, sizeof a.region) || a.margin != b.margin || a.radius != b.radius || a.drag != b.drag ||
        a.button_count != b.button_count || a.opacity != b.opacity || a.hit != b.hit || a.min_press != b.min_press) return false;
    for (u32 i = 0; i < a.button_count; ++i) {
        const TouchpadButton &x = a.buttons[i], &y = b.buttons[i];
        if (strcmp(x.name, y.name) || x.pad != y.pad || x.from != y.from || x.at.x != y.at.x || x.at.y != y.at.y || x.radius != y.radius) return false;
    }
    return true;
}

} // namespace

void test_touchpad_layout(TestCtx& c) {
    c.begin("touchpad layout");
    TouchpadConfig d = touchpad_default();
    KV_CHECK(c, d.button_count == 3 && d.buttons[0].pad == PadButton::South && d.buttons[1].pad == PadButton::East && d.drag);

    // The shipped file says what the built-in layout says.
    const char* shipped = R"({"stick": {"region": [0.0, 0.25, 0.5, 0.75], "margin": 72, "radius": 56, "mode": "drag"},
        "buttons": [{"name": "A", "pad": "South", "from": "bottom_right", "at": [128, 64], "radius": 38},
                    {"name": "B", "pad": "East", "from": "bottom_right", "at": [56, 128], "radius": 38},
                    {"name": "Start", "pad": "Start", "from": "top_right", "at": [40, 40], "radius": 20}],
        "opacity": 0.35})";
    TouchpadConfig f = touchpad_default();
    const char* why = "";
    KV_CHECK(c, touchpad_parse(shipped, strlen(shipped), f, &why) && same(f, d));

    // A partial layout keeps the rest; names are case-blind; clamp mode.
    TouchpadConfig p = touchpad_default();
    const char* part = R"({"stick": {"mode": "clamp", "radius": 40}, "buttons": [{"pad": "west", "from": "top_left", "at": [10, 20]}]})";
    KV_CHECK(c, touchpad_parse(part, strlen(part), p, &why));
    KV_CHECK(c, !p.drag && p.radius == 40 && p.margin == 72 && p.button_count == 1 && p.buttons[0].pad == PadButton::West &&
                strcmp(p.buttons[0].name, "west") == 0 && p.buttons[0].from == TouchCorner::TopLeft && p.buttons[0].radius == 38);

    // What it refuses, leaving the layout as it was.
    const char* bad[] = {
        R"([1, 2])", R"({"stick": {"mode": "float"}})", R"({"stick": {"region": [0, 0, 0]}})",
        R"({"buttons": [{"pad": "Z", "at": [1, 1]}]})", R"({"buttons": [{"pad": "South"}]})",
        R"({"buttons": [{"pad": "South", "at": [1, 1], "from": "middle"}]})", R"({"stick": )",
    };
    for (const char* b : bad) {
        TouchpadConfig k = touchpad_default();
        why = nullptr;
        KV_CHECK(c, !touchpad_parse(b, strlen(b), k, &why) && why && same(k, d));
    }

    // Placement: points from the corner, inside the safe area.
    TouchpadScreen s = phone();
    vec2 a = touchpad_button_center(d, 0, s), st = touchpad_button_center(d, 2, s);
    KV_CHECK(c, near(a.x, 2796 - 177 - 128 * 3) && near(a.y, 1290 - 63 - 64 * 3) && near(touchpad_button_radius(d, 0, s), 114));
    KV_CHECK(c, near(st.x, 2796 - 177 - 120) && near(st.y, 120));
}

void test_touchpad_stick(TestCtx& c) {
    c.begin("touchpad stick");
    TouchpadConfig cfg = touchpad_default();
    TouchpadScreen s = phone();
    const f32 R = 56 * 3; // the radius in pixels
    TouchpadState st;

    // Lands mid-region: the centre is where it landed; the knob follows the thumb.
    TouchPoint t = at(7, 800, 800);
    touchpad_update(cfg, st, &t, 1, s, 0.0);
    KV_CHECK(c, st.stick_touch == 7 && near(st.center.x, 800) && near(st.center.y, 800) && near(st.axis.x, 0) && near(st.axis.y, 0));
    t = at(7, 800 + R * 0.5f, 800);
    touchpad_update(cfg, st, &t, 1, s, 0.016);
    KV_CHECK(c, near(st.axis.x, 0.5f) && near(st.axis.y, 0));
    t = at(7, 800, 800 - R * 0.25f); // up the screen is -y, as a pad's stick
    touchpad_update(cfg, st, &t, 1, s, 0.033);
    KV_CHECK(c, near(st.axis.x, 0) && near(st.axis.y, -0.25f));

    // Drag: past the radius the centre follows; full deflection, centre a radius behind.
    t = at(7, 800 + R * 2.0f, 800);
    touchpad_update(cfg, st, &t, 1, s, 0.05);
    KV_CHECK(c, near(st.axis.x, 1) && near(st.axis.y, 0) && near(st.center.x, 800 + R) && near(st.knob.x, 800 + R * 2.0f));
    // ...and coming back moves the knob, not the centre.
    t = at(7, 800 + R, 800);
    touchpad_update(cfg, st, &t, 1, s, 0.066);
    KV_CHECK(c, near(st.axis.x, 0) && near(st.center.x, 800 + R));

    // Lifting the thumb lets the stick go.
    t = at(7, 800 + R, 800, true);
    touchpad_update(cfg, st, &t, 1, s, 0.083);
    KV_CHECK(c, st.stick_touch == 0 && near(st.axis.x, 0) && near(st.axis.y, 0));

    // Clamp: the centre stays; the knob stops at the radius.
    TouchpadConfig cl = cfg;
    cl.drag = false;
    TouchpadState sc;
    t = at(9, 800, 800);
    touchpad_update(cl, sc, &t, 1, s, 0.0);
    t = at(9, 800 + R * 3.0f, 800 + R * 4.0f);
    touchpad_update(cl, sc, &t, 1, s, 0.016);
    KV_CHECK(c, near(sc.center.x, 800) && near(sc.center.y, 800) && near(sc.axis.x, 0.6f) && near(sc.axis.y, 0.8f) &&
                near(sc.knob.x, 800 + R * 0.6f) && near(sc.knob.y, 800 + R * 0.8f));

    // The margins: a thumb landing at the region's edge puts the centre margin points in.
    // The region: the safe area's left half below its top quarter.
    f32 left = 177, top = (1290 - 63) * 0.25f, bottom = 1290 - 63, m = 72 * 3;
    TouchpadState se;
    t = at(11, left + 5, bottom - 5);
    touchpad_update(cfg, se, &t, 1, s, 0.0);
    KV_CHECK(c, se.stick_touch == 11 && near(se.center.x, left + m) && near(se.center.y, bottom - m));
    // ...where a drag cannot take it past either.
    t = at(11, left - 1000, bottom + 1000);
    touchpad_update(cfg, se, &t, 1, s, 0.016);
    KV_CHECK(c, near(se.center.x, left + m) && near(se.center.y, bottom - m) && near(kv::sqrt(se.axis.x * se.axis.x + se.axis.y * se.axis.y), 1));

    // Outside the region (the top quarter, the right half) no stick starts; nor does a
    // finger already down that slides in.
    TouchpadState so;
    TouchPoint out[2] = {at(12, 800, top - 10), at(13, 2000, 800)};
    touchpad_update(cfg, so, out, 2, s, 0.0);
    KV_CHECK(c, so.stick_touch == 0);
    out[0] = at(12, 800, 800);
    touchpad_update(cfg, so, out, 2, s, 0.016);
    KV_CHECK(c, so.stick_touch == 0 && near(so.axis.x, 0));

    // One stick: a second thumb in the region while the first holds it does nothing.
    TouchpadState s2;
    TouchPoint two[2] = {at(20, 700, 800), at(21, 900, 900)};
    touchpad_update(cfg, s2, two, 1, s, 0.0);
    touchpad_update(cfg, s2, two, 2, s, 0.016);
    KV_CHECK(c, s2.stick_touch == 20 && near(s2.center.x, 700));
}

void test_touchpad_buttons(TestCtx& c) {
    c.begin("touchpad buttons");
    TouchpadConfig cfg = touchpad_default();
    TouchpadScreen s = phone();
    vec2 a = touchpad_button_center(cfg, 0, s), b = touchpad_button_center(cfg, 1, s);
    const u32 south = 1u << (u32)PadButton::South, east = 1u << (u32)PadButton::East;
    TouchpadState st;

    // A held, then a stick at the same time: both.
    TouchPoint t[2] = {at(1, a.x + 10, a.y - 10), at(2, 800, 800)};
    touchpad_update(cfg, st, t, 2, s, 0.0);
    KV_CHECK(c, st.pad_buttons == south && st.down == 1u && st.stick_touch == 2);
    // Slid across to B: the press moves with it (once A's tap hold has passed).
    t[0] = at(1, b.x, b.y);
    touchpad_update(cfg, st, t, 2, s, 0.1);
    KV_CHECK(c, st.pad_buttons == east);
    // A finger just past the drawn edge still counts (hit 1.15); far past does not.
    f32 r = touchpad_button_radius(cfg, 0, s);
    t[0] = at(1, a.x + r * 1.1f, a.y);
    touchpad_update(cfg, st, t, 1, s, 0.2);
    KV_CHECK(c, st.pad_buttons == south);
    t[0] = at(1, a.x + r * 1.3f, a.y);
    touchpad_update(cfg, st, t, 1, s, 0.3);
    KV_CHECK(c, st.pad_buttons == 0);

    // A tap that began and ended between two frames is seen once, as ended, and holds its
    // button min_press, then lets go.
    TouchpadState tap;
    TouchPoint q = at(5, b.x, b.y, true);
    touchpad_update(cfg, tap, &q, 1, s, 1.0);
    KV_CHECK(c, tap.pad_buttons == east);
    touchpad_update(cfg, tap, nullptr, 0, s, 1.0 + cfg.min_press * 0.5);
    KV_CHECK(c, tap.pad_buttons == east);
    touchpad_update(cfg, tap, nullptr, 0, s, 1.0 + cfg.min_press * 1.5);
    KV_CHECK(c, tap.pad_buttons == 0 && tap.down == 0);

    // A thumb landing on a button in the stick's region presses it and starts no stick.
    TouchpadConfig lb = cfg;
    lb.buttons[0].from = TouchCorner::BottomLeft;
    lb.buttons[0].at = vec2{200, 200};
    vec2 l = touchpad_button_center(lb, 0, s);
    TouchpadState sl;
    TouchPoint u = at(8, l.x, l.y);
    touchpad_update(lb, sl, &u, 1, s, 0.0);
    KV_CHECK(c, sl.stick_touch == 0 && sl.pad_buttons == south);
    // The stick's own finger never presses a button it passes over.
    TouchpadState sp;
    TouchPoint w = at(9, l.x + 400, l.y - 300);
    touchpad_update(lb, sp, &w, 1, s, 0.0);
    w = at(9, l.x, l.y);
    touchpad_update(lb, sp, &w, 1, s, 0.016);
    KV_CHECK(c, sp.stick_touch == 9 && sp.pad_buttons == 0);
}
