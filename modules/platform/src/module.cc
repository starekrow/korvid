#include "korvid/platform/module.h"
#include "korvid/platform/audio.h"
#include "korvid/platform/events.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "sdl_internal.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <sys/stat.h>

namespace kv {

namespace {

PlatformMounts g_mounts; // what platform_configure_mounts said, for the next startup

// Idle time: the queue admits what the frame's pumps held back, idle reads included.
IdleResult io_idle(Engine&, f64 seconds, void* user) {
    IoQueue& q = *(IoQueue*)user;
    return q.pump_idle(q.idle_bytes_for(seconds)) ? IdleResult::More : IdleResult::Nothing;
}

bool platform_startup(Engine& engine, ModuleInstance& self) {
    SDL_SetHint(SDL_HINT_APP_NAME, "korvid");
    // Headless until a window opens: video and gamepads come up with the first window
    // (HostWindow::open), so tools, tests and servers run where there is no display.
    if (!SDL_Init(SDL_INIT_EVENTS)) {
        KV_ERROR("platform", "SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    int ver = SDL_GetVersion();
    KV_INFO("platform", "SDL %d.%d.%d, headless until a window opens",
            SDL_VERSIONNUM_MAJOR(ver), SDL_VERSIONNUM_MINOR(ver), SDL_VERSIONNUM_MICRO(ver));
    PlatformState* s = arena_push<PlatformState>(self.arena);
    if (!s) return false;
    self.set_state(s);
    s->base_path = SDL_GetBasePath();
    // Systems reach these under their declared access; the accessors below are for code
    // outside the schedule (the frame loop, startup).
    if (!engine_publish(engine, self, "input.state", &s->input) ||
        !engine_publish(engine, self, "input.actions", &s->actions) ||
        !engine_publish(engine, self, ".window", &s->window)) return false;
    if (!s->io.init(&self.general, 4096, g_mounts.io_backend)) return false;
    // The executable's assets/ directory is the first mount, when there is one; the frame
    // loop mounts packs over it.
    char path[512];
    struct stat st;
    const char* base = s->base_path ? s->base_path : "";
    snprintf(path, sizeof path, "%sassets", base);
    if (!g_mounts.none && stat(path, &st) == 0 && S_ISDIR(st.st_mode)) s->io.mount_dir(path);
    if (g_mounts.exe_name) {
        snprintf(path, sizeof path, "%s%s.kpak", base, g_mounts.exe_name);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) s->io.mount_pack(path);
    }
    for (u32 i = 0; i < g_mounts.pack_count; ++i)
        if (!s->io.mount_pack(g_mounts.packs[i])) return false; // asked for by name: not having it is an error
    if (g_mounts.assets_dir && !s->io.mount_dir(g_mounts.assets_dir)) return false;
    engine_idle_add(engine, "platform.io", io_idle, &s->io);
    return true;
}

void platform_shutdown(Engine& e, ModuleInstance& self) {
    PlatformState* s = self.state_as<PlatformState>();
    platform_audio_close(e); // whoever opened it, it ends with the module
    if (s) {
        s->window.close(); // the service ends with the module, whoever opened it
        input_gamepads_close(s->input);
        s->io.free();
        if (s->pref_path) SDL_free(s->pref_path);
    }
    SDL_Quit();
}

// Phase Input: sample the raw key state once per tick for the simulation.
void input_sample(TickContext& ctx) {
    PlatformState& s = ctx.state_as<PlatformState>();
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

bool platform_video_up(Engine&) { return platform_sdl_video_up(); }

void platform_configure_mounts(const PlatformMounts& mounts) { g_mounts = mounts; }

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

const char* platform_pref_path(Engine& e, const char* org, const char* app) {
    PlatformState* s = state(e);
    if (!s) return "";
    if (!s->pref_path) {
        s->pref_path = SDL_GetPrefPath(org ? org : "korvid", app ? app : "korvid");
        if (!s->pref_path) { KV_WARN("platform", "no preference path: %s", SDL_GetError()); return ""; }
        KV_INFO("platform", "preference path %s", s->pref_path);
    }
    return s->pref_path;
}

const char* platform_base_path(Engine& e) {
    PlatformState* s = state(e);
    return s && s->base_path ? s->base_path : "";
}

} // namespace kv
