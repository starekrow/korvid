-- The project's own packer, published as packer: in the project, the command is this
-- one, and the engine's packer is the candidate that lost.
module "mypacker" {
  output    = "exe",
  platforms = { "host" },
  publish   = "packer",
}
