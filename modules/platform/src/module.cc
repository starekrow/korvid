#include "korvid/platform/module.h"
#include "korvid/platform/settings.h"
#include "korvid/core/format.h"
#include "korvid/core/engine.h"
#include "korvid/core/phase.h"
#include "korvid/core/log.h"
#include "korvid/core/atomic.h"
#include "korvid/core/time.h"
#include "os_paths.h"
#include "os_file.h"
#include "platform_state.h"
#include "wake_watch.h"

namespace kv {

namespace {

PlatformMounts g_mounts; // what platform_configure_mounts said, for the next startup

// The Work cycle (docs/design/cycles.md): in the time the cycle has, the queue admits what
// the frame's pumps held back, idle reads included, sized to that time; more while
// something went in.
void io_idle(TickContext& ctx) {
    PlatformState& s = ctx.state_as<PlatformState>();
    f64 left = ctx.time_left();
    if (left <= 0.0) return;
    if (s.io.pump_idle(s.io.idle_bytes_for(left))) ctx.more();
}

bool platform_startup(Engine&, ModuleInstance& self) {
    PlatformState* s = arena_push<PlatformState>(self.arena);
    if (!s) return false;
    self.set_state(s);
    os_mutex_init(&s->wait_lock);
    os_cond_init(&s->wait_cond);
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
    return true;
}

void platform_shutdown(Engine&, ModuleInstance& self) {
    PlatformState* s = self.state_as<PlatformState>();
    if (!s) return;
    if (s->pump_count) KV_WARN("platform", "%u pump hooks still added at shutdown", s->pump_count);
    wake_watch_destroy(s->watch); // the thread stopped before what it wakes goes
    s->watch = nullptr;
    settings_close(s->settings); // what was set, written
    s->settings = nullptr;
    s->io.free();
    io_bundle_close();
    if (s->waiter.wait) KV_WARN("platform", "a waiter still set at shutdown");
    os_cond_destroy(&s->wait_cond);
    os_mutex_destroy(&s->wait_lock);
}

constinit const SystemDesc k_systems[] = {
    {.name = "platform.io_idle", .phase = Phase::Work, .fn = io_idle, .main_thread = true}, // the full name: nothing to intern
};

} // namespace

KV_MODULE_DEFINE(platform_module) = {
    .name     = "platform",
    .systems  = k_systems,
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

PlatformWoken platform_wait(Engine& e, f64 seconds) {
    PlatformState* s = state(e);
    if (!s) { if (seconds > 0.0) time_sleep(seconds); return PlatformWoken::Time; }
    if (seconds <= 0.0) return PlatformWoken::Time; // no wait: a wake stays for the next
    print_flush_lines(); // nothing printed sits in a buffer while the loop sleeps
    ++s->waits.waits;
    if (s->watch) wake_watch_rearm(s->watch); // what woke the last wait has been read by now
    // A wake that came before the wait began ends it at once.
    if (atomic_exchange(&s->woken, 0u, MO_ACQ_REL)) { ++s->waits.woken; return PlatformWoken::Wake; }
    int ended = s->waiter.wait ? s->waiter.wait(s->waiter.user, seconds) : -1;
    if (ended < 0) {
        // No window to wait on: the condition, till woken or the time is up.
        f64 until = time_now() + seconds;
        os_mutex_lock(&s->wait_lock);
        for (;;) {
            if (atomic_load(&s->woken, MO_ACQUIRE)) break;
            f64 left = until - time_now();
            if (left <= 0.0) break;
            os_cond_timed_wait(&s->wait_cond, &s->wait_lock, left);
        }
        os_mutex_unlock(&s->wait_lock);
        ended = 0;
    }
    if (atomic_exchange(&s->woken, 0u, MO_ACQ_REL)) { ++s->waits.woken; return PlatformWoken::Wake; }
    if (ended > 0) { ++s->waits.window; return PlatformWoken::Window; }
    ++s->waits.timed;
    return PlatformWoken::Time;
}

void platform_wake(Engine& e) {
    if (PlatformState* s = state(e)) platform_wake_state(*s);
}

void platform_wake_state(PlatformState& st) {
    PlatformState* s = &st;
    atomic_store(&s->woken, 1u, MO_RELEASE);
    // Under the lock, so a waiter between its check and its wait cannot miss the signal,
    // and the window's waiter is not cleared while it is called.
    os_mutex_lock(&s->wait_lock);
    os_cond_signal(&s->wait_cond);
    if (s->waiter.wake) s->waiter.wake(s->waiter.user);
    os_mutex_unlock(&s->wait_lock);
}

void platform_set_waiter(Engine& e, const PlatformWaiter* w) {
    PlatformState* s = state(e);
    if (!s) return;
    os_mutex_lock(&s->wait_lock);
    s->waiter = w ? *w : PlatformWaiter{};
    os_mutex_unlock(&s->wait_lock);
}

const char* khandle_kind_name(KHandleKind k) {
    switch (k) {
    case KHandleKind::None:   return "none";
    case KHandleKind::File:   return "file";
    case KHandleKind::Socket: return "socket";
    case KHandleKind::Pipe:   return "pipe";
    case KHandleKind::Notify: return "notify";
    case KHandleKind::Process: return "process";
    }
    return "?";
}

bool platform_watches_kind(Engine&, KHandleKind kind) { return wake_watch_supported() && wake_watch_takes(kind); }

bool platform_wake_set(Engine& e, const void* owner, const KHandle* handles, u32 n) {
    PlatformState* s = state(e);
    if (!s || !wake_watch_supported()) return false;
    // Only what this OS can watch goes to the thread. A handle of another kind is the
    // caller's mistake (platform_watches_kind says beforehand): named, left out, and the
    // answer false, so the owner asks to be woken by the clock as it would with no watch.
    KHandle taken[64];
    u32 count = 0;
    bool all = true;
    for (u32 i = 0; i < n; ++i) {
        if (!handles[i].is_open()) continue;
        if (!wake_watch_takes(handles[i].kind())) {
            KV_ERROR("platform", "platform_wake_set: a %s handle cannot wake a waiting loop on this OS; it is not watched",
                     khandle_kind_name(handles[i].kind()));
            all = false;
            continue;
        }
        if (count == KV_COUNTOF(taken)) { all = false; continue; }
        taken[count++] = handles[i];
    }
    if (!s->watch) {
        if (!count) return all; // nothing to forget, and no thread for nothing
        s->watch = wake_watch_create(*s);
        if (!s->watch) return false;
    }
    return wake_watch_set(s->watch, owner, taken, count) && all;
}

PlatformWaits platform_waits(Engine& e) {
    PlatformState* s = state(e);
    return s ? s->waits : PlatformWaits{};
}

IoQueue* platform_io(Engine& e) {
    PlatformState* s = state(e);
    return s ? &s->io : nullptr;
}

const char* platform_user_dir(Engine& e, UserDir which, const char* org, const char* app) {
    PlatformState* s = state(e);
    if (!s) return "";
    char* out = which == UserDir::Settings ? s->settings_path : which == UserDir::Cache ? s->cache_path : s->pref_path;
    if (!out[0]) {
        if (!os_user_dir(which, org ? org : "korvid", app ? app : "korvid", out, KV_PLATFORM_PATH_MAX)) {
            out[0] = 0;
            return "";
        }
        KV_INFO("platform", "%s directory %s", which == UserDir::Settings ? "settings" : which == UserDir::Cache ? "cache" : "data", out);
    }
    return out;
}


const char* platform_base_dir(Engine& e) {
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
        if (os_path_kind(path) == OsPathKind::File && io_read_file(path, &named)) {
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
