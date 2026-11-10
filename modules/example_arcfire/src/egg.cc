// example_arcfire: the egg. A death is the game's set piece, not a setback: time slows to
// a quarter, the ship breaks into its four quarters, the bullets near it cancel into
// sparks, and the egg drops: a small spherical machine that comes to rest where the ship
// died, builds the new ship around itself as time comes back, and splits along its seam,
// its halves falling away. A shell stays around the new ship for two seconds eating
// bullets. Eggs are lives; a death with none left restarts the stage behind a cover.
//
// Every number below is a sim tick from the death. The egg system runs on the slowed
// clock like everything else (slow time is not a pause), and sets the engine's time scale
// itself each tick, so the sequence is as long in ticks at any frame rate and a demo that
// dies hashes as one that does not would at the same ticks.
#include "arcfire.h"
#include "korvid/core/log.h"

namespace kv::arcfire {

namespace {

constexpr u32 kSlowIn = 6;        // ticks to slow to a quarter
constexpr u32 kEggAt = 10;        // the egg drops
constexpr u32 kRestAt = 40;       // it rests and builds
constexpr u32 kSplitAt = 54;      // it opens and falls away
constexpr u32 kLiveAt = 60;       // the new ship is live, in its shell
constexpr u32 kShellTicks = 120, kShellFade = 20;
constexpr f32 kSlow = 0.25f;
constexpr f32 kCancelRadius = 120.0f;
constexpr f32 kShellRadius = 24.0f;
constexpr u32 kRestartCover = 30; // ticks the restart's cover stays up
const color kShellTint{0.55f, 0.85f, 1.0f, 0.45f};

f32 time_scale_at(u32 t) {
    if (t < kSlowIn) return 1.0f - (1.0f - kSlow) * (f32)(t + 1) / (f32)kSlowIn;
    if (t < kRestAt) return kSlow;
    if (t < kLiveAt) return kSlow + (1.0f - kSlow) * (f32)(t - kRestAt + 1) / (f32)(kLiveAt - kRestAt);
    return 1.0f;
}

// The death: the ship hides and stops meeting anything, its quarters tumble out, the
// bullets near it go in sparks, the camera shakes.
void break_up(TickContext& ctx, Game& g, const EggState& es, Transform2& xf, Sprite& sprite, Collider2& col, Collider2* ring) {
    sprite.tint.a = 0.0f;
    col.layer = 0;
    col.mask = 0;
    if (ring) ring->mask = 0;
    for (u32 q = 0; q < 4; ++q) {
        f32 sx = (q & 1) ? 1.0f : -1.0f, sy = (q & 2) ? -1.0f : 1.0f;
        rect2 uv = ship_uv(0, 0);
        uv = rect2{uv.x + (q & 1 ? uv.w * 0.5f : 0.0f), uv.y + (q & 2 ? uv.h * 0.5f : 0.0f), uv.w * 0.5f, uv.h * 0.5f};
        vec2 v{sx * g.fx.range(1.0f, 2.2f), sy * g.fx.range(1.0f, 2.2f)};
        fx_piece(ctx, g, xf.position + vec2{sx * 16.0f, sy * 16.0f}, TEX_SHIPS, uv, vec2{32, 32}, v, g.fx.range(-0.2f, 0.2f), 90, Z_SHIP);
    }
    u32 cancelled = cancel_bullets(ctx, g, es.at, kCancelRadius, nullptr); // into chips, to collect after
    g.shake = 10.0f;
    KV_INFO("arcfire", "egg: the ship breaks at (%.0f, %.0f), tick %llu, %u bullets cancelled", (f64)es.at.x, (f64)es.at.y,
            (unsigned long long)ctx.tick, cancelled);
}

} // namespace

// Phase Sim, after control: the sequence, a step a tick; and the restart's cover.
void egg(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    Span<EggState> es_row = ctx.write<EggState>(ARC_WORLD(EggState));
    Span<Chance> chance = ctx.write<Chance>(ARC_WORLD(Chance));
    if (!es_row.len || g.ship.is_null()) return;
    EggState& es = es_row[0];
    if (es.restart && --es.restart == 0) hud_cover(e, g, false);

    Table* ships = ctx.write_kind(g_ship_kind);
    Table* eggs = ctx.write_kind(g_egg_kind);
    Table* shells = ctx.write_kind(g_shell_kind);
    Span<Collider2> ring_col = ctx.write<Collider2>("example_arcfire.ring.Collider2");
    Collider2* ring = g.ring.index() < ring_col.len ? &ring_col[g.ring.index()] : nullptr;
    Transform2* xf = ships->get<Transform2>(g.ship);
    Sprite* sprite = ships->get<Sprite>(g.ship);
    Collider2* col = ships->get<Collider2>(g.ship);
    Ship* ship = ships->get<Ship>(g.ship);
    if (!xf) return;

    switch ((EggStep)es.step) {
    case EggStep::None: return;
    case EggStep::Shelled: {
        Handle shell = shells->live() ? shells->handle_at(0) : Handle{};
        if (Sprite* ss = shells->get<Sprite>(shell)) ss->tint.a = kShellTint.a * (es.shell < kShellFade ? (f32)es.shell / (f32)kShellFade : 1.0f);
        if (es.shell && --es.shell) return;
        // The shell goes; the hitbox meets bullets again.
        shells->destroy_all();
        *col = ship_collider();
        es.step = (u32)EggStep::None;
        KV_INFO("arcfire", "egg: the shell is gone at tick %llu", (unsigned long long)ctx.tick);
        return;
    }
    default: break;
    }

    u32 t = es.t;
    engine_time_scale(e, time_scale_at(t));
    if (t == 0) { break_up(ctx, g, es, *xf, *sprite, *col, ring); sfx(ctx, g, SFX_EGG_BREAK); }
    if (t == kEggAt) {
        Rng r;
        r.state = chance[0].state; r.inc = chance[0].inc;
        f32 drift = r.range(-0.3f, 0.3f); // a little, from the sim's stream
        chance[0].state = r.state;
        Sprite s = sprite_of(g, TEX_EGG, rect2_unit(), vec2{32, 32}, Z_SHIP + 1); // over the ship it builds
        eggs->spawn(transform2_at(es.at + vec2{0.0f, 12.0f}), s, EggBody{-1.0f, drift});
    }
    Handle egg_row = eggs->live() ? eggs->handle_at(0) : Handle{};
    Transform2* et = eggs->get<Transform2>(egg_row);
    if (Sprite* pod = eggs->get<Sprite>(egg_row)) { // its core pulses while it works
        f32 pulse = 0.8f + 0.2f * (f32)((t / 4) % 2);
        pod->tint = color{pulse, pulse, pulse, 1.0f};
    }
    if (et && t > kEggAt && t < kRestAt) { // it falls a little, rocking, and settles
        const EggBody* body = eggs->get<EggBody>(egg_row);
        et->position += vec2{body->drift, body->vy};
        et->rotation = 0.15f * (f32)((t / 6) % 2 ? 1 : -1) * (f32)(kRestAt - t) / (f32)(kRestAt - kEggAt);
    }
    if (et && t == kRestAt) { // the ship is built where it rests
        es.at = et->position;
        et->rotation = 0.0f;
        *xf = transform2_at(et->position);
        *ships->prev<Transform2>(g.ship) = *xf; // a teleport: no sweep from where it died
        sfx(ctx, g, SFX_EGG_BLOOM);
    }
    if (et && t >= kRestAt && t < kSplitAt && t % 2 == 0) // building: sparks from the core
        fx_sparks(ctx, g, et->position, 2, color{0.55f, 0.95f, 1.0f, 1.0f}, 2.5f);
    if (et && t == kSplitAt) {
        // It opens along its seam and its halves fall away, turning.
        sfx(ctx, g, SFX_EGG_OPEN);
        fx_piece(ctx, g, et->position + vec2{0, 8}, TEX_EGG, rect2{0, 0, 1, 0.5f}, vec2{32, 16}, vec2{-0.9f, 1.4f}, -0.06f, 70, Z_SHIP + 1, 0.12f);
        fx_piece(ctx, g, et->position + vec2{0, -8}, TEX_EGG, rect2{0, 0.5f, 1, 0.5f}, vec2{32, 16}, vec2{0.9f, 0.4f}, 0.07f, 70, Z_SHIP + 1, 0.12f);
        eggs->destroy_all();
    }
    if (t >= kRestAt && t < kLiveAt) {
        f32 k = (f32)(t - kRestAt + 1) / (f32)(kLiveAt - kRestAt);
        sprite->tint.a = k;
        xf->position.y = es.at.y + 20.0f * (t < kSplitAt ? 0.0f : (f32)(t - kSplitAt + 1) / (f32)(kLiveAt - kSplitAt));
    }
    if (t == kLiveAt) {
        eggs->destroy_all();
        sprite->tint.a = 1.0f;
        ship->live = 1;
        col->layer = L_SHIP;
        col->mask = 0; // the shell takes the bullets until it goes
        if (ring) *ring = ring_collider();
        Sprite s = sprite_of(g, TEX_SOFT, rect2_unit(), vec2{2.4f * kShellRadius, 2.4f * kShellRadius}, Z_SHIP + 2, kShellTint, true);
        shells->spawn(Transform2{*xf}, Local2{}, Parent{g.ship}, s, collider_circle(kShellRadius, L_SHELL, L_BULLET));
        es.shell = kShellTicks;
        es.step = (u32)EggStep::Shelled;
        KV_INFO("arcfire", "egg: the ship is back at tick %llu", (unsigned long long)ctx.tick);
        return;
    }
    es.step = (u32)(t < kEggAt ? EggStep::Breaking : t < kRestAt ? EggStep::Falling : EggStep::Blooming);
    es.t = t + 1;
}

// The ship died (hits.cc). With an egg in hand the sequence begins; with none the stage
// starts over behind a cover, eggs refilled, the score as it was when the stage began.
void on_ship_died(TickContext& ctx, Span<const ShipDied> deaths) {
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    Span<Run> run = ctx.write<Run>(ARC_WORLD(Run));
    Span<EggState> es_row = ctx.write<EggState>(ARC_WORLD(EggState));
    Span<Stage> stage_row = ctx.write<Stage>(ARC_WORLD(Stage));
    if (!run.len || !deaths.len) return;
    const ShipDied& d = deaths[0]; // one death a tick: the hit handlers make sure
    Table* ships = ctx.write_kind(g_ship_kind);
    const Transform2* died_at = ships->get<Transform2>(g.ship);
    Run& r = run[0];
    EggState& es = es_row[0];
    ++r.deaths;
    if (r.eggs > 0) {
        --r.eggs;
        es = EggState{};
        es.step = (u32)EggStep::Breaking;
        es.at = died_at ? died_at->position : kShipStart;
        r.bombs = kBaseBombs; // a death puts the bombs back to two
        KV_INFO("arcfire", "died: %s at tick %llu, %d eggs left", d.cause ? "rammed" : "shot", (unsigned long long)ctx.tick, r.eggs);
        return;
    }
    // Out of eggs: everything the stage made goes, the stage's clock goes back to its
    // start, and anything it had scheduled is of an epoch that no longer spawns.
    const u32 kinds[] = {g_air_kind, g_ground_kind, g_bullet_kind, g_active_kind, g_shot_kind, g_egg_kind, g_shell_kind, g_item_kind, g_formation_kind,
                         g_boss_kind, g_part_kind};
    for (u32 k : kinds) ctx.destroy_all(k);
    stage_reset(stage_row[0], g.stage);
    r.score = r.stage_score;
    r.eggs = 3;
    r.bombs = kBaseBombs;
    ++r.restarts;
    ++r.epoch;
    es = EggState{};
    es.restart = kRestartCover;
    if (Transform2* xf = ships->get<Transform2>(g.ship)) {
        *xf = transform2_at(kShipStart);
        *ships->prev<Transform2>(g.ship) = *xf;
        ships->get<Ship>(g.ship)->live = 1;
        ships->get<Ship>(g.ship)->guard = 0;
        ships->get<Sprite>(g.ship)->tint.a = 1.0f;
        *ships->get<Collider2>(g.ship) = ship_collider();
    }
    engine_time_scale(e, 1.0f);
    hud_cover(e, g, true);
    music(ctx, g, false);
    KV_INFO("arcfire", "restart %u at tick %llu: out of eggs, score back to %lld", r.restarts, (unsigned long long)ctx.tick, (long long)r.score);
}

} // namespace kv::arcfire
