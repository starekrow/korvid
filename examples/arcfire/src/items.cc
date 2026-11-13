// arcfire: items. A cancelled bullet (a bomb, a death) becomes a score chip; the
// last member of a formation shot down to the last one drops a bomb; an item carrier (a
// formation the stage marks) drops what it carries: a weapon item, which shows a type and
// cycles through the four while it drifts down, slower than the rest (taking it takes the
// type it shows: the same as the ship's powers it, another switches to it), a power item,
// an option, an egg; inside the field, wherever its carrier died. Items pop up, fall,
// and come to the ship when it is above the magnet line (or within a short reach of it);
// the graze ring picks them up. A cancel makes at most kChipCap chips, so sixty thousand
// bullets never become sixty thousand rows: the rest are paid in score at once.
#include "arcfire.h"
#include "korvid/core/log.h"
#include "korvid/math/scalar.h"

namespace kv::arcfire {

namespace {

constexpr u32 kItemLife = 900;      // ticks: an item nobody comes for goes
constexpr f32 kFall = 1.6f;         // units a tick, at most
constexpr f32 kWeaponFall = 0.5f;   // a weapon item drifts down, through several turns of its cycle
constexpr f32 kGravity = 0.08f;
constexpr f32 kMagnetSpeed = 9.0f;
constexpr f32 kReach = 40.0f;       // an item this close comes whatever the line
constexpr u32 kCycle = 90;          // ticks a weapon item shows each type
constexpr i32 kPowerScore = 10000;  // a power past the last level pays this
const color kSparkChip{1.0f, 0.95f, 0.5f, 1.0f};

// made/items.png: 16 px cells, the four weapons' letters, then power, then option.
constexpr u32 kItemCells = 6;
rect2 items_uv(u32 cell) { return rect2{(f32)cell / (f32)kItemCells, 0.0f, 1.0f / (f32)kItemCells, 1.0f}; }
constexpr i32 kOptionScore = 20000; // an option past two (an egg past five: egg_gain)


Sprite item_sprite(const Game& g, ItemKind kind, i32 value) {
    if (kind == ItemBomb) return sprite_of(g, TEX_TILES, tile_uv(2, 2), vec2{26, 26}, Z_ITEM);
    if (kind == ItemWeapon) return sprite_of(g, TEX_ITEMS, items_uv((u32)value % WeaponCount), vec2{28, 28}, Z_ITEM);
    if (kind == ItemPower) return sprite_of(g, TEX_ITEMS, items_uv(4), vec2{28, 28}, Z_ITEM);
    if (kind == ItemOption) return sprite_of(g, TEX_ITEMS, items_uv(5), vec2{28, 28}, Z_ITEM);
    if (kind == ItemEgg) return sprite_of(g, TEX_EGG, rect2_unit(), vec2{26, 26}, Z_ITEM);
    return sprite_of(g, TEX_TILES, tile_uv(4, 0), vec2{14, 14}, Z_ITEM, color{1.0f, 1.0f, 1.0f, 0.95f});
}

} // namespace

u32 weapon_shown(const Item& it) { return ((u32)it.value + (kItemLife - it.life) / kCycle) % WeaponCount; }

Collider2 item_collider() { return collider_circle(8.0f, L_ITEM, L_GRAZE); }

void item_drop(TickContext& ctx, const Game& g, ItemKind kind, point2 at, vec2 v, i32 value) {
    Item it{(u32)kind, kind == ItemChip ? kChipValue : value, kItemLife, 0};
    ctx.spawn_later(g_item_kind, transform2_at(at), item_sprite(g, kind, value), Velocity{v}, item_collider(), it);
}

void drop_item(TickContext& ctx, const Game& g, u32 drop, point2 at) {
    if (drop == DropNone) return;
    // A carrier shot down before it flew in (Storm's bolts find them off the field) leaves
    // its item where it can be taken.
    at = point2{clampf(at.x, kFieldLeft + 24.0f, kFieldRight - 24.0f), at.y < kFieldTop - 24.0f ? at.y : kFieldTop - 24.0f};
    KV_INFO("arcfire", "drop: %s at tick %llu, (%.0f, %.0f)", drop >= DropWeapon && drop <= DropWeaponLast ? weapon_name(drop - DropWeapon)
            : drop == DropPower ? "power" : drop == DropOption ? "an option" : drop == DropEgg ? "an egg" : "?", (unsigned long long)ctx.tick, (f64)at.x, (f64)at.y);
    if (drop >= DropWeapon && drop <= DropWeaponLast) item_drop(ctx, g, ItemWeapon, at, vec2{0.0f, 1.8f}, (i32)(drop - DropWeapon));
    else if (drop == DropPower) item_drop(ctx, g, ItemPower, at, vec2{0.0f, 1.8f});
    else if (drop == DropOption) item_drop(ctx, g, ItemOption, at, vec2{0.0f, 1.8f});
    else if (drop == DropEgg) item_drop(ctx, g, ItemEgg, at, vec2{0.0f, 1.8f});
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
                // Its toss from the bullet and the tick: a chip is sim state, so not from the
                // cosmetic stream (which the sparks and the sounds draw from as they please).
                u64 h = seed_of(bullets_table->handle_at(i), ctx.tick, 0xC41Bu);
                f32 a = -0.6f + 1.2f * (f32)(h & 0xFFFFu) / 65535.0f;
                f32 up = 1.2f * (f32)((h >> 16) & 0xFFFFu) / 65535.0f;
                item_drop(ctx, g, ItemChip, p, vec2{kv::sin(a) * 1.2f, 1.2f + up});
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
    Span<const Transform2> ship_xf = ctx.read<Transform2>("arcfire.ship.Transform2");
    Span<const Ship> ships = ctx.read<Ship>("arcfire.ship.Ship");
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
            f32 fall = it[i].kind == ItemWeapon ? kWeaponFall : kFall;
            v[i].v.y = v[i].v.y - kGravity < -fall ? -fall : v[i].v.y - kGravity;
        }
        p += v[i].v;
        if (--it[i].life == 0 || p.y < kFieldBottom - kCullMargin) { ctx.destroy(items_table->handle_at(i)); continue; }
        if (it[i].kind == ItemWeapon) items_table->rows_of<Sprite>()[i].uv = items_uv(weapon_shown(it[i]));
    }
}

// The ring met an item: a chip is score, a bomb is a bomb (up to the most a ship holds).
void on_pickup(TickContext& ctx, Span<const Pickup> picks) {
    Game& g = ctx.state_as<Game>();
    Span<Run> run = ctx.write<Run>(ARC_WORLD(Run));
    Span<const Item> its = ctx.read<Item>("arcfire.item.Item");
    Span<const Transform2> xf = ctx.read<Transform2>("arcfire.item.Transform2");
    Span<Ship> ships = ctx.write<Ship>("arcfire.ship.Ship");
    if (!run.len) return;
    for (const Pickup& p : picks) {
        u32 i = p.item.index();
        if (i >= its.len) continue;
        if (its[i].kind == ItemWeapon || its[i].kind == ItemPower) {
            Ship* s = g.ship.index() < ships.len ? &ships[g.ship.index()] : nullptr;
            u32 type = its[i].kind == ItemWeapon ? weapon_shown(its[i]) : (s ? s->weapon : 0);
            if (s && s->weapon != type) {
                s->weapon = type; // a switch keeps the level
                KV_INFO("arcfire", "pickup: %s at level %u, tick %llu", weapon_name(type), s->power, (unsigned long long)ctx.tick);
            } else if (s && s->power < kMaxPower) {
                ++s->power;
                KV_INFO("arcfire", "pickup: %s to level %u, tick %llu", weapon_name(type), s->power, (unsigned long long)ctx.tick);
            } else {
                run[0].score += kPowerScore;
            }
            sfx(ctx, g, SFX_CHIP, 0.6f);
            fx_sparks(ctx, g, xf[i].position, 10, color{1.0f, 0.8f, 0.4f, 1.0f}, 3.0f);
        } else if (its[i].kind == ItemOption) {
            if (option_add(ctx, g)) KV_INFO("arcfire", "pickup: an option at tick %llu", (unsigned long long)ctx.tick);
            else run[0].score += kOptionScore;
            sfx(ctx, g, SFX_EGG_BLOOM);
        } else if (its[i].kind == ItemEgg) {
            if (egg_gain(run[0])) KV_INFO("arcfire", "pickup: an egg at tick %llu, %d in hand", (unsigned long long)ctx.tick, run[0].eggs);
            sfx(ctx, g, SFX_EGG_BLOOM, 1.3f);
        } else if (its[i].kind == ItemBomb) {
            if (run[0].bombs < kMaxBombs) ++run[0].bombs;
            sfx(ctx, g, SFX_CHIP, 0.7f);
            fx_sparks(ctx, g, xf[i].position, 8, color{0.6f, 0.85f, 1.0f, 1.0f}, 2.5f);
            KV_INFO("arcfire", "pickup: a bomb at tick %llu, %d in hand", (unsigned long long)ctx.tick, run[0].bombs);
        } else {
            run[0].score += its[i].value;
            ++run[0].chips;
            sfx(ctx, g, SFX_CHIP);
        }
        ctx.destroy(p.item);
    }
}

} // namespace kv::arcfire
