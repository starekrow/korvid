// example_arcfire: the module record. What the game declares to the engine (components,
// kinds, events, the systems and handlers of every file), the startup that loads its art
// and its stage, and the sprite helpers every file draws with. The game itself is in the
// files the tables below name.
#include "arcfire.h"
#include "korvid/anim/player.h"
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
#include <stdlib.h>
#include <string.h>

namespace kv::arcfire {

constinit u32 g_world_kind = 0, g_plane_kind = 0, g_ship_kind = 0, g_ring_kind = 0, g_mark_kind = 0, g_shot_kind = 0, g_air_kind = 0,
              g_ground_kind = 0, g_bullet_kind = 0, g_active_kind = 0, g_particle_kind = 0, g_egg_kind = 0, g_shell_kind = 0,
              g_item_kind = 0, g_formation_kind = 0, g_shadow_kind = 0, g_boss_kind = 0, g_part_kind = 0, g_boom_kind = 0, g_beam_kind = 0, g_option_kind = 0;
constinit u32 g_part_down = 0;

// The systems and handlers, each in the file of its part.
void demo(TickContext&);         // pilot.cc
void bot(TickContext&);
void scoring(TickContext&);      // scoring.cc
void on_run_over(TickContext&, Span<const RunOver>);
void boot(TickContext&);         // stage.cc
void scroll(TickContext&);
void stage(TickContext&);
void on_member(TickContext&, Span<const SpawnMember>);
void on_stage_end(TickContext&, Span<const StageEnd>);
void planes(TickContext&);       // planes.cc
void control(TickContext&);      // ship.cc
void shots(TickContext&);        // weapons.cc
void beams(TickContext&);
void options(TickContext&);      // options.cc
void on_beam_hit(TickContext&, Span<const BeamHit>); // hits.cc
void enemies(TickContext&);      // enemies.cc
void bosses(TickContext&);       // boss.cc
void on_part_down(TickContext&, Span<const StateEdge>);
void patterns(TickContext&);     // patterns.cc
void actives(TickContext&);
void bullets(TickContext&);      // bullets.cc
void particles(TickContext&);    // fx.cc
void items(TickContext&);        // items.cc
void on_pickup(TickContext&, Span<const Pickup>);
void bomb(TickContext&);         // bomb.cc
void on_bomb(TickContext&, Span<const Bombed>);
void on_spawned(TickContext&, Span<const Spawned>); // fx.cc: shadows
void on_boom_done(TickContext&, Span<const AnimFinished>);
void egg(TickContext&);          // egg.cc
void on_ship_died(TickContext&, Span<const ShipDied>);
void on_overlap(TickContext&, Span<const OverlapBegin>); // hits.cc
void on_shot_hit(TickContext&, Span<const ShotHit>);
void on_bullet_hit(TickContext&, Span<const BulletHit>);
void on_rammed(TickContext&, Span<const Rammed>);
void on_graze(TickContext&, Span<const Graze>);
void on_shell_hit(TickContext&, Span<const ShellHit>);
void on_kill(TickContext&, Span<const Kill>);
void records(TickContext&);      // records.cc
void screens(TickContext&);      // screens.cc
void pause(TickContext&);        // flow.cc
void on_ui(TickContext&, Span<const UiEvent>);
void on_covered(TickContext&, Span<const UiCovered>);
void camera(TickContext&);
void hud(TickContext&);
void stats_mark(TickContext&);   // stats.cc
void stats(TickContext&);
void on_exit(TickContext&, Span<const ExitRequested>);

namespace {

// ---- schemas -------------------------------------------------------------------------------------

constinit const FieldDesc k_velocity_fields[] = {KV_FIELDS(Velocity, v)};
constinit const FieldDesc k_plane_fields[]    = {KV_FIELDS(Plane, rate, tile, row, rows)};
constinit const FieldDesc k_ship_fields[]     = {KV_FIELDS(Ship, cooldown, live, focus, shots, guard, died, weapon, power, firing, pad)};
constinit const FieldDesc k_formation_fields[] = {KV_FIELDS(Formation, count, killed, gone, drop)};
constinit const FieldDesc k_item_fields[]     = {KV_FIELDS(Item, kind, value, life, pad)};
constinit const FieldDesc k_shot_fields[]     = {KV_FIELDS(Shot, damage, spent, kind, turn)};
constinit const FieldDesc k_beam_fields[]     = {KV_FIELDS(Beam, timer, length, scroll, pad)};
constinit const FieldDesc k_option_fields[]   = {KV_FIELDS(Option, index, pad)};
constinit const FieldDesc k_health_fields[]   = {KV_FIELDS(Health, hp, flash, score, look)};
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
constinit const FieldDesc k_emitter_fields[]  = {KV_FIELDS(Emitter, pattern, at, state, timer)};
constinit const FieldDesc k_runner_fields[]   = {KV_FIELDS(BmlRunner, rng, wait, frame16, frame8, track8, params, last, term, delta, dir, speed, mx, my, flags)};
constinit const FieldDesc k_ground_fields[]   = {KV_FIELDS(Ground, rate, bench, kind, launch, timer, vx, home, range, drop, pad)};
constinit const FieldDesc k_bullet_fields[]   = {KV_FIELDS(Bullet, life, grazed, owner)};
constinit const FieldDesc k_particle_fields[] = {KV_FIELDS(Particle, life, max_life, spin, drag, alpha, fall)};
constinit const FieldDesc k_eggbody_fields[]  = {KV_FIELDS(EggBody, vy, drift)};
constinit const FieldDesc k_dials_fields[]    = {KV_FIELDS(Dials, position, rank, bullet_speed, formation_count, formation_spacing, particles,
                                                           plane_density, enemy_health, bot, invulnerable, loops, scroll_rows, weapon, power, options, chain_window)};
constinit const FieldDesc k_stage_fields[]    = {KV_FIELDS(Stage, distance, speed, ramp_from, ramp_to, ramp_at, ramp_over, cursor, loops, ended, ground_cursor, base, held, waiting, round_up)};
constinit const FieldDesc k_run_fields[]      = {KV_FIELDS(Run, score, stage_score, next_extend, eggs, restarts, epoch, kills, grazes, deaths, bombs, chips, chain, chain_left, chain_best,
                                                           bombs_used)};
constinit const FieldDesc k_eggstate_fields[] = {KV_FIELDS(EggState, step, t, at, shell, restart)};
constinit const FieldDesc k_chance_fields[]   = {KV_FIELDS(Chance, state, inc)};
constinit const FieldDesc k_boss_fields[]     = {KV_FIELDS(Boss, def, phase, state, clock, s, down, hold)};
constinit const FieldDesc k_part_fields[]     = {KV_FIELDS(Part, boss, index, cycle, timer, pad)};

constinit const ComponentDesc k_components[] = {
    KV_COMPONENT(Velocity, k_velocity_fields), KV_COMPONENT(Plane, k_plane_fields),   KV_COMPONENT(Ship, k_ship_fields),
    // Health keeps its previous copy for the parts' trigger (a part's death is an edge).
    KV_COMPONENT(Shot, k_shot_fields),         KV_COMPONENT(Health, k_health_fields, CompPrev), KV_COMPONENT(Mover, k_mover_fields),
    KV_COMPONENT(Emitter, k_emitter_fields),   KV_COMPONENT(Ground, k_ground_fields), KV_COMPONENT(Bullet, k_bullet_fields),
    KV_COMPONENT(Particle, k_particle_fields), KV_COMPONENT(EggBody, k_eggbody_fields), KV_COMPONENT(Dials, k_dials_fields),
    KV_COMPONENT(Stage, k_stage_fields),       KV_COMPONENT(Run, k_run_fields),       KV_COMPONENT(EggState, k_eggstate_fields),
    KV_COMPONENT(Chance, k_chance_fields),       KV_COMPONENT(Runner, k_runner_fields), KV_COMPONENT(Formation, k_formation_fields),
    KV_COMPONENT(Item, k_item_fields),         KV_COMPONENT(Boss, k_boss_fields),     KV_COMPONENT(Part, k_part_fields),
    KV_COMPONENT(Beam, k_beam_fields),         KV_COMPONENT(Option, k_option_fields),
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
    // Chips and pickups; a formation's count of its members.
    {.name = ".item",     .components = "Transform2, Sprite, Velocity, Collider2, Item", .capacity = 4096, .id = &g_item_kind},
    {.name = ".formation", .components = "Formation", .capacity = 64, .id = &g_formation_kind},
    // Shadows on the ground under what flies: cosmetic children.
    {.name = ".shadow",   .components = "Transform2, Local2, Parent, Sprite", .capacity = 512, .id = &g_shadow_kind, .cosmetic = true},
    // A boss's hull, and its parts: children of the hull or of each other (boss.cc).
    {.name = ".boss",     .components = "Transform2, Sprite, Collider2, Boss", .capacity = 2, .id = &g_boss_kind},
    {.name = ".part",     .components = "Transform2, Local2, Parent, Sprite, Collider2, Health, Emitter, Runner, Part", .capacity = 256, .id = &g_part_kind},
    // Explosions: a flipbook each, gone when it has played (fx.cc).
    // The beams: one on the ship (and one on each option), cut each tick at what it meets.
    // Options: children of the ship, bodies that eat bullets (options.cc).
    {.name = ".option",   .components = "Transform2, Local2, Parent, Sprite, Collider2, Option", .capacity = kMaxOptions, .id = &g_option_kind},
    {.name = ".beam",     .components = "Transform2, Local2, Parent, Sprite, Beam", .capacity = 4, .id = &g_beam_kind},
    {.name = ".boom",     .components = "Transform2, Sprite, AnimPlayer", .capacity = 512, .id = &g_boom_kind, .cosmetic = true},
};

// A part's death is its health crossing below one: delivered as a StateEdge the next tick.
constinit const TriggerDesc k_triggers[] = {
    {.name = "example_arcfire.part_down", .field = "example_arcfire.part.Health.hp", .edge = TriggerEdge::CrossedBelow, .value = 1.0, .id = &g_part_down},
};

// ---- events --------------------------------------------------------------------------------------

constinit const FieldDesc k_shot_hit_fields[]  = {KV_FIELDS(ShotHit, shot, target)};
constinit const FieldDesc k_beam_hit_fields[]  = {KV_FIELDS(BeamHit, target, damage, pad, at)};
constinit const FieldDesc k_bullet_hit_fields[] = {KV_FIELDS(BulletHit, bullet)};
constinit const FieldDesc k_rammed_fields[]    = {KV_FIELDS(Rammed, enemy)};
constinit const FieldDesc k_graze_fields[]     = {KV_FIELDS(Graze, bullet)};
constinit const FieldDesc k_shell_hit_fields[] = {KV_FIELDS(ShellHit, bullet)};
constinit const FieldDesc k_kill_fields[]      = {KV_FIELDS(Kill, at, score, flat, formation)};
constinit const FieldDesc k_bombed_fields[]    = {KV_FIELDS(Bombed, at, cancelled, pad)};
constinit const FieldDesc k_pickup_fields[]    = {KV_FIELDS(Pickup, item)};
constinit const FieldDesc k_died_fields[]      = {KV_FIELDS(ShipDied, at, cause, pad)};
constinit const FieldDesc k_member_fields[]    = {KV_FIELDS(SpawnMember, formation, member, epoch, pad, group, offset)};
constinit const FieldDesc k_end_fields[]       = {KV_FIELDS(StageEnd, loops, pad)};
constinit const FieldDesc k_over_fields[]      = {KV_FIELDS(RunOver, loops, pad)};
constinit const EventTypeDesc k_events[] = {
    KV_EVENT_EX(BeamHit, k_beam_hit_fields, EvStaleOk), // a target the beam met may be gone before it lands
    KV_EVENT_EX(ShotHit, k_shot_hit_fields, 0),   KV_EVENT_EX(BulletHit, k_bullet_hit_fields, 0), KV_EVENT_EX(Rammed, k_rammed_fields, 0),
    KV_EVENT_EX(Graze, k_graze_fields, 0),        KV_EVENT_EX(ShellHit, k_shell_hit_fields, 0),   KV_EVENT_EX(Kill, k_kill_fields, 0),
    KV_EVENT_EX(Bombed, k_bombed_fields, 0),      KV_EVENT_EX(Pickup, k_pickup_fields, 0),
    KV_EVENT_EX(ShipDied, k_died_fields, 0),      KV_EVENT_EX(SpawnMember, k_member_fields, EvStaleOk),   KV_EVENT_EX(StageEnd, k_end_fields, 0), KV_EVENT_EX(RunOver, k_over_fields, 0), // SpawnMember: a restart's formations go, their members' events with them
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
    {.name = ".boot", .phase = Phase::PreSim, .fn = boot, .writes = ".world, .plane, .ship, .ring, .mark, .option, .beam, .particle, .shadow, .boom, tilemap.tile, audio.commands", .after = "tilemap.pump", .main_thread = true, .runs = RunWhen::Always},
    // The chain's clock, the extends, a new round's dials: before the sim moves anything.
    {.name = ".scoring", .phase = Phase::PreSim, .fn = scoring, .writes = ".world.Run, .world.Stage, .world.Dials, audio.commands", .after = ".boot",
     .main_thread = true},
    {.name = ".records", .phase = Phase::PreSim, .fn = records, .main_thread = true, .runs = RunWhen::Always},
    {.name = ".screens", .phase = Phase::PreSim, .fn = screens, .reads = "input.actions", .after = ".pause", .main_thread = true, .runs = RunWhen::Always},
    {.name = ".pause", .phase = Phase::PreSim, .fn = pause, .reads = "input.actions, input.state", .after = ".boot", .main_thread = true, .runs = RunWhen::Always},
    {.name = ".bot", .phase = Phase::Sim, .fn = bot, .reads = "^.bullet.Transform2, ^.bullet.Velocity, ^.active.Transform2, ^.active.Velocity, ^.ship.Transform2, ^.air.Transform2, ^.ground.Transform2, ^.part.Transform2, ^.part.Part, ^.part.Health, ^.boss, ^.item.Transform2, ^.item.Item, .world.Dials",
     .main_thread = true},
    {.name = ".scroll", .phase = Phase::Sim, .fn = scroll, .writes = ".world.Stage"},
    {.name = ".stage", .phase = Phase::Sim, .fn = stage, .reads = ".world.Dials, .world.Run", .writes = ".world.Stage, .formation, .boss, .part, audio.commands", .after = ".scroll"},
    {.name = ".planes", .phase = Phase::Sim, .fn = planes, .reads = ".world.Stage, .world.Dials, .plane.Plane, .plane.Sprite",
     .writes = "sprite.layers, .plane", .runs = RunWhen::Always},
    {.name = ".control", .phase = Phase::Sim, .fn = control, .reads = "^.option.Transform2, input.actions, .world.Dials",
     .writes = ".ship.Transform2, .ship.Ship, .ship.Sprite, .shot, .ring.Sprite, .mark.Sprite, audio.commands"},
    // Storm's bolts steer by the enemies as the tick began (^), before anything moves them.
    {.name = ".shots", .phase = Phase::Sim, .fn = shots,
     .reads = "^.air.Transform2, ^.air.Health, ^.ground.Transform2, ^.ground.Health, ^.part.Transform2, ^.part.Part, ^.part.Health, ^.boss.Boss",
     .writes = ".shot", .after = ".control", .state = StateUse::Read},
    // The beam is cut against everything where it is this tick: after every mover and the
    // collision module's compose.
    // Each option's place on the ship, before the collision module composes it.
    {.name = ".options", .phase = Phase::Sim, .fn = options, .reads = ".ship.Ship, .option.Option", .writes = ".option.Local2", .after = ".control",
     .state = StateUse::Read},
    {.name = ".beams", .phase = Phase::Sim, .fn = beams, .reads = "*[Collider2].Transform2, *[Collider2].Collider2, .ship.Ship, .ship.Transform2, .beam.Parent, .option.Parent",
     .writes = ".beam.Local2, .beam.Sprite, .beam.Beam", .state = StateUse::None},
    {.name = ".enemies", .phase = Phase::Sim, .fn = enemies, .reads = ".ship.Transform2, .world.Stage, .world.Run, .world.Dials", .writes = ".air, .ground, .formation", .state = StateUse::Read},
    {.name = ".bosses", .phase = Phase::Sim, .fn = bosses, .reads = ".world.Run, .world.Dials, .world.EggState", .writes = ".boss, .part, .formation, audio.commands",
     .after = ".stage, .enemies", .before = ".patterns", .main_thread = true},
    // The active bullets steer before they fly; the enemies' patterns run after
    // collision.compose, which places a boss's parts this tick and comes after every
    // writer of a transform (the active bullets' among them).
    {.name = ".actives", .phase = Phase::Sim, .fn = actives, .reads = ".ship.Transform2, .world.Dials", .writes = ".active", .after = ".enemies"},
    {.name = ".patterns", .phase = Phase::Sim, .fn = patterns, .reads = ".ship.Transform2, .world.Dials, .air.Transform2, .ground.Transform2, .part.Transform2",
     .writes = ".air.Emitter, .air.Runner, .ground.Emitter, .ground.Runner, .part.Emitter, .part.Runner", .after = ".enemies, .actives"},
    {.name = ".bullets", .phase = Phase::Sim, .fn = bullets, .reads = ".bullet.Velocity, .active.Velocity",
     .writes = ".bullet.Transform2, .bullet.Bullet, .active.Transform2, .active.Bullet", .after = ".actives", .state = StateUse::None},
    {.name = ".egg", .phase = Phase::Sim, .fn = egg, .reads = "^.bullet, ^.active", .writes = ".world.EggState, .world.Chance, .ship, .ring.Collider2, .egg, .shell, audio.commands",
     .after = ".control", .main_thread = true},
    {.name = ".bomb", .phase = Phase::Sim, .fn = bomb, .reads = "input.actions, ^.bullet, ^.active, .world.EggState, .ship.Transform2",
     .writes = ".world.Run, .ship.Ship, .ship.Collider2, audio.commands", .after = ".egg", .main_thread = true},
    {.name = ".items", .phase = Phase::Sim, .fn = items, .reads = ".ship.Transform2, .ship.Ship", .writes = ".item"},
    {.name = ".particles", .phase = Phase::Sim, .fn = particles, .writes = ".particle", .state = StateUse::None},
    {.name = ".camera", .phase = Phase::PostSim, .fn = camera, .reads = "platform_hid.window", .writes = "sprite.camera", .main_thread = true,
     .runs = RunWhen::Always},
    {.name = ".hud", .phase = Phase::PostSim, .fn = hud, .reads = ".world.Run, .world.Stage, .world.Dials, .boss.Boss, .part.Health, .part.Part, .ship.Ship, .option.Option", .before = "ui.draw", .main_thread = true,
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
    {.name = ".on_shot_hit", KV_ON(ShotHit, on_shot_hit), .reads = ".ground.Ground, .air.Transform2, .ground.Transform2, .air.Mover, .shot.Transform2, .part.Transform2, .part.Part, .boss.Boss",
     .writes = ".air.Health, .ground.Health, .shot.Shot, .part.Health, audio.commands"},
    {.name = ".on_bomb", KV_ON(Bombed, on_bomb), .reads = ".ground.Ground, .air.Transform2, .ground.Transform2, .air.Mover, .part.Transform2, .part.Part, .boss.Boss",
     .writes = ".air.Health, .ground.Health, .part.Health, audio.commands", .after = ".on_shot_hit"},
    {.name = ".on_part_down", KV_ON(StateEdge, on_part_down), .reads = ".world.Dials, .part.Part, .part.Transform2, .bullet, .active", .writes = ".boss.Boss, audio.commands"},
    {.name = ".on_boom_done", KV_ON(AnimFinished, on_boom_done)},
    {.name = ".on_bullet_hit", KV_ON(BulletHit, on_bullet_hit), .reads = ".world.Dials",
     .writes = ".bullet.Bullet, .active.Bullet, .world.Run, .ship.Ship"},
    {.name = ".on_rammed", KV_ON(Rammed, on_rammed), .reads = ".world.Dials", .writes = ".ship.Ship", .after = ".on_bullet_hit"},
    {.name = ".on_graze", KV_ON(Graze, on_graze), .reads = ".world.Dials, .bullet.Transform2, .active.Transform2", .writes = ".bullet.Bullet, .active.Bullet, .world.Run, audio.commands",
     .after = ".on_bullet_hit"},
    {.name = ".on_shell_hit", KV_ON(ShellHit, on_shell_hit), .reads = ".bullet.Transform2, .active.Transform2"},
    {.name = ".on_kill", KV_ON(Kill, on_kill), .reads = ".world.Dials", .writes = ".world.Run, .formation, audio.commands", .after = ".on_graze"},
    {.name = ".on_beam_hit", KV_ON(BeamHit, on_beam_hit), .reads = ".ground.Ground, .air.Transform2, .ground.Transform2, .air.Mover, .part.Transform2, .part.Part, .boss.Boss",
     .writes = ".air.Health, .ground.Health, .part.Health, audio.commands", .after = ".on_bomb"},
    {.name = ".on_pickup", KV_ON(Pickup, on_pickup), .reads = ".item.Item, .item.Transform2", .writes = ".world.Run, .ship.Ship, .option, .beam, audio.commands",
     .after = ".on_kill, .on_rammed"},
    {.name = ".on_ship_died", KV_ON(ShipDied, on_ship_died),
     .reads = ".world.Dials", .writes = ".world.Run, .world.EggState, .world.Stage, .ship, audio.commands", .after = ".on_pickup, .on_rammed"},
    {.name = ".on_member", KV_ON(SpawnMember, on_member), .reads = ".world.Run, .world.Dials"},
    {.name = ".on_stage_end", KV_ON(StageEnd, on_stage_end), .reads = ".world.Dials", .writes = ".world.Run", .after = ".on_ship_died"},
    {.name = ".on_run_over", KV_ON(RunOver, on_run_over)},
    {.name = ".on_exit", KV_ON(ExitRequested, on_exit)},
    {.name = ".on_ui", KV_ON(UiEvent, on_ui)},
    {.name = ".on_covered", KV_ON(UiCovered, on_covered)},
    {.name = ".on_spawned", KV_ON(Spawned, on_spawned), .reads = ".air.Transform2, .air.Sprite"},
};

// ---- startup -------------------------------------------------------------------------------------

constinit const char* const k_texture_paths[TEX_COUNT] = {
    "example_arcfire/kenney/ships.png", "example_arcfire/kenney/tiles.png", "example_arcfire/space/stars.png",
    "example_arcfire/space/nebula.png", "example_arcfire/space/cloud.png",  "example_arcfire/made/bullets.png",
    "example_arcfire/made/dot.png",     "example_arcfire/made/ring.png",    "example_arcfire/made/soft.png",
    "example_arcfire/made/spark.png",   "example_arcfire/made/egg.png",     "example_arcfire/made/clouds_low.png",
    "example_arcfire/made/clouds_high.png", "example_arcfire/made/beam.png",    "example_arcfire/made/items.png",
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
    // The run's shape: run plainly, the title; a flag that names a run goes straight into
    // it: arcade unless a stage is named, and the demo, a stress run and the bench are runs
    // of their own. --difficulty is where arcade or the stage starts.
    bool stage_named = false;
    if (const char* name = frameloop_option(e, "--stage")) {
        snprintf(g->stage_name, sizeof g->stage_name, "%s", name);
        stage_named = true;
    }
    g->front_door = !stage_named && !g->demo && !g->stress && !g->autoplay && !g->bench[0] && !frameloop_flag(e, "--arcade");
    g->again = frameloop_flag(e, "--again");
    if (const char* s = frameloop_option(e, "--attract")) g->attract_after = (u32)(atof(s) * 60.0);
    if (const char* diff = frameloop_option(e, "--difficulty")) {
        g->start_named = true;
        if (!strcmp(diff, "easy")) g->start = Position::Easy;
        else if (!strcmp(diff, "hard")) g->start = Position::Hard;
        else if (strcmp(diff, "normal")) KV_WARN("arcfire", "--difficulty %s: easy, normal or hard; Normal it is", diff);
    }
    g->plan = RunPlan{!stage_named && !g->demo && !g->stress && !g->bench[0], stage_named, g->start};
    if (!patterns_load(e, *g)) return false;
    for (u32 i = 0; i < TEX_COUNT; ++i) {
        g->textures[i] = asset_load_texture(e, k_texture_paths[i]);
        g->alpha[i] = material_of(e, g->textures[i], BlendMode::Alpha);
        g->additive[i] = material_of(e, g->textures[i], BlendMode::Additive);
    }
    bosses_load(e, *g);
    sounds_load(e, *g);
    g->boom_clip = fx_boom_clip(e);
    // The stage comes from its file; the sim holds until it has (stage.cc, boot), so how
    // long the disk takes never reaches the state.
    char stage_path[96];
    snprintf(stage_path, sizeof stage_path, "example_arcfire/stages/%s.stage.json", g->stage_name);
    g->stage_blob = asset_load_blob(e, stage_path);
    KV_INFO("arcfire", "%s: %s at %s", g->front_door ? "the title" : g->plan.arcade ? "arcade" : g->plan.stage_select ? "stage select" : "run",
            g->stage_name, position_name((u32)g->start));
    records_load(e, *g);
    engine_hold(e, HoldLoading, true);
    hud_open(e, *g);
    KV_INFO("arcfire", "Arrows or WASD (or a pad's stick) fly, Z or Space (South) fires, Shift (West) focuses, Escape (Start) pauses, Q quits.");
    return true;
}

void arcfire_shutdown(Engine& e, ModuleInstance& self) {
    if (Game* g = (Game*)self.state) {
        stats_summary(e, *g);
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
    case Look::Tank:   return sprite_of(g, TEX_TILES, tile_uv(5, 2), vec2{32, 32}, Z_GROUND);
    case Look::Bunker: return sprite_of(g, TEX_TILES, tile_uv(4, 1), vec2{40, 40}, Z_GROUND);
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
    .triggers   = arcfire::k_triggers,
    .startup    = arcfire::arcfire_startup,
    .shutdown   = arcfire::arcfire_shutdown,
    .memory     = {.reserve = 1024 * 1024},
};
} // namespace kv
