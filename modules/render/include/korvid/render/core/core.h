// Korvid render core: the half of the renderer that is compiled into each backend, and the
// contract between the two (docs/PRDs/Engine-Slice-9-PRD.md). The core shares structure, it
// does not hide the backends: it is built inside the bound backend's library, after that
// backend's renderer_internal.h, and sees its types whole.
//
// How a backend takes part. Its src/render_core.cc (.mm for Metal) is two lines:
//
//     #include "renderer_internal.h"
//     #include "korvid/render/core/core.inl"
//
// and its renderer_internal.h, before it includes this header, defines:
//
//     KV_RB_NAME         "vulkan": the log channel and RenderCaps::backend
//     KV_RB_CLIP_Y_UP    0 or 1: whether clip space has y up, so the core flips the
//                        projections it pushes (ortho2d targets Vulkan's, y down)
//     KV_RB_SHADER_EXT   ".spv": what follows a compiled shader's name in <exe>/shaders/
//     KV_RB_SURFACE      the RendererSurface the backend draws into
//
// The core uses these for their values and never asks which backend it is: there is no
// `#if` on a backend's name in the core.
//
// The rules of the seam:
//   1. No templates over the backend, no virtual functions, no tables of function pointers.
//      Plain structs and plain functions: a name has one definition in a build.
//   2. A function is the core's when its bodies in the backends are the same once the names
//      are normalised. If they are only alike, it stays in the backends.
//   3. The core does not reach into a backend's own state. The backends read and write the
//      core's fields freely: no accessors, no context structs.
//   4. The contract is at the height of a pass ("copy these rows", "draw this run of
//      buckets"), never of an API call.
//   5. What needs no GPU type is not here but in render's linked half (korvid/render/world.h),
//      where it has tests that run without a GPU. A piece that has to be argued into either
//      half is in the wrong one.
//
// Names: rw_ is the linked half's, rc_ this half's, rb_ a backend's.
#pragma once
#include "korvid/render/core/layout.h"

#ifndef KV_RB_NAME
#error "the backend's renderer_internal.h defines KV_RB_NAME before including korvid/render/core/core.h"
#endif
#ifndef KV_RB_CLIP_Y_UP
#error "the backend's renderer_internal.h defines KV_RB_CLIP_Y_UP (0 or 1)"
#endif
#ifndef KV_RB_SHADER_EXT
#error "the backend's renderer_internal.h defines KV_RB_SHADER_EXT"
#endif
#ifndef KV_RB_SURFACE
#error "the backend's renderer_internal.h defines KV_RB_SURFACE"
#endif
