# text

Words on screen, in the engine's own shape. A `Text` component on any kind with a
`Transform2` is laid out as glyph sprites: rows of the kind `text.glyph`, children of the
label through the scene module, one per visible character, drawn from a font asset's
atlas by the same GPU path as everything else. The module ships DejaVu Sans as a default
font. Changing the string is one write; the layout notices and redoes the glyphs.

## How it works

**Fonts are assets** (`korvid/asset/font.h`, in the asset module). `asset_load_font(e,
"text/DejaVuSans.ttf", 28)` returns a handle at once; the record owns a renderer texture
(the atlas) and a material for it (alpha blend, white, so the tint is the color). The
file reads through the I/O queue and `asset.pump_fonts` rasterizes the font's character
set (by default ASCII, the Latin-1 and Latin Extended-A blocks, common punctuation and
U+FFFD: about 360 glyphs) with stb_truetype into the smallest atlas of a fixed ladder that
fits (under 10 ms for 28 px), uploads it, and fills the glyph table (uv, size, offset from
the pen, advance) and the kerning pairs. `asset_load_font(e, path, FontDesc{px, ranges})`
takes another set. `AssetReady` follows. Before that, `font_glyph`
answers null for everything and text lays out as nothing.

**The component.** `Text { font, str[64], scale, tint, layer, space, align, shadow, shadow_offset }`. The
label's `Transform2` is the pen's start on the baseline; `Left` puts the first glyph
there, `Center` and `Right` shift the line. Strings are UTF-8, one line, 64 bytes with
the NUL; `text_set` truncates at a character boundary.

**The system.** `text.layout` runs in PostSim on every tick, paused or not, reading
`*.Text` and writing `text.glyph`. Per label slot it keeps the entity and a hash of
everything that shapes the glyphs (the string, the font and whether it is ready, scale,
tint, layer, space, alignment). When the hash changes it destroys the label's glyphs
(every glyph whose `Parent` is the label) and spawns new ones from `text_layout`, each
with the font's material, the glyph's uv and size times scale, and the label's tint,
layer and space, placed by `Local2` relative to the label. The scene module composes
them after (`scene.compose` reads `*.Local2`, which the layout's kind write covers) and
the extract draws them. A label that dies leaves its glyphs as orphans, which the scene
module destroys at the end of the same tick.

`text.glyph` is a **cosmetic kind**: never part of the state hash. The frame loop's
overlay and a game's HUD can show anything without touching determinism. `text.label`
is sim state like any other kind, so a game's labels are hashed with the rest.

## Using it

```cpp
// A kind of your own with a Text, or the module's text.label:
Text t;
t.font  = g.font;                       // asset_load_font(engine, "text/DejaVuSans.ttf", 28) at startup
t.space = SpriteSpace::Screen;          // window pixels, center origin, +y up
t.align = TextAlign::Center;
t.layer = 50;
text_set(t, "SCORE %d", score);
labels->spawn(transform2_at(point2{0, 300}), t);

// Later, from a system that holds the write on the column:
text_set(texts[i], "SCORE %d", score); // the layout relays out only if the string changed
```

Add `"text"` to your module's `deps`; it brings `asset` and `scene`. Put your own
`.ttf` under your module's `assets/` and load it by `<module>/<file>`; the default is
`text/DejaVuSans.ttf` (Bitstream Vera license, in `assets/DejaVuSans-LICENSE.txt`).

`text_layout(font, text, out, &width)` is the layout as a function, for a game placing
its own things around a line of text; `font_measure(font, str, scale)` is its width.

## Quick reference

Header `korvid/text/text.h`:

| name | kind | what |
|---|---|---|
| `Text` | component | `font` (asset.font), `str[KV_TEXT_MAX]`, `scale` (1), `tint` (white), `layer` (0), `space` (World), `align` (Left) |
| `TextAlign` | enum | `Left`, `Center`, `Right` |
| `text_set(text, fmt, ...)` | function | printf into `str`, truncating; returns what snprintf would |
| `KV_TEXT_MAX` | constant | 64 bytes, NUL included |

Header `korvid/text/module.h`:

| name | kind | what |
|---|---|---|
| `text_module` | record | the module |
| `GlyphPlacement`, `text_layout(font, text, out, width = null)` | record, function | the visible glyphs' centers, sizes and uvs relative to the label; returns the count |
| `text_stats(engine)` | function | last tick's `labels`, `relaid`, `glyphs` |

Header `korvid/asset/font.h` (asset module): `Glyph`, `FontAsset`, `asset_load_font(e, path, px)`,
`asset_font(e, h)`, `font_glyph(font, codepoint)`, `font_kern(font, a, b)`, `font_measure(font, str, scale)`.

What the module registers with the engine:

| name | kind | notes |
|---|---|---|
| `Text` | component | sim state |
| `text.label` | kind | `Transform2`, `Text`; 4096 slots |
| `text.glyph` | kind, cosmetic | `Transform2`, `Sprite`, `Local2`, `Parent`; 65536 slots; never hashed |
| `text.layout` | system, phase PostSim, `Always` | reads `*.Text`; writes `text.glyph` |

Depends on: core, math, render, platform, asset, sprite, scene.

## Behaviour worth knowing

- **A glyph is an entity.** A 20-character label is 20 rows; a score that changes every
  tick respawns its digits every tick. Fine for a simple game; a glyph run as one GPU row
  is the upgrade if a game needs thousands of changing characters.
- **Text appears when the font does.** A label spawned before `AssetReady` lays out as
  nothing and then appears, a frame or two later; the layout notices the font's state.
- **One line of UTF-8, no shaping.** Each code point is one glyph: no wrapping, no
  right-to-left, no ligatures, no combining marks drawn as such. Kerning pairs from the
  font are applied.
- **Glyphs are sampled linearly** (the font's material asks for `TextureFilter::Linear`),
  so text at a scale of its own stays smooth while pixel art beside it stays sharp.
- **A drop shadow is one field.** `Text::shadow` (a color; alpha 0 is none) and
  `shadow_offset` (label units, pixels on screen, +y up) draw every glyph a second time
  under itself: the shadows are keyed under the glyphs in the same layer
  (`SpriteSort::Key`), so a label's shadow never covers another label's glyphs.
- **What the font lacks is U+FFFD.** A code point outside the font's table (outside its
  set, or missing from the file) and a malformed byte both draw the replacement glyph,
  or nothing if the font has none; the first miss per font is logged, and `print
  /fonts/...` counts them.
- **A label's world transform is its pen.** Rotating or scaling the label rotates and
  scales the line, since glyphs are children.
- **Screen-space labels are in pixels at the font's size** times `scale`; world-space
  labels are in world units, so a zoomed camera zooms them.

## Not in this module

Line wrapping, UTF-8, rich text, outlines and shadows, signed-distance-field glyphs,
input fields, a UI layer. A `.font.json` naming a face, size and character set is an
obvious next step; today the size is the call's argument and the set is fixed.
