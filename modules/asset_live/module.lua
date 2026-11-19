-- asset_live: a dev build's connection to a running `goldberg watch` (docs/PRDs/Krome-Slice-6-PRD.md,
-- "A program's connection to goldberg"). It finds the watch through the output tree the program
-- reads (<tree>/.goldberg-port), says who it is, and reloads each output the watch says changed
-- (asset_reload). Optional: a program that does not link it does not reload live. Off in ship.
module "asset_live" {
  deps          = { "core", "platform", "asset" },
  engine_module = true,
}
