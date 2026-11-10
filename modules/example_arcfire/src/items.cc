// example_arcfire: items. A cancelled bullet (a bomb, a death) becomes a score chip; the
// last member of a formation shot down to the last one drops a bomb. Items pop up, fall,
// and come to the ship when it is above the magnet line (or within a short reach of it);
// the graze ring picks them up. A cancel makes at most kChipCap chips, so sixty thousand
// bullets never become sixty thousand rows: the rest are paid in score at once.
#include "arcfire.h"
#include "korvid/core/log.h"
#include <math.h>

namespace kv::arcfire {

namespace {

constexpr u32 kItemLife = 900;      // ticks: an item nobody comes for goes
constexpr f32 kFall = 1.6f;         // units a tick, at most
constexpr f32 kGravity = 0.08f;
constexpr f32 kMagnetSpeed = 9.0f;
constexpr f32 kReach = 40.0f;       // an item this close comes whatever the line
const color kSparkChip{1.0f, 0.95f, 0.5f, 1.0f};

Sprite item_sprite(const Game& g, ItemKind kind) {
    if (kind == ItemBomb) return sprite_of(g, TEX_TILES, tile_uv(2, 2), vec2{26, 26}, Z_ITEM);
    return sprite_of(g, TEX_TILES, tile_uv(4, 0), vec2{14, 14}, Z_ITEM, color{1.0f, 1.0f, 1.0f, 0.95f});
}

} // namespace

Collider2 item_collider() { return collider_circle(8.0f, L_ITEM, L_GRAZE); }

void item_drop(TickContext& ctx, const Game& g, ItemKind kind, point2 at, vec2 v) {
    Item it{(u32)kind, kind == ItemChip ? kChipValue : 0, kItemLife, 0};
    ctx.spawn_later(g_item_kind, transform2_at(at), item_sprite(g, kind), Velocity{v}, item_collider(), it);
}

// Every bullet of both kinds within `radius` of `at` (all of them for radius <= 0) goes; the
// first kChipCap become chips, popping up and out from where they were, a spark with each
// of the first few. Reads the bullets as the tick began (the callers declare it early).
u32 cancel_bullets(TickContext& ctx, Game& g, point2 at, f32 radius, u32* overflow) {
    u32 cancelled = 0, chips = 0;
    const u32 kinds[] = {g_bullet_kind, g_active_kind};
    for (u32 kind : kinds) {
        const Table* bullets_table = ctx.read_kind(kind);
        Span<const Transform2> bt = bullets_table->rows_of<Transform2>();
        for (u32 i = 0; i < bullets_table->rows(); ++i) {
            if (!bullets_table->is_alive(i)) continue;
            point2 p = bt[i].position;
            if (radius > 0.0f && length_sq(p - at) > radius * radius) continue;
            ctx.destroy(bullets_table->handle_at(i));
            ++cancelled;
            if (chips < kChipCap) {
                f32 a = g.fx.range(-0.6f, 0.6f);
                item_drop(ctx, g, ItemChip, p, vec2{sinf(a) * 1.2f, 1.2f + g.fx.range(0.0f, 1.2f)});
                ++chips;
            }
            if (cancelled <= 48) fx_sparks(ctx, g, p, 1, kSparkChip, 1.5f);
        }
    }
    g.stats.chips_made += chips;
    g.stats.chips_refused += cancelled - chips;
    if (overflow) *overflow = cancelled - chips;
    return cancelled;
}

// Phase Sim, after control: items fall, or come to the ship; they go at the bottom or at
// the end of their life.
void items(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    Span<const Transform2> ship_xf = ctx.read<Transform2>("example_arcfire.ship.Transform2");
    Span<const Ship> ships = ctx.read<Ship>("example_arcfire.ship.Ship");
    if (g.ship.index() >= ship_xf.len) return;
    point2 ship = ship_xf[g.ship.index()].position;
    bool live = ships[g.ship.index()].live != 0;
    bool magnet = live && ship.y > kMagnetY;
    Table* items_table = ctx.write_kind(g_item_kind);
    Span<Transform2> t = items_table->rows_of<Transform2>();
    Span<Velocity> v = items_table->rows_of<Velocity>();
    Span<Item> it = items_table->rows_of<Item>();
    for (u32 i = 0; i < items_table->rows(); ++i) {
        if (!items_table->is_alive(i)) continue;
        point2& p = t[i].position;
        vec2 to = ship - p;
        f32 d = length(to);
        if (live && (magnet || d < kReach) && d > 0.01f) v[i].v = to * (kMagnetSpeed / d);
        else {
            v[i].v.x *= 0.95f;
            v[i].v.y = v[i].v.y - kGravity < -kFall ? -kFall : v[i].v.y - kGravity;
        }
        p += v[i].v;
        if (--it[i].life == 0 || p.y < kFieldBottom - kCullMargin) ctx.destroy(items_table->handle_at(i));
    }
}

// The ring met an item: a chip is score, a bomb is a bomb (up to the most a ship holds).
void on_pickup(TickContext& ctx, Span<const Pickup> picks) {
    Game& g = ctx.state_as<Game>();
    Span<Run> run = ctx.write<Run>(ARC_WORLD(Run));
    Span<const Item> its = ctx.read<Item>("example_arcfire.item.Item");
    Span<const Transform2> xf = ctx.read<Transform2>("example_arcfire.item.Transform2");
    if (!run.len) return;
    for (const Pickup& p : picks) {
        u32 i = p.item.index();
        if (i >= its.len) continue;
        if (its[i].kind == ItemBomb) {
            if (run[0].bombs < kMaxBombs) ++run[0].bombs;
            fx_sparks(ctx, g, xf[i].position, 8, color{0.6f, 0.85f, 1.0f, 1.0f}, 2.5f);
            KV_INFO("arcfire", "pickup: a bomb at tick %llu, %d in hand", (unsigned long long)ctx.tick, run[0].bombs);
        } else {
            run[0].score += its[i].value;
            ++run[0].chips;
        }
        ctx.destroy(p.item);
    }
}

} // namespace kv::arcfire
