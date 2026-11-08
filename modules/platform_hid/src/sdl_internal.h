// Korvid platform_hid: what the SDL3 sources share.
#pragma once

namespace kv {

// SDL video and gamepads, once; true when up. The module starts with events only.
bool platform_sdl_video_up();
// Their release at shutdown, if they came up.
void platform_sdl_video_down();

} // namespace kv
