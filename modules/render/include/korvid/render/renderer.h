// Korvid render: the renderer as the game sees it. Link-time dispatch: the bound backend
// module defines these. No graphics API type appears here; the window is an opaque handle.
//
// Resources are renderer-issued handles wrapping GPU indices: a texture handle's index
// is its bindless slot, a material handle's index is its row. The outer engine keeps
// them and copies the numbers; it never resolves anything per frame. Everything that
// differs between backends is negotiated once through RenderCaps.
#pragma once
#include "korvid/render/material.h"
#include "korvid/render/sprite.h"

namespace kv {

struct Engine;
struct Renderer; // opaque; owned by the backend module, which destroys it at its shutdown

struct RendererDesc {
    void* native_window = nullptr; // HostWindow::native_handle()
    u32   width         = 0;       // current framebuffer size in pixels
    u32   height        = 0;
    bool  vsync         = true;
    bool  validation    = KV_DEBUG != 0; // request the validation layers when present
    u32   sprite_rows   = 0;       // the sprite tables' first size, a hint (0: the backend's default); they grow past it
};

// Pixel formats the outer engine may hand over. Cooked assets arrive in one of these;
// the runtime never transcodes.
enum class TextureFormat : u8 { RGBA8_SRGB, RGBA8_UNORM, Count };

struct TextureDesc {
    TextureFormat format = TextureFormat::RGBA8_SRGB;
    const char*   name   = ""; // for tools; copied
};

// What the backend is, once, at startup.
struct RenderCaps {
    const char* backend          = "";
    u32         max_textures     = 0;    // bindless slots, including the default at 0
    u32         max_materials    = 0;
    u32         frames_in_flight = 0;
    bool        srgb_output      = false; // the swapchain encodes linear to sRGB in hardware
    bool        texture_formats[(u32)TextureFormat::Count] = {};
};

struct RenderView {
    f32   alpha = 0.0f;    // interpolation point between previous and current tick, [0, 1)
    rect2 view;            // the world rect that fills the window, +y up, before rotation
    f32   rotation = 0.0f; // the camera's rotation about the rect's center, radians counter-clockwise
    color clear_color{0.09f, 0.09f, 0.11f, 1.0f};
    // Screen-space sprites are placed in window pixels, center origin, +y up; the renderer
    // knows the window's size and needs nothing more for them.
};

// What kind of window surface the bound backend draws into, for whoever opens the window
// before a renderer exists (the frame loop maps it to platform_hid's WindowSurface). Known
// at link time: the binding decides it.
enum class RendererSurface : u8 { Vulkan, Metal, D3D12 };
RendererSurface renderer_surface();

// One renderer per process in slice 1. Null if one exists already or creation failed.
Renderer*  renderer_create(Engine& engine, const RendererDesc& desc);
// Early teardown only; the backend module's shutdown destroys whatever is still alive.
void       renderer_destroy(Renderer* r);
// The process's renderer, or null before renderer_create.
Renderer*  renderer_get(Engine& engine);
RenderCaps renderer_caps(const Renderer* r);

// The window's framebuffer size changed; the backend recreates what depends on it.
void       renderer_resize(Renderer* r, u32 width, u32 height);
// The window's surface is going away (native_window null: the backend waits for the GPU
// and lets go of everything made from it, before returning) or there is a new one (the
// backend draws to it from the next frame). For an OS that takes the surface while the
// program runs: Android, through platform_surface_hook. Between the two no frame may be
// drawn; renderer_frame without a surface runs its uploads and presents nothing. A backend
// whose surface lives as long as its window returns true and does nothing.
bool       renderer_set_surface(Renderer* r, void* native_window);

// ---- the render world -------------------------------------------------------------
// Applies a tick's worth (or a frame's worth) of sprite deltas to the persistent world.
void       renderer_apply(Renderer* r, Span<const SpriteDelta> deltas);
u32        renderer_sprite_count(const Renderer* r);

// Draws the world as of the last deltas, interpolated by view.alpha, and presents.
// Uploads (textures, changed rows) are submitted even when nothing is presented.
// Returns false when nothing was presented (zero-size window, swapchain being
// rebuilt); that is not an error.
bool       renderer_render(Renderer* r, const RenderView& view);

// Seconds the last renderer_render spent blocked on the GPU or the display (fences,
// acquire, present). The frame loop reads it as its slack: time it may spend on idle
// work before the next render without delaying a frame.
f64        renderer_last_wait(const Renderer* r);

// What the GPU did with the last frame whose results are in (the frame before the one
// rendering now), when the backend measures it: in a dev build while the profiler records,
// by timestamps around the frame's passes and a count of the shader invocations. `valid`
// is false otherwise. The same spans go to the profiler's "gpu" track.
struct RendererGpuStats {
    bool valid = false;
    f64  frame_ms = 0.0, upload_ms = 0.0, sort_ms = 0.0, draw_ms = 0.0;
    u64  vertices = 0, fragments = 0, compute = 0; // shader invocations; 0 when the device cannot count them
    f32  overdraw = 0.0f;                          // fragments over the frame's pixels
};
RendererGpuStats renderer_gpu_stats(const Renderer* r);

// Asks for the next presented frame as a binary PPM at `path`. A dev facility: the frame
// loop's --screenshot. The frame that carries it waits for its own fence.
bool       renderer_screenshot(Renderer* r, const char* path);
// Reads the next frame's GPU sort back and verifies it against the rows: every bucket in
// order, rows ascending within a bucket, nothing missing or doubled. The frame loop's
// --check-sort. Logs the result; the renderer counts passes and failures.
bool       renderer_check_sort(Renderer* r);

// ---- the full-frame pass ----------------------------------------------------------
// Layers below `layer` are drawn into a target of the renderer's own; a game's post shader
// (a fragment shader in <exe>/shaders/, named as a material's is, its interface in
// render's shaders/post_fragment.slangh) then draws that image to the window, and the
// layers at and above `layer` draw over it (the HUD, the UI). A null shader turns the pass
// off: the frame is drawn straight to the window, as without one, at no cost. A shader
// that will not load leaves the pass off, logged once; false then.
constexpr u32 KV_POST_PARAMS = 16; // float4s
bool       renderer_post_layer(Renderer* r, i32 layer, const char* shader);
// The post shader's parameters for the frames from the next one on: `count` float4s (at
// most KV_POST_PARAMS; more are dropped), copied.
void       renderer_post_params(Renderer* r, const f32* params, u32 count);

// ---- textures ---------------------------------------------------------------------
// A new texture's slot shows the default texture until the first upload lands, so a
// pending asset renders as a placeholder through the same slot. The slot is the stable
// thing; an upload is a whole image of any size, tightly packed, and replaces what the
// slot showed before. Resident before the next frame that references it.
Handle renderer_texture_create(Renderer* r, const TextureDesc& desc);
bool   renderer_texture_upload(Renderer* r, Handle texture, u32 width, u32 height, const void* pixels, usize bytes);
void   renderer_texture_destroy(Renderer* r, Handle texture); // deferred past the frames in flight
u32    renderer_texture_slot(const Renderer* r, Handle texture); // the GPU index; 0 if stale

// ---- materials --------------------------------------------------------------------
// A material is a row of a GPU table; the handle's index is the row. Sprites refer to the
// row, so an edit reaches every sprite that uses it at the next frame.
Handle renderer_material_create(Renderer* r, const MaterialDesc& desc);
bool   renderer_material_set(Renderer* r, Handle material, const MaterialDesc& desc);
bool   renderer_material_get(const Renderer* r, Handle material, MaterialDesc* out); // false if stale
void   renderer_material_destroy(Renderer* r, Handle material); // its sprites fall back to the default
Handle renderer_default_material(const Renderer* r); // default texture, alpha blend, white

} // namespace kv
