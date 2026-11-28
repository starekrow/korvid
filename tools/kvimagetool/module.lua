-- kvimagetool, the image tool: compares screenshots picture by picture (two files, or two directories
-- by name) within a tolerance, writes amplified differences, and turns screenshots into
-- PNGs to keep as references. An engine tool, as kpack is: its verbs are CLI commands, its
-- work a system over the I/O queue. Built for the host, as bin/kvimagetool (host/bin/kvimagetool
-- when the host build is not the target's).
module "kvimagetool" {
  output        = "exe",
  platforms     = { "host" },
  publish       = true, -- on the path through kormake link tools
  deps          = { "core", "platform", "cli", "tool", "inflate", "image" },
  engine_module = true,
}
