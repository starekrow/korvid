# typeset

Paragraphs of text in a font, for everyone who draws text: Krome's `text` and `edit`, the
text module's labels and `Paragraph`, and in time `ui`'s labels. It depends on fonts
(`asset`) and nothing that draws (`docs/PRDs/Krome-Slice-3-PRD.md`, "Paragraphs").

## How it works

`typeset_paragraph(font, text, len, params, out)` decodes the text once, breaks it into
lines greedily, and places each line's glyphs with kerning, aligned against the width given
(or the widest line when there is none):

- **Breaking** after a run of spaces (which hang past the edge and are not counted in the
  line's width), after a hyphen, a dash or a slash; `\n` breaks; a word too long for its line
  breaks inside it, between code points.
- **Lines** are `line_height` apart (the font's own by default), each baseline on a whole
  pixel, centered in its line when the height is the caller's.
- **Pens** are where each glyph starts; a drawer snaps each glyph's box to whole pixels, or,
  with `subpixel`, draws the quarter-pixel copy the glyph table holds (`font_glyph_at`).
- **A mask** (`mask`, a code point) lays out that glyph for every character but `\n`, a
  password's bullets, with every offset still the text's: carets, hits and selections work
  on the real bytes.

`typeset_measure` is the same without keeping the glyphs: the widest line unwrapped, the
narrowest width the text can take (its longest unbreakable run), and its height at a width.
Krome's layout asks it both before and after it knows widths.

**Geometry** on a laid-out paragraph, for an editor: the caret's rectangle at a byte
offset, the offset nearest a point, the offset a line up or down at a remembered x, the
rectangles a selection covers, and where a page ends at a height (`typeset_fit`). Words and
code points: next and previous, and the word around an offset.

**The edit buffer** (`TypesetEdit`): UTF-8 text with a caret and an anchor (the selection
between them), changes that replace the selection, and undo and redo by groups: typing
until the caret moves, a pause of a second, or a word begun after a space; deleting
likewise; a paste, a cut, a line break and a deletion of a selection each alone. The
oldest groups go past `max_steps`. Krome's `edit` holds one for the field with the keyboard.

## Behaviour worth knowing

- **Left to right, no shaping** beyond kerning, and kerning only within the Latin set the font
  read it for.
- **Its working arrays are its own**: the output's can be in the scratch arena.
