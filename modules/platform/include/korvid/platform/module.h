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
#include "korvid/core/os_thread.h"
#include "korvid/platform/net.h"

namespace kv {

struct Engine;

KV_MODULE_DECLARE(platform_module);

constexpr u32 KV_PLATFORM_PATH_MAX  = 512;
constexpr u32 KV_PLATFORM_PUMP_MAX  = 8;

// Called from platform_pump before the I/O queue's own pump: a device module (the window)
// turns its OS events into bus events there.
typedef void (*PlatformPumpFn)(Engine& e, void* user);

// A device module's wait (the window's), for platform_wait. `wait` blocks till its OS has
// something for the next pump, `wake` is called, or `seconds` pass, and takes nothing off
// any queue: 1 when it ended early, 0 when the time was up, -1 when it cannot wait now
// (no window open), and the platform waits on its own. `wake` ends a wait under way or
// the next to begin, from any thread.
struct PlatformWaiter {
    int  (*wait)(void* user, f64 seconds) = nullptr;
    void (*wake)(void* user) = nullptr;
    void* user = nullptr;
};

// What ended each platform_wait, counted from the start.
struct PlatformWaits {
    u64 waits = 0;   // every platform_wait given time to wait
    u64 window = 0;  // the window had something
    u64 woken = 0;   // platform_wake
    u64 timed = 0;   // the time was up
};

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
    // platform_wait's: the window's wait if one is set, else a condition of its own.
    PlatformWaiter waiter;
    PlatformWaits  waits;
    u32     woken = 0;      // atomic: platform_wake since the last wait took it
    OsMutex wait_lock;
    OsCond  wait_cond;
    struct WakeWatch* watch = nullptr; // platform_wake_set's thread and its sockets, once one is given
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

// Blocks the calling thread till the window has something for the pump, platform_wake is
// called, or `seconds` pass, whichever is first (KR26). It takes nothing off any queue:
// the pump after it reads what came. With no window it waits on a condition. A frame
// loop with nothing to do waits here between passes. No time (0 or less) is no wait: it
// returns Time at once and leaves a wake for the next.
enum class PlatformWoken : u8 { Time, Window, Wake };
PlatformWoken platform_wait(Engine& e, f64 seconds);
// Ends the wait under way, or the next to begin. Any thread; not a signal handler.
void platform_wake(Engine& e);
// The window's wait: platform_hid sets it at startup and clears it (null) at shutdown.
void platform_set_waiter(Engine& e, const PlatformWaiter* w);
// What ended the waits so far: for a loop's last line, and a measurement.
PlatformWaits platform_waits(Engine& e);

// The sockets `owner` wants the loop woken by (KR26): a watched waker's (owner: the waker's
// user, engine_add_watched_waker), the CLI's. The set replaces what the owner said before;
// none (n 0) forgets it. The platform watches every owner's from one thread of its own,
// blocked in poll (WSAPoll on Windows), started when the first socket is given, and calls
// platform_wake when one can be read. A socket that woke it is watched again at the next
// platform_wait (after the pass that should have read it), so one nobody drains does not
// spin the thread. False where the platform cannot watch (platform_watches) or the sets
// pass 64 sockets: the owner then asks to be woken soon (engine_want_wake_at), as a poll would.
// A handle of a kind this OS cannot watch (platform_watches_kind: on Windows anything but a
// socket, everywhere a file on a disk) is an error, logged with its kind: it is left out,
// the rest are watched, and the answer is false.
bool platform_wake_set(Engine& e, const void* owner, const KHandle* handles, u32 n);
// Whether platform_wake_set watches here (every platform has the thread now). A waker
// picks engine_add_watched_waker or engine_add_waker by it.
bool platform_watches(Engine& e);
// Whether a handle of this kind can be given to platform_wake_set here: a socket everywhere;
// a pipe or a notify descriptor on POSIX only (Windows' watch is WSAPoll, which takes sockets).
bool platform_watches_kind(Engine& e, KHandleKind kind);

} // namespace kv
