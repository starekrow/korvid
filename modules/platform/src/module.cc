#include "korvid/platform/module.h"
#include "korvid/platform/events.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include <SDL3/SDL.h>

namespace kv {

namespace {

bool platform_startup(Engine&, ModuleInstance& self) {
    SDL_SetHint(SDL_HINT_APP_NAME, "korvid");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        KV_ERROR("platform", "SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    int ver = SDL_GetVersion();
    KV_INFO("platform", "SDL %d.%d.%d, video driver '%s'",
            SDL_VERSIONNUM_MAJOR(ver), SDL_VERSIONNUM_MINOR(ver), SDL_VERSIONNUM_MICRO(ver),
            SDL_GetCurrentVideoDriver());
    PlatformState* s = (PlatformState*)mem_alloc(sizeof(PlatformState));
    *s = PlatformState{};
    self.state = s;
    return true;
}

void platform_shutdown(Engine&, ModuleInstance& self) {
    PlatformState* s = (PlatformState*)self.state;
    if (s) s->window.close(); // the service ends with the module, whoever opened it
    mem_free(s);
    SDL_Quit();
}

// Phase Input: sample the raw key state once per tick for the simulation.
void input_sample(TickContext& ctx) {
    ((PlatformState*)ctx.state)->input.sample();
}

constinit const EventTypeDesc k_events[] = {
    KV_EVENT(WindowResized),
    KV_EVENT(FocusChanged),
    KV_EVENT(CloseRequested),
    KV_EVENT(KeyDown),
    KV_EVENT(KeyUp),
};
constinit const ResourceDesc k_resources[] = {{"input.state"}};
constinit const char* const  k_input_writes[] = {"input.state"};
constinit const SystemDesc   k_systems[] = {
    {"input.sample", Phase::Input, input_sample, {}, k_input_writes},
};

} // namespace

KV_MODULE_DEFINE(platform_module) = {
    "platform", k_events, k_resources, k_systems, platform_startup, platform_shutdown,
};

static PlatformState* state(Engine& e) {
    return (PlatformState*)engine_module_state(e, "platform");
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
    if (s && s->window.is_open()) s->window.pump(e.events, s->input);
}

} // namespace kv
