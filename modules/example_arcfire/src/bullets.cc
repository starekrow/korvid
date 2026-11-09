// example_arcfire: enemy bullets. One kind holds every one of them: a position, a
// velocity, a life in ticks, whether it has grazed the ship, who fired it. They fly
// straight, they die at the end of their life or past the field's edge, and they meet only
// the hitbox, the graze ring and the shell (never each other, never an enemy).
#include "arcfire.h"
#include <math.h>

namespace kv::arcfire {

namespace {

constexpr u32 kBulletLife = 600;           // ticks: ten seconds, longer than any crossing
constexpr f32 kFanDeg = 9.0f;              // between the bullets of one aimed volley
const color kBulletTint{1.0f, 0.55f, 0.62f, 1.0f};

} // namespace

Collider2 bullet_collider() { return collider_circle(4.0f, L_BULLET, L_SHIP | L_GRAZE | L_SHELL); }

// `count` bullets (times the density dial, one at least) in a fan centered on the line to
// the target, at `speed` times the speed dial. Leads the target by nothing: moderate.
u32 bullets_fire_aimed(Table& bullets_table, const Game& g, const Dials& d, point2 from, point2 target, u32 count, f32 speed, Handle owner) {
    u32 n = (u32)lroundf((f32)count * d.bullet_density);
    if (n == 0) n = 1;
    vec2 to = target - from;
    f32 aim = length_sq(to) > 1e-6f ? atan2f(to.y, to.x) : -KV_PI * 0.5f;
    f32 fan = kFanDeg * (KV_PI / 180.0f);
    f32 v = speed * d.bullet_speed;
    u32 made = 0;
    for (u32 k = 0; k < n; ++k) {
        f32 a = aim + ((f32)k - (f32)(n - 1) * 0.5f) * fan;
        Sprite s = sprite_of(g, TEX_BULLETS, rect2{0.0f, 0.0f, 0.5f, 1.0f}, vec2{26, 26}, Z_BULLET, kBulletTint, true);
        if (bullets_table.spawn(transform2_at(from), s, Velocity{vec2{cosf(a) * v, sinf(a) * v}}, bullet_collider(), Bullet{kBulletLife, 0, owner}).is_null())
            break; // the kind is full: the stress dial finds this
        ++made;
    }
    return made;
}

// Phase Sim, after the enemies: every bullet moves and ages, in parallel.
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
