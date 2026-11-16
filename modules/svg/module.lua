-- Inner engine: SVG files read into patherizer's shapes (docs/PRDs/Krome-Slice-5-PRD.md, "The
-- SVG tool"). goldberg links it to cook `.svg` into `.kshape` in process; kvscript has it as
-- kv.svg; a game may link it. The runtime never reads SVG.
module "svg" {
  deps      = { "core", "math", "xml", "patherizer" },
  test_deps = { "image" }, -- the tests read resvg's reference pictures
}
