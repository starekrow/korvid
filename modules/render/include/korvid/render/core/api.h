// The renderer's API as render's dispatcher and the backends share it (docs/PRDs/
// CPU-Renderer-PRD.md, "Two backends in one build"). Each backend compiles the core in a
// namespace of its own (kv::rb_vk, kv::rb_cpu: its KV_RB_NS) and so defines every function
// below there; render/src/dispatch.cc defines renderer.h's once, in kv, as a switch over the
// backends the build binds, on the byte a renderer starts with (RenderCore::backend_id).
// With one backend bound the switch has one case. The API is not the hot path: it is called
// a few times a frame and per resource, never per sprite.
#pragma once
#include "korvid/render/renderer.h"
#include "korvid/render/ui.h"

// A backend's byte (its KV_RB_ID).
#define KV_RENDER_ID_VULKAN 1
#define KV_RENDER_ID_METAL  2
#define KV_RENDER_ID_D3D12  3
#define KV_RENDER_ID_CPU    4

// Every function of the API that takes a renderer, as X(return type, name, (parameters),
// (arguments)): the renderer parameter is r_ and the arguments pass r, which the dispatcher
// makes the backend's own pointer.
#define KV_RENDER_API(X) \
    X(void, renderer_destroy, (Renderer* r_), (r)) \
    X(RenderCaps, renderer_caps, (const Renderer* r_), (r)) \
    X(void, renderer_resize, (Renderer* r_, u32 width, u32 height), (r, width, height)) \
    X(bool, renderer_set_surface, (Renderer* r_, void* native_window), (r, native_window)) \
    X(void, renderer_apply, (Renderer* r_, Span<const SpriteDelta> deltas), (r, deltas)) \
    X(u32, renderer_sprite_count, (const Renderer* r_), (r)) \
    X(bool, renderer_render, (Renderer* r_, const RenderView& view), (r, view)) \
    X(f64, renderer_last_wait, (const Renderer* r_), (r)) \
    X(RendererGpuStats, renderer_gpu_stats, (const Renderer* r_), (r)) \
    X(bool, renderer_screenshot, (Renderer* r_, const char* path), (r, path)) \
    X(bool, renderer_check_sort, (Renderer* r_), (r)) \
    X(bool, renderer_post_layer, (Renderer* r_, i32 layer, const char* shader), (r, layer, shader)) \
    X(void, renderer_post_params, (Renderer* r_, const f32* params, u32 count), (r, params, count)) \
    X(Handle, renderer_texture_create, (Renderer* r_, const TextureDesc& desc), (r, desc)) \
    X(bool, renderer_texture_upload, (Renderer* r_, Handle texture, u32 width, u32 height, const void* pixels, usize bytes), (r, texture, width, height, pixels, bytes)) \
    X(bool, renderer_texture_update, (Renderer* r_, Handle texture, const void* pixels, u32 width, u32 height, u32 x, u32 y, u32 w, u32 h), (r, texture, pixels, width, height, x, y, w, h)) \
    X(void, renderer_texture_destroy, (Renderer* r_, Handle texture), (r, texture)) \
    X(u32, renderer_texture_slot, (const Renderer* r_, Handle texture), (r, texture)) \
    X(Handle, renderer_material_create, (Renderer* r_, const MaterialDesc& desc), (r, desc)) \
    X(bool, renderer_material_set, (Renderer* r_, Handle material, const MaterialDesc& desc), (r, material, desc)) \
    X(bool, renderer_material_get, (const Renderer* r_, Handle material, MaterialDesc* out), (r, material, out)) \
    X(void, renderer_material_destroy, (Renderer* r_, Handle material), (r, material)) \
    X(Handle, renderer_default_material, (const Renderer* r_), (r)) \
    X(void, renderer_ui_set, (Renderer* r_, i32 layer, const UiList& list), (r, layer, list)) \
    X(void, renderer_ui_clear, (Renderer* r_, i32 layer), (r, layer)) \
    X(void, renderer_ui_set, (Renderer* r_, u32 surface, i32 layer, const UiList& list), (r, surface, layer, list)) \
    X(void, renderer_ui_clear, (Renderer* r_, u32 surface, i32 layer), (r, surface, layer)) \
    X(RendererUiStats, renderer_ui_stats, (const Renderer* r_), (r)) \
    X(bool, renderer_surface_add, (Renderer* r_, u32 surface, void* native_window, u32 width, u32 height, bool transparent), (r, surface, native_window, width, height, transparent)) \
    X(void, renderer_surface_remove, (Renderer* r_, u32 surface), (r, surface)) \
    X(void, renderer_surface_resize, (Renderer* r_, u32 surface, u32 width, u32 height), (r, surface, width, height)) \
    X(void, renderer_surface_visible, (Renderer* r_, u32 surface, bool visible), (r, surface, visible)) \
    X(bool, renderer_surface_pending, (const Renderer* r_), (r)) \
    X(u64, renderer_surface_presents, (const Renderer* r_, u32 surface), (r, surface))

// What each backend defines in its namespace besides KV_RENDER_API: making and finding its
// renderer, its surfaces, and whether it can draw here at all (the loader there, a device; a
// reason when not, for the log). Each backend's own (renderer.cc), not the core's.
#define KV_RENDER_BACKEND_API \
    struct Renderer; \
    Renderer* renderer_create(Engine& engine, const RendererDesc& desc); \
    Renderer* renderer_get(Engine& engine); \
    RendererSurface renderer_surface(); \
    u8 renderer_window_surface(); \
    bool renderer_probe(const char** why);
