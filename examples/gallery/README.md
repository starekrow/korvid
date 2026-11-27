# gallery

The renderer's features in fixed scenes, each drawn and written as a screenshot, so two
backends can be compared picture by picture, or one backend before and after a change.
Slice 7's parity check between renderer-vulkan and renderer-metal
(docs/PRDs/Engine-Slice-7-PRD.md, R9).

It drives the render interface directly: a window, a renderer, a stream of sprite deltas it
builds itself, and views with chosen interpolation points. No sim, no frame loop, no libm in
any position (the textures are made with integer arithmetic), so two backends' pictures
differ only where their GPUs do. Neither of its shaders reads the clock. The window opens at
whatever size in window units gives a framebuffer of exactly 960x540 pixels on the display
(480x270 on a 2x Retina Mac, 960x540 on a 1x desktop), so every backend draws the same
pixels.

    build/macos-dev/bin/gallery --out DIR      # ten PPMs into DIR (a dev or debug build: ship cannot screenshot)
    build/macos-dev/bin/gallery --scene sort   # one scene
    build/macos-dev/bin/picture compare DIR examples/gallery/references/metal    # against the committed references
    build/macos-dev/bin/picture png DIR OUT            # a set of PNGs, to keep as references

| Scene | What it draws |
| --- | --- |
| `blends` | alpha discs, additive discs over them, an opaque glyph and a tinted disc over both, on a repeating checker |
| `transform` | an asymmetric glyph ("F") at eight rotations, mirrored each way, stretched, and through three uv rects |
| `filter` | the checker magnified with nearest and with linear sampling; a disc magnified linearly |
| `sort` | discs sorted by key (the reverse of row order), by feet (lower in front), and with tied keys (row order) |
| `interpolate_0`, `_50`, `_90` | glyphs moved, turned and doubled between two ticks, drawn at alpha 0, 0.5 and 0.9; a disc set only before, unmoved |
| `camera` | the view zoomed 2x and turned a sixth of a turn; screen sprites in the four corners and the center, untouched by it |
| `shaders` | a game's sprite shader (`gallery_rings`) and a post shader (`gallery_post`: the right half's channels rotated, a line at the middle, a mark in the top left corner) under a glyph at the post layer |
| `ui` | the UI primitive (`korvid/render/ui.h`): rounded boxes with every corner, 1 and 2 px borders, gradients down, across and diagonal, soft shadows, an image in a box, the middle cell of a sheet at 6x and 11x nearest and at 6x, 11x and 2.75x linear (none of its neighbours' blue may show), the sheet tiled every 12 pixels from a box's top left, its cell greyed, its hue turned, at half and 1.6 times its gain, glyphs, a clip cutting a box's corner and glyphs inside a nested clip, shapes rasterized by patherizer as Krome draws them (an icon at 20, 40 and 80 px, white and tinted, and a coloured badge with gradients, a clip, group opacity and dashes), over a sprite background. Vulkan's and D3D12's references are the same picture; Metal's is within one step of one channel of it (rounding). The gallery leaves the scene out where `renderer_caps(r).ui` is false. Since Krome slice 7's regions (Nov 26) Metal's and D3D12's `ui` references are stale until their machines draw them again |

`references/metal/` holds the pictures renderer-metal draws on an M3 Max (macOS 26.7), as
PNGs; `references/vulkan/` those renderer-vulkan draws on Linux (a Radeon 780M, RADV 26.0.8,
GNOME on Wayland; `blends` and `shaders` drawn again Nov 11 for the new background).
Each backend is compared against its own set for its regressions, and the two sets against
each other for parity; the first comparison of the two fixes the picture tool's tolerance
(`tools/picture`), which is not loosened after. Against Metal the Vulkan set passes
seven scenes at the defaults (`transform` with 87 pixels off at the glyph's edges); `blends`
and `shaders` failed at 6.6% and 6.4%, every checker cell edge a pixel over in one triangle
of the field's quad. Their background was 900x480 units with the checker repeated 15x8,
7.5 pixels a texel, so every texel edge fell on a pixel's centre, where nearest sampling's
rounding is each GPU's own. A decision for the scene, not the tolerance: the background is
960x512 now, 8 pixels a texel, every edge on a pixel edge. Both sets have the two new
pictures (the other seven are unchanged), and they agree in all nine at the defaults:
`blends`, `shaders` and `sort` within 2 levels, the filtering scenes within 1, `camera` with
4 pixels and `transform` with 87 past the threshold, at the glyph's and the screen
sprites' edges.

## Files

| File | What it is |
| --- | --- |
| `src/main.cc` | the textures, materials and scenes, the window at 960x540 pixels, the screenshots |
| `shaders/gallery_rings.slang` | a sprite shader: rings in the material's colors, cut to a disc |
| `shaders/gallery_post.slang` | a post shader over the layers below 5 |
| `references/metal/`, `references/vulkan/` | the committed pictures, PNG, by backend |
