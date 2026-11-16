# The CPU renderer

Nov 15, 2026. Notes on `renderer-cpu` as it is built (docs/PRDs/CPU-Renderer-PRD.md): what each milestone found, and
the numbers each port reports.

## Milestone 1: the seam

- **Two backends in one build.** Each backend's code and its compiled-in core live in `kv::<KV_RB_NS>`; render's
  dispatcher (`render/src/dispatch.cc`) is the only definition of `renderer.h`. kormake passes the bound backends as
  `KV_BIND_RENDER_<MODULE>=<place>` and `KV_BIND_RENDER_COUNT`. Linux links with `--start-group`: render's dispatcher
  calls into the backends that depend on render. render is built without a unity file, so a program that never calls
  the renderer (render's own tests) does not pull the dispatcher in.
- **Choosing.** `renderer_choose` runs before the window opens: `--renderer`, then `KV_RENDERER`, then each bound
  backend's `renderer_probe` in the binding's order. Vulkan's start-up no longer fails without a 1.3 loader when another
  backend is bound; its probe then says no.
- **The CPU backend so far** clears its target to the view's color, presents through `HostWindow::software_frame` (SDL's
  window surface on Linux; a DIB blitted with `SetDIBitsToDevice` on Windows, unbuilt here), and takes screenshots,
  with a window or headless (`gallery --headless`).

## The Slang spike

Can a game's fragment shader run on the CPU through `slangc -target cpp`? Yes, with three workarounds, at about 6 ns a
pixel for `gallery_rings` (one texture sample, a `length`, a `frac`, a `lerp`), scalar, `-O2`, one core. The spike's
wrapper and host are in `modules/renderer-cpu/notes/` (`spike_rings_cpu.slang.txt`, `spike_host.cc.txt`).

- **A compute-shaped wrapper.** The CPU target has no fragment stage. The engine's wrapper includes the game's file and
  defines a compute entry, `cs_span`, that steps the varyings across a span of pixels and calls `fs_main` for each.
- **`discard`** is refused outside a fragment stage. The wrapper defines `discard` as a macro returning a colour whose
  alpha no shader writes (-1e30) before including the game's file; the span reads it as "pixel kept".
- **The entry attribute.** Left on, `[shader("fragment")]` makes Slang's C++ take `fs_main`'s varyings by pointer while
  the wrapper's call passes them by value, and the generated C++ does not compile. The wrapper defines
  `shader(stage)` as `ForceInline`.
- **What the generated C++ needs.** Slang pastes its CPU prelude in, preprocessed with the host's system headers (9,300
  lines for one shader): it is generated and compiled on the platform it runs on. The shader's globals are one struct
  (`GlobalParams_0`, its fields named `gPush_0`, `gMaterials_0`, `gTextures_0`, ...); buffers are `{data, count}`.
  Textures are Slang's `ITexture`, a virtual interface the host implements (`Sample`, `SampleLevel`, `Load`,
  `GetDimensions`): the CPU renderer answers it with its own sampler. That interface is Slang's, at the shader
  boundary; the seam's no-virtuals rule stays the renderer's own.
- **Names carry Slang's suffixes** (`_0`), deterministically. The glue the shader rule will append to each generated
  file fills `GlobalParams_0` by those names, and a test will hold them.
