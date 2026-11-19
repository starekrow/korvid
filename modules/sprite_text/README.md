# sprite_text

Words on screen, in the engine's own shape. A `Text` component on any kind with a
`Transform2` is laid out as glyph sprites: rows of the kind `sprite_text.glyph`, children of the
label through the scene module, one per visible character, drawn from a font asset's
atlas by the same GPU path as everything else. Changing the string is one write; the layout notices and redoes the glyphs.

## How it works

**Fonts are assets** (`korvid/asset/font.h`, in the asset module). `asset_load_font(e,
"dejavu/DejaVuSans.ttf", 28)` returns a handle at once; the record owns a renderer texture
(the atlas) and a material for it (alpha blend, white, so the tint is the color). The
file reads through the I/O queue and `asset.pump_fonts` rasterizes the font's character
set (by default ASCII, the Latin-1 and Latin Extended-A blocks, common punctuation and
U+FFFD: about 360 glyphs) with stb_truetype into the smallest atlas of a fixed ladder that
fits (under 10 ms for 28 px), uploads it, and fills the glyph table (uv, size, offset from
the pen, advance) and the kerning pairs. `asset_load_font(e, path, FontDesc{px, ranges})`
takes another set. `AssetReady` follows. Before that, `font_glyph`
answers null for everything and text lays out as nothing.

**The component.** `Text { font, str[64], scale, tint, layer, space, align, shadow, shadow_offset, wrap, line_height, subpixel }`. The
label's `Transform2` is the pen's start on the baseline; `Left` puts the first glyph
there, `Center` and `Right` shift the line. Strings are UTF-8, 64 bytes with the NUL;
`sptext_set` truncates at a character boundary.

**Lines** (Krome slice 3). `wrap` (the label's units) breaks a label into lines through
`typeset` (`modules/typeset`), as does a `\n`; they go down from the first baseline,
`line_height` apart (the font's by default), aligned as one line is: `Left` from the pen,
`Center` and `Right` about it, inside the wrap's width. `subpixel` keeps the pens to a
quarter pixel. A label of one line with none of these is laid out as it always was.

**Paragraph.** For more than 64 bytes: a `Paragraph { string, shown }` beside the `Text`
(the module's own `sprite_text.label` has one) draws a string the module keeps
(`sptext_paragraph_set(e, para, utf8)`, any length, freed with `sptext_paragraph_free`) in the
Text's font, look and wrap, its first `shown` code points (all by default), so it can be
revealed a character at a time without reflowing. `sptext_paragraph_fit(e, text, para,
height)` says how many code points fill a height, for a game that shows a page at a time.
Sim code writes the strings, so a replay writes the same ones; the handle is what is hashed.

**The system.** `sprite_text.layout` runs in the Frame cycle (`docs/design/cycles.md`), once
before each frame drawn whatever the sim does, reading `*.Text` and `*.Transform2` and
writing `sprite_text.glyph`. Per label slot it keeps the entity and a hash of
everything that shapes the glyphs (the string, the font and whether it is ready, scale,
tint, layer, space, alignment). When the hash changes it destroys the label's glyphs
(every glyph whose `Parent` is the label) and spawns new ones from `sptext_layout`, each
with the font's material, the glyph's uv and size times scale, and the label's tint,
layer and space, placed by `Local2` relative to the label and placed in the world at once
(the label's transform composed with it, as the scene would), so the frame that shows new
text shows it in place. From the next tick on the scene module keeps the glyphs with their
label (`scene.compose` reads `*.Local2`), and the extract draws them. A label that dies leaves its glyphs as orphans, which the scene
module destroys at the end of the same tick.

`sprite_text.glyph` is a **cosmetic kind**: never part of the state hash. The frame loop's
overlay and a game's HUD can show anything without touching determinism. `sprite_text.label`
is sim state like any other kind, so a game's labels are hashed with the rest.

## Using it

```cpp
// A kind of your own with a Text, or the module's sprite_text.label:
Text t;
t.font  = g.font;                       // asset_load_font(engine, KV_DEJAVU_SANS, 28) at startup
t.space  = SpriteSpace::Screen;         // window pixels, +y up, from the anchor
t.anchor = ScreenAnchor::Top;           // the top edge's middle, on any window
t.align  = TextAlign::Center;
t.layer  = 50;
sptext_set(t, "SCORE %d", score);
labels->spawn(transform2_at(point2{0, -60}), t); // 60 px below the top edge

// Later, from a system that holds the write on the column:
sptext_set(texts[i], "SCORE %d", score); // the layout relays out only if the string changed
```

Add `"sprite_text"` to your module's `deps`; it brings `asset` and `scene`. Put your own
`.ttf` under your module's `assets/` and load it by `<module>/<file>`, or depend on
`dejavu` and load `KV_DEJAVU_SANS` (DejaVu Sans, the engine's default font: `modules/dejavu`).

`sptext_layout(font, text, out, &width)` is the layout as a function, for a game placing
its own things around a line of text; `font_measure(font, str, scale)` is its width.

## Quick reference

Header `korvid/sprite_text/sprite_text.h`:

| name | kind | what |
|---|---|---|
| `Text` | component | `font` (asset.font), `str[KV_SPTEXT_MAX]`, `scale` (1), `tint` (white), `layer` (0), `space` (World), `anchor` (Center: where screen text is measured from), `align` (Left) |
| `TextAlign` | enum | `Left`, `Center`, `Right` |
| `sptext_set(text, fmt, ...)` | function | printf into `str`, truncating; returns what snprintf would |
| `KV_SPTEXT_MAX` | constant | 64 bytes, NUL included |

Header `korvid/sprite_text/module.h`:

| name | kind | what |
|---|---|---|
| `sprite_text_module` | record | the module |
| `GlyphPlacement`, `sptext_layout(font, text, out, width = null)` | record, function | the visible glyphs' centers, sizes and uvs relative to the label; returns the count |
| `sptext_stats(engine)` | function | last tick's `labels`, `relaid`, `glyphs` |

Header `korvid/asset/font.h` (asset module): `Glyph`, `FontAsset`, `asset_load_font(e, path, px)`,
`asset_font(e, h)`, `font_glyph(font, codepoint)`, `font_kern(font, a, b)`, `font_measure(font, str, scale)`.

What the module registers with the engine:

| name | kind | notes |
|---|---|---|
| `Text` | component | sim state |
| `sprite_text.label` | kind | `Transform2`, `Text`; 4096 slots |
| `sprite_text.glyph` | kind, cosmetic | `Transform2`, `Sprite`, `Local2`, `Parent`; 65536 slots; never hashed |
| `sprite_text.layout` | system, phase Frame | reads `*.Text`, `*.Transform2`; writes `sprite_text.glyph` |

Depends on: core, math, render, platform, asset, sprite, scene.

## Behaviour worth knowing

- **A glyph is an entity.** A 20-character label is 20 rows; a score that changes every
  tick respawns its digits every tick. Fine for a simple game; a glyph run as one GPU row
  is the upgrade if a game needs thousands of changing characters.
- **Text appears when the font does.** A label spawned before `AssetReady` lays out as
  nothing and then appears, a frame or two later; the layout notices the font's state.
- **No shaping.** Each code point is one glyph: no right-to-left, no ligatures, no
  combining marks drawn as such. Kerning pairs from the font are applied (the Latin set).
- **Glyphs are sampled linearly** (the font's material asks for `TextureFilter::Linear`),
  so text at a scale of its own stays smooth while pixel art beside it stays sharp.
- **A drop shadow is one field.** `Text::shadow` (a color; alpha 0 is none) and
  `shadow_offset` (label units, pixels on screen, +y up) draw every glyph a second time
  under itself: the shadows are keyed under the glyphs in the same layer
  (`SpriteSort::Key`), so a label's shadow never covers another label's glyphs.
- **What no face has is U+FFFD.** A code point no face of the font has (the font, then its
  fallback list) and a malformed byte both draw the replacement glyph, or nothing if the font
  has none; the first miss per font is logged, and `print /fonts/...` counts them.
- **A label's world transform is its pen.** Rotating or scaling the label rotates and
  scales the line, since glyphs are children.
- **Screen-space labels are in pixels at the font's size** times `scale`; world-space
  labels are in world units, so a zoomed camera zooms them.

## Not in this module

Line wrapping, UTF-8, rich text, outlines and shadows, signed-distance-field glyphs,
input fields, a UI layer. A `.font.json` naming a face, size and character set is an
obvious next step; today the size is the call's argument and the set is fixed.
