// example_arcfire: effects. Sparks and debris are particles, a cosmetic kind: they drift,
// spin, slow and fade over their life and are never part of the state. Their randomness
// is the module's own stream, not the sim's, so a dial that changes how many sparks fly
// changes nothing the sim draws.
#include "arcfire.h"
#include <math.h>

namespace kv::arcfire {

void fx_piece(TickContext& ctx, Game& g, point2 at, Tex tex, rect2 uv, vec2 size, vec2 v, f32 spin, u32 life, i32 layer) {
    Sprite s = sprite_of(g, tex, uv, size, layer);
    ctx.spawn_later(g_particle_kind, transform2_at(at), s, Velocity{v}, Particle{life, life, spin, 0.97f, 1.0f, 0});
}

void fx_sparks(TickContext& ctx, Game& g, point2 at, u32 count, color tint, f32 speed) {
    for (u32 i = 0; i < count; ++i) {
        f32 a = g.fx.range(0.0f, 2.0f * KV_PI);
        f32 v = speed * g.fx.range(0.3f, 1.0f);
        u32 life = 10 + g.fx.below(14);
        Sprite s = sprite_of(g, TEX_SPARK, rect2_unit(), vec2{5, 5}, Z_FX, tint, true);
        ctx.spawn_later(g_particle_kind, transform2_at(at), s, Velocity{vec2{cosf(a) * v, sinf(a) * v}}, Particle{life, life, 0.0f, 0.9f, 1.0f, 0});
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
                s[i].tint.a = q.alpha * (f32)q.life / (f32)(q.max_life ? q.max_life : 1);
            }
        });
    }
}

} // namespace kv::arcfire
