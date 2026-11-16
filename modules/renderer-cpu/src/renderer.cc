// renderer-cpu: making and ending the renderer, and the contract's rb_ steps
// (korvid/render/core/core.h). Milestone 1 of docs/PRDs/CPU-Renderer-PRD.md: the seam proved
// with a backend that clears its target to the view's color, presents it through the
// window's software surface, and takes screenshots; headless with no window. The sort, the
// tiles and the pixels arrive in milestone 2.
#include "renderer_internal.h"
#include "korvid/renderer-cpu/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/memory.h"
#include "korvid/core/time.h"
#include "korvid/math/scalar.h"
#include "korvid/platform/module.h"
#include "korvid/platform_hid/host_window.h"
#include "korvid/platform_hid/module.h"
#include <string.h>

namespace kv { namespace KV_RB_NS {

namespace {

void cpu_free(Renderer* r, Buffer& b) {
    if (b.data) free_in(r->general, b.data, b.size);
    b = Buffer{};
}

// A linear color's channel as an sRGB byte, correctly rounded. Not on a hot path yet: the
// clear, once a frame.
u8 srgb_byte(f32 c) {
    if (!(c > 0.0f)) return 0;
    if (c >= 1.0f) return 255;
    f32 s = c <= 0.0031308f ? c * 12.92f : 1.055f * kv::pow(c, 1.0f / 2.4f) - 0.055f;
    return (u8)(s * 255.0f + 0.5f);
}

} // namespace

bool cpu_alloc(Renderer* r, u64 bytes, Buffer& out) {
    out = Buffer{};
    if (!bytes) return true;
    out.data = (u8*)alloc_in(r->general, (usize)bytes, 16);
    if (!out.data) { KV_ERROR(KV_RB_NAME, "out of memory for %llu bytes", (unsigned long long)bytes); return false; }
    memset(out.data, 0, (usize)bytes);
    out.size = bytes;
    return true;
}

// ---- lifetime -------------------------------------------------------------------------------

Renderer* renderer_create(Engine& engine, const RendererDesc& desc) {
    RendererCpuState* owner = engine_module_state_as<RendererCpuState>(engine, "renderer-cpu");
    if (!owner) { KV_ERROR(KV_RB_NAME, "renderer-cpu module is not started"); return nullptr; }
    if (owner->renderer) { KV_ERROR(KV_RB_NAME, "a renderer already exists"); return nullptr; }
    Allocator* ga = owner->general;
    Renderer* r = (Renderer*)ga->alloc(ga, sizeof(Renderer), alignof(Renderer));
    *r = Renderer{};
    r->general    = ga;
    r->owner_slot = &owner->renderer;
    owner->renderer = reinterpret_cast<kv::Renderer*>(r); // the public, opaque type: render's dispatcher reads its first byte
    for (FrameInFlight& f : r->frames) { f.garbage.alloc = ga; f.dirty_slots.alloc = ga; }
    if (desc.native_window) {
        r->window = platform_window(engine);
        if (!r->window || r->window->native_handle() != desc.native_window) {
            KV_ERROR(KV_RB_NAME, "the window to draw into is not the platform's");
            renderer_destroy(r);
            return nullptr;
        }
    }
    if (const PlatformState* ps = engine_module_state_as<PlatformState>(engine, "platform"))
        format(r->base_path, sizeof r->base_path, "%s", ps->base_path);
    r->start_time  = time_now();
    r->validation  = desc.validation;
    r->vsync       = desc.vsync;
    r->want_width  = desc.width;
    r->want_height = desc.height;

    bool ok = rc_create_pools(r);
    ok = ok && rb_create_tables(r, desc.sprite_rows > kInitialSpriteRows ? desc.sprite_rows : kInitialSpriteRows);
    ok = ok && cpu_alloc(r, sizeof(GpuMaterial) * kMaxMaterials, r->material_table);
    for (FrameInFlight& f : r->frames) ok = ok && cpu_alloc(r, sizeof(f32) * 4 * kLayerCount, f.layers);
    ok = ok && rc_ring_init(r, kInitialRingBytes);
    ok = ok && rc_create_defaults(r);
    if (!ok) {
        renderer_destroy(r);
        return nullptr;
    }
    if (r->window) KV_INFO(KV_RB_NAME, "renderer up: into the window's software surface");
    else KV_INFO(KV_RB_NAME, "renderer up: headless, %ux%u", r->want_width, r->want_height);
    return r;
}

void renderer_destroy(Renderer* r) {
    if (!r) return;
    free_in(mem_general(), r->check_keys, sizeof(f32) * (r->check_rows ? r->check_rows : 1));
    r->check_keys = nullptr;
    if (r->presenting && r->window) r->window->present_software();
    for (FrameInFlight& f : r->frames) {
        f.garbage_owned = f.garbage.len; // nothing is in flight: all of it
        rc_run_garbage(r, f);
        cpu_free(r, f.layers);
        cpu_free(r, f.ui);
        f.garbage.free();
        f.dirty_slots.free();
    }
    for (u32 i = 0; i < r->textures.len; ++i)
        if (r->textures.is_alive(i)) cpu_free(r, r->textures.items[i].pixels);
    cpu_free(r, r->tables.sprites);
    cpu_free(r, r->tables.sorted);
    cpu_free(r, r->material_table);
    cpu_free(r, r->ring.buf);
    cpu_free(r, r->headless);
    cpu_free(r, r->shot_buffer);
    cpu_free(r, r->check_buffer);
    rc_free_pools(r);
    KV_INFO(KV_RB_NAME, "renderer down after %llu frames, %llu deltas, %llu rows uploaded, ring peak %llu KB (%u grows)",
            (unsigned long long)r->frame_count, (unsigned long long)r->world.deltas_applied, (unsigned long long)r->rows_uploaded,
            (unsigned long long)(r->ring.peak / 1024), r->ring.grows);
    if (r->owner_slot) *r->owner_slot = nullptr;
    Allocator* ga = r->general;
    ga->free(ga, r, sizeof(Renderer));
}

Renderer* renderer_get(Engine& engine) {
    RendererCpuState* s = engine_module_state_as<RendererCpuState>(engine, "renderer-cpu");
    return s ? reinterpret_cast<Renderer*>(s->renderer) : nullptr;
}

// The CPU can always draw.
bool renderer_probe(const char** why) {
    *why = "";
    return true;
}

bool renderer_set_surface(Renderer* r, void* native_window) {
    if (!r) return false;
    if (r->presenting && r->window) { r->window->present_software(); r->presenting = false; }
    if (!native_window) r->window = nullptr; // headless until a window comes back
    return true;
}

void renderer_resize(Renderer* r, u32 width, u32 height) {
    if (!r) return;
    r->want_width  = width;
    r->want_height = height;
}

// ---- resources ------------------------------------------------------------------------------

void* rb_buffer_ptr(const RbBuffer& b) { return b.data; }

void rb_buffer_retire(Renderer* r, RbBuffer& b) {
    if (b.data) rc_defer(r, b);
    b = Buffer{};
}

bool rb_ring_buffer(Renderer* r, u64 bytes, RbBuffer& out) { return cpu_alloc(r, bytes, out); }

void rb_garbage_destroy(Renderer* r, const RbGarbage& g) {
    if (g.data) free_in(r->general, g.data, g.size);
}

bool rb_texture_create(Renderer* r, Texture& t) {
    rb_buffer_retire(r, t.pixels);
    t.resident = false;
    return cpu_alloc(r, (u64)t.width * t.height * 4, t.pixels);
}

// A frame never runs while the API is called, so a texture's pixels are copied at once.
bool rb_stage_texture(Renderer*, u32, Texture& t, const u8* pixels) {
    if (!t.pixels.data) return false;
    memcpy(t.pixels.data, pixels, (usize)t.width * t.height * 4);
    t.resident = true;
    return true;
}

bool rb_stage_texture_region(Renderer*, u32, Texture& t, const u8* pixels, u32 x, u32 y, u32 w, u32 h) {
    if (!t.pixels.data || x + w > t.width || y + h > t.height) return false;
    for (u32 row = y; row < y + h; ++row) {
        usize at = ((usize)row * t.width + x) * 4;
        memcpy(t.pixels.data + at, pixels + at, (usize)w * 4);
    }
    t.resident = true;
    return true;
}

void rb_defer_destroy_texture(Renderer* r, Texture& t) {
    rb_buffer_retire(r, t.pixels);
    t.resident = false;
}

bool rb_game_pipe_load(Renderer*, GamePipe&) { return false; } // milestone 4: its blend's engine slot until then
bool rb_post_set(Renderer*, i32, Name) { return false; }        // milestone 4: the pass is off until then

bool rb_create_tables(Renderer* r, u32 rows) {
    u32 capacity = (rows + kBlockRows - 1) / kBlockRows * kBlockRows;
    Buffer sprites, sorted;
    if (!cpu_alloc(r, (u64)capacity * sizeof(GpuSprite), sprites) || !cpu_alloc(r, (u64)capacity * sizeof(u32), sorted)) {
        cpu_free(r, sprites);
        return false;
    }
    rb_buffer_retire(r, r->tables.sprites);
    rb_buffer_retire(r, r->tables.sorted);
    r->tables.capacity = capacity;
    r->tables.sprites  = sprites;
    r->tables.sorted   = sorted;
    return true;
}

// ---- the frame ------------------------------------------------------------------------------

bool rb_frame_wait(Renderer*, FrameInFlight&, f64*) { return true; } // the last frame ended inside its own renderer_render
void rb_frame_slot(Renderer*, FrameInFlight&, u32) {}               // textures are read where they are

bool rb_frame_acquire(Renderer* r, FrameInFlight& f, f64*) {
    f.have_image = false;
    f.width = f.height = 0;
    r->target = nullptr;
    if (r->window) {
        HostWindow::SoftwareFrame sf;
        if (!r->window->software_frame(&sf) || !sf.width || !sf.height) return true; // no image: the frame still takes its uploads
        r->presenting    = true;
        r->target        = sf.pixels;
        r->target_width  = sf.width;
        r->target_height = sf.height;
        r->target_pitch  = sf.pitch;
        r->target_rgba   = sf.rgba;
    } else {
        u32 w = r->want_width, h = r->want_height;
        if (!w || !h) return true;
        u64 bytes = (u64)w * h * 4;
        if (r->headless.size < bytes) {
            cpu_free(r, r->headless);
            if (!cpu_alloc(r, bytes, r->headless)) return false;
        }
        r->target        = r->headless.data;
        r->target_width  = w;
        r->target_height = h;
        r->target_pitch  = w * 4;
        r->target_rgba   = false;
    }
    f.have_image = true;
    f.width  = r->target_width;
    f.height = r->target_height;
    return true;
}

bool rb_frame_submit(Renderer*, FrameInFlight&) { return true; } // it ran as it was recorded

bool rb_frame_present(Renderer* r, FrameInFlight&, f64*) {
    if (r->presenting && r->window) r->window->present_software();
    r->presenting = false;
    return true;
}

bool rb_frame_finish(Renderer*, FrameInFlight&) { return true; }

bool rb_record_begin(Renderer*, FrameInFlight&) { return true; }
void rb_upload_begin(Renderer*, FrameInFlight&) {}

void rb_copy_rows(Renderer*, FrameInFlight&, RbBuffer& table, const RingSpan& span, u64 offset, u64 bytes) {
    if (table.data && offset + bytes <= table.size) memcpy(table.data + offset, span.ptr, (usize)bytes);
}

void rb_upload_end(Renderer*, FrameInFlight&) {}

void rb_sort(Renderer*, FrameInFlight&, const PushData&) {}       // milestone 2
void rb_read_sort(Renderer*, FrameInFlight&) {}
void rb_sort_end(Renderer*, FrameInFlight&, bool) {}
bool rb_post_prepare(Renderer*, u32, u32) { return false; }       // milestone 4: straight to the window

// A pass clears the target to the view's color (linear, written as sRGB bytes).
void rb_pass_begin(Renderer* r, FrameInFlight&, RcPass, const RenderView& view) {
    if (!r->target) return;
    u8 cr = srgb_byte(view.clear_color.r), cg = srgb_byte(view.clear_color.g), cb = srgb_byte(view.clear_color.b);
    u32 px = r->target_rgba ? (u32)cr | (u32)cg << 8 | (u32)cb << 16 | 0xFF000000u : (u32)cb | (u32)cg << 8 | (u32)cr << 16 | 0xFF000000u;
    for (u32 y = 0; y < r->target_height; ++y) {
        u32* row = (u32*)(r->target + (usize)y * r->target_pitch);
        for (u32 x = 0; x < r->target_width; ++x) row[x] = px;
    }
}

void rb_pass_post(Renderer*, FrameInFlight&, const RenderView&) {}
void rb_bind_sprites(Renderer*, FrameInFlight&, const PushData&) {}
u32  rb_draw_run(Renderer*, FrameInFlight&, const DrawRun&) { return 0; } // milestone 2
void rb_pass_end(Renderer*, FrameInFlight&, RcPass) {}

void rb_read_window(Renderer* r, FrameInFlight&) {
    if (!r->target) return;
    u64 bytes = (u64)r->target_width * r->target_height * 4;
    if (r->shot_buffer.size < bytes) {
        cpu_free(r, r->shot_buffer);
        if (!cpu_alloc(r, bytes, r->shot_buffer)) { r->shot_pending = false; return; }
    }
    for (u32 y = 0; y < r->target_height; ++y)
        memcpy(r->shot_buffer.data + (usize)y * r->target_width * 4, r->target + (usize)y * r->target_pitch, (usize)r->target_width * 4);
    r->shot_width    = r->target_width;
    r->shot_height   = r->target_height;
    r->shot_pitch    = r->target_width * 4;
    r->shot_rgba     = r->target_rgba;
    r->shot_pending  = false;
    r->shot_recorded = true;
}

bool rb_ui_buffer(Renderer* r, FrameInFlight& f, u64 bytes) {
    Buffer b;
    if (!cpu_alloc(r, bytes, b)) return false;
    rb_buffer_retire(r, f.ui);
    f.ui = b;
    return true;
}

void rb_ui_draw(Renderer*, FrameInFlight&, const PushData&) {} // milestone 3
bool rb_record_end(Renderer*, FrameInFlight&) { return true; }

} } // namespace kv::KV_RB_NS
