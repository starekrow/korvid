# Krome Slice 5 — Shapes

Nov 15, 2026 · @starekrow · draft

## Goal and exit criteria

Slice 5 gives Krome pictures that are drawn, not stored. An icon or a piece of UI art is a
**shape**: paths, fills and strokes, kept as data and rasterized by the engine at the exact
pixel size it is drawn at. The target decides the resolution, not the build. A 16 px icon on
a 1x display and the same icon at 2x are each rasterized for their pixels, and a DPI or UI
scale change draws them again.

The master PRD (`Krome-Master-PRD.md`, slice 5, K21 and K21a) asks for four things:

- the rasterizer, a module of the engine's own (`patherizer`);
- the shape asset;
- the SVG tool that makes shape assets;
- Krome's shape cache, and icons in the standard prelude.

The design's "Vector shapes" (`docs/design/krome.md`) gives the subset.

It is done when:

- **`patherizer` rasterizes the subset**: lines, quadratic and cubic curves; both fill rules;
  strokes with every join, cap and dash; solid, linear and radial paint; transforms; clip
  paths; group opacity.
  - It depends on `core` and `math` and nothing else, and nothing in it is Krome's.
  - It draws the same bytes on every platform: a test hashes a corpus of shapes at several
    sizes, and the hashes are the same on Linux, Windows, macOS and iOS.
  - Its coverage is exact where it can be: a rectangle on pixel edges, a pixel-aligned
    stroke, a circle's area within 0.5%.
  - Its pictures agree with resvg's within the picture tool's tolerance, over a corpus of
    test shapes and every icon, at 16, 24, 48 and 200 px. resvg's pictures are committed;
    a script makes them again.
- **The SVG tool** turns an SVG file into a shape asset and names what it cannot do.
  - The converter reads SVG 1.1's shapes, paths, transforms, groups, `clipPath`, gradients
    and `style=`.
  - Anything outside the subset (text, filters, masks, patterns, markers, images, CSS
    selectors) is an error that names the file, the line and the element, as a `.kim`
    error does (K11). Nothing outside the subset is dropped without a word.
  - goldberg cooks `.svg` to the shape asset in process, as it does `.kim`.
- **The shape asset** (`.kshape`, KBOR) is loaded by the asset module (`asset_load_shape`),
  held by handle like a texture, and reloads when its file changes.
- **Krome draws shapes**:
  - `image="icons/check.svg"` on an `image` node draws the shape, sized by the layout like
    a texture (its natural size is its view box, in logical pixels).
  - The shape is rasterized at the node's pixel size and the window's scale, cached by
    shape, size and colour.
  - Parts painted `currentColor` take the node's `color`, so one icon serves both themes
    and every state. A shape painted only `currentColor` is rasterized once and tinted, so
    a colour change costs nothing.
  - A still screen rasterizes nothing (K13); a resize or a scale change rasterizes what is
    drawn at its new size, once.
- **`krome/std` has icons** and uses them where it draws marks today:
  - the checkbox's check (today a filled box);
  - the dropdown's arrow and the tree's arrows (today the glyphs `▾` and `▸`);
  - a dialog's and a tab's close;
  - a table header's sort direction;
  - the badges' info, warning and error.

  kromedemo's gallery page shows every icon in both themes, and kromescreens' screens use
  them.
- **Scripts and games can rasterize too**: kvscript's `kv.patherizer` and `kv.svg` (an asset
  script makes a PNG from an SVG at any size), and a game's C++ through `patherizer`'s API into
  its own buffer.
- `patherizer_tests` covers the rasterizer headless; `krome_tests` covers the cache and
  `currentColor`; the gallery's `ui` scene gains shapes; `render-gate` passes on every
  backend.

## What the code has

- **Krome's `image` node** (`modules/krome/src/view.cc`) takes an asset path, loads it as a
  texture (`asset_load_texture`) and draws it as a textured box of the UI primitive. With no
  `bg`, the look's `tint` multiplies it, so a white picture can already take a colour.
  Layout measures it at the texture's size in logical pixels.
- **Glyph pages** (`modules/asset/src/font.cc`) are the model for a cache of small pictures
  made on demand:
  - a shelf packer fills fixed-size pages;
  - `renderer_texture_update` sends a rectangle (Vulkan and the CPU renderer copy only the
    rectangle; Metal and D3D12 send the page again);
  - Krome redraws when what it waited on is ready.
- **std's marks are glyphs or boxes.**
  - The checkbox's selected look is a filled box, with no check.
  - The dropdown's arrow is `text "▾"`.
  - kromescreens' tree arrows are `▾` and `▸` written into its model.
  - Nothing has a close mark or an icon.
- **What a converter needs is here**:
  - `xml` (yxml behind a tree API) reads SVG;
  - `kbor` and `kimb` write and read the asset;
  - goldberg runs built-in converters in process (`builtin: "kim"` in `conventions.berg`).
- **kvscript** has `kv.image` (PNG in and out) and `kv.font`, and nothing that draws a path.
- **The engine's math** (`kv::` scalar functions, near correctly rounded) gives the same
  floats on every platform. With `-ffp-contract=off` everywhere, a rasterizer written on it
  draws the same bytes everywhere.

## The shape

### Where things are

| path | what |
|---|---|
| `modules/patherizer/` | NEW. The rasterizer: paths, flattening, fills, strokes, paint, clips, groups; a shape in memory (`PathShape`) and its drawing into a caller's RGBA buffer |
| `modules/svg/` | NEW. SVG to a shape: `xml` in, a `PathShape` out, every refusal with its line; the `.kshape` writer |
| `modules/asset/` | the shape asset: `asset_load_shape`, `ShapeAsset` (a `PathShape` read from `.kshape`) |
| `tools/goldberg/` | the `svg` built-in converter; `*.svg` in `conventions.berg` |
| `tools/kvscript/` | `kv.patherizer` and `kv.svg` |
| `modules/krome/` | the shape cache, `image=` taking a shape, `currentColor` |
| `modules/krome/assets/` | std's icons (`icons/*.svg`) and the definitions that use them |

### The rasterizer: `patherizer`

A shape is a tree: groups (opacity, a clip, children) over paths. A path has commands
(move, line, quadratic, cubic, close), a transform, a fill and a stroke. The asset keeps the
transform rather than baking it into the points, because a stroke under a non-uniform scale
must be widened before the transform, as SVG does.

Drawing a shape at `w x h` pixels:

1. **Flatten.** Each curve is cut into lines in device space. The tolerance is 0.02 pixel,
   chosen from the curve's control points under the full transform. A 16 px icon gets a
   few segments a curve, a 1000 px one many.
2. **Stroke.** A stroke is expanded to a fill outline in the path's own space, then
   transformed:
   - joins miter (with its limit), round and bevel;
   - caps butt, round and square;
   - dashes cut the path first, with their offset.

   The outline is filled nonzero, so its overlaps at tight bends do not leave holes.
3. **Fill.** Coverage is accumulated per cell, as FreeType's grey rasterizer does: each line
   adds its signed area and cover to the cells it crosses. A sweep along each row turns the
   running total into coverage.
   - Nonzero clamps the total.
   - Even-odd folds it: a total past one turns back.
   - Coverage is exact for edges that do not overlap within a pixel, and the usual,
     accepted approximation where they do.
   - The arithmetic is integer: 1/256 pixel positions, 8.8 areas. Only the flattening and
     the transforms are float, on the engine's math.
4. **Paint.** Coverage times the paint, over the buffer:
   - solid colour;
   - a linear or radial gradient (two points, or a centre, a focus and a radius) with any
     number of stops and `pad`, `reflect` or `repeat`;
   - opacity per fill and stroke.

   Gradients interpolate in sRGB, as SVG and every authoring tool do, unless the shape says
   `color-interpolation: linearRGB`.
5. **Clip and group.** A clip path is rasterized to a coverage mask of its own and
   multiplies what it clips. A group with opacity below 1 draws into a layer of its own,
   then composites. Layers come from a scratch arena and are freed with the call.

The output is RGBA8, sRGB-encoded, straight alpha: what the UI primitive's textured box
reads. A caller may ask for premultiplied instead.

The API is small:

- `patherizer_draw(const PathShape&, PathTarget&, const PathOptions&)`. The target is a
  buffer, its size and pitch. The options are the view box to fit, the current colour and
  the format.
- `patherizer_draw_path(...)` for one path, for a caller with no shape (kvscript's).
- The shape builder: `patherizer_shape_*`, used by `svg` and by tests.

### The shape asset: `.kshape`

KBOR, written by `svg`, read by the asset module, one per SVG file:

- the view box;
- the paints (solid, gradients with their stops and transform);
- the paths (commands as `f32` pairs, a transform, fill and stroke with their paint
  indices);
- the groups;
- the lines of the SVG each came from, in a sidecar array, as `.kimb` keeps its lines.

`currentColor` is a paint of its own, resolved when the shape is drawn. The asset says
whether the shape is one colour (every paint `currentColor`) or coloured, so Krome need not
look.

The runtime never reads SVG. A game may link `svg` to read one, as it may link `kim`.

### The SVG tool: `svg`

A converter that reads what icon sets and authoring tools write:

- `svg` with `viewBox`, `width`, `height`;
- `g`, `path` (all of its commands, arcs converted to cubics), `rect` (with `rx`, `ry`),
  `circle`, `ellipse`, `line`, `polyline`, `polygon`;
- `transform` lists;
- `defs`, `use` (of a shape or group in the same file), `clipPath`, `linearGradient`,
  `radialGradient`;
- presentation attributes and `style="..."` declarations: fill, stroke, the stroke's
  properties, opacity, fill-opacity, stroke-opacity, fill-rule, clip-rule, clip-path,
  color, display, visibility;
- colours as names, `#rgb`, `#rrggbb`, `rgb()`, and `currentColor`.

Anything else is an error with its line and element, for example "icons/save.svg:12: `filter`
is not drawn (shapes have no filters)". `--lenient` turns the errors into warnings for
assets nobody can fix.

goldberg runs it in process (`builtin: "svg"`), `*.svg` meets it in `conventions.berg`, and
the converted shape lands as `<name>.kshape`. A screen names the SVG (`image=
"icons/check.svg"`); the asset module finds the cooked shape by the path, as it finds a
`.kimb` for a `.kim`.

### Krome

- **`image` takes a shape.** A path whose cooked asset is a shape draws as one. Layout
  measures it at its view box in logical pixels, or by `w`, `h`, or one of them and the
  view box's aspect, as an image is measured now.
- **Colour.** The converter marks each shape one of two kinds:
  - **One colour**: every paint is `currentColor`, at any opacity. It is rasterized white
    and drawn as a textured box tinted by the node's `color` (and its `tint`, which
    multiplies any image). A hover, a theme switch or a transition changes the tint, not
    the pixels.
  - **Coloured**: it has a colour of its own, with or without `currentColor`. It is
    rasterized with `currentColor` resolved to the node's `color`, and that colour is part
    of its cache key.
- **The cache.**
  - A shape drawn at a node is rasterized at the node's pixel rectangle (logical size times
    the window's scale), kept in pages as glyphs are.
  - Its key is shape and pixel size, and the colour for a coloured shape.
  - A page is 1024 x 1024. A shape larger than a page gets a texture of its own.
  - Entries not drawn for a while are freed, a page at a time.
- **When it rasterizes.** A shape is rasterized at paint, on the engine's thread, the first
  frame it is drawn at that size and colour.
  - A shape over 256 x 256 pixels is rasterized on a job instead. Its node draws nothing
    until it is ready, then the frame is asked for again, as a glyph page does.
  - A still screen asks for nothing.
- **Colour transitions of a coloured shape.** A coloured shape whose `color` changes is
  rasterized again in the new colour. A transition would rasterize every frame of it, so the
  shape cross-fades between its two end colours' rasterizations instead. A one-colour
  shape eases its tint, as a box eases its fill.

### Icons in `krome/std`

A subset of **Material Symbols** (Google, Apache 2.0), vendored in
`modules/krome/assets/icons/` with its licence, which `krome`'s `licenses` names so a
package ships it.

- **Which files.** The Outlined style, weight 400, unfilled, at the 20 px optical size
  (`<name>_20px.svg`), from `google/material-design-icons` at a pinned commit. The commit
  and the list are in the directory's README.
- **One colour.** Material's files have no fill, which SVG paints black. Vendoring puts
  `fill="currentColor"` on each root element, so every icon is a one-colour shape. A small
  script does the fetch and the edit; it is in the directory and runs again for a new
  icon.
- **The set**, about twenty, named as Material names them:
  - `check`, `remove` (an indeterminate check);
  - `expand_more`, `expand_less`, `chevron_left`, `chevron_right`;
  - `arrow_drop_down`, `arrow_right` (the dropdown's and the tree's);
  - `close`, `add`, `search`, `menu`;
  - `arrow_upward`, `arrow_downward` (sort);
  - `info`, `warning`, `error`;
  - `folder`, `draft`, `settings`, `refresh`.
- **Where std uses them**: the checkbox's check, the dropdown's arrow, the tree's arrows, a
  closable tab's and dialog's close, a table header's sort, and the badges' icons.
  kromescreens' tree stops writing glyphs into its model. A screen or a game may use any
  icon by its path.
- **Sizes.** Material draws at the 20 px size on a 20 px grid. std uses them at 20 and 16
  logical pixels, and at 14 inside the checkbox. Below 20 they are scaled, not hinted.

## Out of scope

- **Text in shapes, filters, masks** (as opposed to clip paths), patterns, markers, embedded
  images, CSS beyond `style=`, animation. These are refused, by name.
- **Hinting.** A shape is drawn as its geometry says. Material's icons are drawn on their
  20 px grid, so they are sharpest at 20 px and its multiples.
- **Shapes as sprites in games.** A game can rasterize into its own texture with `patherizer`,
  but no `Sprite` takes a shape, and no material does.
- **Rasterizing on the GPU.**
- **Hot reload of the screens that use a shape.** Slice 6's. The shape asset reloads; a
  screen that holds it draws the new one when its handle rebinds, which is what slice 6
  builds on.

## Requirements

| ID | Requirement | Priority |
|---|---|---|
| KP1 | `patherizer`: paths of lines and quadratic and cubic curves, flattened to 0.02 pixel under the full transform | P0 |
| KP2 | Fills: nonzero and even-odd, integer cell coverage, exact for edges that do not overlap within a pixel | P0 |
| KP3 | Strokes expanded to fills: miter (with limit), round and bevel joins; butt, round and square caps; dashes with offset; widened before a non-uniform transform | P0 |
| KP4 | Paint: solid, linear and radial gradients (pad, reflect, repeat), opacity per fill and stroke; sRGB interpolation, linearRGB on request | P0 |
| KP5 | Clip paths, and group opacity through a layer | P1 |
| KP6 | The same bytes on every platform: a hashed corpus agrees on Linux, Windows, macOS and iOS | P0 |
| KP6a | Within tolerance of resvg's pictures, committed, over the corpus and the icons at 16, 24, 48 and 200 px | P0 |
| KP7 | `patherizer` depends on `core` and `math` only, and has no Krome in it | P0 |
| KP8 | The shape asset `.kshape` (KBOR), with the source's lines; `asset_load_shape` and `ShapeAsset`; reload on change | P0 |
| KP9 | `svg`: the subset above to a shape; every refusal names its file, line and element; `--lenient` | P0 |
| KP10 | goldberg's `svg` built-in converter and `*.svg` in the conventions | P0 |
| KP11 | Krome: `image=` takes a shape, measured by its view box, rasterized at the node's pixel size and the window's scale | P0 |
| KP12 | Krome's shape cache: pages by shape, size and colour; large shapes on a job; still screens rasterize nothing; scale changes rasterize once | P0 |
| KP13 | One-colour shapes rasterized once and tinted by `color`; coloured shapes keyed by colour, their transitions cross-faded | P0 |
| KP14 | std's icons, Material Symbols vendored with their licence; std's definitions and kromescreens' tree use them | P1 |
| KP15 | kvscript `kv.patherizer` and `kv.svg` | P2 |
| KP16 | The gallery's `ui` scene draws shapes; render-gate passes on every backend | P0 |
| KP17 | Numbers: an icon at 16, 32 and 64 px; a 1000 px illustration; the first frame of a screen of 50 icons, at 1x and 2x | P1 |

## Milestones

Each leaves the tree building and every example running.

1. **Fills** (KP1, KP2, KP4, KP6, KP7). `patherizer` with paths, flattening, both fill rules,
   solid and gradient paint. Headless tests:
   - analytic coverage (rectangles, circles, half-planes at known angles);
   - the fill rules on self-crossing paths;
   - gradients against computed values;
   - the hashed corpus, and resvg's pictures of it (`tools/patherizer-refs.sh` makes them).

   Material's icons are vendored here, since the corpus and resvg's pictures include them.
   Until `svg` exists (milestone 3), the tests build the icons' paths through the shape
   builder from their path data.
2. **Strokes, clips and groups** (KP3, KP5). Stroke expansion with every join, cap and dash;
   clip masks; group layers. Tests for each, and the corpus grows.
3. **SVG and the asset** (KP8 to KP10, KP15). `svg`, `.kshape`, `asset_load_shape`, goldberg's
   converter, kvscript's bindings. The icons and the corpus are converted from their SVG files
and draw as the builder's did. A test reads
   each refusal's line.
4. **Krome** (KP11 to KP14). `image=` shapes, the cache, one-colour and coloured shapes, the
   cross-fade; Material's icons in std's definitions; kromedemo's gallery page and
   kromescreens.
5. **Close** (KP16, KP17). The gallery's shapes, the render gate on every backend (the other
   machines run theirs), the numbers, the READMEs, the master PRD corrected where this
   slice proved it wrong.

## Decided

From the master PRD's review (Nov 15):

- **Shapes rasterize in a module of their own**, general-purpose, built for SVG's needs.
  stb_truetype stays as it is.
- **SVG is an import format.** The runtime never reads XML.
- **Rasterized on the CPU, at the drawn size, cached by shape and size.**

In review of this draft (Nov 15):

- **The converter is C++**: the `svg` module, run by goldberg in process as `kim` is, and by
  kvscript as `kv.svg`. The master PRD's "a Lua asset tool" is superseded.
- **The rasterizer is `patherizer`.**
- **std's icons are Material Symbols** (Apache 2.0), a vendored subset.
- **One-colour shapes are tinted; shapes with colours of their own are keyed by colour.** A
  shape painted only `currentColor` is rasterized once, white, and the textured box tints
  it, so a colour change or a transition costs nothing. A shape with a colour of its own
  is rasterized with `currentColor` resolved, keyed by that colour.
- **resvg is the reference.** `resvg` (0.46) is installed on the Linux machine; its pictures
  are made once and committed.
- **The extension is `.kshape`.**
