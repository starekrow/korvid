#include "korvid/platform/module.h"
#include "korvid/core/format.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "os_paths.h"
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
    if (!os_bundle_archive() && !os_base_path(s->base_path, sizeof s->base_path)) {
        KV_WARN("platform", "the executable's directory is unknown; assets resolve from the working directory");
        s->base_path[0] = 0;
    }
    if (!s->io.init(&self.general, 4096, g_mounts.io_backend)) return false;
    // A program whose files are inside an archive (an Android app's package): what would
    // be assets/ beside the executable is the archive's entries under <prefix>assets/, and
    // <exe>.kpak a stored entry mounted where it lies. There is no directory to look in.
    if (const char* archive = os_bundle_archive()) {
        s->base_path[0] = 0;
        char name[KV_PLATFORM_PATH_MAX];
        if (!g_mounts.none) {
            format(name, sizeof name, "%sassets/", kBundlePrefix);
            s->io.mount_pack(archive, name);
        }
        u64 at = 0, size = 0;
        if (g_mounts.exe_name) {
            format(name, sizeof name, "%s%s.kpak", kBundlePrefix, g_mounts.exe_name);
            if (io_archive_entry(archive, name, &at, &size)) s->io.mount_pack_at(archive, at, size);
        }
        for (u32 i = 0; i < g_mounts.pack_count; ++i)
            if (!s->io.mount_pack(g_mounts.packs[i])) return false;
        if (g_mounts.assets_dir && !s->io.mount_dir(g_mounts.assets_dir)) return false;
        engine_idle_add(engine, "platform.io", io_idle, &s->io);
        return true;
    }
    // What the program reads is the first mount, when there is one; the frame loop mounts
    // packs over it. In a build tree that is the output tree goldberg cooked for this
    // executable, named by <exe>.assets beside it (one line: the directory; kormake writes
    // the file). A packaged program has no such file and reads assets/ beside itself.
    char path[KV_PLATFORM_PATH_MAX * 2];
    if (!g_mounts.none && platform_assets_dir(path, sizeof path)) s->io.mount_dir(path);
    if (g_mounts.exe_name) {
        format(path, sizeof path, "%s%s.kpak", s->base_path, g_mounts.exe_name);
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
    io_bundle_close();
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

bool platform_exe_dir(char* out, u32 cap) { return os_base_path(out, cap); }

bool platform_assets_dir(char* out, u32 cap) {
    char base[KV_PLATFORM_PATH_MAX], exe[128], path[KV_PLATFORM_PATH_MAX + 160];
    if (!os_base_path(base, sizeof base)) return false;
    if (os_exe_name(exe, sizeof exe)) {
        format(path, sizeof path, "%s%s.assets", base, exe);
        IoBytes named;
        if (os_path_kind(path) == OsPathKind::File && io_read_now(path, &named)) {
            usize n = 0;
            while (n < named.size && n + 1 < cap && named.data[n] != '\n' && named.data[n] != '\r') { out[n] = (char)named.data[n]; ++n; }
            out[n] = 0;
            named.free();
            if (n && os_path_kind(out) == OsPathKind::Dir) return true;
            KV_WARN("platform", "%s names '%s', which is not a directory: the assets were not cooked (build the program, or run goldberg cook)", path, out);
            return false;
        }
    }
    int n = format(out, cap, "%sassets", base);
    return n > 0 && (u32)n < cap && os_path_kind(out) == OsPathKind::Dir;
}


} // namespace kv
