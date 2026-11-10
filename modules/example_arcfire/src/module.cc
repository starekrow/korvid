// example_arcfire: the module record. What the game declares to the engine (components,
// kinds, events, the systems and handlers of every file), the startup that loads its art
// and its stage, and the sprite helpers every file draws with. The game itself is in the
// files the tables below name.
#include "arcfire.h"
#include "korvid/asset/blob.h"
#include "korvid/asset/material.h"
#include "korvid/asset/scope.h"
#include "korvid/asset/texture.h"
#include "korvid/core/log.h"
#include "korvid/frameloop/frameloop.h"
#include "korvid/platform_hid/actions.h"
#include "korvid/ui/ui.h"
#include <stddef.h>
#include <stdio.h>

namespace kv::arcfire {

constinit u32 g_world_kind = 0, g_plane_kind = 0, g_ship_kind = 0, g_ring_kind = 0, g_mark_kind = 0, g_shot_kind = 0, g_air_kind = 0,
              g_ground_kind = 0, g_bullet_kind = 0, g_active_kind = 0, g_particle_kind = 0, g_egg_kind = 0, g_shell_kind = 0;

// The systems and handlers, each in the file of its part.
void demo(TickContext&);         // pilot.cc
void bot(TickContext&);
void boot(TickContext&);         // stage.cc
void scroll(TickContext&);
void stage(TickContext&);
void on_member(TickContext&, Span<const SpawnMember>);
void on_stage_end(TickContext&, Span<const StageEnd>);
void planes(TickContext&);       // planes.cc
void control(TickContext&);      // ship.cc
void shots(TickContext&);
void enemies(TickContext&);      // enemies.cc
void patterns(TickContext&);     // patterns.cc
void bullets(TickContext&);      // bullets.cc
void particles(TickContext&);    // fx.cc
void egg(TickContext&);          // egg.cc
void on_ship_died(TickContext&, Span<const ShipDied>);
void on_overlap(TickContext&, Span<const OverlapBegin>); // hits.cc
void on_shot_hit(TickContext&, Span<const ShotHit>);
void on_bullet_hit(TickContext&, Span<const BulletHit>);
void on_rammed(TickContext&, Span<const Rammed>);
void on_graze(TickContext&, Span<const Graze>);
void on_shell_hit(TickContext&, Span<const ShellHit>);
void on_kill(TickContext&, Span<const Kill>);
void pause(TickContext&);        // hud.cc
void camera(TickContext&);
void hud(TickContext&);
void stats_mark(TickContext&);   // stats.cc
void stats(TickContext&);
void on_exit(TickContext&, Span<const ExitRequested>);

namespace {

// ---- schemas -------------------------------------------------------------------------------------

constinit const FieldDesc k_velocity_fields[] = {KV_FIELDS(Velocity, v)};
constinit const FieldDesc k_plane_fields[]    = {KV_FIELDS(Plane, rate, row, col, rows)};
constinit const FieldDesc k_ship_fields[]     = {KV_FIELDS(Ship, cooldown, live, focus, shots)};
constinit const FieldDesc k_shot_fields[]     = {KV_FIELDS(Shot, damage, spent)};
constinit const FieldDesc k_health_fields[]   = {KV_FIELDS(Health, hp, flash, score, look)};
// The path is a struct of its own (path.h), so its fields are named by hand.
constinit const FieldDesc k_mover_fields[] = {
    {"pts", FieldType::F32, 2 * kPathPoints, (u16)offsetof(Mover, path.pts)},
    {"seg", FieldType::F32, kPathPoints - 1, (u16)offsetof(Mover, path.seg)},
    {"length", FieldType::F32, 1, (u16)offsetof(Mover, path.length)},
    {"count", FieldType::U32, 1, (u16)offsetof(Mover, path.count)},
    {"speed", FieldType::F32, 1, (u16)offsetof(Mover, speed)},
    {"s", FieldType::F32, 1, (u16)offsetof(Mover, s)},
};
constinit const FieldDesc k_emitter_fields[]  = {KV_FIELDS(Emitter, pattern, at, state, timer)};
constinit const FieldDesc k_runner_fields[]   = {KV_FIELDS(BmlRunner, rng, wait, frame16, frame8, track8, params, last, term, delta, dir, speed, mx, my, flags)};
constinit const FieldDesc k_ground_fields[]   = {KV_FIELDS(Ground, rate, bench)};
constinit const FieldDesc k_bullet_fields[]   = {KV_FIELDS(Bullet, life, grazed, owner)};
constinit const FieldDesc k_particle_fields[] = {KV_FIELDS(Particle, life, max_life, spin, drag, alpha, fall)};
constinit const FieldDesc k_eggbody_fields[]  = {KV_FIELDS(EggBody, vy, drift)};
constinit const FieldDesc k_dials_fields[]    = {KV_FIELDS(Dials, position, rank, bullet_speed, formation_count, formation_spacing, particles,
                                                           plane_density, enemy_health, bot, invulnerable, loops)};
constinit const FieldDesc k_stage_fields[]    = {KV_FIELDS(Stage, distance, speed, ramp_from, ramp_to, ramp_at, ramp_over, cursor, loops, ended, pad)};
constinit const FieldDesc k_run_fields[]      = {KV_FIELDS(Run, score, stage_score, eggs, restarts, epoch, kills, grazes, deaths)};
constinit const FieldDesc k_eggstate_fields[] = {KV_FIELDS(EggState, step, t, at, shell, restart)};
constinit const FieldDesc k_chance_fields[]   = {KV_FIELDS(Chance, state, inc)};

constinit const ComponentDesc k_components[] = {
    KV_COMPONENT(Velocity, k_velocity_fields), KV_COMPONENT(Plane, k_plane_fields),   KV_COMPONENT(Ship, k_ship_fields),
    KV_COMPONENT(Shot, k_shot_fields),         KV_COMPONENT(Health, k_health_fields), KV_COMPONENT(Mover, k_mover_fields),
    KV_COMPONENT(Emitter, k_emitter_fields),   KV_COMPONENT(Ground, k_ground_fields), KV_COMPONENT(Bullet, k_bullet_fields),
    KV_COMPONENT(Particle, k_particle_fields), KV_COMPONENT(EggBody, k_eggbody_fields), KV_COMPONENT(Dials, k_dials_fields),
    KV_COMPONENT(Stage, k_stage_fields),       KV_COMPONENT(Run, k_run_fields),       KV_COMPONENT(EggState, k_eggstate_fields),
    KV_COMPONENT(Chance, k_chance_fields),       KV_COMPONENT(Runner, k_runner_fields),
};

// ---- kinds ---------------------------------------------------------------------------------------
// Capacity is address space (rows commit as they are used), so the bullet kinds are sized
// for the stress dial; --capacity KIND=N goes past them for a measurement.
constinit const KindDesc k_kinds[] = {
    {.name = ".world",    .components = "Dials, Stage, Run, EggState, Chance", .capacity = 1, .id = &g_world_kind},
    {.name = ".plane",    .components = "Transform2, Sprite, Plane", .capacity = 4096, .id = &g_plane_kind, .cosmetic = true},
    {.name = ".ship",     .components = "Transform2, Sprite, Collider2, Ship", .capacity = 1, .id = &g_ship_kind},
    // The graze ring and the hitbox's dot: children of the ship.
    {.name = ".ring",     .components = "Transform2, Local2, Parent, Sprite, Collider2", .capacity = 1, .id = &g_ring_kind},
    {.name = ".mark",     .components = "Transform2, Local2, Parent, Sprite", .capacity = 1, .id = &g_mark_kind},
    {.name = ".shot",     .components = "Transform2, Sprite, Velocity, Collider2, Shot", .capacity = 2048, .id = &g_shot_kind},
    {.name = ".air",      .components = "Transform2, Sprite, Collider2, Health, Mover, Emitter, Runner", .capacity = 256, .id = &g_air_kind},
    {.name = ".ground",   .components = "Transform2, Sprite, Collider2, Health, Emitter, Ground, Runner", .capacity = 256, .id = &g_ground_kind},
    {.name = ".bullet",   .components = "Transform2, Sprite, Velocity, Collider2, Bullet", .capacity = 65536, .id = &g_bullet_kind},
    // Bullets that run a program of their own (a needle that waits, a seed that bursts).
    {.name = ".active",   .components = "Transform2, Sprite, Velocity, Collider2, Bullet, Runner", .capacity = 8192, .id = &g_active_kind},
    {.name = ".particle", .components = "Transform2, Sprite, Velocity, Particle", .capacity = 32768, .id = &g_particle_kind, .cosmetic = true},
    {.name = ".egg",      .components = "Transform2, Sprite, EggBody", .capacity = 1, .id = &g_egg_kind},
    {.name = ".shell",    .components = "Transform2, Local2, Parent, Sprite, Collider2", .capacity = 1, .id = &g_shell_kind},
};

// ---- events --------------------------------------------------------------------------------------

constinit const FieldDesc k_shot_hit_fields[]  = {KV_FIELDS(ShotHit, shot, target)};
constinit const FieldDesc k_bullet_hit_fields[] = {KV_FIELDS(BulletHit, bullet)};
constinit const FieldDesc k_rammed_fields[]    = {KV_FIELDS(Rammed, enemy)};
constinit const FieldDesc k_graze_fields[]     = {KV_FIELDS(Graze, bullet)};
constinit const FieldDesc k_shell_hit_fields[] = {KV_FIELDS(ShellHit, bullet)};
constinit const FieldDesc k_kill_fields[]      = {KV_FIELDS(Kill, at, score, pad)};
constinit const FieldDesc k_died_fields[]      = {KV_FIELDS(ShipDied, at, cause, pad)};
constinit const FieldDesc k_member_fields[]    = {KV_FIELDS(SpawnMember, formation, member, epoch, pad)};
constinit const FieldDesc k_end_fields[]       = {KV_FIELDS(StageEnd, loops, pad)};
constinit const EventTypeDesc k_events[] = {
    KV_EVENT_EX(ShotHit, k_shot_hit_fields, 0),   KV_EVENT_EX(BulletHit, k_bullet_hit_fields, 0), KV_EVENT_EX(Rammed, k_rammed_fields, 0),
    KV_EVENT_EX(Graze, k_graze_fields, 0),        KV_EVENT_EX(ShellHit, k_shell_hit_fields, 0),   KV_EVENT_EX(Kill, k_kill_fields, 0),
    KV_EVENT_EX(ShipDied, k_died_fields, 0),      KV_EVENT_EX(SpawnMember, k_member_fields, 0),   KV_EVENT_EX(StageEnd, k_end_fields, 0),
};

constinit const ResourceDesc k_resources[] = {{".stats"}};

constinit const InputActionDesc k_actions[] = {
    {"move",  ActionKind::Axis2,  "keys W S A D, keys Up Down Left Right, stick left, dpad"},
    {"shot",  ActionKind::Button, "key Z, key Space, pad South"},
    {"focus", ActionKind::Button, "key LShift, key RShift, pad West"},
    {"bomb",  ActionKind::Button, "key X, pad East"},
    {"pause", ActionKind::Button, "key Escape, pad Start"},
    {"quit",  ActionKind::Button, "key Q"},
    // The UI's: the pause screen and the covers read these.
    {"confirm", ActionKind::Button, "key Enter, pad South"},
    {"cancel",  ActionKind::Button, "key Backspace, pad East"},
};

// ---- the schedule -----------------------------------------------------------------------------
// Sim goes: the bot, the scroll and the stage's marks, the planes, the ship, its shots,
// the enemies and their bullets, the egg, the particles; then the collision module's
// broadphase, whose overlaps the handlers sort into hits at the boundary.

constinit const SystemDesc k_systems[] = {
    {.name = ".demo", .phase = Phase::Input, .fn = demo, .before = "input.sample", .main_thread = true},
    {.name = ".boot", .phase = Phase::PreSim, .fn = boot, .writes = ".world, .plane, .ship, .ring, .mark, .ground", .main_thread = true, .runs = RunWhen::Always},
    {.name = ".pause", .phase = Phase::PreSim, .fn = pause, .reads = "input.actions", .after = ".boot", .main_thread = true, .runs = RunWhen::Always},
    {.name = ".bot", .phase = Phase::Sim, .fn = bot, .reads = "^.bullet.Transform2, ^.bullet.Velocity, ^.active.Transform2, ^.active.Velocity, ^.ship.Transform2, ^.air.Transform2, ^.ground.Transform2, .world.Dials",
     .main_thread = true},
    {.name = ".scroll", .phase = Phase::Sim, .fn = scroll, .writes = ".world.Stage"},
    {.name = ".stage", .phase = Phase::Sim, .fn = stage, .reads = ".world.Dials, .world.Run", .writes = ".world.Stage", .after = ".scroll"},
    {.name = ".planes", .phase = Phase::Sim, .fn = planes, .reads = ".world.Stage, .plane.Plane, .plane.Sprite", .writes = ".plane.Transform2", .state = StateUse::None},
    {.name = ".control", .phase = Phase::Sim, .fn = control, .reads = "input.actions, .world.Dials",
     .writes = ".ship.Transform2, .ship.Ship, .ship.Sprite, .shot, .ring.Sprite, .mark.Sprite"},
    {.name = ".shots", .phase = Phase::Sim, .fn = shots, .reads = ".shot.Velocity", .writes = ".shot.Transform2", .after = ".control", .state = StateUse::None},
    {.name = ".enemies", .phase = Phase::Sim, .fn = enemies, .reads = ".ship.Transform2, .world.Stage", .writes = ".air, .ground", .state = StateUse::Read},
    {.name = ".patterns", .phase = Phase::Sim, .fn = patterns, .reads = ".ship.Transform2, .world.Dials, .air.Transform2, .ground.Transform2",
     .writes = ".air.Emitter, .air.Runner, .ground.Emitter, .ground.Runner, .active", .after = ".enemies"},
    {.name = ".bullets", .phase = Phase::Sim, .fn = bullets, .reads = ".bullet.Velocity, .active.Velocity",
     .writes = ".bullet.Transform2, .bullet.Bullet, .active.Transform2, .active.Bullet", .after = ".patterns", .state = StateUse::None},
    {.name = ".egg", .phase = Phase::Sim, .fn = egg, .reads = "^.bullet, ^.active", .writes = ".world.EggState, .world.Chance, .ship, .ring.Collider2, .egg, .shell",
     .after = ".control", .main_thread = true},
    {.name = ".particles", .phase = Phase::Sim, .fn = particles, .writes = ".particle", .state = StateUse::None},
    {.name = ".camera", .phase = Phase::PostSim, .fn = camera, .reads = "platform_hid.window", .writes = "sprite.camera", .main_thread = true,
     .runs = RunWhen::Always},
    {.name = ".hud", .phase = Phase::PostSim, .fn = hud, .reads = ".world.Run, .world.Stage", .before = "ui.draw", .main_thread = true,
     .runs = RunWhen::Always},
    {.name = ".stats_mark", .phase = Phase::PostSim, .fn = stats_mark, .before = "sprite.extract", .main_thread = true, .runs = RunWhen::Always},
    {.name = ".stats", .phase = Phase::PostSim, .fn = stats, .reads = "sprite.deltas", .writes = ".stats", .after = ".hud", .main_thread = true,
     .runs = RunWhen::Always},
};

// The handlers that write the run go in a chain, in the order a tick's hits settle: a
// bullet on the hitbox first (it cancels its own graze), then the grazes, the kills, the
// death, the stage's end.
constinit const HandlerDesc k_handlers[] = {
    {.name = ".on_overlap", KV_ON(OverlapBegin, on_overlap)},
    {.name = ".on_shot_hit", KV_ON(ShotHit, on_shot_hit), .reads = ".air.Transform2, .ground.Transform2", .writes = ".air.Health, .ground.Health, .shot.Shot"},
    {.name = ".on_bullet_hit", KV_ON(BulletHit, on_bullet_hit), .reads = ".world.Dials",
     .writes = ".bullet.Bullet, .active.Bullet, .world.Run, .ship.Ship"},
    {.name = ".on_rammed", KV_ON(Rammed, on_rammed), .reads = ".world.Dials", .writes = ".ship.Ship", .after = ".on_bullet_hit"},
    {.name = ".on_graze", KV_ON(Graze, on_graze), .reads = ".bullet.Transform2, .active.Transform2", .writes = ".bullet.Bullet, .active.Bullet, .world.Run",
     .after = ".on_bullet_hit"},
    {.name = ".on_shell_hit", KV_ON(ShellHit, on_shell_hit), .reads = ".bullet.Transform2, .active.Transform2"},
    {.name = ".on_kill", KV_ON(Kill, on_kill), .reads = ".world.Dials", .writes = ".world.Run", .after = ".on_graze"},
    {.name = ".on_ship_died", KV_ON(ShipDied, on_ship_died),
     .writes = ".world.Run, .world.EggState, .world.Stage, .ship", .after = ".on_kill, .on_rammed"},
    {.name = ".on_member", KV_ON(SpawnMember, on_member), .reads = ".world.Run, .world.Dials"},
    {.name = ".on_stage_end", KV_ON(StageEnd, on_stage_end), .writes = ".world.Run", .after = ".on_ship_died"},
    {.name = ".on_exit", KV_ON(ExitRequested, on_exit)},
};

// ---- startup -------------------------------------------------------------------------------------

constinit const char* const k_texture_paths[TEX_COUNT] = {
    "example_arcfire/kenney/ships.png", "example_arcfire/kenney/tiles.png", "example_arcfire/space/stars.png",
    "example_arcfire/space/nebula.png", "example_arcfire/space/cloud.png",  "example_arcfire/made/bullets.png",
    "example_arcfire/made/dot.png",     "example_arcfire/made/ring.png",    "example_arcfire/made/soft.png",
    "example_arcfire/made/spark.png",   "example_arcfire/made/egg.png",
};

Handle material_of(Engine& e, Handle texture, BlendMode blend) {
    const MaterialAsset* m = asset_material(e, asset_material_for_texture(e, texture, blend, TextureFilter::Nearest, asset_scope_global(e)));
    return m ? m->material : Handle{};
}

bool arcfire_startup(Engine& e, ModuleInstance& self) {
    Game* g = arena_push<Game>(self.arena);
    if (!g) return false;
    self.set_state(g);
    if (!engine_publish(e, self, ".stats", &g->stats)) return false;
    if (!input_actions(e, k_actions)) return false;
    const InputActions* a = input_actions_get(e);
    g->act_move = a->find("move"); g->act_shot = a->find("shot"); g->act_focus = a->find("focus");
    g->act_bomb = a->find("bomb"); g->act_pause = a->find("pause"); g->act_quit = a->find("quit");
    g->demo = frameloop_flag(e, "--demo");
    g->stress = frameloop_flag(e, "--stress");
    g->autoplay = frameloop_flag(e, "--autoplay");
    g->fx = Rng::seeded(0xa2cf12e, 3);
    if (const char* bench = frameloop_option(e, "--bench")) snprintf(g->bench, sizeof g->bench, "%s", bench);
    if (!patterns_load(e, *g)) return false;
    for (u32 i = 0; i < TEX_COUNT; ++i) {
        g->textures[i] = asset_load_texture(e, k_texture_paths[i]);
        g->alpha[i] = material_of(e, g->textures[i], BlendMode::Alpha);
        g->additive[i] = material_of(e, g->textures[i], BlendMode::Additive);
    }
    // The stage comes from its file; the sim holds until it has (stage.cc, boot), so how
    // long the disk takes never reaches the state.
    g->stage_blob = asset_load_blob(e, "example_arcfire/stages/shoreline.stage.json");
    engine_hold(e, HoldLoading, true);
    hud_open(e, *g);
    KV_INFO("arcfire", "Arrows or WASD (or a pad's stick) fly, Z or Space (South) fires, Shift (West) focuses, Escape (Start) pauses, Q quits.");
    return true;
}

void arcfire_shutdown(Engine&, ModuleInstance& self) {
    if (Game* g = (Game*)self.state) {
        stats_summary(*g);
        bml_free(g->lib);
    }
}

} // namespace

// ---- sprites -------------------------------------------------------------------------------------

Sprite sprite_of(const Game& g, Tex tex, rect2 uv, vec2 size, i32 layer, color tint, bool additive) {
    Sprite s;
    s.material = additive ? g.additive[tex] : g.alpha[tex];
    s.uv = uv;
    s.size = size;
    s.layer = layer;
    s.tint = tint;
    return s;
}

rect2 ship_uv(u32 col, u32 row) { return rect2{(f32)col * 0.25f, (f32)row * 0.5f, 0.25f, 0.5f}; }
rect2 tile_uv(u32 col, u32 row) { return rect2{(f32)col * (16.0f / 192.0f), (f32)row * (16.0f / 160.0f), 16.0f / 192.0f, 16.0f / 160.0f}; }

Sprite enemy_sprite(const Game& g, Look look) {
    switch (look) {
    case Look::Red:    return sprite_of(g, TEX_SHIPS, ship_uv(1, 1), vec2{64, 64}, Z_AIR);
    case Look::Green:  return sprite_of(g, TEX_SHIPS, ship_uv(2, 1), vec2{64, 64}, Z_AIR);
    case Look::Turret: return sprite_of(g, TEX_TILES, tile_uv(4, 2), vec2{32, 32}, Z_GROUND);
    default:           return sprite_of(g, TEX_SHIPS, ship_uv(1, 0), vec2{64, 64}, Z_AIR);
    }
}

} // namespace kv::arcfire

namespace kv {
KV_MODULE_DEFINE(example_arcfire_module) = {
    .name       = "example_arcfire",
    .events     = arcfire::k_events,
    .components = arcfire::k_components,
    .kinds      = arcfire::k_kinds,
    .resources  = arcfire::k_resources,
    .systems    = arcfire::k_systems,
    .handlers   = arcfire::k_handlers,
    .startup    = arcfire::arcfire_startup,
    .shutdown   = arcfire::arcfire_shutdown,
    .memory     = {.reserve = 1024 * 1024},
};
} // namespace kv
