// example_arcfire: effects. Sparks and debris are particles, a cosmetic kind: they drift,
// spin, slow and fade over their life and are never part of the state. Their randomness
// is the module's own stream, not the sim's, so a dial that changes how many sparks fly
// changes nothing the sim draws. Explosions are a flipbook of Kenney's burst tiles on a
// cosmetic row that goes when its clip ends.
#include "arcfire.h"
#include "korvid/anim/clip.h"
#include "korvid/anim/player.h"
#include <math.h>

namespace kv::arcfire {

namespace {
const vec2 kShadowOffset{18.0f, -34.0f}; // an air row's altitude, as its shadow shows it
} // namespace

void fx_piece(TickContext& ctx, Game& g, point2 at, Tex tex, rect2 uv, vec2 size, vec2 v, f32 spin, u32 life, i32 layer, f32 fall) {
    fx_piece_of(ctx, g.alpha[tex], at, uv, size, v, spin, life, layer, fall);
}

void fx_piece_of(TickContext& ctx, Handle material, point2 at, rect2 uv, vec2 size, vec2 v, f32 spin, u32 life, i32 layer, f32 fall) {
    Sprite s;
    s.material = material;
    s.uv = uv;
    s.size = size;
    s.layer = layer;
    ctx.spawn_later(g_particle_kind, transform2_at(at), s, Velocity{v}, Particle{life, life, spin, 0.97f, 1.0f, fall});
}

// The burst: a small flash, the star, the ball, its bits, the smoke (tiles row 0), four
// ticks each.
Handle fx_boom_clip(Engine& e) {
    constexpr u32 kFrames[] = {6, 4, 5, 7, 8};
    constexpr f32 kFrame = 4.0f / 60.0f;
    AnimKey keys[KV_COUNTOF(kFrames)];
    for (u32 i = 0; i < KV_COUNTOF(kFrames); ++i) {
        rect2 uv = tile_uv(kFrames[i], 0);
        keys[i].time = (f32)i * kFrame;
        keys[i].v[0] = uv.x; keys[i].v[1] = uv.y; keys[i].v[2] = uv.w; keys[i].v[3] = uv.h;
    }
    AnimTrackDesc track;
    track.channel = AnimChannel::Uv;
    track.keys = Span<const AnimKey>(keys, KV_COUNTOF(keys));
    AnimClipDesc desc;
    desc.name = "example_arcfire.boom";
    desc.duration = (f32)KV_COUNTOF(kFrames) * kFrame;
    desc.tracks = Span<const AnimTrackDesc>(&track, 1);
    return anim_create(e, desc);
}

void fx_boom(TickContext& ctx, Game& g, point2 at, f32 size) {
    Sprite s = sprite_of(g, TEX_TILES, tile_uv(6, 0), vec2{size, size}, Z_FX);
    ctx.spawn_later(g_boom_kind, transform2_at(at, g.fx.range(0.0f, 2.0f * KV_PI)), s, anim_player(g.boom_clip));
}

// An explosion's clip has played: the row goes.
void on_boom_done(TickContext& ctx, Span<const AnimFinished> done) {
    for (const AnimFinished& f : done)
        if (f.entity.kind() == engine_table(*ctx.engine, g_boom_kind)->kind) ctx.destroy(f.entity);
}

void fx_sparks(TickContext& ctx, Game& g, point2 at, u32 count, color tint, f32 speed) {
    for (u32 i = 0; i < count; ++i) {
        f32 a = g.fx.range(0.0f, 2.0f * KV_PI);
        f32 v = speed * g.fx.range(0.3f, 1.0f);
        u32 life = 10 + g.fx.below(14);
        Sprite s = sprite_of(g, TEX_SPARK, rect2_unit(), vec2{5, 5}, Z_FX, tint, true);
        ctx.spawn_later(g_particle_kind, transform2_at(at), s, Velocity{vec2{cosf(a) * v, sinf(a) * v}}, Particle{life, life, 0.0f, 0.9f, 1.0f, 0.0f});
    }
}

// Shadows: an air row, once it exists, gets one: its sprite in black, a third opaque, down
// and right of it on the ground's layer, a cosmetic child that dies with it. The child's
// offset is in its parent's frame (an enemy faces down), so it is turned back first.
void on_spawned(TickContext& ctx, Span<const Spawned> spawns) {
    Game& g = ctx.state_as<Game>();
    Span<const Transform2> xf = ctx.read<Transform2>("example_arcfire.air.Transform2");
    Span<const Sprite> sp = ctx.read<Sprite>("example_arcfire.air.Sprite");
    for (const Spawned& s : spawns) {
        if (s.kind != g_air_kind || s.entity.is_null() || s.entity.index() >= xf.len) continue;
        Sprite shadow = sp[s.entity.index()];
        shadow.tint = color{0.0f, 0.0f, 0.04f, 0.33f};
        shadow.layer = Z_SHADOW;
        shadow.material = g.alpha[TEX_SHIPS];
        f32 r = -xf[s.entity.index()].rotation;
        vec2 o = kShadowOffset;
        Local2 local = local2_at(point2{o.x * cosf(r) - o.y * sinf(r), o.x * sinf(r) + o.y * cosf(r)});
        ctx.spawn_later(g_shadow_kind, Transform2{}, local, Parent{s.entity}, shadow);
    }
}

// Phase Sim: every particle moves, slows, turns and fades; at the end of its life it goes.
void particles(TickContext& ctx) {
    for (KindRows k : ctx.view<Transform2, Sprite, Velocity, Particle>()) {
        Span<Transform2> t = k.col<Transform2>();
        Span<Sprite> s = k.col<Sprite>();
        Span<Velocity> v = k.col<Velocity>();
        Span<Particle> p = k.col<Particle>();
        parallel_for(ctx, k.rows(), 1024, [&](ChunkContext& cc, u32 begin, u32 end) {
            for (u32 i = begin; i < end; ++i) {
                if (!k.alive(i)) continue;
                Particle& q = p[i];
                if (q.life == 0) { cc.destroy(k.handle_at(i)); continue; }
                --q.life;
                t[i].position += v[i].v;
                t[i].rotation += q.spin;
                v[i].v *= q.drag;
                v[i].v.y -= q.fall;
                s[i].tint.a = q.alpha * (f32)q.life / (f32)(q.max_life ? q.max_life : 1);
            }
        });
    }
}

} // namespace kv::arcfire
