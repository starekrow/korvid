// Korvid platform_hid: what module.cc asks of the backend. One backend per platform, chosen
// by the toolchain's source directories: src/sdl3/ (SDL3, on Linux) or src/win32/ (Win32).
// Each also implements HostWindow, the keymap and the gamepad entry points in input.h.
#pragma once

namespace kv {

// At module startup and shutdown: headless, so a test with this module runs anywhere.
bool hid_backend_startup();
void hid_backend_shutdown();
// The display and gamepads, once; true when up. Brought up by the first window.
bool hid_video_up();
// Their release at shutdown, if they came up.
void hid_video_down();

} // namespace kv
