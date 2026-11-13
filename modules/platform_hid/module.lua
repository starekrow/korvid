-- The window, the keyboard, the mouse and gamepads, and the input action map over them:
-- what a person at the machine touches and looks at. The window and keyboard backend is
-- per platform: SDL3 on Linux (src/sdl3/), native Win32 on Windows (src/win32/), UIKit on iOS (src/ios/), a NativeActivity on
-- Android (src/android/). Gamepads
-- come from SDL3 everywhere (src/sdl3_pads/). Nothing outside this module and
-- platform_audio sees either.
module "platform_hid" {
  deps          = { "core", "math", "platform" },
  external      = { sdl3 = { ">=3.2", platforms = { "linux", "windows", "macos" } }, uikit = { ">=1", platforms = { "ios", "iossim" } },
                    android = { ">=33", platforms = { "android", "androidemu" } } },
  engine_module = true,
}
