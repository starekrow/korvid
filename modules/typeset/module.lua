-- typeset: paragraphs of text in a font (docs/PRDs/Krome-Slice-3-PRD.md, "Paragraphs"):
-- line breaking, alignment, line height, measuring and fitting; the caret, hit and
-- selection geometry an editor needs; the edit buffer. Krome, text and ui link it; it
-- depends on fonts (asset) and nothing that draws.
module "typeset" {
  deps = { "core", "math", "asset" },
}
