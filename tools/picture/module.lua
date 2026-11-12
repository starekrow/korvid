-- The picture tool: compares screenshots picture by picture (two files, or two directories
-- by name) within a tolerance, writes amplified differences, and turns screenshots into
-- PNGs to keep as references. An engine tool, as kpack is: its verbs are CLI commands, its
-- work a system over the I/O queue. Built for the host, as bin/picture (host/bin/picture
-- when the host build is not the target's).
module "tool_picture" {
  output        = "exe",
  exe           = "picture",
  platforms     = { "host" },
  deps          = { "core", "platform", "cli", "tool", "inflate", "image" },
  engine_module = true,
}
