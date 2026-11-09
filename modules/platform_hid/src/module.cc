#include "korvid/platform_hid/module.h"
#include "korvid/platform_hid/events.h"
#include "korvid/platform/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "hid_backend.h"
#include "korvid/core/utf8.h"
#include <string.h>

namespace kv {

namespace {

// From platform_pump, before the I/O queue: OS events become bus events and raw input.
void hid_pump(Engine& e, void* user) {
    HidState& s = *(HidState*)user;
    if (s.window.is_open()) s.window.pump(e.events, s.input);
}

bool hid_startup(Engine& engine, ModuleInstance& self) {
    if (!hid_backend_startup()) return false;
    HidState* s = arena_push<HidState>(self.arena);
    if (!s) return false;
    self.set_state(s);
    // Systems reach these under their declared access; the accessors below are for code
    // outside the schedule (the frame loop, startup).
    if (!engine_publish(engine, self, "input.state", &s->input) ||
        !engine_publish(engine, self, "input.actions", &s->actions) ||
        !engine_publish(engine, self, ".window", &s->window)) return false;
    return platform_pump_add(engine, hid_pump, s);
}

void hid_shutdown(Engine& e, ModuleInstance& self) {
    HidState* s = self.state_as<HidState>();
    if (s) {
        platform_pump_remove(e, hid_pump, s);
        s->window.close(); // the service ends with the module, whoever opened it
        input_gamepads_close(s->input);
    }
    hid_backend_shutdown();
}

// Phase Input: sample the raw key state once per tick for the simulation.
void input_sample(TickContext& ctx) {
    HidState& s = ctx.state_as<HidState>();
    if (s.text_override_len || s.text_override_backspaces) {
        s.input.raw.type(s.text_override);
        s.input.raw.backspaces += s.text_override_backspaces;
        s.text_override[0] = 0; s.text_override_len = 0; s.text_override_backspaces = 0;
    }
    s.input.sample();
    input_actions_sample(s.actions, s.input.sampled);
}

constinit const EventTypeDesc k_events[] = {
    KV_EVENT(WindowResized),
    KV_EVENT(FocusChanged),
    KV_EVENT(CloseRequested),
    KV_EVENT(KeyDown),
    KV_EVENT(KeyUp),
    KV_EVENT(MouseMoved),
    KV_EVENT(MouseButtonDown),
    KV_EVENT(MouseButtonUp),
    KV_EVENT(MouseWheel),
    KV_EVENT(GamepadConnected),
    KV_EVENT(GamepadDisconnected),
    KV_EVENT(PadButtonDown),
    KV_EVENT(PadButtonUp),
};
constinit const ResourceDesc k_resources[] = {{"input.state"}, {"input.actions"}, {".window"}};
constinit const SystemDesc   k_systems[] = {
    {.name = "input.sample", .phase = Phase::Input, .fn = input_sample, .writes = "input.state, input.actions", .runs = RunWhen::Always},
};

} // namespace

KV_MODULE_DEFINE(platform_hid_module) = {
    .name      = "platform_hid",
    .events    = k_events,
    .resources = k_resources,
    .systems   = k_systems,
    .startup   = hid_startup,
    .shutdown  = hid_shutdown,
    .memory    = {.reserve = 64 * 1024},
};

static HidState* state(Engine& e) {
    return engine_module_state_as<HidState>(e, "platform_hid");
}

HostWindow* platform_open_window(Engine& e, const WindowDesc& desc) {
    HidState* s = state(e);
    if (!s) { KV_ERROR("hid", "module not started"); return nullptr; }
    if (s->window.is_open()) { KV_ERROR("hid", "a window is already open"); return nullptr; }
    return s->window.open(desc) ? &s->window : nullptr;
}

bool platform_video_up(Engine&) { return hid_video_up(); }

HostWindow* platform_window(Engine& e) {
    HidState* s = state(e);
    return s && s->window.is_open() ? &s->window : nullptr;
}

void platform_text_input(Engine& e, bool on) {
    HidState* s = state(e);
    if (!s) return;
    u32 before = s->text_requests;
    if (on) ++s->text_requests; else if (s->text_requests) --s->text_requests;
    if ((before == 0) != (s->text_requests == 0) && s->window.is_open()) s->window.text_input(s->text_requests != 0);
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
