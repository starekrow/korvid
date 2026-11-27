-- A sample program on a private path (modulepath { "samples", private = true }): built
-- from inside the fixture, hidden from the project beside it.
module "demo" {
  output = "exe",
  deps   = { "base" },
}
