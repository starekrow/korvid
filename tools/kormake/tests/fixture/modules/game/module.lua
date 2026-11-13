-- An executable on an interface (the bound backend links too), packed, with tests.
module "game" {
  output   = "exe",
  deps     = { "gfx" },
  tools    = { "packer" },
  licenses = { "LICENSE.txt" },
}
