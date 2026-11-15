-- kimc: the command line over the kim module (docs/PRDs/Krome-Slice-2-PRD.md). .kim text
-- to .kimb, and (slice 2, milestone 2) checking a file against its preludes. An engine
-- tool, as kbor is: its verbs are CLI commands, its work a system over the I/O queue.
-- Built for the host, as bin/kimc.
module "kimc" {
  output        = "exe",
  platforms     = { "host" },
  deps          = { "core", "platform", "cli", "tool", "kim" },
  engine_module = true,
}
