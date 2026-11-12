// arcfire: enemy bullets. Two kinds: `bullet`, every plain one (a position, a
// velocity, a life in ticks, whether it has grazed the ship, who fired it), and `active`,
// the same and a Runner, for a bullet whose pattern gives it a program of its own. They fly
// straight, they die at the end of their life or past the field's edge, and they meet only
// the hitbox, the graze ring and the shell (never each other, never an enemy).
#include "arcfire.h"
#include <math.h>

namespace kv::arcfire {

namespace {

constexpr u32 kBulletLife = 900;           // ticks: fifteen seconds, longer than any crossing (a seed waits, then bursts)
const color kBulletTint{1.0f, 0.55f, 0.62f, 1.0f};
const color kActiveTint{0.62f, 0.85f, 1.0f, 1.0f}; // a bullet with a program reads apart

} // namespace

Collider2 bullet_collider() { return collider_circle(4.0f, L_BULLET, L_SHIP | L_GRAZE | L_SHELL); }

// A runner's fire: a plain bullet, or an active one with its own runner when its definition
// has actions. By the chunk's commands; the speed dial scales what the pattern said.
void bullet_fire(ChunkContext& cc, const Game& g, const Dials& d, point2 at, const BmlFire& f, Handle owner, u64 seed) {
    vec2 v = bml_velocity(f.dir, f.speed) * d.bullet_speed;
    bool needle = f.bullet < sizeof g.bullet_look && g.bullet_look[f.bullet] == 1;
    rect2 uv = needle ? rect2{0.5f, 0.0f, 0.5f, 1.0f} : rect2{0.0f, 0.0f, 0.5f, 1.0f};
    Transform2 xf = transform2_at(at, needle ? -f.dir * (KV_PI / 180.0f) : 0.0f);
    if (!bml_bullet_actions(g.lib, f.bullet)) {
        cc.spawn_later(g_bullet_kind, xf, sprite_of(g, TEX_BULLETS, uv, vec2{26, 26}, Z_BULLET, kBulletTint, true), Velocity{v}, bullet_collider(),
                       Bullet{kBulletLife, 0, owner});
        return;
    }
    Runner r;
    bml_start_bullet(g.lib, r, f, seed);
    cc.spawn_later(g_active_kind, xf, sprite_of(g, TEX_BULLETS, uv, vec2{26, 26}, Z_BULLET, kActiveTint, true), Velocity{v}, bullet_collider(),
                   Bullet{kBulletLife, 0, owner}, r);
}

Bullet* bullet_row(TickContext& ctx, Handle h) {
    static const u32 k_plain = ctx.column_id("arcfire.bullet.Bullet");
    static const u32 k_active = ctx.column_id("arcfire.active.Bullet");
    bool active = h.kind() == engine_table_now(*ctx.engine, g_active_kind)->kind;
    Span<Bullet> rows = ctx.write<Bullet>(active ? k_active : k_plain);
    return h.index() < rows.len ? &rows[h.index()] : nullptr;
}

const Transform2* bullet_xf(TickContext& ctx, Handle h) {
    static const u32 k_plain = ctx.column_id("arcfire.bullet.Transform2");
    static const u32 k_active = ctx.column_id("arcfire.active.Transform2");
    bool active = h.kind() == engine_table_now(*ctx.engine, g_active_kind)->kind;
    Span<const Transform2> rows = ctx.read<Transform2>(active ? k_active : k_plain);
    return h.index() < rows.len ? &rows[h.index()] : nullptr;
}

// Phase Sim, after the patterns: every bullet of both kinds moves and ages, in parallel.
void bullets(TickContext& ctx) {
    for (KindRows k : ctx.view<Transform2, const Velocity, Bullet>()) {
        Span<Transform2> t = k.col<Transform2>();
        Span<const Velocity> v = k.col<const Velocity>();
        Span<Bullet> b = k.col<Bullet>();
        parallel_for(ctx, k.rows(), 1024, [&](ChunkContext& cc, u32 begin, u32 end) {
            for (u32 i = begin; i < end; ++i) {
                if (!k.alive(i)) continue;
                t[i].position += v[i].v;
                if (b[i].life) --b[i].life;
                if (!b[i].life || !in_field(t[i].position, kCullMargin)) cc.destroy(k.handle_at(i));
            }
        });
    }
}

} // namespace kv::arcfire
