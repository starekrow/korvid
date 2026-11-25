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
    // platform_wait's: the window's wait if one is set, else an event of its own (os_event_wait: to the
    // time asked for, where a condition's wait on Windows rounds up to 15.6 ms).
    PlatformWaiter waiter;
    PlatformWaits  waits;
    u32     woken = 0;      // atomic: platform_wake since the last wait took it
    OsMutex wait_lock;
    OsEvent wait_event;
    struct WakeWatch* watch = nullptr; // platform_wake_set's thread and its sockets, once one is given
    // What each owner is watched for (watch.cc): what it stated last (platform_watch), and
    // what stands until it closes or the owner takes it back (platform_watch_add). The
    // thread is given the two together.
    struct WatchMember { const void* owner; Handle h; bool standing; };
    Array<WatchMember> members;
    // Alerts (alert.h): the sources the last Input phase saw on entities, which the waker
    // states for a waiting loop; the waker is added when the first is seen.
    Array<Handle> alert_sources;
    bool    alert_waker = false;
};

} // namespace kv
