-- The packer: builds a .kpak from a directory or a file list, and lists, verifies,
-- extracts and repacks any zip. An engine tool: its verbs are CLI commands, its work a
-- system over the I/O queue. The "pack" rule runs once per executable that names this
-- tool, over that executable's cooked assets (the tree goldberg wrote, %{assets}). Built for the host whatever the target, as
-- bin/kpack (host/bin/kpack when the host build is not the target's).
module "kpack" {
  output        = "exe",
  platforms     = { "host" },
  deps          = { "core", "platform", "cli", "tool", "pack", "inflate" },
  engine_module = true,
  exports       = {
    rule "pack" {
      per     = "exe",
      inputs  = "assets",
      outputs = "%{exe}.kpak",
      configs = {}, -- by name in every config: goldberg cooks loose, in ship too, until packs are made from its graph
      command = "%{tool} pack %{out} %{assets}",
    },
  },
}
