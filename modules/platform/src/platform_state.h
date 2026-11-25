// Korvid platform: the module's state. The module's own: everything in it is reached
// through a function in module.h or io.h (platform_io, platform_base_dir, platform_wait).
#pragma once
#include "korvid/platform/module.h"

namespace kv {

struct PlatformState {
    IoQueue io;                               // the process's read queue, pumped by platform_pump
    char    base_path[KV_PLATFORM_PATH_MAX];  // the executable's directory, trailing separator
    char    pref_path[KV_PLATFORM_PATH_MAX];  // the per-user data directory once asked for (UserDir::Data)
    char    settings_path[KV_PLATFORM_PATH_MAX]; // UserDir::Settings, once asked for
    char    cache_path[KV_PLATFORM_PATH_MAX];    // UserDir::Cache, once asked for
    struct SettingsStore* settings = nullptr;    // platform_settings, once asked for
    struct  Pump { PlatformPumpFn fn; void* user; } pumps[KV_PLATFORM_PUMP_MAX];
    u32     pump_count = 0;
    // platform_wait's: the window's wait if one is set, else a condition of its own.
    PlatformWaiter waiter;
    PlatformWaits  waits;
    u32     woken = 0;      // atomic: platform_wake since the last wait took it
    OsMutex wait_lock;
    OsCond  wait_cond;
    struct WakeWatch* watch = nullptr; // platform_wake_set's thread and its sockets, once one is given
    // Alerts (alert.h): the sources the last Input phase saw on entities, which the waker
    // states for a waiting loop; the waker is added when the first is seen.
    Array<Handle> alert_sources;
    bool    alert_waker = false;
};

} // namespace kv
