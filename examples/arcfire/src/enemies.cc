// arcfire: the enemies. An air row follows its formation's path by distance and
// starts its pattern once, where its emitter says along the path; a ground row rides the
// scroll and runs its pattern while the ship is above it (it does not turn around).
// Both flash white for the ticks after a hit, by drawing their sprite added instead of
// blended, and both leave the field without a word.
#include "arcfire.h"
#include "korvid/math/scalar.h"

namespace kv::arcfire {

namespace {

constexpr u32 kBunkerEvery = 240; // ticks between a bunker's launches

// The hit flash: the sprite's own texture, added, for the ticks the flash lasts.
void flash(const Game& g, Health& h, Sprite& s, Tex tex) {
    if (h.flash) { --h.flash; s.material = g.additive[tex]; }
    else s.material = g.alpha[tex];
}

} // namespace

// Phase Sim: movers, emitters started and stopped, the flash. The patterns themselves run
// in `patterns` (patterns.cc), after this.
void enemies(TickContext& ctx) {
    const Game& g = ctx.state_as<const Game>();
    Span<const Transform2> ship_xf = ctx.read<Transform2>("arcfire.ship.Transform2");
    Span<const Stage> stage_row = ctx.read<Stage>(ARC_WORLD(Stage));
    if (!stage_row.len || g.ship.index() >= ship_xf.len) return;
    point2 target = ship_xf[g.ship.index()].position;

    KTable* air = ctx.write_ktable(g_air_ktable);
    KTable* formations = ctx.write_ktable(g_formation_ktable);
    Span<Transform2> at = air->rows_of<Transform2>();
    Span<Mover> mv = air->rows_of<Mover>();
    Span<Emitter> em = air->rows_of<Emitter>();
    Span<Runner> run = air->rows_of<Runner>();
    Span<Health> hp = air->rows_of<Health>();
    Span<Sprite> sp = air->rows_of<Sprite>();
    for (u32 i = 0; i < air->rows(); ++i) {
        if (!air->is_alive(i)) continue;
        Mover& m = mv[i];
        m.s += m.speed;
        if (m.s >= m.path.length) { // away: its formation will not be shot down whole
            if (Formation* f = formations->get<Formation>(m.formation)) {
                if (f->killed + ++f->gone >= f->count) formations->destroy(m.formation);
            }
            ctx.destroy(air->handle_at(i));
            continue;
        }
        at[i].position = path_at(m.path, m.s);
        if (hp[i].look == (u32)Look::Debris) at[i].rotation += 0.02f + 0.015f * (f32)(i % 3); // wreckage tumbles
        // Its pattern starts where its formation said along the path, once it is on the field.
        if (em[i].state == EmitIdle && m.s >= em[i].at * m.path.length && in_field(at[i].position, -16.0f))
            emitter_start(g, em[i], run[i], air->handle_at(i), ctx.tick);
        Look look = (Look)hp[i].look;
        flash(g, hp[i], sp[i], look == Look::Turret ? TEX_TILES : look == Look::Boat ? TEX_BOAT : look == Look::Debris ? TEX_DEBRIS : TEX_SHIPS);
    }

    KTable* ground = ctx.write_ktable(g_ground_ktable);
    Span<Transform2> gt = ground->rows_of<Transform2>();
    Span<Emitter> ge = ground->rows_of<Emitter>();
    Span<Runner> gr = ground->rows_of<Runner>();
    Span<Ground> gg = ground->rows_of<Ground>();
    Span<Health> gh = ground->rows_of<Health>();
    Span<Sprite> gs = ground->rows_of<Sprite>();
    Span<const Run> run_row = ctx.read<Run>(ARC_WORLD(Run));
    Span<const Dials> dials_row = ctx.read<Dials>(ARC_WORLD(Dials));
    f32 speed = stage_row[0].speed;
    for (u32 i = 0; i < ground->rows(); ++i) {
        if (!ground->is_alive(i)) continue;
        point2& p = gt[i].position;
        p.y -= speed * gg[i].rate;
        if (p.y < kFieldBottom - kCullMargin) { ctx.destroy(ground->handle_at(i)); continue; }
        Ground& gk = gg[i];
        if (gk.kind == GroundTank && gk.range > 0.0f) { // along its road, turning at the ends
            p.x += gk.vx;
            if ((gk.vx > 0.0f && p.x > gk.home + gk.range) || (gk.vx < 0.0f && p.x < gk.home - gk.range)) gk.vx = -gk.vx;
        }
        if (gk.kind == GroundBunker && in_field(p, -16.0f) && gk.timer && --gk.timer == 0 && run_row.len) {
            // A launch: a formation of popcorn up out of the bunker, one event a member.
            gk.timer = kBunkerEvery;
            const FormationDef& f = g.stage.formations[gk.launch];
            u32 count = (u32)kv::lround((f32)f.count * dials_row[0].formation_count);
            if (count == 0) count = 1;
            Handle group = formations->spawn(Formation{count, 0, 0, 0});
            for (u32 m = 0; m < count; ++m) ctx.schedule(SpawnMember{gk.launch, m, run_row[0].epoch, 0, group, p}, ctx.tick + (u64)m * f.spacing);
        }
        // A ground row fires while the ship is above it (it does not turn round), its pattern
        // again a moment after it ends; the bench's fires whatever the ship does.
        Emitter& e = ge[i];
        bool above = gg[i].bench || (target.y > p.y + 24.0f && p.y < kFieldTop - 8.0f);
        if (!above) { e.state = EmitIdle; e.timer = 0; }
        else if (e.state == EmitDone) { e.state = EmitIdle; e.timer = gg[i].bench ? 60 : 30; }
        else if (e.state == EmitIdle && (e.timer == 0 || --e.timer == 0)) emitter_start(g, e, gr[i], ground->handle_at(i), ctx.tick);
        flash(g, gh[i], gs[i], TEX_TILES);
    }
}

// ---- what this file declares (its part of the module: module.cc lists it) -----------------------
namespace enemies_part {
constinit const FieldDesc k_ground_fields[]    = {KV_FIELDS(Ground, rate, bench, kind, launch, timer, vx, home, range, drop, pad)};
constinit const FieldDesc k_formation_fields[] = {KV_FIELDS(Formation, count, killed, gone, drop)};
// The path is a struct of its own (path.h), so its fields are named by hand.
constinit const FieldDesc k_mover_fields[] = {
    {"pts", FieldType::F32, 2 * kPathPoints, (u16)offsetof(Mover, path.pts)},
    {"seg", FieldType::F32, kPathPoints - 1, (u16)offsetof(Mover, path.seg)},
    {"length", FieldType::F32, 1, (u16)offsetof(Mover, path.length)},
    {"count", FieldType::U32, 1, (u16)offsetof(Mover, path.count)},
    {"speed", FieldType::F32, 1, (u16)offsetof(Mover, speed)},
    {"s", FieldType::F32, 1, (u16)offsetof(Mover, s)},
    {"formation", FieldType::Handle, 1, (u16)offsetof(Mover, formation)},
};
constinit const ComponentDesc k_components[] = {
    KV_COMPONENT(Mover, k_mover_fields),
    KV_COMPONENT(Ground, k_ground_fields),
    KV_COMPONENT(Formation, k_formation_fields),
};
constinit const KTableDesc k_ktables[] = {
    {.name = ".air",      .components = "Transform2, Sprite, Collider2, Health, Mover, Emitter, Runner", .capacity = 256, .id = &g_air_ktable},
    {.name = ".ground",   .components = "Transform2, Sprite, Collider2, Health, Emitter, Ground, Runner", .capacity = 256, .id = &g_ground_ktable},
    {.name = ".formation", .components = "Formation", .capacity = 64, .id = &g_formation_ktable},
};
constinit const SystemDesc k_systems[] = {
    {.name = ".enemies", .phase = Phase::Sim, .fn = enemies, .reads = ".ship.Transform2, .world.Stage, .world.Run, .world.Dials", .writes = ".air, .ground, .formation", .state = StateUse::Read},
};

constinit const TexFile k_textures[] = {
    {TEX_BOAT, "arcfire/generated/boat.png"},
    {TEX_DEBRIS, "arcfire/generated/debris.png"},
};
// The boats' and the debris's art.
bool startup(Engine& e, ModuleInstance& self) {
    Game& g = *self.state_as<Game>();
    textures_load(e, g, k_textures);
    return true;
}
} // namespace enemies_part
KV_MODULE_PART(arcfire_enemies) = {
    .components = enemies_part::k_components,
    .ktables    = enemies_part::k_ktables,
    .systems    = enemies_part::k_systems,
    .startup    = enemies_part::startup,
};

} // namespace kv::arcfire
