# The D3D12 renderer: what the port found

Nov 13, 2026. An exploration on the `d3d12` branch: a Direct3D 12 backend for the render
interface, beside Vulkan on Windows, as the road to Xbox (the GDK's d3d12 is the same API)
and to Windows on ARM. `modules/renderer-d3d12`, bound by `kormake.lua`'s
`bind "render" { windows = "renderer-d3d12" }`. Measured on an AMD Radeon 780M (driver
32.0.13074), Windows 11 26200, Slang 2026.17 with DXC 1.9.

**Result: it works, and it is mostly the Vulkan backend.** The gallery's nine scenes match
Vulkan's within the picture tool's tolerance (five bit-identical; the rest differ in 4 to 32
pixels of 518,400, at quad edges that sit exactly on pixel centers); arcfire's demo runs
700 frames, 18k sprites, three game shaders and the post pass with the debug layer on and
0 errors; every GPU sort check passes. Everything above the frame (the render world,
materials, texture slots, the frame ring, deferred destruction) is renderer-vulkan's code
with the prefix changed. Every SPIR-V the build makes, the engine's and the games', is
byte-identical to before the shader changes below, so Vulkan cannot have moved; Metal's
define path is untouched, but no Mac checked it.

## The sharp edges, and what each became

| Edge | What it is | What it became |
| --- | --- | --- |
| Base instance | The sprite vertex shader reads `gSorted[base + iid]` with `SV_StartInstanceLocation`. DXIL has that only from SM 6.8 (Agility SDK, new drivers); D3D12's `SV_InstanceID` excludes the base. | Under `KV_TARGET_D3D12`, `vs_main` takes `uint row : ROW` and the backend binds the sorted rows as an instance-rate vertex buffer: a per-instance fetch does honour `StartInstanceLocation`, so the indirect draws and the sort are unchanged. The same trick works on Vulkan and Metal, should one path be wanted everywhere. |
| One entry point per blob | SPIR-V and a Metal library hold every entry point of a file; a DXIL blob holds one (a DXIL *library* holds many, but a pipeline state cannot use one). kormake's shader rule is one output per `.slang`. | The rule writes `<stem>.dxil.zip`: slangc's own container, one signed blob per entry point (`children/<n>/<n>.dxil`, plus Slang's IR, ignored). `d3d_shader.cc` reads it through `pack` and `inflate` and finds each entry by the name in its PSV0 part. No build tool of ours. DXC 1.9 signs the blobs itself: no `dxil.dll`. |
| Bindings are registers | Slang assigns HLSL registers by declaration order, so a game shader that declares less of the interface gets different ones, and one root signature cannot serve every shader. | `render/shaders/binding.slangh`: `KV_REG(t0)` and friends pin each declaration's register under `KV_TARGET_D3D12` and expand to nothing elsewhere. |
| The shader format follows the backend, not the OS | Windows can now have either backend; a rule variant was per platform or toolchain family. | kormake: a rule's `platforms` may also name a module bound to an interface (`["renderer-d3d12"] = { ... }`), which wins. One `bind` line flips both the backend and the shaders. |
| Sampler arrays | `gSamplers[i.filter]` is an array a shader indexes; static samplers cannot back it (the debug layer refuses the pipeline). | A two-entry sampler heap and a root table. |
| Clip space | y up, as Metal's. | The Metal backend's flip of the projections; `post.slang` flips under either define. |
| `windows.h` | `minwindef.h` defines `near` and `far` as nothing, which eats `TimerWheel::far` in the next file of the unity build. | `#undef` after the include in `renderer_internal.h`. Any module that includes `windows.h` in a header will meet this. |
| Surface kinds | `RendererSurface` and `WindowSurface` gained `D3D12`. The frame loop and the gallery each map one to the other. | Two copies of the switch. `render` and `platform_hid` cannot see each other, so the map has no single home yet. |

## How the backend differs from Vulkan's, by design

- One root signature for the sort and the sprites: the push block as 28 root constants, the
  tables as root descriptors (a table that grows is rebound by address, nothing rewritten),
  the bindless textures as one unbounded SRV table per frame in flight, the samplers a table.
- Buffers remember their resource state; a transition goes where Vulkan has a barrier, and
  a null UAV barrier between sort passes.
- Indirect draws are `ExecuteIndirect` with a draw-only command signature: the sort's
  `uint4` per bucket is `D3D12_DRAW_ARGUMENTS` as it is `VkDrawIndirectCommand`.
- The swapchain is flip model: UNORM buffers, sRGB render target views. Frame pacing is the
  latency waitable object (where Vulkan waits in acquire). A resize waits for the GPU,
  since DXGI resizes only idle buffers.
- Every resource is committed. Few are made; a suballocator would be a later step.
- The debug layer's messages are drained into the log each frame (the info queue; the
  callback is Windows 11 only), counted like Vulkan's validation messages.
- GPU timing: timestamp and pipeline-statistics queries resolved to a readback buffer; the
  queue's clock calibration pairs a timestamp with QPC, which is `os_ticks` on Windows.
- The UI primitive (Krome's boxes and text): `sprite_ui.slang`'s `vs_ui` and `fs_ui`, its lists an
  upload-heap buffer read as a raw buffer through one more root SRV (`t8`; a slot with no
  lists yet binds the layers' buffer there, since every root parameter is set before a
  draw). One `DrawInstanced(6, items)` a layer, no vertex input, blended one, one minus
  source alpha. The gallery's `ui` picture is Vulkan's to the pixel, and `kromedemo --demo`
  runs under the debug layer with no message.
- Part of a texture (glyph pages): the rectangle staged at a footprint's pitch, and the
  live texture moved from shader read to copy destination and back around its copy.

## Not done

- Xbox: the GDK's headers (`d3d12_x.h`/`d3d12_xs.h`), its swapchain-less presentation
  and memory model are a platform of their own; nothing here was built against them.
- ARM64: nothing here is x64-specific, but no ARM64 toolchain or device tried it.
- Resizing and minimizing were written, not exercised: every run was scripted.
- Shader model 6.0 throughout; SM 6.6's `ResourceDescriptorHeap` would make the bindless
  table unnecessary, at the price of the oldest hardware.
- The shared CPU half (render world, materials, ring) was copied, not shared, when this was written: a third
  copy was the point to pull it out. Slice 9 did: `render`'s core (docs/design/renderer.md, "The core and the
  backends"). The surface kinds' two switches went with it (`renderer_window_surface`).
- kormake's own test (`tools/kormake/tests/run.sh`) now runs on Windows under Git Bash too,
  against the same expected files, and its fixture has a bound-module rule variant. The
  regenerated expectations were made on Windows; a Linux or macOS run should confirm them.
