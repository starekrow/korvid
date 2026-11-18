# patherizer

Paths, filled and stroked, rasterized on the CPU at the size they are drawn
(docs/PRDs/Krome-Slice-5-PRD.md). It is the engine's own vector rasterizer, general-purpose
and built for what SVG needs: Krome draws its shapes and std's icons with it, the `svg`
module builds its shapes, kvscript has it as `kv.patherizer`, and a game may call it
directly. It depends on `core` and `math` and nothing else. Design notes and what each
milestone found: `docs/design/patherizer.md`.

## What it draws

- **Paths** of lines, quadratics and cubics (SVG path data read by `PathBuilder::svg_path`,
  arcs converted to cubics), filled with the nonzero or even-odd rule.
- **Strokes** of any width with miter, round and bevel joins (and the miter limit), butt,
  round and square caps, and dashes with an offset, widened into fills in the path's own
  space, so a stroke under a transform is the transformed stroke.
- **Paint**: solid, `currentColor` (`PathOptions::current`), linear and radial (with a
  focal point) gradients with pad, reflect and repeat, their stops interpolated in sRGB
  or in linear light.
- **Groups** with transforms, opacity (drawn through a layer, as SVG means it) and clips
  (another group's outlines as a coverage mask).

## How it works

Curves are flattened to within 0.02 px (Wang's bound picks the steps). Coverage is
FreeType's cell method: each edge adds signed area and cover to the cells it crosses, in
1/256 px integers, a band of 64 rows at a time, and a row's sweep turns them into exact
area coverage (nonzero clamps, even-odd folds). Everything after the flattening is integer
arithmetic, and the flattening and gradients use the engine's own math, so a shape draws
the same bytes on every platform; the tests hold a hash of the whole corpus. Paint is
composited into the target as 16-bit premultiplied colour and written back as sRGB RGBA8,
straight alpha (the UI primitive's) or premultiplied.

Known limits, each held by a test: two paths sharing a pixel along an edge are each
blended by coverage (conflation: a seam can show a little of what is under it), a path
whose parts overlap inside one pixel can count that pixel's overlap twice (coverage is
summed before the rule is applied, per cell, not per point), and a gradient is sampled at
the pixel's centre.

## Using it

```cpp
#include "korvid/patherizer/patherizer.h"

PathShape s;
s.init(mem_general());
s.view[2] = s.view[3] = 24;                      // the view box the target shows
PathBuilder b(s);
b.svg_path("M4 12l5 5L20 6");
PathStroke st;
st.width = 2; st.cap = LineCap::Round; st.join = LineJoin::Round;
b.stroke(s.add_current(), st);
u8 px[48 * 48 * 4] = {};
PathOptions o;
o.current[0] = 255; o.current[1] = 128; o.current[2] = 0; o.current[3] = 255;
patherizer_draw(s, PathTarget{px, 48, 48, 48 * 4, false}, o);
s.free();
```

The view box fills the target, scaled the same each way and centred (SVG's `xMidYMid
meet`), or `stretch`ed. `one_colour()` says whether every paint is `currentColor`, the
shapes a caller can draw once in white and tint.

**`.kshape`** is the shape as a file: KBOR, little-endian packed arrays, a sidecar of
source lines for messages (`patherizer_write_kshape`, `patherizer_read_kshape`, which
checks every index so drawing need check nothing). The `svg` module's cook writes them; the
asset module loads them (`asset_load_shape`).

## Tests

`patherizer_tests`: analytic coverage (rectangles, circles, half-planes), the fill rules,
gradients against computed values, every join, cap and dash, clips and groups, and a
corpus of 31 shapes drawn at several sizes against resvg's pictures of them
(`tests/references`, made by `tools/patherizer-refs.sh`: resvg at 8x, box-filtered) and
hashed. `--truth NAME SIZE` prints a 16x supersampled picture of a case; `--write-svg DIR`
writes the corpus as SVG; `KV_PATHERIZER_DUMP=DIR` writes the pictures that differ.

## Numbers

On the Linux machine (dev build, -O2), `svg_tests --bench` (KP17):

| what | time |
|---|---|
| an icon at 16 px (mean of std's 21; slowest, `settings`) | 3.2 us (6.4) |
| an icon at 32 px | 9.0 us (15.0) |
| an icon at 64 px | 30 us (45) |
| the first frame of 50 icons at 20 px, scale 1 (their rasterization) | 0.21 ms |
| the same at scale 2 | 0.65 ms |
| `shapes.svg` at 1000 x 1000 (8 nodes: fills and strokes) | 14 ms |
| `structure.svg` at 1000 x 1000 (17 nodes: groups, `use`, clips) | 15 ms |
| `paint.svg` at 1000 x 1000 (12 nodes: gradients, opacity) | 48 ms |

Icons cost nothing worth a job. A large illustration is one to three frames, so Krome
rasterizes a shape over 256 x 256 on a task worker, its node empty (or its last size
stretched) until it lands, and the frame it first appears in is not stalled.
