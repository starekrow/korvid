-- The kbor tool: JSON to KBOR and back, and normalizing a KBOR file (docs/design/kbor.md).
-- An engine tool, as kpack and kvimagetool are: its verbs are CLI commands, its work a system
-- over the I/O queue. The format itself is core's (korvid/core/kbor.h). Built for the
-- host, as bin/kbor (host/bin/kbor when the host build is not the target's).
module "kbor" {
  output        = "exe",
  platforms     = { "host" },
  publish       = true, -- on the path through kormake link tools
  deps          = { "core", "platform", "cli", "tool" },
  engine_module = true,
}
