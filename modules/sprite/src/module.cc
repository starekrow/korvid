#include "korvid/sprite/module.h"
#include "korvid/sprite/camera.h"
#include "korvid/sprite/components.h"
#include "korvid/sprite/extract.h"
#include "korvid/core/profile.h"
#include "korvid/sprite/layers.h"
#include "korvid/sprite/world.h"
#include "korvid/core/engine.h"
#include "korvid/math/fields.h"
#include <string.h>

namespace kv {

namespace {

struct SpriteState {
    SpriteWorld        world;
    ExtractState       extract;
    Array<SpriteDelta> deltas;
    Camera2            camera;
    SpriteLayers       layers;
};

bool sprite_startup(Engine& engine, ModuleInstance& self) {
    SpriteState* s = arena_push<SpriteState>(self.arena);
    if (!s) return false;
    s->deltas.alloc = s->extract.ktables.alloc = &self.general;
    s->world.ktable = engine_ktable(engine, "sprite.entity"); // the engine built it from the ktable list
    if (!s->world.ktable) return false;
    if (!engine_publish(engine, self, ".camera", &s->camera)) return false;
    if (!engine_publish(engine, self, ".layers", &s->layers)) return false;
    self.set_state(s);
    return true;
}

void sprite_shutdown(Engine&, ModuleInstance& self) {
    SpriteState* s = self.state_as<SpriteState>();
    if (!s) return;
    s->extract.free();
    s->deltas.free();
}

// The Frame cycle: what the ticks since the last frame changed, in every ktable with
// Transform2 and Sprite, appended for the renderer. The engine copied current into previous
// at each tick's PreSim.
void sprite_extract(TickContext& ctx) {
    SpriteState& s = ctx.state_as<SpriteState>();
    u32 before = s.deltas.len;
    u64 total = ctx.engine->clock.tick_total;
    u32 ticks = (u32)(total - s.extract.seen_ticks);
    s.extract.seen_ticks = total;
    extract_sprites(*ctx.engine, s.extract, &s.layers, s.deltas, ticks);
    KV_MARK_V("extract deltas", s.deltas.len - before);
}

// Phase PreSim, every tick, paused or not: the layers' offsets as the tick begins, the
// layers' previous copy (the extract's catch-up when a frame holds several ticks).
void sprite_layers_tick(TickContext& ctx) {
    SpriteState& s = ctx.state_as<SpriteState>();
    memcpy(s.layers.tick_start, s.layers.offset, sizeof s.layers.offset);
}

// The schemas. Transform2 keeps a previous-tick copy for interpolation; Sprite does not.
constinit const FieldDesc k_transform2_fields[] = {KV_FIELDS(Transform2, position, rotation, scale)};
constinit const FieldDesc k_sprite_fields[]     = {KV_FIELDS(Sprite, material, uv, size, tint, layer, space, anchor, sort, order)};
constinit const ComponentDesc k_components[] = {
    KV_COMPONENT(Transform2, k_transform2_fields, CompPrev),
    KV_COMPONENT(Sprite, k_sprite_fields),
};

// The ktable. Any ktable with these two components is a sprite to the extract.
constinit const KTableDesc    k_ktables[] = {
    {.name = ".entity", .components = "Transform2, Sprite", .capacity = KV_POOL_DEFAULT_MAX},
};

constinit const ResourceDesc k_resources[] = {{".deltas"}, {".camera"}, {".layers"}};
constinit const SystemDesc   k_systems[]   = {
    {.name = ".layers_tick", .phase = Phase::PreSim, .fn = sprite_layers_tick, .writes = ".layers", .runs = RunWhen::Always},
    {.name = ".extract", .phase = Phase::Frame, .fn = sprite_extract, .reads = "*.Transform2, *.Sprite", .writes = ".deltas, .layers"},
};

} // namespace

KV_MODULE_DEFINE(sprite_module) = {
    .name       = "sprite",
    .components = k_components,
    .ktables    = k_ktables,
    .resources  = k_resources,
    .systems    = k_systems,
    .startup    = sprite_startup,
    .shutdown   = sprite_shutdown,
    .memory     = {.reserve = 64 * 1024},
};

const ComponentDesc& transform2_schema() { return k_components[0]; }
const ComponentDesc& sprite_schema()     { return k_components[1]; }

SpriteWorld* sprite_world(Engine& e) {
    SpriteState* s = engine_module_state_as<SpriteState>(e, "sprite");
    return s ? &s->world : nullptr;
}

Array<SpriteDelta>* sprite_deltas(Engine& e) {
    SpriteState* s = engine_module_state_as<SpriteState>(e, "sprite");
    return s ? &s->deltas : nullptr;
}

void sprite_rows_hint(Engine& e, u32 rows) {
    if (SpriteState* s = engine_module_state_as<SpriteState>(e, "sprite")) s->extract.rows_hint = rows;
}

SpriteLayers* sprite_layers(Engine& e) {
    SpriteState* s = engine_module_state_as<SpriteState>(e, "sprite");
    return s ? &s->layers : nullptr;
}

Camera2* sprite_camera(Engine& e) {
    SpriteState* s = engine_module_state_as<SpriteState>(e, "sprite");
    return s ? &s->camera : nullptr;
}

namespace {

// One axis of the clamp: the view's half-extent inside [lo, lo + len], or the middle.
f32 clamp_axis(f32 c, f32 half, f32 lo, f32 len) {
    if (len <= 0.0f) return c;
    if (half * 2.0f >= len) return lo + len * 0.5f;
    if (c - half < lo) return lo + half;
    if (c + half > lo + len) return lo + len - half;
    return c;
}

} // namespace

Camera2 camera_resolve(Engine& e, const Camera2& cam, u32 width, u32 height, f32 alpha) {
    Camera2 out = cam;
    if (!cam.follow.is_null()) {
        KTable* t = engine_ktable_of(e, cam.follow);
        const Transform2* now = t && t->valid(cam.follow) ? t->get<Transform2>(cam.follow) : nullptr;
        if (now) {
            const Transform2* was = t->prev<Transform2>(cam.follow);
            point2 p = now->position;
            if (was) p = point2{was->position.x + (now->position.x - was->position.x) * alpha,
                                was->position.y + (now->position.y - was->position.y) * alpha};
            out.center = point2{p.x + cam.offset.x, p.y + cam.offset.y};
        }
    }
    f32 z = cam.zoom > 1e-6f ? cam.zoom : 1e-6f;
    f32 hw = (f32)width / z * 0.5f, hh = (f32)height / z * 0.5f;
    out.center.x = clamp_axis(out.center.x, hw, cam.bounds.x, cam.bounds.w);
    out.center.y = clamp_axis(out.center.y, hh, cam.bounds.y, cam.bounds.h);
    if (cam.pixel_snap) {
        // The view's corner on a world pixel: the screen's pixels then each cover whole
        // texels (at an integer zoom), whatever the window's parity.
        f32 left = kv::floor((out.center.x - hw) * z + 0.5f) / z, bottom = kv::floor((out.center.y - hh) * z + 0.5f) / z;
        out.center = point2{left + hw, bottom + hh};
    }
    return out;
}

} // namespace kv
