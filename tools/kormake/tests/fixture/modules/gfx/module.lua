-- An interface that compiles nothing but stages a tool's outputs (shaders).
module "gfx" {
  output = "headers",
  deps   = { "base" },
  tools  = { compiler = { install = "shaders" } },
}
