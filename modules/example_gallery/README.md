# example_gallery

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

    build/macos-dev/bin/example_gallery --out DIR      # nine PPMs into DIR (a dev or debug build: ship cannot screenshot)
    build/macos-dev/bin/example_gallery --scene sort   # one scene
    build/macos-dev/bin/picture compare DIR modules/example_gallery/references/metal    # against the committed references
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

`references/metal/` holds the pictures renderer-metal draws on an M3 Max (macOS 26.7), as
PNGs; `references/vulkan/` those renderer-vulkan draws on Linux (a Radeon 780M, RADV 26.0.8,
GNOME on Wayland, Nov 10); against Metal they fail and pass in the same counts Windows's did.
Each backend is compared against its own set for its regressions, and the two sets against
each other for parity; the first comparison of the two fixes the picture tool's tolerance
(`modules/tool_picture`), which is not loosened after. Against Metal the Vulkan set passes
seven scenes at the defaults (`transform` with 87 pixels off at the glyph's edges); `blends`
and `shaders` fail at 6.6% and 6.4%, every checker cell edge a pixel over in one triangle
of the field's quad: the checker's cells sit on pixel boundaries under nearest sampling,
where two GPUs round a texel edge differently. A decision for the scene, not the tolerance.

## Files

| File | What it is |
| --- | --- |
| `src/main.cc` | the textures, materials and scenes, the window at 960x540 pixels, the screenshots |
| `shaders/gallery_rings.slang` | a sprite shader: rings in the material's colors, cut to a disc |
| `shaders/gallery_post.slang` | a post shader over the layers below 5 |
| `references/metal/`, `references/vulkan/` | the committed pictures, PNG, by backend |
