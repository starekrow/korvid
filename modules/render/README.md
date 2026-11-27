# render

The render interface, the shaders every backend draws with, and the half of the renderer
the backends share. A backend (`renderer-vulkan`, `renderer-metal`, `renderer-d3d12`)
provides the interface; the binding is per platform in the toolchains and `kormake.lua`.
The design is in `docs/design/renderer.md`; its last section, "The core and the backends",
is the map of this module.

| Where | What it is |
| --- | --- |
| `include/korvid/render/renderer.h`, `sprite.h`, `material.h` | the interface a game and the outer engine see: the delta stream, textures, materials, the frame, the post pass |
| `include/korvid/render/ui.h`, `src/ui.cc`, `shaders/ui.slang` | the UI primitive Krome draws on: boxes (rounded, bordered, shaded, a gradient, an image or a region of one, sampled linear or nearest, every sample held half a texel inside its uv rect) and glyphs, in lists by layer |
| `include/korvid/render/world.h`, `src/world.cc` | the linked half: `RenderWorld` and what works on it with no GPU type (`rw_`). Links with any backend |
| `include/korvid/render/core/layout.h` | the constants and the GPU layouts, mirrored by `shaders/sprite_common.slangh`: change one and change the other |
| `include/korvid/render/core/core.h` | the contract between the core and a backend, and its rules |
| `include/korvid/render/core/core.inl`, `frame.inl` | the compiled-in half (`rc_`): built inside each backend by its `src/render_core.cc`, against that backend's types. Never compiled on their own |
| `shaders/` | the sprite, sort and post shaders, and the headers a game's shaders include (`sprite_fragment.slangh`, `post_fragment.slangh`) |
| `tests/` | `render_tests`: the world from a delta stream, the runs, the push block, the sort verifier; no GPU |

## Writing a backend

Everything a backend owes the core is declared in `core.h`: five `#define`s, two types, five
records to complete by deriving, and the `rb_` functions. `tools/render-gate.sh` is the
check that it draws what the others draw; `tools/render-overlap.sh` counts what the
backends still hold in common.
