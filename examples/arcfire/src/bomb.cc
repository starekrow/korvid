// arcfire: the bomb. With one in hand and the ship live, the bomb action clears the
// field: every bullet becomes a chip (items.cc), every enemy on the field takes the blast
// (a handler, as a shot's hit is, so a kill pays as usual), the screen flashes and shakes,
// and for two seconds the hitbox meets nothing (the ring still grazes and picks up). Two
// bombs an egg; a death puts the count back to two; a formation shot down to the last one
// drops another.
#include "arcfire.h"
#include "korvid/core/log.h"
#include "korvid/core/time.h"
#include "korvid/platform_hid/actions.h"

namespace kv::arcfire {

namespace {

constexpr u32 kGuardTicks = 120;
constexpr i32 kBlastDamage = 20;
const color kSparkBlast{1.0f, 0.8f, 0.45f, 1.0f};

} // namespace

// Phase Sim, after the egg (both write the ship): the bomb's press, and its guard running out.
void bomb(TickContext& ctx) {
    static const u32 k_actions_id = ctx.resource_id("input.actions");
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    const InputActions& a = *ctx.resource<const InputActions>(k_actions_id);
    Span<Run> run = ctx.write<Run>(ARC_WORLD(Run));
    Span<const EggState> es = ctx.read<EggState>(ARC_WORLD(EggState));
    Span<Ship> ships = ctx.write<Ship>("arcfire.ship.Ship");
    Span<Collider2> cols = ctx.write<Collider2>("arcfire.ship.Collider2");
    Span<const Transform2> xf = ctx.read<Transform2>("arcfire.ship.Transform2");
    u32 i = g.ship.index();
    if (!run.len || i >= ships.len) return;
    Ship& s = ships[i];
    bool egg_idle = es[0].step == (u32)EggStep::None;
    // The guard: the hitbox back when it runs out, unless the egg has it (its shell).
    if (s.guard && --s.guard == 0 && egg_idle) cols[i] = ship_collider();
    if (!action_pressed(a, g.act_bomb) || !s.live || s.guard || run[0].bombs <= 0 || !egg_idle) return;

    --run[0].bombs;
    ++run[0].bombs_used;
    s.guard = kGuardTicks;
    cols[i].mask = 0;
    u32 overflow = 0;
    f64 t0 = time_now(); // the log's, not the sim's: what a field's cancel costs
    u32 cancelled = cancel_bullets(ctx, g, xf[i].position, 0.0f, &overflow);
    f64 ms = (time_now() - t0) * 1e3;
    run[0].score += (i64)overflow * kChipValue; // past the chips' cap, paid at once
    ctx.emit(Bombed{xf[i].position, cancelled, 0});
    hud_flash(e);
    sfx(ctx, g, SFX_BOMB);
    sfx(ctx, g, SFX_RUMBLE);
    g.shake = 14.0f;
    fx_sparks(ctx, g, xf[i].position, 24, kSparkBlast, 6.0f);
    fx_shimmer(g, xf[i].position, 520.0f, 1.0f, 60);
    KV_INFO("arcfire", "bomb: tick %llu, %u bullets cancelled (%u past the chips' cap) in %.2f ms, %d bombs left", (unsigned long long)ctx.tick, cancelled,
            overflow, ms, run[0].bombs);
}

// The blast: every enemy on the field takes it; a kill is a kill. A boss's open parts take
// it too (their trigger does the rest); its hull and armor do not.
void on_bomb(TickContext& ctx, Span<const Bombed> blasts) {
    Game& g = ctx.state_as<Game>();
    for (u32 n = 0; n < blasts.len; ++n) {
        const u32 kinds[] = {g_air_kind, g_ground_kind};
        for (u32 kind : kinds) {
            bool air = kind == g_air_kind;
            Span<const Health> hp = ctx.read<Health>(air ? "arcfire.air.Health" : "arcfire.ground.Health");
            Span<const Transform2> xf = ctx.read<Transform2>(air ? "arcfire.air.Transform2" : "arcfire.ground.Transform2");
            KindRows t = ctx.rows(kind); // liveness and handles, declared with the columns
            for (u32 i = 0; i < t.rows(); ++i) {
                if (!t.alive(i) || hp[i].hp <= 0 || !in_field(xf[i].position, 0.0f)) continue;
                fx_sparks(ctx, g, xf[i].position, 3, kSparkBlast, 3.0f);
                bool killed = false;
                hurt(ctx, g, t.handle_at(i), kBlastDamage, xf[i].position, &killed); // a kill pays and drops as a shot's does
            }
        }
        Span<Health> php = ctx.write<Health>("arcfire.part.Health");
        Span<const Transform2> pxf = ctx.read<Transform2>("arcfire.part.Transform2");
        KindRows parts = ctx.rows(g_part_kind);
        for (u32 i = 0; i < parts.rows(); ++i) {
            if (!parts.alive(i) || php[i].hp <= 0 || !in_field(pxf[i].position, 0.0f) || part_absorbs(g, ctx, parts.handle_at(i))) continue;
            php[i].hp -= kBlastDamage;
            php[i].flash = 4;
            fx_sparks(ctx, g, pxf[i].position, 3, kSparkBlast, 3.0f);
        }
    }
}

} // namespace kv::arcfire
