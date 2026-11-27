// Korvid platform_hid: what module.cc asks of the backend. One backend per platform, chosen
// by the toolchain's source directories: src/sdl3/ (SDL3, on Linux and macOS), src/win32/
// (Win32) or src/ios/ (UIKit).
// Each also implements HostWindow, the keymap and the gamepad entry points in input.h.
#pragma once

namespace kv {

struct HostWindow;

// At module startup and shutdown: headless, so a test with this module runs anywhere.
bool hid_backend_startup();
void hid_backend_shutdown();
// The display and gamepads, once; true when up. Brought up by the first window.
bool hid_video_up();
// Their release at shutdown, if they came up.
void hid_video_down();
// Whether a window without focus holds the sim (HoldFocus): on iOS an app without focus is
// not on screen, and its game waits. On the desktop, a game that wants it asks.
bool hid_backend_holds_unfocused();
// How many windows the backend opens at once: KV_WINDOWS on the desktop, 1 on a phone.
u32  hid_backend_windows();

// The module's windows, KV_WINDOWS of them by id, open or not (window_parts.cc keeps the
// pointer): the main window's pump gives each event to the one it is for. Null before the
// module starts.
void        hid_windows_set(HostWindow* list);
HostWindow* hid_window(u32 id);

// HostWindow::shape, grips and bounds keep what they are given (src/window_parts.cc), then
// ask the backend to apply what the window holds now: its shape_, grips_ and bounds_. A
// backend calls them again itself when a resize or a new scale changes what they mean.
// False where the platform cannot.
bool hid_window_shape(HostWindow& w);
bool hid_window_grips(HostWindow& w);
void hid_window_bounds(HostWindow& w);
// What close() calls on every backend: the kept shape freed, the grips and bounds gone.
void hid_window_parts_reset(HostWindow& w);

} // namespace kv
