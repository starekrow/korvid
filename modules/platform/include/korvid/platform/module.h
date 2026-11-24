// Korvid platform: module record, state and services. The operating system as a tool or a
// game sees it with no display and no devices: the file read queue and its mounts, the
// executable's and the user's directories, and sockets (net.h). No SDL and no external
// library; the window, input and gamepads are platform_hid's, the audio device
// platform_audio's, so a tool links this and nothing heavy.
// The module has one system, "platform.io_idle" in the Work cycle (docs/design/cycles.md),
// which serves the I/O queue's idle reads and whatever the frame's pumps held back under
// their byte budget, in the time the cycle has left.
#pragma once
#include "korvid/core/module.h"
#include "korvid/platform/io.h"

namespace kv {

struct Engine;

KV_MODULE_DECLARE(platform_module);

constexpr u32 KV_PLATFORM_PATH_MAX  = 512;
constexpr u32 KV_PLATFORM_PUMP_MAX  = 8;

// Called from platform_pump before the I/O queue's own pump: a device module (the window)
// turns its OS events into bus events there.
typedef void (*PlatformPumpFn)(Engine& e, void* user);

// The module's state: for code outside the schedule (the frame loop, a tool).
struct PlatformState {
    IoQueue io;                               // the process's read queue, pumped by platform_pump
    char    base_path[KV_PLATFORM_PATH_MAX];  // the executable's directory, trailing separator
    char    pref_path[KV_PLATFORM_PATH_MAX];  // the per-user data directory once asked for (UserDir::Data)
    char    settings_path[KV_PLATFORM_PATH_MAX]; // UserDir::Settings, once asked for
    char    cache_path[KV_PLATFORM_PATH_MAX];    // UserDir::Cache, once asked for
    struct SettingsStore* settings = nullptr;    // platform_settings, once asked for
    struct  Pump { PlatformPumpFn fn; void* user; } pumps[KV_PLATFORM_PUMP_MAX];
    u32     pump_count = 0;
};

// Where read_asset looks, set before engine_init (the frame loop's --pack and --assets).
// At startup the platform mounts, in this order, so later ones win: <exe dir>/assets/ if
// it exists; <exe dir>/<exe name>.kpak if it exists; each of `packs`; `assets_dir`.
struct PlatformMounts {
    const char* exe_name = nullptr;     // for the default pack's name; null: none looked for
    const char* packs[8] = {};
    u32         pack_count = 0;
    const char* assets_dir = nullptr;
    bool        none = false;           // mount nothing, not even <exe dir>/assets/ (a tool)
    IoBackend   io_backend = IoBackend::Auto; // the frame loop's --io sync|ring
};
void platform_configure_mounts(const PlatformMounts& mounts);

// Once per frame: the pump hooks run (the window's events become bus events and raw
// input), then the I/O queue reaps its completions.
void platform_pump(Engine& e);
// Adds or removes a pump hook. A module adds one at startup and removes it at shutdown.
bool platform_pump_add(Engine& e, PlatformPumpFn fn, void* user);
void platform_pump_remove(Engine& e, PlatformPumpFn fn, void* user);

} // namespace kv
