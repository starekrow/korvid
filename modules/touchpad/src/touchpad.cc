// The touchpad's layout and logic: plain arithmetic over the touches, testable without a
// screen. module.cc feeds it each frame and draws what it says.
#include "korvid/touchpad/touchpad.h"
#include "korvid/core/json.h"
#include "korvid/core/memory.h"
#include "korvid/math/scalar.h"
#include <string.h>

namespace kv {

namespace {

void set_button(TouchpadButton& b, const char* name, PadButton pad, TouchCorner from, vec2 at, f32 radius) {
    strncpy(b.name, name, sizeof b.name - 1);
    b.pad = pad;
    b.from = from;
    b.at = at;
    b.radius = radius;
}

bool same_ascii_nocase(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b)
        if ((*a | 0x20) != (*b | 0x20)) return false;
    return *a == *b;
}

bool parse_pad(const char* s, PadButton* out) {
    for (u32 i = 0; i < (u32)PadButton::Count; ++i)
        if (same_ascii_nocase(s, pad_button_name((PadButton)i))) { *out = (PadButton)i; return true; }
    return false;
}

bool parse_corner(const char* s, TouchCorner* out) {
    static const char* const names[] = {"top_left", "top_right", "bottom_left", "bottom_right"};
    for (u32 i = 0; i < 4; ++i)
        if (strcmp(s, names[i]) == 0) { *out = (TouchCorner)i; return true; }
    return false;
}


// The safe area, pixels: x0, y0, x1, y1.
void safe_rect(const TouchpadScreen& s, f32 r[4]) {
    r[0] = s.insets[0];
    r[1] = s.insets[1];
    r[2] = s.width - s.insets[2];
    r[3] = s.height - s.insets[3];
}

// Where a stick may sit: the region, pulled in by the margin (to its middle when the
// margins meet).
void stick_bounds(const TouchpadConfig& c, const TouchpadScreen& s, f32 b[4]) {
    f32 r[4];
    safe_rect(s, r);
    f32 w = r[2] - r[0], h = r[3] - r[1];
    f32 x0 = r[0] + c.region[0] * w, y0 = r[1] + c.region[1] * h;
    f32 x1 = x0 + c.region[2] * w, y1 = y0 + c.region[3] * h;
    f32 m = c.margin * s.scale;
    b[0] = x0 + m; b[2] = x1 - m;
    b[1] = y0 + m; b[3] = y1 - m;
    if (b[0] > b[2]) b[0] = b[2] = (x0 + x1) * 0.5f;
    if (b[1] > b[3]) b[1] = b[3] = (y0 + y1) * 0.5f;
}

bool in_region(const TouchpadConfig& c, const TouchpadScreen& s, f32 x, f32 y) {
    f32 r[4];
    safe_rect(s, r);
    f32 w = r[2] - r[0], h = r[3] - r[1];
    f32 x0 = r[0] + c.region[0] * w, y0 = r[1] + c.region[1] * h;
    return x >= x0 && x < x0 + c.region[2] * w && y >= y0 && y < y0 + c.region[3] * h;
}

bool on_button(const TouchpadConfig& c, const TouchpadScreen& s, u32 i, f32 x, f32 y) {
    vec2 p = touchpad_button_center(c, i, s);
    f32 r = touchpad_button_radius(c, i, s) * c.hit;
    f32 dx = x - p.x, dy = y - p.y;
    return dx * dx + dy * dy <= r * r;
}

} // namespace

TouchpadConfig touchpad_default() {
    TouchpadConfig c;
    set_button(c.buttons[0], "A", PadButton::South, TouchCorner::BottomRight, vec2{128, 64}, 38);
    set_button(c.buttons[1], "B", PadButton::East, TouchCorner::BottomRight, vec2{56, 128}, 38);
    set_button(c.buttons[2], "Start", PadButton::Start, TouchCorner::TopRight, vec2{40, 40}, 20);
    c.button_count = 3;
    return c;
}

bool touchpad_parse(const char* json, usize len, TouchpadConfig& out, const char** why) {
    MemScope scope;
    JsonDoc doc;
    if (!json_parse(json, len, mem_scratch(), doc)) { *why = doc.error; return false; }
    const JsonValue& root = doc.root();
    if (!root.is_object()) { *why = "the layout is not an object"; return false; }
    TouchpadConfig c = out;
    if (const JsonValue* st = json_get(doc, root, "stick")) {
        f32 r[4];
        const JsonValue* region = json_get(doc, *st, "region");
        if (region) {
            if (json_numbers(doc, region, r, 4) != 4 || r[2] <= 0 || r[3] <= 0) { *why = "stick.region is not [x, y, w, h] with a size"; return false; }
            memcpy(c.region, r, sizeof r);
        }
        c.margin = (f32)json_number(json_get(doc, *st, "margin"), c.margin);
        c.radius = (f32)json_number(json_get(doc, *st, "radius"), c.radius);
        if (c.radius <= 0) { *why = "stick.radius is not positive"; return false; }
        const char* mode = json_string(json_get(doc, *st, "mode"), c.drag ? "drag" : "clamp");
        if (strcmp(mode, "drag") == 0) c.drag = true;
        else if (strcmp(mode, "clamp") == 0) c.drag = false;
        else { *why = "stick.mode is not drag or clamp"; return false; }
    }
    if (const JsonValue* buttons = json_get(doc, root, "buttons")) {
        if (!buttons->is_array()) { *why = "buttons is not an array"; return false; }
        if (buttons->count() > KV_TOUCHPAD_BUTTONS) { *why = "more than 8 buttons"; return false; }
        c.button_count = 0;
        for (const JsonValue* b = json_first(doc, *buttons); b; b = json_next(doc, *b)) {
            TouchpadButton& tb = c.buttons[c.button_count];
            tb = TouchpadButton{};
            const char* pad = json_string(json_get(doc, *b, "pad"), nullptr);
            if (!pad || !parse_pad(pad, &tb.pad)) { *why = "a button's pad is not a PadButton's name"; return false; }
            strncpy(tb.name, json_string(json_get(doc, *b, "name"), pad), sizeof tb.name - 1);
            const char* from = json_string(json_get(doc, *b, "from"), "bottom_right");
            if (!parse_corner(from, &tb.from)) { *why = "a button's from is not a corner"; return false; }
            f32 at[2];
            if (json_numbers(doc, json_get(doc, *b, "at"), at, 2) != 2) { *why = "a button has no at [x, y]"; return false; }
            tb.at = vec2{at[0], at[1]};
            tb.radius = (f32)json_number(json_get(doc, *b, "radius"), tb.radius);
            if (tb.radius <= 0) { *why = "a button's radius is not positive"; return false; }
            ++c.button_count;
        }
    }
    c.opacity = clampf((f32)json_number(json_get(doc, root, "opacity"), c.opacity), 0.0f, 1.0f);
    out = c;
    return true;
}

vec2 touchpad_button_center(const TouchpadConfig& c, u32 index, const TouchpadScreen& s) {
    const TouchpadButton& b = c.buttons[index];
    f32 r[4];
    safe_rect(s, r);
    f32 ax = b.at.x * s.scale, ay = b.at.y * s.scale;
    bool right = b.from == TouchCorner::TopRight || b.from == TouchCorner::BottomRight;
    bool bottom = b.from == TouchCorner::BottomLeft || b.from == TouchCorner::BottomRight;
    return vec2{right ? r[2] - ax : r[0] + ax, bottom ? r[3] - ay : r[1] + ay};
}

f32 touchpad_button_radius(const TouchpadConfig& c, u32 index, const TouchpadScreen& s) {
    return c.buttons[index].radius * s.scale;
}

void touchpad_update(const TouchpadConfig& c, TouchpadState& st, const TouchPoint* touches, u32 count, const TouchpadScreen& s, f64 now) {
    auto seen_before = [&](u32 id) {
        for (u32 k = 0; k < KV_MAX_TOUCHES; ++k) if (st.seen[k] == id) return true;
        return false;
    };
    auto on_any_button = [&](f32 x, f32 y) {
        for (u32 i = 0; i < c.button_count; ++i) if (on_button(c, s, i, x, y)) return true;
        return false;
    };
    f32 b[4];
    stick_bounds(c, s, b);
    f32 radius = c.radius * s.scale;

    // The stick: the finger on it, while it lasts; else the first new finger that lands in
    // the region (and not on a button) takes it, its centre where it landed, kept inside
    // the margins.
    const TouchPoint* stick = nullptr;
    for (u32 i = 0; i < count; ++i)
        if (st.stick_touch && touches[i].id == st.stick_touch && !touches[i].ended) stick = &touches[i];
    if (!stick) {
        st.stick_touch = 0;
        for (u32 i = 0; i < count && !stick; ++i) {
            const TouchPoint& t = touches[i];
            if (t.ended || seen_before(t.id) || !in_region(c, s, t.x, t.y) || on_any_button(t.x, t.y)) continue;
            stick = &t;
            st.stick_touch = t.id;
            st.center = vec2{clampf(t.x, b[0], b[2]), clampf(t.y, b[1], b[3])};
        }
    }
    if (stick) {
        f32 dx = stick->x - st.center.x, dy = stick->y - st.center.y;
        f32 len = kv::sqrt(dx * dx + dy * dy);
        if (len > radius) {
            if (c.drag) {
                // The centre follows the thumb out to the radius, as far as the margins let it.
                f32 pull = (len - radius) / len;
                st.center.x = clampf(st.center.x + dx * pull, b[0], b[2]);
                st.center.y = clampf(st.center.y + dy * pull, b[1], b[3]);
                dx = stick->x - st.center.x;
                dy = stick->y - st.center.y;
                len = kv::sqrt(dx * dx + dy * dy);
            }
            if (len > radius) { dx *= radius / len; dy *= radius / len; }
        }
        st.knob = vec2{st.center.x + dx, st.center.y + dy};
        st.axis = vec2{dx / radius, dy / radius};
    } else {
        st.axis = vec2{0, 0};
    }

    // The buttons: held while a finger other than the stick's is on one (one that slides
    // from button to button moves the press with it); a tap holds its button min_press, so
    // a tick samples it even when it began and ended between two frames.
    st.down = 0;
    st.pad_buttons = 0;
    for (u32 i = 0; i < c.button_count; ++i) {
        bool touched = false;
        for (u32 k = 0; k < count && !touched; ++k)
            if (touches[k].id != st.stick_touch && on_button(c, s, i, touches[k].x, touches[k].y)) touched = true;
        if (touched && now + c.min_press > st.held_until[i]) st.held_until[i] = now + c.min_press;
        if (touched || now < st.held_until[i]) {
            st.down |= 1u << i;
            st.pad_buttons |= 1u << (u32)c.buttons[i].pad;
        }
    }

    // The touches known now; an ended one is not, so the same id coming back is new.
    for (u32 k = 0; k < KV_MAX_TOUCHES; ++k) st.seen[k] = k < count && !touches[k].ended ? touches[k].id : 0;
}

} // namespace kv
