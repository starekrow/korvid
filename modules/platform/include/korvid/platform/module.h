// Korvid platform: module record, state and services. Startup initializes SDL's video,
// event and gamepad subsystems. The module owns the window and the input state: the window is a
// platform service opened on request and closed at module shutdown, never by the caller;
// "input.sample" (phase Input) samples the keys into "input.state" once per tick. Both are
// published resources: a system declares "input.state" or "platform.window" in its sets
// and reaches them with ctx.resource<const Input>() and ctx.resource<const HostWindow>().
// The module registers one idle task, "platform.io", which serves the I/O queue's idle
// reads and whatever the frame's pumps held back under their byte budget.
#pragma once
#include "korvid/core/module.h"
#include "korvid/platform/host_window.h"
#include "korvid/platform/input.h"
#include "korvid/platform/io.h"

namespace kv {

struct Engine;

KV_MODULE_DECLARE(platform_module);

struct PlatformState {
    Input       input;
    HostWindow  window; // one window in slice 1
    IoQueue     io;
    const char* base_path = nullptr;
};

// Opens the window. Null if one is already open or SDL refused. Owned by the module.
HostWindow* platform_open_window(Engine& e, const WindowDesc& desc);
// The open window, or null. For code outside the schedule; systems use the resource.
HostWindow* platform_window(Engine& e);
// The module's input, or null if the module is not started. Likewise.
Input*      platform_input(Engine& e);
// Once per frame: OS events become bus events and raw input state; the I/O queue reaps
// its completions.
void        platform_pump(Engine& e);

} // namespace kv
