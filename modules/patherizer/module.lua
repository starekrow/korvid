-- Inner engine: paths, filled and stroked, rasterized on the CPU at the size they are drawn
-- (docs/PRDs/Krome-Slice-5-PRD.md). Shapes are plain data; drawing is integer coverage and the
-- engine's math, so a shape draws the same bytes on every platform. Nothing here knows Krome:
-- the svg module builds shapes from SVG files, the asset module reads .kshape, kvscript and
-- games call it directly. Its tests read resvg's reference pictures (PNG), hence `image`.
module "patherizer" {
  deps      = { "core", "math" },
  test_deps = { "image" },
  test_files = { "modules/patherizer/tests/references", "modules/krome/assets/icons", "modules/krome/icons/NAMES" },
}
