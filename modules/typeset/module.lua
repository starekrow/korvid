-- typeset: paragraphs of text in a font (docs/PRDs/Krome-Slice-3-PRD.md, "Paragraphs"):
-- line breaking, alignment, line height, measuring and fitting; the caret, hit and
-- selection geometry an editor needs; the edit buffer. Krome, text and ui link it. It
-- draws nothing itself, but fonts are assets and a font asset is an atlas texture, so
-- asset brings render with it: typeset without a renderer wants font metrics split from
-- atlases (docs/design/principles-review.md, 14), which nothing has needed yet.
module "typeset" {
  deps = { "core", "math", "asset" },
}
