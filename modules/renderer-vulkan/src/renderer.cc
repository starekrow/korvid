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

u32 rv_material_row_of(const Renderer* r, Handle material) {
    return r->materials.valid(material) ? material.index() : 0;
}

u32 rv_bucket_of(i32 layer, u32 slot) {
    if (layer < kLayerMin) layer = kLayerMin;
    if (layer > kLayerMax) layer = kLayerMax;
    u32 b = slot < kSlotCount ? slot : (u32)BlendMode::Alpha;
    return (u32)(layer - kLayerMin) * kSlotCount + b;
}

const char* blend_mode_name(BlendMode mode) {
    switch (mode) {
    case BlendMode::Opaque:   return "opaque";
    case BlendMode::Alpha:    return "alpha";
    case BlendMode::Additive: return "additive";
    default:                  return "?";
    }
}

bool blend_mode_parse(const char* s, BlendMode* out) {
    if (!s) return false;
    if (strcmp(s, "opaque") == 0)   { *out = BlendMode::Opaque;   return true; }
    if (strcmp(s, "alpha") == 0)    { *out = BlendMode::Alpha;    return true; }
    if (strcmp(s, "additive") == 0) { *out = BlendMode::Additive; return true; }
    return false;
}

namespace {

void mark_sprite_dirty(Renderer* r, u32 slot) {
    u32 w = slot >> 6;
    if (w >= r->sprite_dirty.len) r->sprite_dirty.resize_fill(w + 1, 0);
    r->sprite_dirty[w] |= (u64)1 << (slot & 63);
}

void write_placement(f32 out[4], f32* scale_y, const Placement2& p) {
    out[0] = p.position.x; out[1] = p.position.y; out[2] = p.rotation; out[3] = p.scale.x;
    *scale_y = p.scale.y;
}

} // namespace

// ---- lifetime -------------------------------------------------------------------------------

RendererSurface renderer_surface() { return RendererSurface::Vulkan; }

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
    r->window      = desc.native_window;
    if (const PlatformState* ps = engine_module_state_as<PlatformState>(engine, "platform"))
        format(r->base_path, sizeof r->base_path, "%s", ps->base_path);
    r->start_time = time_now();
    r->validation  = desc.validation;
    r->vsync       = desc.vsync;
    r->want_width  = desc.width;
    r->want_height = desc.height;

    bool ok = rv_create_instance(r);
    if (ok && !vk_platform_create_surface(r->window, r->instance, &r->surface)) ok = false;
    ok = ok && rv_create_device(r);
    ok = ok && rv_create_frames(r);
    ok = ok && rv_gpu_profile_create(r);
    ok = ok && rv_create_resource_pools(r);
    ok = ok && rv_create_tables(r, desc.sprite_rows > kInitialSpriteRows ? desc.sprite_rows : kInitialSpriteRows);
    ok = ok && rv_create_static_buffers(r);
    // The ring holds a frame's uploads for every frame in flight: at most every row of the
    // tables a frame (after a growth), so a hint for the tables is one for the ring too.
    VkDeviceSize ring = (VkDeviceSize)desc.sprite_rows * sizeof(GpuSprite) * kFramesInFlight;
    ok = ok && rv_ring_init(r, ring > kInitialRingBytes ? ring : kInitialRingBytes);
    ok = ok && rv_create_descriptors(r);
    ok = ok && rv_load_shaders(r);
    ok = ok && rv_create_default_texture(r);
    ok = ok && rv_create_compute_pipelines(r);
    ok = ok && rv_recreate_swapchain(r); // also builds the graphics pipelines, which need the format
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
        rv_destroy_pipelines_now(r);
        rv_post_destroy_now(r);
        rv_destroy_swapchain_now(r);
        rv_destroy_frames(r); // runs every frame's garbage
        rv_destroy_resources(r);
        rv_gpu_profile_destroy(r);
        vkDestroyDevice(r->device, nullptr);
    } else {
        rv_destroy_resources(r); // pools and arrays only
    }
    if (r->surface)   vkDestroySurfaceKHR(r->instance, r->surface, nullptr);
    if (r->messenger) vkDestroyDebugUtilsMessengerEXT(r->instance, r->messenger, nullptr);
    if (r->instance)  vkDestroyInstance(r->instance, nullptr);
    KV_INFO("vulkan", "renderer down after %llu frames, %llu deltas, %llu rows uploaded, ring peak %llu KB (%u grows); %u validation errors, %u warnings",
            (unsigned long long)r->frame_count, (unsigned long long)r->deltas_applied, (unsigned long long)r->rows_uploaded,
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

RenderCaps renderer_caps(const Renderer*) {
    RenderCaps c;
    c.backend          = "vulkan";
    c.max_textures     = kMaxTextures;
    c.max_materials    = kMaxMaterials;
    c.frames_in_flight = kFramesInFlight;
    c.srgb_output      = true;
    c.texture_formats[(u32)TextureFormat::RGBA8_SRGB]  = true;
    c.texture_formats[(u32)TextureFormat::RGBA8_UNORM] = true;
    return c;
}

namespace {

// A name whose holder died goes to the next live record with the same name, if any.
template <class T>
void rename_after_destroy(Pool<T>& pool, Map<u64, Handle>& by_name, Name name, Handle gone) {
    Handle* at = by_name.find(name.hash);
    if (!at || *at != gone) return;
    for (u32 i = 0; i < pool.len; ++i)
        if (pool.is_alive(i) && pool.items[i].name == name) { *at = pool.handle_at(i); return; }
    by_name.remove(name.hash);
}

} // namespace

// ---- textures -------------------------------------------------------------------------------

Handle renderer_texture_create(Renderer* r, const TextureDesc& desc) {
    if (!r || (u32)desc.format >= (u32)TextureFormat::Count) return Handle{};
    Texture t{};
    t.format = desc.format;
    ++r->textures_made;
    if (desc.name && desc.name[0]) t.name = name_intern(desc.name);
    else {
        char derived[32];
        format(derived, sizeof derived, "texture:%u", r->textures_made);
        t.name = name_intern(derived);
    }
    Handle h = r->textures.create(t);
    if (h.is_null()) { KV_ERROR("vulkan", "out of texture slots (%u)", kMaxTextures); return h; }
    r->texture_by_name.get_or_insert(t.name.hash, h); // the first live one keeps the name
    rv_mark_slot_dirty(r, h.index()); // shows the default until the upload lands
    return h;
}

bool renderer_texture_upload(Renderer* r, Handle texture, u32 width, u32 height, const void* pixels, usize bytes) {
    Texture* t = r ? r->textures.get(texture) : nullptr;
    if (!t || texture.index() == 0 || !pixels) return false;
    if (width == 0 || height == 0 || width > 16384 || height > 16384 || bytes != (usize)width * height * 4) {
        KV_ERROR("vulkan", "texture '%s': %ux%u with %llu bytes is not a tightly packed RGBA8 image",
                 name_str(t->name), width, height, (unsigned long long)bytes);
        return false;
    }
    t->width  = width;
    t->height = height;
    if (!rv_create_texture_image(r, *t)) return false;
    if (!rv_stage_texture(r, texture.index(), *t, (const u8*)pixels)) return false;
    rv_mark_slot_dirty(r, texture.index());
    return true;
}

void renderer_texture_destroy(Renderer* r, Handle texture) {
    Texture* t = r ? r->textures.get(texture) : nullptr;
    if (!t || texture.index() == 0) return; // the default stays
    rv_defer_destroy_texture(r, *t);
    u32 slot = texture.index();
    Name name = t->name;
    r->textures.destroy(texture);
    rename_after_destroy(r->textures, r->texture_by_name, name, texture);
    rv_mark_slot_dirty(r, slot); // back to the default, in each set once its frame has passed
}

u32 renderer_texture_slot(const Renderer* r, Handle texture) {
    return r && r->textures.valid(texture) ? texture.index() : 0;
}

// ---- materials ------------------------------------------------------------------------------

namespace {

// What an unnamed material is: its texture's name and everything it draws with, hashed.
// The same content made in any order gets the same name.
Name derived_material_name(const Renderer* r, const MaterialRecord& m) {
    const Texture* t = r->textures.get(m.texture);
    u64 h = hash_mix(0x6d6174657269616cull ^ (t ? t->name.hash : 0));
    h = hash_mix(h ^ (u64)m.blend ^ ((u64)m.filter << 8));
    h = hash_mix(h ^ hash_bytes(&m.tint, sizeof m.tint));
    h = hash_mix(h ^ hash_bytes(m.params, sizeof m.params));
    h = hash_mix(h ^ m.shader.hash);
    char text[32];
    format(text, sizeof text, "material:%016llx", (unsigned long long)h);
    return name_intern(text);
}

void fill_material(Renderer* r, MaterialRecord& m, const MaterialDesc& desc) {
    m.texture = desc.texture;
    m.blend   = (u32)desc.blend < (u32)BlendMode::Count ? desc.blend : BlendMode::Alpha;
    m.shader  = desc.shader && desc.shader[0] ? name_intern(desc.shader) : Name{};
    m.slot    = rv_material_slot(r, m.shader, m.blend);
    m.filter  = (u32)desc.filter < (u32)TextureFilter::Count ? desc.filter : TextureFilter::Nearest;
    m.tint    = desc.tint;
    for (u32 i = 0; i < KV_MATERIAL_PARAMS; ++i) m.params[i] = desc.params[i];
}

} // namespace

Handle renderer_material_create(Renderer* r, const MaterialDesc& desc) {
    if (!r) return Handle{};
    MaterialRecord m{};
    fill_material(r, m, desc);
    if (desc.name && desc.name[0]) m.name = name_intern(desc.name);
    else {
        m.name = derived_material_name(r, m);
#if KV_DEV
        bool inserted = false;
        r->material_by_name.get_or_insert(m.name.hash, Handle{}, &inserted);
        if (inserted) { r->material_by_name.remove(m.name.hash); KV_INFO("vulkan", "material named %s by its content", name_str(m.name)); }
#endif
    }
    Handle h = r->materials.create(m);
    if (h.is_null()) { KV_ERROR("vulkan", "out of materials (%u)", kMaxMaterials); return h; }
    r->material_by_name.get_or_insert(m.name.hash, h);
    rv_mark_material_dirty(r, h.index());
    return h;
}

bool renderer_material_set(Renderer* r, Handle material, const MaterialDesc& desc) {
    MaterialRecord* m = r ? r->materials.get(material) : nullptr;
    if (!m) return false;
    u32 before = m->slot;
    fill_material(r, *m, desc);
    if (m->slot != before) rv_rebind_material(r, material.index()); // the sprites' buckets moved
    rv_mark_material_dirty(r, material.index());
    return true;
}

bool renderer_material_get(const Renderer* r, Handle material, MaterialDesc* out) {
    const MaterialRecord* m = r ? r->materials.get(material) : nullptr;
    if (!m || !out) return false;
    out->texture = m->texture;
    out->blend   = m->blend;
    out->filter  = m->filter;
    out->tint    = m->tint;
    out->name    = name_str(m->name);
    out->shader  = m->shader.is_null() ? nullptr : name_str(m->shader);
    for (u32 i = 0; i < KV_MATERIAL_PARAMS; ++i) out->params[i] = m->params[i];
    return true;
}

void renderer_material_destroy(Renderer* r, Handle material) {
    if (!r || material == r->default_material || !r->materials.valid(material)) return;
    u32 row = material.index();
    Name name = r->materials.get(material)->name;
    r->materials.destroy(material);
    rename_after_destroy(r->materials, r->material_by_name, name, material);
    rv_rebind_material(r, row);      // its sprites fall back to the default
    rv_mark_material_dirty(r, row);  // the row is rewritten as a copy of the default
}

bool renderer_post_layer(Renderer* r, i32 layer, const char* shader) {
    if (!r) return false;
    return rv_post_set(r, layer, shader && *shader ? name_intern(shader) : Name{});
}

void renderer_post_params(Renderer* r, const f32* params, u32 count) {
    if (!r) return;
    if (count > KV_POST_PARAMS) count = KV_POST_PARAMS;
    if (count) memcpy(r->post.params, params, sizeof(f32) * 4 * count);
    r->post.param_count = count;
}

Handle renderer_default_material(const Renderer* r) { return r ? r->default_material : Handle{}; }

f64 renderer_last_wait(const Renderer* r) { return r ? r->last_wait : 0.0; }

bool renderer_check_sort(Renderer* r) {
    if (!r) return false;
    r->check_pending = true;
    return true;
}

bool renderer_screenshot(Renderer* r, const char* path) {
    if (!r || !path || strlen(path) >= sizeof r->shot_path) return false;
    strncpy(r->shot_path, path, sizeof r->shot_path - 1);
    r->shot_pending = true;
    return true;
}

// ---- the render world ---------------------------------------------------------------------

// The surface taken away, or a new one. Letting go: the GPU finishes, the swapchain and
// every retired one go (they were made from the surface and must not outlive it), then the
// surface. Taking: a surface for the new window, the swapchain built by the next frame.
bool renderer_set_surface(Renderer* r, void* native_window) {
    if (!r || !r->device) return false;
    if (r->surface) {
        vkDeviceWaitIdle(r->device);
        rv_destroy_swapchain_now(r);
        for (FrameInFlight& f : r->frames) {
            f.garbage_owned = f.garbage.len; // the device is idle: all of it
            rv_run_garbage(r, f);
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

void renderer_apply(Renderer* r, Span<const SpriteDelta> deltas) {
    if (!r) return;
    KV_ZONE("apply");
    for (const SpriteDelta& d : deltas) {
        ++r->deltas_applied;
        switch (d.kind) {
        case SpriteDeltaKind::Tick:
            // A new tick. Rows keep their stamps; a row's current placement becomes its
            // previous one the first time it is set in a later tick, and a row that is not
            // set did not move. Nothing is visited.
            ++r->tick;
            memcpy(r->layer_prev, r->layer_cur, sizeof r->layer_prev); // the layers' offsets likewise, all 256 of them
            break;
        case SpriteDeltaKind::Layer: {
            i32 l = d.state.layer < -128 ? -128 : d.state.layer > 127 ? 127 : d.state.layer;
            f32* cur = r->layer_cur[l + 128];
            cur[0] = d.state.placement.position.x;
            cur[1] = d.state.placement.position.y;
            if (d.state.snap) memcpy(r->layer_prev[l + 128], cur, sizeof(f32) * 2);
            break;
        }
        case SpriteDeltaKind::Set: {
            const RenderSpriteState& st = d.state;
            u32 material = rv_material_row_of(r, st.material);
            u32 bucket   = rv_bucket_of(st.layer, r->materials.items[material].slot);
            RenderSprite* s;
            u32 slot;
            if (u32* found = r->sprite_of_entity.find(d.entity)) {
                slot = *found;
                s = &r->sprites.items[slot];
                if (s->gpu.stamp != r->tick) { // first Set this tick: where it is now is where it was
                    memcpy(s->gpu.prev, s->gpu.cur, sizeof s->gpu.prev);
                    s->gpu.scale_y[0] = s->gpu.scale_y[1];
                    s->gpu.stamp = r->tick;
                }
                if (st.snap) write_placement(s->gpu.prev, &s->gpu.scale_y[0], st.placement); // a teleport: no history
                if (bucket != s->bucket) {
                    --r->bucket_counts[s->bucket];
                    ++r->bucket_counts[bucket];
                    if (s->keyed) { --r->keyed_counts[s->bucket]; ++r->keyed_counts[bucket]; }
                    s->bucket = bucket;
                }
            } else {
                RenderSprite fresh{};
                fresh.entity = d.entity;
                fresh.bucket = bucket;
                Handle h = r->sprites.create(fresh);
                if (h.is_null()) { KV_ERROR("vulkan", "render world is full (%u sprites)", kMaxSprites); break; }
                slot = h.index();
                s = &r->sprites.items[slot];
                r->sprite_of_entity.set(d.entity, slot);
                ++r->bucket_counts[bucket];
                write_placement(s->gpu.prev, &s->gpu.scale_y[0], st.placement); // born here: no history
                s->gpu.stamp = r->tick;
            }
            write_placement(s->gpu.cur, &s->gpu.scale_y[1], st.placement);
            s->gpu.size[0] = st.size.x; s->gpu.size[1] = st.size.y;
            s->gpu.uv[0] = st.uv.x; s->gpu.uv[1] = st.uv.y; s->gpu.uv[2] = st.uv.w; s->gpu.uv[3] = st.uv.h;
            s->gpu.tint[0] = st.tint.r; s->gpu.tint[1] = st.tint.g; s->gpu.tint[2] = st.tint.b; s->gpu.tint[3] = st.tint.a;
            s->gpu.material = material;
            s->gpu.layer    = st.layer;
            i32 ax = 0, ay = 0;
            screen_anchor_dir(st.anchor, &ax, &ay);
            u32 anchor = (u32)(ax < 0 ? 1 : ax > 0 ? 2 : 0) | (u32)(ay < 0 ? 1 : ay > 0 ? 2 : 0) << 2;
            s->gpu.flags    = kSpriteAlive | (st.space == SpriteSpace::Screen ? kSpriteScreen : 0) | ((u32)st.sort << kSpriteSortShift) |
                              (anchor << kSpriteAnchorShift);
            s->gpu.order    = st.order;
            bool keyed = st.sort != SpriteSort::Row;
            if (keyed != s->keyed) { if (keyed) ++r->keyed_counts[bucket]; else --r->keyed_counts[bucket]; s->keyed = keyed; }
            mark_sprite_dirty(r, slot);
            break;
        }
        case SpriteDeltaKind::Remove: {
            if (u32* found = r->sprite_of_entity.find(d.entity)) {
                u32 slot = *found;
                --r->bucket_counts[r->sprites.items[slot].bucket];
                if (r->sprites.items[slot].keyed) --r->keyed_counts[r->sprites.items[slot].bucket];
                r->sprites.destroy(r->sprites.handle_at(slot));
                r->sprite_of_entity.remove(d.entity);
                mark_sprite_dirty(r, slot); // the row is rewritten dead
            }
            break;
        }
        }
    }
}

u32 renderer_sprite_count(const Renderer* r) { return r ? r->sprites.live : 0; }

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
    rv_gpu_collect(r, f); // its timestamps and counts, now that the fence has passed
    rv_ring_release(r, f);
    rv_run_garbage(r, f);
    if (r->sprites.len > r->tables.capacity) {
        // Growth remarks every row dirty, so the next frame uploads the table whole: logged
        // with the frame, so a hitch can be found by it (FrameLoopConfig::sprite_rows avoids it).
        KV_INFO("vulkan", "sprite tables grow at frame %llu: %u rows live, %u held", (unsigned long long)r->frame_count, r->sprites.len, r->tables.capacity);
        if (!rv_create_tables(r, r->sprites.len)) return false;
    }
    if (f.tables_dirty) rv_write_tables_set(r, f);
    // The layers' offsets for this frame: previous and current, the shader interpolates.
    f32* lo = (f32*)f.layers.mapped;
    for (u32 i = 0; i < kLayerCount; ++i) {
        lo[i * 4 + 0] = r->layer_prev[i][0]; lo[i * 4 + 1] = r->layer_prev[i][1];
        lo[i * 4 + 2] = r->layer_cur[i][0];  lo[i * 4 + 3] = r->layer_cur[i][1];
    }
    for (u32 slot : f.dirty_slots) rv_write_texture_slot(r, f.textures, slot);
    f.dirty_slots.clear();

    // Without a surface (Android has taken it) the frame runs its uploads and presents nothing.
    if (r->surface && r->swapchain_dirty && !rv_recreate_swapchain(r)) return false;

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

    if (!rv_record_frame(r, f, have_image, image_index, view)) return false;
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
    rv_ring_submitted(r, f);

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
        if (r->shot_recorded) rv_write_screenshot(r);
        if (r->check_recorded) rv_verify_sort(r);
    }

    r->last_wait = waited;
    r->frame_index = (r->frame_index + 1) % kFramesInFlight;
    ++r->frame_count;
    return have_image;
}

} // namespace kv
