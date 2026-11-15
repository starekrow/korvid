-- kim: Korvid Interface Markup, the text form of a Krome screen (docs/PRDs/Krome-Slice-2-PRD.md).
-- The parser and the compiler from .kim text to .kimb, the document as canonical KBOR.
-- It knows the grammar, not the meaning: words, styles and tokens are resolved by Krome
-- when it loads a .kimb. Core only, so goldberg, kimc and a game that reads text can link
-- it without Krome; Krome itself does not.
module "kim" {
  deps = { "core" },
}
