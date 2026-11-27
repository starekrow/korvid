#include "korvid/platform_hid/module.h"
#include "korvid/platform_hid/events.h"
#include "korvid/platform/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/cli.h"
#include "hid_backend.h"
#include "korvid/core/utf8.h"
#include <string.h>

namespace kv {

namespace {

// From platform_pump, before the I/O queue: OS events become bus events and raw input. The
// main window's pump takes every window's events.
void hid_pump(Engine& e, void* user) {
    HidState& s = *(HidState*)user;
    if (!s.windows[0].is_open()) return;
    s.windows[0].pump(e.events, s.input);
    bool focused = false;
    for (HostWindow& w : s.windows) {
        if (!w.is_open()) continue;
        w.send_visibility(e.events);
        focused = focused || w.focused();
    }
    if (hid_backend_holds_unfocused()) engine_hold(e, HoldFocus, !focused);
}

// The window the pointer is in, or the main one.
HostWindow* pointer_window(HidState& s) {
    HostWindow& w = s.windows[s.input.raw.mouse.window < KV_WINDOWS ? s.input.raw.mouse.window : 0];
    return w.is_open() ? &w : s.windows[0].is_open() ? &s.windows[0] : nullptr;
}

// platform_wait's waiter: the window's wait while it is open; the platform's own before.
int hid_wait(void* user, f64 seconds) {
    HidState& s = *(HidState*)user;
    if (!s.windows[0].is_open()) return -1;
    return s.windows[0].wait(seconds) ? 1 : 0; // every window's events end it: they share a queue
}
void hid_wake(void* user) { ((HidState*)user)->windows[0].wake(); }

bool hid_startup(Engine& engine, ModuleInstance& self) {
    if (!hid_backend_startup()) return false;
    HidState* s = arena_push<HidState>(self.arena);
    if (!s) return false;
    self.set_state(s);
    for (u32 i = 0; i < KV_WINDOWS; ++i) s->windows[i].id_ = (u8)i;
    hid_windows_set(s->windows);
    // Systems reach these under their declared access; the accessors below are for code
    // outside the schedule (the frame loop, startup).
    if (!engine_publish(engine, self, "input.state", &s->input) ||
        !engine_publish(engine, self, "input.actions", &s->actions) ||
        !engine_publish(engine, self, ".window", &s->windows[0])) return false;
    PlatformWaiter w{hid_wait, hid_wake, s};
    platform_set_waiter(engine, &w);
    return platform_pump_add(engine, hid_pump, s);
}

void hid_shutdown(Engine& e, ModuleInstance& self) {
    HidState* s = self.state_as<HidState>();
    if (s) {
        platform_pump_remove(e, hid_pump, s);
        platform_set_waiter(e, nullptr);
        for (u32 i = KV_WINDOWS; i-- > 0;) s->windows[i].close(); // the service ends with the module, whoever opened it; the main one last
        input_gamepads_close(s->input);
        s->clipboard.free();
    }
    hid_windows_set(nullptr);
    hid_backend_shutdown();
}

// Phase Input: sample the raw key state once per tick for the simulation.
void input_sample(TickContext& ctx) {
    HidState& s = ctx.state_as<HidState>();
    if (s.text_override_len || s.text_override_backspaces) {
        s.input.raw.type(s.text_override);
        s.input.raw.backspaces += s.text_override_backspaces;
        // As events too, as a host's text is.
        for (const char* p = s.text_override; *p;) {
            TextTyped t{};
            usize left = strlen(p);
            usize n = utf8_prefix(p, left < sizeof t.text - 1 ? left : sizeof t.text - 1);
            if (!n) break;
            memcpy(t.text, p, n);
            ctx.emit(t);
            p += n;
        }
        s.text_override[0] = 0; s.text_override_len = 0; s.text_override_backspaces = 0;
    }
    for (u32 i = 0; i < s.key_override_count; ++i) {
        const HidState::KeyEdge& k = s.key_overrides[i];
        s.input.raw.set(k.key, k.down);
        if (k.down) ctx.emit(KeyDown{k.key, 0, false});
        else ctx.emit(KeyUp{k.key, 0});
    }
    s.key_override_count = 0;
    if (s.compose_pending) { ctx.emit(s.compose_override); s.compose_pending = false; }
    if (s.pointer_override) {
        // As the mouse would: the state, and the events its changes make.
        MouseState& m = s.input.raw.mouse;
        u8 was = m.buttons;
        m.x = s.pointer_x;
        m.y = s.pointer_y;
        m.window = s.pointer_window;
        m.desktop = false; // put there by a script, not the mouse: no place on the desktop to go by
        m.buttons = s.pointer_buttons;
        m.wheel += s.pointer_wheel;
        for (u32 b = 0; b < (u32)MouseButton::Count; ++b) {
            bool before = (was >> b) & 1u, now = (m.buttons >> b) & 1u;
            if (now && !before) ctx.emit(MouseButtonDown{(MouseButton)b, m.x, m.y, m.window});
            if (before && !now) ctx.emit(MouseButtonUp{(MouseButton)b, m.x, m.y, m.window});
        }
        if (s.pointer_wheel != 0.0f) ctx.emit(MouseWheel{0.0f, s.pointer_wheel, m.window});
        s.pointer_override = false;
        s.pointer_wheel = 0.0f;
    }
    // The CLI's presses stand in for the devices while they last.
    for (u32 i = 0; i < s.press_count;) {
        HidPress& p = s.presses[i];
        ActionState& a = s.actions.actions[p.action];
        a.override_set = true;
        a.override_exact = false;
        a.override_sim = false; // a person's press: whatever tick it lands on
        a.override_down = true;
        a.override_axis = p.axis;
        if (--p.ticks == 0) p = s.presses[--s.press_count];
        else ++i;
    }
    s.input.sample();
    input_actions_sample(s.actions, s.input.sampled, ctx.paused);
}

// `press ACTION [TICKS] [X Y]`: hold a named action for TICKS sampled ticks (6), an Axis2
// toward (X, Y), so a script over the CLI can work menus and drive a game as a player does.
void cli_press(CliContext& ctx, Span<const char* const> args) {
    HidState* s = engine_module_state_as<HidState>(*ctx.engine, "platform_hid");
    if (!s || args.len < 1) { ctx.fail("press ACTION [TICKS] [X Y]"); return; }
    u32 id = s->actions.find(args[0]);
    if (id == KV_ACTION_NONE) { ctx.fail("no action '%s'", args[0]); return; }
    u32 ticks = 6;
    vec2 axis{0, 0};
    if (args.len >= 2 && !cli_arg_u32(ctx, args[1], "TICKS", &ticks)) return;
    if (args.len >= 4 && (!cli_arg_f32(ctx, args[2], "X", &axis.x) || !cli_arg_f32(ctx, args[3], "Y", &axis.y))) return;
    if (ticks == 0 || s->press_count >= KV_HID_PRESSES) { ctx.fail(ticks ? "too many presses held" : "TICKS must be at least 1"); return; }
    s->presses[s->press_count++] = HidPress{id, ticks, axis};
    ctx.print("%s held for %u ticks", args[0], ticks);
}

constinit const EventTypeDesc k_events[] = {
    KV_EVENT(WindowResized),
    KV_EVENT(FocusChanged),
    KV_EVENT(CloseRequested),
    KV_EVENT(WindowVisibility),
    KV_EVENT(WindowMoved),
    KV_EVENT(KeyDown),
    KV_EVENT(KeyUp),
    KV_EVENT(MouseMoved),
    KV_EVENT(MouseButtonDown),
    KV_EVENT(MouseButtonUp),
    KV_EVENT(MouseWheel),
    KV_EVENT(TextTyped),
    KV_EVENT(TextComposing),
    KV_EVENT(GamepadConnected),
    KV_EVENT(GamepadDisconnected),
    KV_EVENT(PadButtonDown),
    KV_EVENT(PadButtonUp),
};
constinit const ResourceDesc k_resources[] = {{"input.state"}, {"input.actions"}, {".window"}};
// `type TEXT...`: typed text for the next sample, the words joined by spaces; `\n` ends a
// line (a field submits).
void cli_type(CliContext& ctx, Span<const char* const> args) {
    char text[KV_INPUT_TEXT_MAX];
    usize n = 0;
    for (u32 i = 0; i < args.len && n + 1 < sizeof text; ++i) {
        if (i) text[n++] = ' ';
        for (const char* c = args[i]; *c && n + 1 < sizeof text; ++c) {
            if (c[0] == '\\' && c[1] == 'n') { text[n++] = '\n'; ++c; }
            else text[n++] = *c;
        }
    }
    text[n] = 0;
    input_text_override(*ctx.engine, text);
    ctx.print("typed %u bytes", (u32)n);
}

constinit const CliCommandDesc k_commands[] = {
    {.name = "type", .help = "TEXT...: typed text for the next sample (\\n ends a line)", .fn = cli_type},
    {.name = "press", .help = "ACTION [TICKS] [X Y]: hold an action for TICKS sampled ticks (6), an Axis2 toward X Y", .fn = cli_press},
};
constinit const SystemDesc   k_systems[] = {
    {.name = "input.sample", .phase = Phase::Input, .fn = input_sample, .writes = "input.state, input.actions",
     .emits = "MouseButtonDown, MouseButtonUp, MouseWheel, TextTyped, TextComposing, KeyDown, KeyUp", .runs = RunWhen::Always},
};

} // namespace

KV_MODULE_DEFINE(platform_hid_module) = {
    .name      = "platform_hid",
    .events    = k_events,
    .resources = k_resources,
    .systems   = k_systems,
    .commands  = k_commands,
    .startup   = hid_startup,
    .shutdown  = hid_shutdown,
    .memory    = {.reserve = 64 * 1024},
};

static HidState* state(Engine& e) {
    return engine_module_state_as<HidState>(e, "platform_hid");
}

// The window's on_modal hook, from inside an OS modal loop that holds platform_pump: the
// events queued so far reach the bus, then the frame loop's frame runs. Not re-entered: a
// frame that somehow started another modal loop would otherwise nest frames.
static void hid_modal(void* ctx) {
    Engine& e = *(Engine*)ctx;
    HidState* s = state(e);
    if (!s || !s->modal_frame || s->in_modal_frame) return;
    s->in_modal_frame = true;
    s->windows[0].drain(e.events, s->input); // every window's, as pump
    for (HostWindow& w : s->windows) if (w.is_open()) w.send_visibility(e.events);
    s->modal_frame(s->modal_user);
    s->in_modal_frame = false;
}

HostWindow* platform_open_window(Engine& e, const WindowDesc& desc) {
    HidState* s = state(e);
    if (!s) { KV_ERROR("hid", "module not started"); return nullptr; }
    u32 most = hid_backend_windows();
    u32 id = 0;
    while (id < most && s->windows[id].is_open()) ++id;
    if (id == most) {
        KV_ERROR("hid", most == 1 ? "a window is already open, and this platform has one" : "%u windows are open, the most there are", most);
        return nullptr;
    }
    HostWindow& w = s->windows[id];
    if (!w.open(desc)) return nullptr;
    w.on_modal(hid_modal, &e);
    if (s->text_requests) w.text_input(true);
    if (id) KV_INFO("window", "window %u opened beside the main one", id);
    return &w;
}

void platform_close_window(Engine& e, HostWindow* w) {
    HidState* s = state(e);
    if (!s || !w || w < s->windows || w >= s->windows + KV_WINDOWS) return;
    if (w == &s->windows[0]) { KV_WARN("hid", "the main window closes with the module, not by platform_close_window"); return; }
    if (s->input.raw.mouse.window == w->id()) s->input.raw.mouse.window = 0;
    w->close();
}

HostWindow* platform_window_by_id(Engine& e, u32 id) {
    HidState* s = state(e);
    return s && id < KV_WINDOWS && s->windows[id].is_open() ? &s->windows[id] : nullptr;
}

void platform_modal_frame(Engine& e, void (*fn)(void* user), void* user) {
    HidState* s = state(e);
    if (!s) return;
    s->modal_frame = fn;
    s->modal_user  = user;
}

void platform_surface_hook(Engine& e, void (*fn)(void* user, void* native), void* user) {
    HidState* s = state(e);
    if (s) s->windows[0].on_surface(fn, user);
}

bool platform_video_up(Engine&) { return hid_video_up(); }

HostWindow* platform_window(Engine& e) {
    HidState* s = state(e);
    return s && s->windows[0].is_open() ? &s->windows[0] : nullptr;
}

bool platform_can_place(Engine&) { return hid_backend_can_place(); }

bool platform_display_bounds(Engine& e, const HostWindow* window, rect2* out) {
    const HostWindow* w = window ? window : platform_window(e);
    return w && w->is_open() && out && w->display_bounds(out);
}

bool platform_window_visible(Engine& e) {
    HostWindow* w = platform_window(e);
    return !w || w->visible();
}

void platform_text_input(Engine& e, bool on) {
    HidState* s = state(e);
    if (!s) return;
    u32 before = s->text_requests;
    if (on) ++s->text_requests; else if (s->text_requests) --s->text_requests;
    if ((before == 0) == (s->text_requests == 0)) return;
    for (HostWindow& w : s->windows) if (w.is_open()) w.text_input(s->text_requests != 0);
}

void input_pointer_override(Engine& e, f32 x, f32 y, u8 buttons, f32 wheel, u32 window) {
    HidState* s = engine_module_state_as<HidState>(e, "platform_hid");
    if (!s) return;
    s->pointer_override = true;
    s->pointer_x = x;
    s->pointer_y = y;
    s->pointer_buttons = buttons;
    s->pointer_wheel += wheel;
    s->pointer_window = window < KV_WINDOWS ? (u8)window : 0;
}

void input_text_override(Engine& e, const char* text, u32 backspaces) {
    HidState* s = state(e);
    if (!s) return;
    u32 n = text ? (u32)strlen(text) : 0;
    if (s->text_override_len + n >= sizeof s->text_override) n = (u32)sizeof s->text_override - 1 - s->text_override_len;
    memcpy(s->text_override + s->text_override_len, text, n);
    s->text_override_len += n;
    s->text_override[s->text_override_len] = 0;
    s->text_override_backspaces += backspaces;
}

void input_key_override(Engine& e, Key key, bool down) {
    HidState* s = state(e);
    if (!s || s->key_override_count == KV_COUNTOF(s->key_overrides)) return;
    s->key_overrides[s->key_override_count++] = HidState::KeyEdge{key, down};
}

void input_compose_override(Engine& e, const char* text, u32 caret) {
    HidState* s = state(e);
    if (!s) return;
    s->compose_override = TextComposing{};
    usize n = text ? strlen(text) : 0;
    n = utf8_prefix(text ? text : "", n < sizeof s->compose_override.text - 1 ? n : sizeof s->compose_override.text - 1);
    if (n) memcpy(s->compose_override.text, text, n);
    s->compose_override.caret = caret > n ? (u32)n : caret;
    s->compose_pending = true;
}

void platform_text_area(Engine& e, rect2 r, f32 caret) {
    HidState* s = state(e);
    if (!s) return;
    // The focused window's: the field with the keyboard is in it. The main one when none is.
    HostWindow* at = s->windows[0].is_open() ? &s->windows[0] : nullptr;
    for (HostWindow& w : s->windows) if (w.is_open() && w.focused()) { at = &w; break; }
    if (at) at->text_area(r, caret);
}

bool platform_clipboard_get(Engine& e, Array<char>& out) {
    HidState* s = state(e);
    if (!s) return false;
    if (s->windows[0].is_open() && s->windows[0].clipboard_get(out)) return true;
    if (!s->clipboard.len) return false;
    memcpy(out.push_n(s->clipboard.len), s->clipboard.data, s->clipboard.len);
    return true;
}

void platform_clipboard_set(Engine& e, const char* utf8) {
    HidState* s = state(e);
    if (!s) return;
    usize n = utf8 ? strlen(utf8) : 0;
    s->clipboard.clear();
    if (n) memcpy(s->clipboard.push_n((u32)n), utf8, n);
    if (s->windows[0].is_open()) s->windows[0].clipboard_set(utf8 ? utf8 : "");
}

void platform_cursor(Engine& e, CursorShape shape) {
    HidState* s = state(e);
    if (!s) return;
    if (HostWindow* w = pointer_window(*s)) w->cursor(shape);
}

void platform_capture_mouse(Engine& e, bool on) {
    HidState* s = state(e);
    if (!s) return;
    if (HostWindow* w = pointer_window(*s)) w->capture_mouse(on);
}

void InputState::type(const char* utf8) {
    const char* p = utf8 ? utf8 : "";
    while (*p) {
        const char* start = p;
        if (!utf8_next(p)) break;
        u32 n = (u32)(p - start);
        if (text_len + n >= KV_INPUT_TEXT_MAX) break;
        memcpy(text + text_len, start, n);
        text_len += n;
    }
    text[text_len] = 0;
}

Input* platform_input(Engine& e) {
    HidState* s = state(e);
    return s ? &s->input : nullptr;
}

} // namespace kv
