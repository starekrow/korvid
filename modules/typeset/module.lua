-- typeset: paragraphs of text in a font (docs/PRDs/Krome-Slice-3-PRD.md, "Paragraphs"):
-- line breaking, alignment, line height, measuring and fitting; the caret, hit and
-- selection geometry an editor needs; the edit buffer. Krome, text and ui link it. It
-- draws nothing, and links nothing that does: fonts are assets, and a font's pages reach
-- the renderer only through asset_render, which typeset does not depend on.
module "typeset" {
  deps = { "core", "math", "asset" },
}
