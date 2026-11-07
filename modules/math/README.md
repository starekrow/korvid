# math

2D math as plain structs and free functions: vectors and points that are different
types, rectangles, linear RGBA colors, 2x3 affine transforms, and the field traits that
let all of them appear in component schemas. Header only; every function is inline; no
SIMD, no templates. The platform's `libm` does the transcendentals, which is why
determinism is promised within one build and platform and not across them.

## How it works

`vec2` is a direction or a displacement; `point2` is a position. Point minus point is a
vector, point plus vector is a point, and point plus point does not compile. That is the
whole reason they are two types. Everything is `f32`.

`rect2` is an origin and a size, used for UV rectangles and for the view. `color` is four
linear floats; tints multiply, and there are no 8-bit sRGB constants on purpose because
the renderer encodes to sRGB in hardware on the way out.

`affine2` is a 2x3 column-major transform: `a, b` the x axis, `c, d` the y axis, `tx, ty`
the translation. `affine2_trs(position, radians, scale)` builds the sprite transform
(scale, then rotate, then move); `ortho2d(view)` maps a world rectangle onto Vulkan clip
space with world +y up; `operator*` composes (right applied first); `inverse` undoes a
non-singular one.

`fields.h` tells the schema system what the types look like: `vec2` and `point2` are two
`F32`, `rect2` and `color` are four. Include it where a component with such fields is
declared, and `KV_FIELDS(Transform2, position, rotation, scale)` just works, as do the
hash, the describe and the trace for every row.

## Using it

```cpp
#include "korvid/math/math.h"    // everything
#include "korvid/math/fields.h"  // where a component is declared

point2 at{100.0f, 50.0f};
vec2   v = heading * speed * dt;
at += v;                                     // point + vec
f32 d = distance(at, other);                 // point - point, then length
vec2 n = normalize(perp(dir));
affine2 m = affine2_trs(at, rotation, vec2{2.0f, 2.0f});
point2 corner = m * point2{-0.5f, -0.5f};    // points get the translation
vec2   axis   = m * vec2{1.0f, 0.0f};        // vectors do not
rect2  uv = rect2{0.0f, 0.0f, w / 128.0f, h / 128.0f};   // outside [0, 1] tiles
color  c  = color_rgba(0.3f, 0.8f, 1.0f) * tint;
```

Interpolate placements per field and rebuild with `affine2_trs` where rotation matters;
`lerp(affine2, affine2)` is component-wise and shrinks a rotation through the middle.

## Quick reference

Header `korvid/math/vec2.h`:

| name | kind | what |
|---|---|---|
| `vec2{x, y}`, `point2{x, y}` | records | a displacement; a position |
| `+ - * /`, `+= -= *=`, `==` | operators | vec with vec, vec with scalar, `*` component-wise; point with vec; point minus point |
| `dot`, `cross`, `length`, `length_sq`, `normalize`, `perp`, `distance` | functions | `cross` is the z of the 3D cross; `perp` rotates +90 degrees; `normalize` of zero is zero |
| `lerp(a, b, t)` | function | for vec2, point2, f32 |
| `as_vec(p)`, `as_point(v)` | functions | the explicit conversions |
| `clampf`, `nearly(a, b, eps)`, `KV_PI` | functions, constant | scalars |

Header `korvid/math/rect2.h`:

| name | kind | what |
|---|---|---|
| `rect2{x, y, w, h}` | record | min corner and size |
| `rect2_from_center(c, size)`, `rect2_unit()` | functions | from a center; the full-texture UV rect |
| `rect_min`, `rect_max`, `rect_center`, `rect_size`, `rect_contains(r, p)`, `rect_overlaps(a, b)` | functions | queries; `contains` is inclusive, `overlaps` strict |

Header `korvid/math/color.h`:

| name | kind | what |
|---|---|---|
| `color{r, g, b, a}` | record | linear, defaults to opaque white |
| `*` (color, scalar), `==`, `lerp` | operators, function | tints multiply |
| `color_white`, `color_black`, `color_red`, `color_green`, `color_blue`, `color_clear`, `color_rgba(r, g, b, a)` | functions | constants and a constructor |

Header `korvid/math/affine2.h`:

| name | kind | what |
|---|---|---|
| `affine2{a, b, c, d, tx, ty}` | record | column-major 2x3; identity by default |
| `affine2_identity`, `affine2_translate(t)`, `affine2_scale(s)`, `affine2_rotate(radians)`, `affine2_trs(p, radians, s)` | functions | constructors |
| `transform_point(m, p)`, `transform_vec(m, v)`, `m * p`, `m * v` | functions, operators | points translate, vectors do not |
| `m * n` | operator | compose: `n` first, then `m` |
| `determinant`, `inverse` | functions | singular inverts to the identity |
| `lerp`, `nearly` | functions | component-wise |
| `ortho2d(view)` | function | a world rect onto clip space, +y up on screen |

Header `korvid/math/fields.h`: `KV_FIELD_TRAITS` for `vec2`, `point2`, `rect2`, `color`
(`F32` x 2, 2, 4, 4), with a static assert that the types are packed floats.

Header `korvid/math/math.h`: the umbrella.

The module registers nothing with the engine; it has no `ModuleDesc`.

## Behaviour worth knowing

- **Angles are radians, counter-clockwise, +y up.** `affine2_rotate(KV_PI / 2)` turns
  the x axis into +y.
- **Rotation lerps linearly.** `lerp(0, 2 * KV_PI, 0.5)` is a half turn; the sprite
  module interpolates rotation as a scalar, so a sprite that spins past a full turn
  should keep its angle unwrapped if it wants smooth interpolation.
- **Compare with `nearly`**, not `==`, after any arithmetic; `==` is exact and is there
  for the schema's change detection, where exact is right.
- **No 3D, no doubles.** `f64` appears in the engine for time only.

## Not in this module

3D vectors and matrices, quaternions, SIMD, integer vectors, bounding shapes beyond
`rect2`, random numbers (`core/random.h`), noise, splines, easing. The collision module
owns its circle and box tests.
