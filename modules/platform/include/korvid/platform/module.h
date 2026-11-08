// Korvid platform: module record, state and services. Startup initializes SDL's video,
// event and gamepad subsystems. The module owns the window and the input state: the window is a
// platform service opened on request and closed at module shutdown, never by the caller;
// "input.sample" (phase Input) samples the keys into "input.state" once per tick. Both are
// published resources: a system declares "input.state" or "platform.window" in its sets
// and reaches them with ctx.resource<const Input>() and ctx.resource<const HostWindow>().
// The module registers one idle task, "platform.io", which serves the I/O queue's idle
// reads and whatever the frame's pumps held back under their byte budget.
#pragma once
#include "korvid/platform/actions.h"
#include "korvid/core/module.h"
#include "korvid/platform/host_window.h"
#include "korvid/platform/input.h"
#include "korvid/platform/io.h"

namespace kv {

struct Engine;

KV_MODULE_DECLARE(platform_module);

// The module's state: for code outside the schedule (the frame loop, a tool). Systems
// reach the input and the window as resources.
struct PlatformState {
    Input       input;     // raw at frame rate, sampled per tick
    HostWindow  window;    // one window per process; closed at module shutdown
    IoQueue     io;        // the process's read queue, pumped by platform_pump
    InputActions actions;  // what the game named, from the sampled input each tick
    const char* base_path = nullptr; // the executable's directory, trailing separator; SDL's
    char*       pref_path = nullptr; // the per-user directory once asked for; SDL's, freed at shutdown
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
};
void platform_configure_mounts(const PlatformMounts& mounts);

// Opens the window. Null if one is already open or SDL refused. Owned by the module.
HostWindow* platform_open_window(Engine& e, const WindowDesc& desc);
// The platform starts headless. This brings up video and gamepads (once; they stay up
// until the platform shuts down) for whatever needs a display before a window exists,
// such as a renderer loading the Vulkan loader. Opening a window calls it. False with a
// log line when there is no video driver.
bool        platform_video_up(Engine& e);
// The open window, or null. For code outside the schedule; systems use the resource.
HostWindow* platform_window(Engine& e);
// The module's input, or null if the module is not started. Likewise.
Input*      platform_input(Engine& e);
// Once per frame: OS events become bus events and raw input state; the I/O queue reaps
// its completions.
void        platform_pump(Engine& e);

} // namespace kv
