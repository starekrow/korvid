-- The render interface: the packet the outer engine writes, the API the game calls.
-- A backend (renderer-vulkan) provides it; the binding is per platform in kormake.lua.
module "render" {
  output = "headers",
  deps   = { "core", "math" },
}
