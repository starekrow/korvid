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
