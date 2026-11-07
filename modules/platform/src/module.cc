#include "korvid/platform/module.h"
#include "korvid/platform/events.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include <SDL3/SDL.h>

namespace kv {

namespace {

bool platform_startup(Engine& engine, ModuleInstance& self) {
    SDL_SetHint(SDL_HINT_APP_NAME, "korvid");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        KV_ERROR("platform", "SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    int ver = SDL_GetVersion();
    KV_INFO("platform", "SDL %d.%d.%d, video driver '%s'",
            SDL_VERSIONNUM_MAJOR(ver), SDL_VERSIONNUM_MINOR(ver), SDL_VERSIONNUM_MICRO(ver),
            SDL_GetCurrentVideoDriver());
    PlatformState* s = arena_push<PlatformState>(self.arena);
    if (!s) return false;
    self.set_state(s);
    s->base_path = SDL_GetBasePath();
    // Systems reach these under their declared access; the accessors below are for code
    // outside the schedule (the frame loop, startup).
    if (!engine_publish(engine, self, "input.state", &s->input) ||
        !engine_publish(engine, self, "platform.window", &s->window)) return false;
    return s->io.init(&self.general, 4096);
}

void platform_shutdown(Engine&, ModuleInstance& self) {
    PlatformState* s = self.state_as<PlatformState>();
    if (s) {
        s->window.close(); // the service ends with the module, whoever opened it
        s->io.free();
    }
    SDL_Quit();
}

// Phase Input: sample the raw key state once per tick for the simulation.
void input_sample(TickContext& ctx) {
    ctx.state_as<PlatformState>().input.sample();
}

constinit const EventTypeDesc k_events[] = {
    KV_EVENT(WindowResized),
    KV_EVENT(FocusChanged),
    KV_EVENT(CloseRequested),
    KV_EVENT(KeyDown),
    KV_EVENT(KeyUp),
};
constinit const ResourceDesc k_resources[] = {{"input.state"}, {"platform.window"}};
constinit const char* const  k_input_writes[] = {"input.state"};
constinit const SystemDesc   k_systems[] = {
    {.name = "input.sample", .phase = Phase::Input, .fn = input_sample, .writes = k_input_writes},
};

} // namespace

KV_MODULE_DEFINE(platform_module) = {
    .name      = "platform",
    .events    = k_events,
    .resources = k_resources,
    .systems   = k_systems,
    .startup   = platform_startup,
    .shutdown  = platform_shutdown,
    .memory    = {.reserve = 64 * 1024},
};

static PlatformState* state(Engine& e) {
    return engine_module_state_as<PlatformState>(e, "platform");
}

HostWindow* platform_open_window(Engine& e, const WindowDesc& desc) {
    PlatformState* s = state(e);
    if (!s) { KV_ERROR("platform", "module not started"); return nullptr; }
    if (s->window.is_open()) { KV_ERROR("platform", "a window is already open"); return nullptr; }
    return s->window.open(desc) ? &s->window : nullptr;
}

HostWindow* platform_window(Engine& e) {
    PlatformState* s = state(e);
    return s && s->window.is_open() ? &s->window : nullptr;
}

Input* platform_input(Engine& e) {
    PlatformState* s = state(e);
    return s ? &s->input : nullptr;
}

void platform_pump(Engine& e) {
    PlatformState* s = state(e);
    if (!s) return;
    if (s->window.is_open()) s->window.pump(e.events, s->input);
    s->io.pump();
}

IoQueue* platform_io(Engine& e) {
    PlatformState* s = state(e);
    return s ? &s->io : nullptr;
}

const char* platform_base_path(Engine& e) {
    PlatformState* s = state(e);
    return s && s->base_path ? s->base_path : "";
}

} // namespace kv
