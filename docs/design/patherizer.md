# patherizer

Nov 16, 2026. Notes on `patherizer`, the engine's path rasterizer, as Krome slice 5 builds it
(`docs/PRDs/Krome-Slice-5-PRD.md`): what each milestone found.

## Milestone 1: fills

- **What it draws.**
  - Paths of lines and quadratic and cubic curves, flattened in the target's pixels.
  - Both fill rules.
  - Solid, `currentColor`, linear and radial (focal) gradients, with pad, reflect and repeat,
    interpolated in sRGB, or in linear light on request.
  - Group transforms.
  - Composited premultiplied in 16 bits, over the target's own pixels.

  Strokes, clip paths and group opacity are milestone 2's.
- **SVG path data is patherizer's** (`PathBuilder::svg_path`): every command, relative and
  absolute, with smooth curves and arcs (endpoint form to cubics, a quarter turn each). It
  sits with the builder, not in `svg`, because path data is useful without an SVG file
  (kvscript, a game's own outlines). `svg` will use it.
  - Numbers are read by hand: up to 18 digits of mantissa, then one scale by a power of ten
    in double. So every platform reads the same float.
- **Coverage** is FreeType's cell method, all integer:
  - positions in 1/256 pixel;
  - each line adds its signed height and its height times its x offsets to the cells it
    crosses, splitting at each row and each cell boundary with floor divisions;
  - a sweep turns a row's cells into the winding area of each pixel.

  Nonzero clamps that area; even-odd folds it. It is exact where edges do not overlap inside
  a pixel: a rectangle on pixel edges has no partial pixel at all, half a pixel in gives
  exactly 128, and a half-plane at a slope gives its area to a 255th. Bands of 64 rows bound
  the memory.
- **Flattening's tolerance is 0.02 pixel, not the PRD's 0.1.**
  - At 0.1, a chord inside a curve took up to about 25 levels from every pixel it passed: a
    circle's edge came out a little thin all the way round. A circle's total area was still
    within 0.5%, which is why the area test did not see it. resvg's pictures did.
  - Wang's bound puts the segments a curve needs at `sqrt(0.75 M / tolerance)`, so the cost
    is 2.2 times the segments. They are cheap next to the cells.
- **resvg as the reference.** `tools/patherizer-refs.sh` draws the test corpus (19 shapes,
  written as SVG by `patherizer_tests --write-svg`) and std's 21 icons at 16, 24, 48 and
  200 px.
  - resvg's tiny-skia anti-aliases with 4 x 4 samples, so its edges are good to about an
    eighth of a pixel's coverage: a rectangle 0.8 into a pixel comes out at 192 (0.75), where
    the exact answer is 204. So each reference is drawn at four times its size and
    box-filtered down by kvscript, which is 16 x 16 samples a pixel.
  - The test compares premultiplied channels: within 8 levels on all but 0.5% of the
    pixels, the picture tool's defaults. 149 pictures pass. The largest difference is 89,
    one pixel of the even-odd pentagram at 48 px, where several edges cross.
  - Where the two differ by what each is rather than by a mistake, the smaller sizes are not
    compared (the case's `min_size`) and only the hash holds them:
    - the pentagrams' crossings at 16 and 24 px, where the accumulated winding is the cell
      method's approximation (FreeType's too);
    - gradients that change much inside one pixel (reflect at 16, the focal radial below
      48, a repeat every seven pixels below 200), where a pixel's centre is not its
      supersampled average.
- **The same bytes everywhere.** The corpus and the icons at every size hash to
  `75ec614d43dcf830` in debug, dev and ship on Linux, and the test holds that hash. Another
  platform that gets another drew a pixel differently: the float work is IEEE operations
  and the engine's math with contraction off, and the rest is integer.
- **Std's icons** are vendored (`modules/krome/icons/`, `vendor.sh`, pinned to
  `google/material-design-icons` at `10ecc14`), the SVG files in `modules/krome/assets/icons/`.
  - goldberg's conventions know `*.svg` as a type of its own, with `"cook": false` until
    the `svg` converter exists (milestone 3): a type with no converter fails the cook.
  - `krome`'s `licenses` names the icons' Apache 2.0 licence.
- **kormake: `test_deps`.** A module's tests may use modules the module does not:
  patherizer's read resvg's PNGs with `image`, while patherizer itself depends on `core`
  and `math` only.

## Milestone 2: strokes, clips and groups

- **Strokes** (`stroke.cc`) are widened in the path's own space and then transformed, as
  SVG does: a circle stroked 4 wide under a stretch of 3 across is 12 wide at its sides and
  4 at its top (a test). The flattening tolerance there is the target's divided by how far
  the transform stretches. The outlines are filled nonzero:
  - an open piece is one outline: the left side, the end cap, the right side back, the
    start cap;
  - a closed subpath is two outlines wound opposite ways;
  - joins miter (with its limit, falling back to bevel), round and bevel; caps butt, round
    and square; dashes with their offset;
  - a zero-length piece draws a round cap's dot or a square cap's square.
- **The inner side of a turn, and why it matters here.** The cell method adds the coverage
  of everything a path draws, so an outline that overlaps itself counts the overlap twice
  inside a pixel. That is fine where the overlap fills whole pixels, and not at an edge.
  The first strokes pivoted through the vertex on every inner turn, as many strokers do.
  On a flattened curve that is every vertex, and each pivot's small loop overlapped the
  stroke: a stroked circle's inner edge came out a tenth of a pixel too heavy. Now:
  - the inner side turns where the two offset edges cross, for every gentle turn (a curve's)
    and for a sharp one whose crossing falls within both segments; it pivots only at a sharp
    turn between short segments;
  - an open piece whose end segment is shorter than the join's reach at its far vertex (a
    dash that cut a chord just short of its end) drops that vertex, which moves the outline
    by less than the flattening's tolerance;
  - a dash on at both the end and the start of a closed subpath is one piece across the
    seam, not two capped ones that overlap there, as resvg draws it.
- **Clips and group opacity.**
  - A group with opacity below 1, or a clip, draws into a canvas of its own, then over the
    one under it, times the opacity and the clip's coverage.
  - A clip is a group listed in `PathShape::clips`, drawn only as a clip. Its paths'
    outlines, in the clipped group's user space after the clip's own transform, make a
    coverage mask: the union of each path's coverage, by its rule, whatever it is painted
    with.
- **The references at 8 times, not 4.** At 4 times, resvg's own stroker was the error.
  - The test learned `--truth NAME SIZE`: for each pixel where ours and resvg's differ, it
    prints a third value, the shape drawn by patherizer at 16 times the size and averaged.
  - On the stroked circle at 48 px the truth sided with ours (within 3 levels) against
    resvg's (10 to 15 off). Its stroker approximates the offset curves with curves of its
    own, to a tolerance that is still about 0.06 of a final pixel at 4 times.
  - At 8 times that error halves, and the references are drawn so (32 x 32 samples a pixel).
    The rings' areas are exact either way: ours within 0.03% at 48, 200 and 800 px, resvg's
    within 0.003%.
- **Conflation, and where the corpus avoids it.** Two paths that share a pixel without
  overlapping combine their partial coverages as a + b (1 - a), where a supersampled
  picture has a + b. Every analytic rasterizer does this: browsers, and Skia at 1x.
  - The corpus keeps its shapes more than a pixel apart at 16 px, so it tests strokes and
    not that.
  - Its round-capped dashes have gaps wider than the caps. The dash list is even, since an
    odd one repeats with its dashes and gaps swapped.
  - Overlapping pieces of one path are the cell method's known approximation, as at a
    pentagram's points.
- **Numbers so far.** 197 pictures (the corpus's 31 shapes and the 21 icons, at their sizes)
  agree with resvg's. The largest difference is 89, one pixel of the even-odd pentagram at
  48 px. The corpus hashes to `866c2af2152dfe1b` in debug, dev and ship.

## Milestone 3: SVG and the asset

- **`.kshape`** (`patherizer/src/kshape.cc`). The asset module reads it and `svg` writes it,
  so it lives with the shape:
  - KBOR, one map, its large arrays packed as little-endian bytes field by field: verbs,
    points, paints, stops, strokes, dashes, nodes, clips;
  - the nodes' source lines in a sidecar that can be left out;
  - every index checked on the way in, so drawing checks nothing.

  The 21 icons cook to about 500 bytes each.
- **`svg`** (a module: core, math, xml, patherizer) reads what icon sets and editors write:
  - every shape element, `path` (through patherizer's path data), `g`, `a` and `use`;
  - transform lists; presentation attributes and `style="..."` with SVG's inheritance;
    every colour syntax and the CSS keywords;
  - gradients in both unit systems, with `href` chains, `gradientTransform` and the spreads,
    `objectBoundingBox` measured from the curves' extremes, not their control points;
  - clip paths in user space; opacity; `visibility`, `display`; `url(#id) fallback`.
- **What it refuses.** Text, images, filters, masks, patterns, markers, `<style>`,
  animation, nested `<svg>`, and `clipPathUnits="objectBoundingBox"` are refused with the
  file and the line. `lenient` makes them warnings and leaves them out. Editors' namespaced
  elements and attributes, titles and metadata are skipped without a word.
- **Its tests:**
  - three hand-written files against resvg's pictures, at 200 px;
  - the icons, converted, drawing the bytes their path data drew through the builder;
  - refusals with their lines, and lenient;
  - a `.kshape` round trip.

  At 200 px because what is under test is what SVG means. At small sizes the pictures also
  differ by patherizer's known limits (conflation where a fill's edge and a thin stroke
  share pixels; coincident edges under a clip, c x c where the truth is c), which its own
  tests hold at every size. One finding: resvg does not know `rebeccapurple` and leaves
  that stroke undrawn, so the test file uses `purple`.
- **goldberg** runs `svg` in process (`builtin: "svg"`, version `KV_SVG_VERSION`):
  - `*.svg` cooks to `<name>.kshape`, and its refusals fail the cook with their lines;
  - the conventions know both types.

  std's icons are cooked into every program that uses krome.
- **The asset module** has a shape kind (`asset_load_shape`, `ShapeAsset`, `one_colour`)
  beside blobs. A path naming the source (`icons/check.svg`) loads its cook.
  - Nothing in the asset module reloads a changed file yet, shapes or any other kind. That
    is slice 6's rebind, so the PRD's "reloads when its file changes" waits for it.
- **kvscript**: `kv.svg.load` and `parse`, `kv.patherizer.read`, and a shape's `draw` into a
  kv.image, `encode`, `view` and `one_colour`.

## Milestone 4: Krome draws shapes

- **Where it is.** `modules/krome/src/shapes.cc`: a cache of pictures beside the view. An
  image node whose path ends `.svg` or `.kshape` loads a shape (`asset_load_shape`); layout
  measures it at its view box; paint asks the cache for a box at the node's pixel rectangle.
- **The key** is the asset, the pixel size, and the colour: all ones for a one-colour shape
  (drawn white, the box tinted by `color` times `tint`), currentColor's RGBA8 for a coloured
  one. A hover or a theme switch over std's icons rasterizes nothing.
- **Pages.** 1024 x 1024 RGBA8 sRGB, straight alpha, shelves with a pixel between pictures.
  A shape past 512 pixels on either side has a page of its own, its size. Past 8 shared
  pages, the one used longest ago is cleared and reused. A page's texture is made the first
  time it is drawn with a renderer, and after each paint only its changed rectangle goes up
  (`renderer_texture_update`); without a renderer (the tests) the pages are kept in memory
  all the same, so the cache can be tested by its pixels.
- **When it rasterizes.** At paint, on the engine's thread, every size. The PRD puts a shape
  over 256 x 256 on a job; that is left for M5's numbers to decide (an icon at 20 px is
  microseconds; a large illustration is the case to measure).
- **Colour transitions.** A coloured shape whose text colour is easing draws two boxes: the
  start colour's picture fading out and the end colour's fading in, smoothstep as the
  colours ease. Two pictures, not one per frame.
- **Failed images.** A missing or broken image used to keep `krome_settled` false for ever
  (and so a `--shots` run waited for ever): it now counts as done, draws nothing and
  measures nothing. Found by writing `image="krome/icons/{i.name}.svg"`: a property is not
  interpolated (only text is), so the path is bound from the model instead (`image=@i.path`).
- **std.** The checkbox draws `check` (14 px in a 16 px box, white on the accent, shown when
  selected); the box and the radio dot went from 14 to 16 to hold it. The dropdown's arrow
  is `arrow_drop_down` at 20. A tree's arrows are its model's: kromedemo's gallery and
  kromescreens name `arrow_right` and `arrow_drop_down` and draw `image=@n.arrow` at 20,
  where they wrote ▸ and ▾ as text. Material draws on a 20 px grid with wide margins, so at
  12 or 16 the marks are small; 20 is their size.
- **Not in std yet**, so not given icons: closable tabs and dialogs (no close button exists),
  a table header's sort arrow (the sort's direction is the screen's, not the column's), and
  badges with icons. Each is a widget change more than an icon change.
- **The gallery** has an Icons page: every icon at 16, 20 and 32, and at 32 in the accent;
  `--demo --shots` takes it in both themes (`gallery_icons`, `gallery_icons_light`).
