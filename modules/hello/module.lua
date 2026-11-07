module "hello" {
  output = "exe",
  deps   = { "core", "platform", "math", "render", "asset", "sprite" },
}
