#include "korvid/platform_hid/actions.h"
#include "korvid/platform_hid/input.h"
#include "korvid/platform_hid/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/utf8.h"
#include "korvid/math/scalar.h"
#include <string.h>

namespace kv {

namespace {

bool key_named(const char* n, u16* out) {
    for (u32 k = 1; k < (u32)Key::Count; ++k)
        if (!strcmp(key_name((Key)k), n)) { *out = (u16)k; return true; }
    return false;
}

bool button_named(const char* n, u8* out) {
    for (u32 b = 0; b < (u32)PadButton::Count; ++b)
        if (!strcmp(pad_button_name((PadButton)b), n)) { *out = (u8)b; return true; }
    return false;
}

// One comma-separated binding: words separated by spaces.
bool parse_binding(const char* text, ActionKind kind, ActionBinding& b, const char** why) {
    char words[6][24];
    u32 n = 0;
    for (const char* p = text; *p && n < 6;) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        u32 l = 0;
        while (p[l] && p[l] != ' ' && p[l] != '\t') ++l;
        if (l >= sizeof words[0]) { *why = "a word is too long"; return false; }
        memcpy(words[n], p, l);
        words[n++][l] = 0;
        p += l;
    }
    b = ActionBinding{};
    if (n == 2 && !strcmp(words[0], "key")) { b.kind = BindingKind::Key; if (!key_named(words[1], &b.keys[0])) { *why = "unknown key"; return false; } return true; }
    if (n == 2 && !strcmp(words[0], "pad")) { b.kind = BindingKind::PadButton; if (!button_named(words[1], &b.button)) { *why = "unknown pad button"; return false; } return true; }
    if (kind == ActionKind::Axis2) {
        if (n == 5 && !strcmp(words[0], "keys")) {
            b.kind = BindingKind::Keys4;
            for (u32 i = 0; i < 4; ++i) if (!key_named(words[i + 1], &b.keys[i])) { *why = "unknown key"; return false; }
            return true;
        }
        if (n == 2 && !strcmp(words[0], "stick")) {
            b.kind = BindingKind::Stick;
            if (!strcmp(words[1], "left")) b.stick = 0; else if (!strcmp(words[1], "right")) b.stick = 1; else { *why = "stick left or right"; return false; }
            return true;
        }
        if (n == 1 && !strcmp(words[0], "dpad")) { b.kind = BindingKind::Dpad; return true; }
    }
    *why = kind == ActionKind::Axis2 ? "expected key, pad, keys U D L R, stick left|right or dpad" : "expected key <Key> or pad <Button>";
    return false;
}

HidState* pstate(Engine& e) { return engine_module_state_as<HidState>(e, "platform_hid"); }

vec2 clamp_unit(vec2 v) {
    f32 l = kv::sqrt(v.x * v.x + v.y * v.y);
    return l > 1.0f ? vec2{v.x / l, v.y / l} : v;
}

} // namespace

u32 InputActions::find(const char* name) const {
    for (u32 i = 0; i < count; ++i) if (!strcmp(actions[i].name, name)) return i;
    return KV_ACTION_NONE;
}

bool input_actions(Engine& e, Span<const InputActionDesc> descs) {
    HidState* s = pstate(e);
    if (!s) return false;
    if (descs.len > KV_ACTIONS) { KV_ERROR("input", "%u actions, more than %u", (u32)descs.len, KV_ACTIONS); return false; }
    InputActions next;
    next.dead_zone = s->actions.dead_zone;
    for (const InputActionDesc& d : descs) {
        ActionState& a = next.actions[next.count++];
        a = ActionState{};
        utf8_copy(a.name, sizeof a.name, d.name);
        a.kind = d.kind;
        // Split the bindings on commas.
        const char* p = d.bindings ? d.bindings : "";
        while (*p) {
            char one[64];
            u32 l = 0;
            while (p[l] && p[l] != ',') ++l;
            if (l >= sizeof one) { KV_ERROR("input", "action '%s': a binding is too long", d.name); return false; }
            memcpy(one, p, l);
            one[l] = 0;
            p += l + (p[l] == ',' ? 1 : 0);
            const char* why = nullptr;
            if (a.binding_count >= KV_ACTION_BINDINGS) { KV_ERROR("input", "action '%s': more than %u bindings", d.name, KV_ACTION_BINDINGS); return false; }
            if (!parse_binding(one, d.kind, a.bindings[a.binding_count], &why)) { KV_ERROR("input", "action '%s': '%s': %s", d.name, one, why); return false; }
            ++a.binding_count;
        }
    }
    s->actions = next;
    return true;
}

const InputActions* input_actions_get(Engine& e) {
    HidState* s = pstate(e);
    return s ? &s->actions : nullptr;
}

void input_action_override(Engine& e, const char* name, bool down, vec2 axis) {
    HidState* s = pstate(e);
    if (!s) return;
    u32 id = s->actions.find(name);
    if (id == KV_ACTION_NONE) return;
    ActionState& a = s->actions.actions[id];
    a.override_set   = true;
    a.override_exact = false;
    a.override_sim   = !e.clock.paused;
    a.override_down  = down;
    a.override_axis  = axis;
}

void input_action_override_exact(Engine& e, const char* name, bool down, vec2 axis) {
    input_action_override(e, name, down, axis);
    HidState* s = pstate(e);
    u32 id = s ? s->actions.find(name) : KV_ACTION_NONE;
    if (id != KV_ACTION_NONE) s->actions.actions[id].override_exact = true;
}

bool action_repeat(const InputActions& a, u32 id, u32 delay, u32 interval) {
    if (id >= a.count || !a.actions[id].down) return false;
    u32 h = a.actions[id].held;
    if (h == 1) return true;
    if (h <= delay || interval == 0) return false;
    return (h - 1 - delay) % interval == 0;
}

void input_actions_sample(InputActions& as, const InputState& in, bool paused) {
    const GamepadState* pad = in.pad();
    for (u32 i = 0; i < as.count; ++i) {
        ActionState& a = as.actions[i];
        a.was_down = a.down;
        bool down = false;
        vec2 axis{0, 0};
        f32 best = 0.0f;
        for (u32 k = 0; k < a.binding_count; ++k) {
            const ActionBinding& b = a.bindings[k];
            vec2 v{0, 0};
            switch (b.kind) {
            case BindingKind::Key:       if (in.down((Key)b.keys[0])) v = vec2{1, 0}; break;
            case BindingKind::PadButton: if (pad && pad->down((PadButton)b.button)) v = vec2{1, 0}; break;
            case BindingKind::Keys4:
                v = vec2{(in.down((Key)b.keys[3]) ? 1.0f : 0.0f) - (in.down((Key)b.keys[2]) ? 1.0f : 0.0f),
                         (in.down((Key)b.keys[0]) ? 1.0f : 0.0f) - (in.down((Key)b.keys[1]) ? 1.0f : 0.0f)};
                break;
            case BindingKind::Stick:
                if (pad) {
                    f32 x = pad->axis(b.stick ? PadAxis::RightX : PadAxis::LeftX), y = -pad->axis(b.stick ? PadAxis::RightY : PadAxis::LeftY);
                    f32 l = kv::sqrt(x * x + y * y);
                    // Rescaled past the dead zone, so a stick just outside it is slow, not a jump.
                    if (l > as.dead_zone) { f32 s = (l - as.dead_zone) / (1.0f - as.dead_zone) / l; v = vec2{x * s, y * s}; }
                }
                break;
            case BindingKind::Dpad:
                if (pad) v = vec2{(pad->down(PadButton::DpadRight) ? 1.0f : 0.0f) - (pad->down(PadButton::DpadLeft) ? 1.0f : 0.0f),
                                  (pad->down(PadButton::DpadUp) ? 1.0f : 0.0f) - (pad->down(PadButton::DpadDown) ? 1.0f : 0.0f)};
                break;
            }
            f32 m = v.x * v.x + v.y * v.y;
            if (m > best) { best = m; axis = v; }
        }
        axis = clamp_unit(axis);
        down = best > 0.0f;
        if (a.override_set && !(paused && a.override_sim)) {
            down = a.override_down;
            axis = a.kind != ActionKind::Axis2 ? (down ? vec2{1, 0} : vec2{0, 0}) : a.override_exact ? a.override_axis : clamp_unit(a.override_axis);
            if (a.kind == ActionKind::Axis2) down = axis.x != 0.0f || axis.y != 0.0f;
            a.override_set = false;
        }
        a.down = down;
        a.axis = a.kind == ActionKind::Button ? (down ? vec2{1, 0} : vec2{0, 0}) : axis;
        a.held = down ? a.held + 1 : 0;
    }
}

} // namespace kv
