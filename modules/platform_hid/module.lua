-- The window, the keyboard, the mouse and gamepads, and the input action map over them:
-- what a person at the machine touches and looks at. SDL3 is the backend; nothing outside
-- this module and platform_audio sees it.
module "platform_hid" {
  deps          = { "core", "math", "platform" },
  external      = { sdl3 = ">=3.2" },
  engine_module = true,
}
