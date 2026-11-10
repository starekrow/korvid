// example_arcfire: effects. Sparks and debris are particles, a cosmetic kind: they drift,
// spin, slow and fade over their life and are never part of the state. Their randomness
// is the module's own stream, not the sim's, so a dial that changes how many sparks fly
// changes nothing the sim draws. Explosions are a flipbook of Kenney's burst tiles on a
// cosmetic row that goes when its clip ends, in three sizes (fx_blast), the bigger two
// with a ring of bent air (fx_shimmer, drawn by hud.cc's shimmer through a post shader). The bullets' glows
// (dark halos, which is what reads over a pale sea) are a cosmetic kind kept slot for slot
// with the bullets (glows, below).
#include "arcfire.h"
#include "korvid/anim/clip.h"
#include "korvid/anim/player.h"
#include "korvid/core/job.h"
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

void fx_blast(TickContext& ctx, Game& g, point2 at, Blast b) {
    Span<const Dials> dials = ctx.read<Dials>(ARC_WORLD(Dials));
    u32 sparks = dials.len ? dials[0].particles : 6;
    const color hot{1.0f, 0.75f, 0.35f, 1.0f};
    switch (b) {
    case Blast::Small: // popcorn: a pop
        fx_boom(ctx, g, at, 40.0f);
        fx_sparks(ctx, g, at, sparks, hot, 3.0f);
        g.shake = g.shake > 2.0f ? g.shake : 2.0f;
        break;
    case Blast::Medium: // a ground row, a part: two bursts, one beside the other
        fx_boom(ctx, g, at, 64.0f);
        fx_boom(ctx, g, at + vec2{g.fx.range(-14.0f, 14.0f), g.fx.range(6.0f, 16.0f)}, 44.0f);
        fx_sparks(ctx, g, at, sparks * 2, hot, 4.0f);
        fx_shimmer(g, at, 90.0f, 0.6f, 30);
        g.shake = g.shake > 5.0f ? g.shake : 5.0f;
        break;
    case Blast::Large: { // a boss: the big one, a ring of sparks, the screen white for a moment
        fx_boom(ctx, g, at, 160.0f);
        for (u32 i = 0; i < 4; ++i) fx_boom(ctx, g, at + vec2{g.fx.range(-60.0f, 60.0f), g.fx.range(-50.0f, 50.0f)}, 72.0f);
        u32 ring = sparks * 6;
        for (u32 i = 0; i < ring; ++i) {
            f32 a = 2.0f * KV_PI * (f32)i / (f32)(ring ? ring : 1);
            Sprite s = sprite_of(g, TEX_SPARK, rect2_unit(), vec2{6, 6}, Z_FX, hot, true);
            ctx.spawn_later(g_particle_kind, transform2_at(at), s, Velocity{vec2{cosf(a) * 7.0f, sinf(a) * 7.0f}}, Particle{28, 28, 0.0f, 0.95f, 1.0f, 0.0f});
        }
        hud_flash(*ctx.engine);
        fx_shimmer(g, at, 300.0f, 1.0f, 50);
        g.shake = 14.0f;
        break;
    }
    }
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
// offset is in its parent's frame (an enemy faces down), so it is turned back first. Only
// over ground: a stage without a map (Orbit) has none.
void on_spawned(TickContext& ctx, Span<const Spawned> spawns) {
    Game& g = ctx.state_as<Game>();
    if (!g.stage.map[0]) return; // in space (Orbit) there is no ground to fall on
    Span<const Transform2> xf = ctx.read<Transform2>("example_arcfire.air.Transform2");
    Span<const Sprite> sp = ctx.read<Sprite>("example_arcfire.air.Sprite");
    for (const Spawned& s : spawns) {
        if (s.kind != g_air_kind || s.entity.is_null() || s.entity.index() >= xf.len) continue;
        Sprite shadow = sp[s.entity.index()];
        shadow.tint = color{0.0f, 0.0f, 0.04f, 0.33f};
        shadow.layer = Z_SHADOW;
        shadow.material = g.alpha[TEX_SHIPS];
        const Transform2& at = xf[s.entity.index()];
        f32 r = -at.rotation;
        vec2 o = kShadowOffset;
        Local2 local = local2_at(point2{o.x * cosf(r) - o.y * sinf(r), o.x * sinf(r) + o.y * cosf(r)});
        // Born where the composition will put it, so its first frame is not drawn moving
        // there from the world's origin (a streak across the field, once an enemy).
        ctx.spawn_later(g_shadow_kind, transform2_at(at.position + o, at.rotation), local, Parent{s.entity}, shadow);
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

// Phase Sim, after the bullets fly: a soft dark disc under every enemy bullet, so they read
// against the islands and the clouds. Glow row i is bullet slot i's (the plain kind's
// in `.glow`, the active kind's in `.aglow`), spawned as the bullet table's rows grow and
// never destroyed; it takes its bullet's transform, the previous one too, so it is drawn
// moving exactly as the bullet is (but for its first tick under a bullet, which is a jump:
// the renderer would otherwise draw it moving from wherever its last bullet died), and a
// slot with no bullet has a glow of no size.
void glows(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    Span<const Dials> dials = ctx.read<Dials>(ARC_WORLD(Dials));
    bool on = dials.len && dials[0].glows;
    const u32 pairs[2][2] = {{g_bullet_kind, g_glow_kind}, {g_active_kind, g_aglow_kind}};
    for (const auto& pr : pairs) {
        const Table* src = engine_table(e, pr[0]);
        Table* dst = ctx.write_kind(pr[1]);
        u32 n = src->rows();
        if (!on && dst->rows() == 0) continue;
        while (on && dst->rows() < n && !dst->spawn(transform2_at(point2{0, 0}), Sprite{}, Glow{}).is_null()) {}
        if (dst->rows() < n) n = dst->rows();
        Span<const Transform2> cur = src->rows_of<Transform2>();
        Span<const Transform2> prev = src->prev_rows_of<Transform2>();
        Span<const Sprite> look = src->rows_of<Sprite>();
        Column* col = dst->column<Transform2>();
        Transform2* dcur = (Transform2*)col->data;
        Transform2* dprev = (Transform2*)col->prev;
        Span<Sprite> ds = dst->rows_of<Sprite>();
        Span<Glow> under = dst->rows_of<Glow>();
        // Blended, deep and saturated: over a pale sea a glow that adds light only whitens
        // the bullet; a dark halo is what makes it stand out, over the islands and clouds too.
        Sprite round = sprite_of(g, TEX_SOFT, rect2_unit(), vec2{60, 60}, Z_BULLET - 1, color{0.4f, 0.0f, 0.1f, 0.9f});
        Sprite needle = sprite_of(g, TEX_SOFT, rect2_unit(), vec2{48, 48}, Z_BULLET - 1, color{0.0f, 0.1f, 0.36f, 0.9f});
        parallel_for(ctx, dst->rows(), 2048, [&](ChunkContext&, u32 begin, u32 end) {
            for (u32 i = begin; i < end; ++i) {
                if (on && i < n && src->is_alive(i)) {
                    dcur[i] = cur[i];
                    // A glow taking a bullet it was not under starts where the bullet is: a
                    // move from where its last one died would be drawn across the field.
                    Handle h = src->handle_at(i);
                    bool fresh = under[i].bullet != h;
                    under[i].bullet = h;
                    if (dprev) dprev[i] = fresh || !prev.len ? cur[i] : prev[i];
                    const Sprite& want = look[i].uv.x > 0.25f ? needle : round;
                    if (ds[i].size.x != want.size.x) ds[i] = want;
                } else if (ds[i].size.x != 0.0f) {
                    ds[i].size = vec2{0, 0};
                    ds[i].tint.a = 0.0f;
                }
            }
        });
    }
}

void fx_shimmer(Game& g, point2 at, f32 reach, f32 strength, u32 life) {
    Game::Ring* slot = &g.rings[0];
    for (Game::Ring& r : g.rings) {
        if (!r.life) { slot = &r; break; }
        if (r.life - r.age < slot->life - slot->age) slot = &r; // the nearest its end gives way
    }
    *slot = Game::Ring{at, 0, life ? life : 1, reach, strength};
}

} // namespace kv::arcfire
