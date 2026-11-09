#include "korvid/platform/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "os_paths.h"
#include <stdio.h>
#include "os_file.h"

namespace kv {

namespace {

PlatformMounts g_mounts; // what platform_configure_mounts said, for the next startup

// Idle time: the queue admits what the frame's pumps held back, idle reads included.
IdleResult io_idle(Engine&, f64 seconds, void* user) {
    IoQueue& q = *(IoQueue*)user;
    return q.pump_idle(q.idle_bytes_for(seconds)) ? IdleResult::More : IdleResult::Nothing;
}

bool platform_startup(Engine& engine, ModuleInstance& self) {
    PlatformState* s = arena_push<PlatformState>(self.arena);
    if (!s) return false;
    self.set_state(s);
    if (!os_base_path(s->base_path, sizeof s->base_path)) {
        KV_WARN("platform", "the executable's directory is unknown; assets resolve from the working directory");
        s->base_path[0] = 0;
    }
    if (!s->io.init(&self.general, 4096, g_mounts.io_backend)) return false;
    // The executable's assets/ directory is the first mount, when there is one; the frame
    // loop mounts packs over it.
    char path[KV_PLATFORM_PATH_MAX + 64];
    snprintf(path, sizeof path, "%sassets", s->base_path);
    if (!g_mounts.none && os_path_kind(path) == OsPathKind::Dir) s->io.mount_dir(path);
    if (g_mounts.exe_name) {
        snprintf(path, sizeof path, "%s%s.kpak", s->base_path, g_mounts.exe_name);
        if (os_path_kind(path) == OsPathKind::File) s->io.mount_pack(path);
    }
    for (u32 i = 0; i < g_mounts.pack_count; ++i)
        if (!s->io.mount_pack(g_mounts.packs[i])) return false; // asked for by name: not having it is an error
    if (g_mounts.assets_dir && !s->io.mount_dir(g_mounts.assets_dir)) return false;
    engine_idle_add(engine, "platform.io", io_idle, &s->io);
    return true;
}

void platform_shutdown(Engine&, ModuleInstance& self) {
    PlatformState* s = self.state_as<PlatformState>();
    if (!s) return;
    if (s->pump_count) KV_WARN("platform", "%u pump hooks still added at shutdown", s->pump_count);
    s->io.free();
}

} // namespace

KV_MODULE_DEFINE(platform_module) = {
    .name     = "platform",
    .startup  = platform_startup,
    .shutdown = platform_shutdown,
    .memory   = {.reserve = 64 * 1024},
};

static PlatformState* state(Engine& e) {
    return engine_module_state_as<PlatformState>(e, "platform");
}

void platform_configure_mounts(const PlatformMounts& mounts) { g_mounts = mounts; }

void platform_pump(Engine& e) {
    PlatformState* s = state(e);
    if (!s) return;
    for (u32 i = 0; i < s->pump_count; ++i) s->pumps[i].fn(e, s->pumps[i].user);
    s->io.pump();
}

bool platform_pump_add(Engine& e, PlatformPumpFn fn, void* user) {
    PlatformState* s = state(e);
    if (!s) { KV_ERROR("platform", "module not started"); return false; }
    if (s->pump_count == KV_PLATFORM_PUMP_MAX) { KV_ERROR("platform", "too many pump hooks"); return false; }
    s->pumps[s->pump_count++] = {fn, user};
    return true;
}

void platform_pump_remove(Engine& e, PlatformPumpFn fn, void* user) {
    PlatformState* s = state(e);
    if (!s) return;
    for (u32 i = 0; i < s->pump_count; ++i) {
        if (s->pumps[i].fn != fn || s->pumps[i].user != user) continue;
        for (u32 j = i + 1; j < s->pump_count; ++j) s->pumps[j - 1] = s->pumps[j]; // keeps the order
        --s->pump_count;
        return;
    }
}

IoQueue* platform_io(Engine& e) {
    PlatformState* s = state(e);
    return s ? &s->io : nullptr;
}

const char* platform_pref_path(Engine& e, const char* org, const char* app) {
    PlatformState* s = state(e);
    if (!s) return "";
    if (!s->pref_path[0]) {
        if (!os_pref_path(org ? org : "korvid", app ? app : "korvid", s->pref_path, sizeof s->pref_path)) {
            s->pref_path[0] = 0;
            return "";
        }
        KV_INFO("platform", "preference path %s", s->pref_path);
    }
    return s->pref_path;
}

const char* platform_base_path(Engine& e) {
    PlatformState* s = state(e);
    return s ? s->base_path : "";
}

} // namespace kv
