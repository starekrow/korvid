# svg

SVG files read into patherizer's shapes (docs/PRDs/Krome-Slice-5-PRD.md, "The SVG tool").
SVG is an import format: goldberg runs this module in process to cook `.svg` into
`.kshape`, kvscript has it as `kv.svg`, and the runtime never reads XML. It depends on
`core`, `math`, `xml` and `patherizer`.

## What it reads

What icon sets and authoring tools write:

- `svg` with `viewBox`, `width`, `height`;
- `g`, `path` (every command; arcs become cubics), `rect` (with `rx`, `ry`), `circle`,
  `ellipse`, `line`, `polyline`, `polygon`;
- `transform` lists; `defs` and `use` (of a shape or a group in the same file);
- `clipPath` (user space), `linearGradient`, `radialGradient` (both unit systems, `href`
  chains, `gradientTransform`, the spreads; `objectBoundingBox` measured from the curves'
  extremes, not their control points);
- presentation attributes and `style="..."`: fill, stroke and the stroke's properties,
  opacity, fill-opacity, stroke-opacity, fill-rule, clip-rule, clip-path, color, display,
  visibility, inherited as SVG inherits them;
- colours as the CSS names, `#rgb`, `#rrggbb`, `rgb()`, and `currentColor`.

## What it refuses

Text, images, filters, masks, patterns, markers, `<style>` stylesheets, animation, nested
`<svg>` and `clipPathUnits="objectBoundingBox"` are errors, each with its file and line
(`icons/save.svg:12: filter is not drawn (shapes have no filters)`), never dropped without
a word. `SvgOptions::lenient` (kvscript's `{lenient = true}`) makes them warnings and
leaves them out. goldberg's cook is strict: it has no switch for lenient yet, so a file
with refused parts fails its cook until it is fixed or converted by a kvscript step. Editors' namespaced elements and attributes,
titles and metadata are skipped silently.

## Using it

```cpp
#include "korvid/svg/svg.h"

PathShape shape;
shape.init(mem_general());
SvgMessages msgs;
bool ok = svg_to_shape(text, len, SvgOptions{}, shape, msgs);
for (u32 i = 0; i < msgs.items.len; ++i) {
    char line[300];
    msgs.format_one(i, "icons/check.svg", line, sizeof line);
    KV_INFO("svg", "%s", line);
}
```

In a game, name the SVG and let the cook and the asset module do the rest:
`asset_load_shape(e, "icons/check.svg")` loads `icons/check.kshape`, and in Krome
`image="icons/check.svg"` draws it. `KV_SVG_VERSION` keys goldberg's cook: a change here
that changes what a file converts to bumps it, and every `.svg` cooks again.

## Tests

`svg_tests`: three hand-written files (`tests/files`) against resvg's pictures at 200 px,
std's icons converted from their files drawing the bytes their path data drew through the
builder, each refusal with its line, lenient mode, and a `.kshape` round trip. `--bench`
prints the slice's numbers (patherizer's README has them).
