// Korvid renderer-vulkan: the Vulkan 1.3 sprite renderer behind korvid/render/renderer.h.
// Dynamic rendering, synchronization2 and descriptor indexing; no render passes, no
// framebuffers. The render world is a persistent GPU sprite table, slot-addressed by the
// pool the deltas land in: renderer_apply writes rows, a frame copies only the rows that
// changed, compute sorts and culls them into buckets of (layer, blend) and writes one
// indirect draw per bucket, and the CPU records a draw per run of buckets it knows to be
// non-empty. Textures are bindless slots; materials are rows of a GPU table. The outer
// engine holds the handles; the only inputs are deltas, uploads and material edits.
// See renderer_internal.h for how the source is split.
#include "renderer_internal.h"
#include "korvid/core/hash.h"
#include "korvid/core/format.h"
#include "korvid/renderer-vulkan/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/profile.h"
#include "korvid/core/time.h"
#include "korvid/platform/module.h"
#include <string.h>

namespace kv {

// ---- shared helpers ----------------------------------------------------------------------

// ---- lifetime -------------------------------------------------------------------------------


Renderer* renderer_create(Engine& engine, const RendererDesc& desc) {
    RendererVulkanState* owner = engine_module_state_as<RendererVulkanState>(engine, "renderer-vulkan");
    if (!owner || !vkGetInstanceProcAddr) {
        KV_ERROR("vulkan", "renderer-vulkan module is not started");
        return nullptr;
    }
    if (owner->renderer) {
        KV_ERROR("vulkan", "a renderer already exists");
        return nullptr;
    }
    Allocator* ga = owner->general;
    Renderer* r = (Renderer*)ga->alloc(ga, sizeof(Renderer), alignof(Renderer));
    *r = Renderer{}; // plain data: default member values, no constructor to run
    r->general      = ga;
    r->owner_slot   = &owner->renderer;
    owner->renderer = r;
    for (FrameInFlight& f : r->frames) { f.garbage.alloc = ga; f.dirty_slots.alloc = ga; }
    r->uploads.alloc = ga;
    r->window      = desc.native_window;
    if (const PlatformState* ps = engine_module_state_as<PlatformState>(engine, "platform"))
        format(r->base_path, sizeof r->base_path, "%s", ps->base_path);
    r->start_time = time_now();
    r->validation  = desc.validation;
    r->vsync       = desc.vsync;
    r->want_width  = desc.width;
    r->want_height = desc.height;

    bool ok = rb_create_instance(r);
    if (ok && !vk_platform_create_surface(r->window, r->instance, &r->surface)) ok = false;
    ok = ok && rb_create_device(r);
    ok = ok && rb_create_frames(r);
    ok = ok && rb_gpu_profile_create(r);
    ok = ok && rc_create_pools(r);
    ok = ok && rb_create_tables(r, desc.sprite_rows > kInitialSpriteRows ? desc.sprite_rows : kInitialSpriteRows);
    ok = ok && rb_create_static_buffers(r);
    // The ring holds a frame's uploads for every frame in flight: at most every row of the
    // tables a frame (after a growth), so a hint for the tables is one for the ring too.
    VkDeviceSize ring = (VkDeviceSize)desc.sprite_rows * sizeof(GpuSprite) * kFramesInFlight;
    ok = ok && rc_ring_init(r, ring > kInitialRingBytes ? ring : kInitialRingBytes);
    ok = ok && rb_create_descriptors(r);
    ok = ok && rb_load_shaders(r);
    ok = ok && rb_create_default_texture(r);
    ok = ok && rb_create_compute_pipelines(r);
    ok = ok && rb_recreate_swapchain(r); // also builds the graphics pipelines, which need the format
    if (!ok) {
        renderer_destroy(r);
        return nullptr;
    }
    KV_INFO("vulkan", "renderer up: %u frames in flight, %u texture slots, %u material rows, %u buckets, validation %s",
            kFramesInFlight, kMaxTextures, kMaxMaterials, kBucketCount, r->validation ? "on" : "off");
    return r;
}

void renderer_destroy(Renderer* r) {
    if (!r) return;
    free_in(mem_general(), r->check_keys, sizeof(f32) * (r->check_rows ? r->check_rows : 1));
    r->check_keys = nullptr;
    if (r->device) {
        vkDeviceWaitIdle(r->device); // the one wait-idle: nothing is in flight after this
        rb_destroy_pipelines_now(r);
        rb_post_destroy_now(r);
        rb_destroy_swapchain_now(r);
        rb_destroy_frames(r); // runs every frame's garbage
        rb_destroy_resources(r);
        rb_gpu_profile_destroy(r);
        vkDestroyDevice(r->device, nullptr);
    } else {
        rb_destroy_resources(r); // pools and arrays only
    }
    if (r->surface)   vkDestroySurfaceKHR(r->instance, r->surface, nullptr);
    if (r->messenger) vkDestroyDebugUtilsMessengerEXT(r->instance, r->messenger, nullptr);
    if (r->instance)  vkDestroyInstance(r->instance, nullptr);
    KV_INFO("vulkan", "renderer down after %llu frames, %llu deltas, %llu rows uploaded, ring peak %llu KB (%u grows); %u validation errors, %u warnings",
            (unsigned long long)r->frame_count, (unsigned long long)r->world.deltas_applied, (unsigned long long)r->rows_uploaded,
            (unsigned long long)(r->ring.peak / 1024), r->ring.grows, r->validation_errors, r->validation_warnings);
    if (r->checks_passed || r->checks_failed) KV_INFO("vulkan", "sort checks: %u passed, %u failed", r->checks_passed, r->checks_failed);
    if (r->owner_slot) *r->owner_slot = nullptr;
    Allocator* ga = r->general;
    ga->free(ga, r, sizeof(Renderer));
}

Renderer* renderer_get(Engine& engine) {
    RendererVulkanState* s = engine_module_state_as<RendererVulkanState>(engine, "renderer-vulkan");
    return s ? s->renderer : nullptr;
}

// ---- textures -------------------------------------------------------------------------------

// ---- materials ------------------------------------------------------------------------------

// ---- the render world ---------------------------------------------------------------------

// The surface taken away, or a new one. Letting go: the GPU finishes, the swapchain and
// every retired one go (they were made from the surface and must not outlive it), then the
// surface. Taking: a surface for the new window, the swapchain built by the next frame.
bool renderer_set_surface(Renderer* r, void* native_window) {
    if (!r || !r->device) return false;
    if (r->surface) {
        vkDeviceWaitIdle(r->device);
        rb_destroy_swapchain_now(r);
        for (FrameInFlight& f : r->frames) {
            f.garbage_owned = f.garbage.len; // the device is idle: all of it
            rc_run_garbage(r, f);
        }
        vkDestroySurfaceKHR(r->instance, r->surface, nullptr);
        r->surface = VK_NULL_HANDLE;
        KV_INFO("vulkan", "surface released");
    }
    r->window = native_window;
    if (!native_window) return true;
    if (!vk_platform_create_surface(native_window, r->instance, &r->surface)) return false;
    VkBool32 present = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(r->gpu, r->queue_family, r->surface, &present);
    if (!present) { KV_ERROR("vulkan", "the new surface cannot be presented to from the renderer's queue"); return false; }
    r->swapchain_dirty = true;
    KV_INFO("vulkan", "surface taken");
    return true;
}

void renderer_resize(Renderer* r, u32 width, u32 height) {
    if (!r) return;
    r->want_width      = width;
    r->want_height     = height;
    r->swapchain_dirty = true;
}

// ---- the frame ------------------------------------------------------------------------------

bool renderer_render(Renderer* r, const RenderView& view) {
    if (!r) return false;
    FrameInFlight& f = r->frames[r->frame_index];

    // Wait for this slot's previous frame; what it queued for destruction can go, its span
    // of the ring is free, and its descriptor sets may be rewritten.
    f64 waited = 0.0;
    {
        KV_ZONE_WAIT("frame fence");
        f64 t = time_now();
        VK_CHECK(vkWaitForFences(r->device, 1, &f.fence, VK_TRUE, UINT64_MAX));
        waited += time_now() - t;
    }
    r->last_wait = 0.0;
    rb_gpu_collect(r, f); // its timestamps and counts, now that the fence has passed
    rc_ring_release(r, f);
    rc_run_garbage(r, f);
    if (r->world.sprites.len > r->tables.capacity) {
        // Growth remarks every row dirty, so the next frame uploads the table whole: logged
        // with the frame, so a hitch can be found by it (FrameLoopConfig::sprite_rows avoids it).
        KV_INFO("vulkan", "sprite tables grow at frame %llu: %u rows live, %u held", (unsigned long long)r->frame_count, r->world.sprites.len, r->tables.capacity);
        if (!rb_create_tables(r, r->world.sprites.len)) return false;
    }
    if (f.tables_dirty) rb_write_tables_set(r, f);
    // The layers' offsets for this frame: previous and current, the shader interpolates.
    f32* lo = (f32*)f.layers.mapped;
    for (u32 i = 0; i < kLayerCount; ++i) {
        lo[i * 4 + 0] = r->world.layer_prev[i][0]; lo[i * 4 + 1] = r->world.layer_prev[i][1];
        lo[i * 4 + 2] = r->world.layer_cur[i][0];  lo[i * 4 + 3] = r->world.layer_cur[i][1];
    }
    for (u32 slot : f.dirty_slots) rb_write_texture_slot(r, f.textures, slot);
    f.dirty_slots.clear();

    // Without a surface (Android has taken it) the frame runs its uploads and presents nothing.
    if (r->surface && r->swapchain_dirty && !rb_recreate_swapchain(r)) return false;

    bool have_image = r->surface && r->swapchain.handle != VK_NULL_HANDLE;
    u32  image_index = 0;
    if (have_image) {
        KV_ZONE_WAIT("acquire");
        f64 t = time_now();
        VkResult acq = vkAcquireNextImageKHR(r->device, r->swapchain.handle, UINT64_MAX, f.acquire,
                                             VK_NULL_HANDLE, &image_index);
        waited += time_now() - t;
        if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
            r->swapchain_dirty = true; // the semaphore was not signaled; the frame still runs its uploads
            have_image = false;
        } else if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
            KV_ERROR("vulkan", "vkAcquireNextImageKHR: %s", vk_result_name(acq));
            return false;
        } else if (acq == VK_SUBOPTIMAL_KHR && !r->swapchain.unrotated) {
            r->swapchain_dirty = true; // rebuild next frame; draw this one
        }
    }

    if (!rb_record_frame(r, f, have_image, image_index, view)) return false;
    VK_CHECK(vkResetFences(r->device, 1, &f.fence)); // only once the submit is certain

    VkSemaphoreSubmitInfo wait{};
    wait.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    wait.semaphore = f.acquire;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signal{};
    signal.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal.semaphore = have_image ? r->swapchain.present[image_index] : VK_NULL_HANDLE;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cbi{};
    cbi.sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cbi.commandBuffer = f.cmd;
    VkSubmitInfo2 si{};
    si.sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    si.waitSemaphoreInfoCount   = have_image ? 1 : 0;
    si.pWaitSemaphoreInfos      = &wait;
    si.commandBufferInfoCount   = 1;
    si.pCommandBufferInfos      = &cbi;
    si.signalSemaphoreInfoCount = have_image ? 1 : 0;
    si.pSignalSemaphoreInfos    = &signal;
    f.submit_ticks = os_ticks(); // where an uncalibrated GPU timeline starts
    VK_CHECK(vkQueueSubmit2(r->queue, 1, &si, f.fence));
    f.submitted = true;
    f.garbage_owned = f.garbage.len; // destroyed when its fence passes; anything queued later waits for the next
    rc_ring_submitted(r, f);

    if (have_image) {
        KV_ZONE_WAIT("present");
        VkPresentInfoKHR pi{};
        pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores    = &r->swapchain.present[image_index];
        pi.swapchainCount     = 1;
        pi.pSwapchains        = &r->swapchain.handle;
        pi.pImageIndices      = &image_index;
        f64 t = time_now();
        VkResult pres = vkQueuePresentKHR(r->queue, &pi);
        waited += time_now() - t;
        // An unrotated swapchain on a turned screen is suboptimal for as long as it lives,
        // by choice (vk_swapchain.cc): only a resize rebuilds it.
        if (pres == VK_ERROR_OUT_OF_DATE_KHR || (pres == VK_SUBOPTIMAL_KHR && !r->swapchain.unrotated)) r->swapchain_dirty = true;
        else if (pres != VK_SUCCESS && pres != VK_SUBOPTIMAL_KHR) KV_ERROR("vulkan", "vkQueuePresentKHR: %s", vk_result_name(pres));
    }

    if (r->shot_recorded || r->check_recorded) { // a dev stall: wait for this frame and read it back now
        VK_CHECK(vkWaitForFences(r->device, 1, &f.fence, VK_TRUE, UINT64_MAX));
        if (r->shot_recorded) rc_write_screenshot(r);
        if (r->check_recorded) rc_verify_sort(r);
    }

    r->last_wait = waited;
    r->frame_index = (r->frame_index + 1) % kFramesInFlight;
    ++r->frame_count;
    return have_image;
}

} // namespace kv
