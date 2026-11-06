// Korvid platform: module record, state and services. Startup initializes SDL's video
// and event subsystems. The module owns the window and the input state: the window is a
// platform service opened on request and closed at module shutdown, never by the caller;
// "input.sample" (phase Input) samples the keys into "input.state" once per tick.
#pragma once
#include "korvid/core/module.h"
#include "korvid/platform/host_window.h"
#include "korvid/platform/input.h"

namespace kv {

struct Engine;

KV_MODULE_DECLARE(platform_module);

struct PlatformState {
    Input      input;
    HostWindow window; // one window in slice 1
};

// Opens the window. Null if one is already open or SDL refused. Owned by the module.
HostWindow* platform_open_window(Engine& e, const WindowDesc& desc);
// The open window, or null.
HostWindow* platform_window(Engine& e);
// The module's input, or null if the module is not started.
Input*      platform_input(Engine& e);
// Once per frame: OS events become bus events and raw input state.
void        platform_pump(Engine& e);

} // namespace kv
