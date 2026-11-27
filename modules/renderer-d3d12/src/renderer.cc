// Korvid renderer-d3d12: the Direct3D 12 sprite renderer behind korvid/render/renderer.h,
// the same design as renderer-vulkan's: a persistent GPU sprite table, slot-addressed by the
// pool the deltas land in; a frame copies only the rows that changed; compute sorts and culls
// them into buckets of (layer, blend) and writes one indirect draw per bucket; the CPU
// records an ExecuteIndirect per run of buckets it knows to be non-empty. Textures are
// bindless slots in a descriptor table; materials are rows of a GPU table. Everything above
// the frame (the render world, materials, the texture slots) is the Vulkan backend's code,
// word for word but the prefix: renderer_internal.h says where the two part.
#include "renderer_internal.h"
#include "korvid/core/hash.h"
#include "korvid/core/format.h"
#include "korvid/renderer-d3d12/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/profile.h"
#include "korvid/core/time.h"
#include "korvid/platform/module.h"
#include <string.h>

namespace kv { namespace KV_RB_NS {

// ---- shared helpers ----------------------------------------------------------------------

// ---- lifetime -------------------------------------------------------------------------------


Renderer* renderer_create(Engine& engine, const RendererDesc& desc) {
    RendererD3d12State* owner = engine_module_state_as<RendererD3d12State>(engine, "renderer-d3d12");
    if (!owner) {
        KV_ERROR("d3d12", "renderer-d3d12 module is not started");
        return nullptr;
    }
    if (owner->renderer) {
        KV_ERROR("d3d12", "a renderer already exists");
        return nullptr;
    }
    Allocator* ga = owner->general;
    Renderer* r = (Renderer*)ga->alloc(ga, sizeof(Renderer), alignof(Renderer));
    *r = Renderer{};
    r->general      = ga;
    r->owner_slot   = &owner->renderer;
    owner->renderer = reinterpret_cast<kv::Renderer*>(r); // the public, opaque type: render's dispatcher reads its first byte
    for (FrameInFlight& f : r->frames) { f.garbage.alloc = ga; f.dirty_slots.alloc = ga; }
    r->uploads.alloc = ga;
    r->window = (HWND)desc.native_window;
    if (platform_io(engine)) format(r->base_path, sizeof r->base_path, "%s", platform_base_dir(engine)); // the platform is up
    r->io = platform_io(engine);
    r->start_time  = time_now();
    r->validation  = desc.validation;
    r->markers     = desc.validation || KV_DEV;
    r->vsync       = desc.vsync;
    r->transparent = desc.transparent;
    r->want_width  = desc.width;
    r->want_height = desc.height;

    bool ok = rb_create_device(r);
    ok = ok && rb_create_frames(r);
    ok = ok && rb_gpu_profile_create(r);
    ok = ok && rc_create_pools(r);
    ok = ok && rb_create_tables(r, desc.sprite_rows > kInitialSpriteRows ? desc.sprite_rows : kInitialSpriteRows);
    ok = ok && rb_create_static_buffers(r);
    u64 ring = (u64)desc.sprite_rows * sizeof(GpuSprite) * kFramesInFlight;
    ok = ok && rc_ring_init(r, ring > kInitialRingBytes ? ring : kInitialRingBytes);
    ok = ok && rb_create_heaps_and_root(r);
    ok = ok && rb_load_shaders(r);
    ok = ok && rc_create_defaults(r);
    ok = ok && rb_create_compute_pipelines(r);
    ok = ok && rb_recreate_swapchain(r); // also builds the graphics pipelines
    if (r->device) rb_drain_messages(r);
    if (!ok) {
        renderer_destroy(r);
        return nullptr;
    }
    KV_INFO("d3d12", "renderer up on %s: %u frames in flight, %u texture slots, %u material rows, %u buckets, debug layer %s",
            r->adapter_name, kFramesInFlight, kMaxTextures, kMaxMaterials, kBucketCount, r->info_queue ? "on" : "off");
    return r;
}

void renderer_destroy(Renderer* r) {
    if (!r) return;
    if (r->io) r->io->drain(r); // shaders asked for and not come: none lands after this
    free_in(mem_general(), r->check_keys, sizeof(f32) * (r->check_rows ? r->check_rows : 1));
    r->check_keys = nullptr;
    if (r->device) {
        rb_wait_idle(r); // the one wait-idle: nothing is in flight after this
        rb_destroy_pipelines(r);
        rb_post_destroy(r);
        for (Surface& s : r->surfaces) if (s.live) { rb_surface_destroy(r, s); s.live = false; }
        rb_destroy_swapchain(r);
        rb_destroy_frames(r); // runs every frame's garbage
        rb_destroy_resources(r);
        rb_gpu_profile_destroy(r);
        rb_drain_messages(r);
    } else {
        rb_destroy_resources(r);
    }
    rb_destroy_device(r);
    KV_INFO("d3d12", "renderer down after %llu frames, %llu deltas, %llu rows uploaded, ring peak %llu KB (%u grows); %u debug layer errors, %u warnings",
            (unsigned long long)r->frame_count, (unsigned long long)r->world.deltas_applied, (unsigned long long)r->rows_uploaded,
            (unsigned long long)(r->ring.peak / 1024), r->ring.grows, r->validation_errors, r->validation_warnings);
    if (r->checks_passed || r->checks_failed) KV_INFO("d3d12", "sort checks: %u passed, %u failed", r->checks_passed, r->checks_failed);
    if (r->owner_slot) *r->owner_slot = nullptr;
    Allocator* ga = r->general;
    ga->free(ga, r, sizeof(Renderer));
}

Renderer* renderer_get(Engine& engine) {
    RendererD3d12State* s = engine_module_state_as<RendererD3d12State>(engine, "renderer-d3d12");
    return s ? reinterpret_cast<Renderer*>(s->renderer) : nullptr;
}

// Whether this backend can draw here before a window exists. The device is found by
// renderer_create; one that will not do fails there, and render's dispatcher falls back to
// the next bound backend.
bool renderer_probe(const char** why) {
    *why = "";
    return true;
}


// ---- textures -------------------------------------------------------------------------------

// ---- materials ------------------------------------------------------------------------------

// ---- the render world ---------------------------------------------------------------------

// The swapchain's window lives as long as the renderer: nothing to let go of.
bool renderer_set_surface(Renderer*, void*) { return true; }

void renderer_resize(Renderer* r, u32 width, u32 height) {
    if (!r) return;
    r->want_width      = width;
    r->want_height     = height;
    r->swapchain_dirty = true;
}

// ---- the frame ------------------------------------------------------------------------------

// The steps of the core's frame (korvid/render/core/frame.inl) that wait, acquire, submit
// and present. renderer_render itself is the core's.

bool rb_frame_wait(Renderer* r, FrameInFlight& f, f64* waited) {
    {
        KV_ZONE_WAIT("frame fence");
        f64 t = time_now();
        if (!rb_wait_fence(r, f.fence_value)) return false;
        *waited += time_now() - t;
    }
    rb_gpu_collect(r, f);
    return true;
}

void rb_frame_slot(Renderer* r, FrameInFlight& f, u32 slot) { rb_write_texture_slot(r, (u32)(&f - r->frames), slot); }

bool rb_frame_acquire(Renderer* r, FrameInFlight& f, f64* waited) {
    rb_shaders_pump(r); // the game and post shaders that have come since the last frame
    if (r->swapchain_dirty && !rb_recreate_swapchain(r)) return false;
    f.have_image = r->swapchain.handle != nullptr && !r->main_skipped; // skipped: only other windows draw
    f.image      = 0;
    if (f.have_image) {
        // The frame latency object: signaled once a back buffer is free to draw into. What
        // vkAcquireNextImageKHR's wait is on Vulkan.
        KV_ZONE_WAIT("acquire");
        f64 t = time_now();
        WaitForSingleObjectEx(r->swapchain.waitable, 1000, TRUE);
        *waited += time_now() - t;
        f.image = r->swapchain.handle->GetCurrentBackBufferIndex();
    }
    f.width  = f.have_image ? r->swapchain.width : 0;
    f.height = f.have_image ? r->swapchain.height : 0;
    return true;
}

bool rb_frame_submit(Renderer* r, FrameInFlight& f) {
    ID3D12CommandList* lists[] = {f.cmd};
    f.submit_ticks = os_ticks();
    r->queue->ExecuteCommandLists(1, lists);
    D3D_CHECK(r->queue->Signal(r->fence, r->fence_next));
    f.fence_value = r->fence_next++;
    return true;
}

// Presents when there is an image, then drains the debug layer's messages for the frame.
bool rb_frame_present(Renderer* r, FrameInFlight& f, f64* waited) {
    if (f.have_image) {
        KV_ZONE_WAIT("present");
        f64 t = time_now();
        UINT flags = !r->vsync && r->swapchain.tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
        HRESULT hr = r->swapchain.handle->Present(r->vsync ? 1 : 0, flags);
        *waited += time_now() - t;
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            KV_ERROR("d3d12", "Present: %s (removed: %s)", hr_name(hr), hr_name(r->device->GetDeviceRemovedReason()));
            rb_drain_messages(r);
            return false;
        }
        if (FAILED(hr)) KV_ERROR("d3d12", "Present: %s", hr_name(hr));
    }
    rb_drain_messages(r);
    return true;
}

bool rb_frame_finish(Renderer* r, FrameInFlight& f) { return rb_wait_fence(r, f.fence_value); }

} } // namespace kv::KV_RB_NS
