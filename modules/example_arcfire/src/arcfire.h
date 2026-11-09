// example_arcfire: what the game's files share. The components and events are declared
// here and registered in module.cc; each system and handler lives in the file named for
// its part of the game (planes, ship, enemies, bullets, hits, egg, stage, hud, pilot,
// stats), and this header is how they find each other.
//
// What is sim state: every kind but the cosmetic ones (planes, particles), and the one
// row of `.world`, which holds the dials, the stage's clock, the run (score, eggs,
// restarts), the egg sequence and the sim's random stream, so the hash covers them all.
// The module's own state (Game, below) is presentation and bookkeeping: asset handles, the
// stage as read from its file (never changed after), the UI's screens, the counters.
#pragma once
#include "korvid/example_arcfire/module.h"
#include "field.h"
#include "path.h"
#include "korvid/collision/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/random.h"
#include "korvid/math/fields.h"
#include "korvid/scene/parent.h"
#include "korvid/sprite/sprite.h"
#include "korvid/sprite/transform2.h"

namespace kv::arcfire {

// ---- collision layers ----------------------------------------------------------------------
// Everything that hits is in one plane; the masks say who meets whom.
constexpr u32 L_SHIP   = 1;  // the hitbox: the ship's own row
constexpr u32 L_SHOT   = 2;  // the player's shots
constexpr u32 L_AIR    = 4;
constexpr u32 L_GROUND = 8;
constexpr u32 L_BULLET = 16; // every enemy bullet
constexpr u32 L_GRAZE  = 32; // the ring around the hitbox
constexpr u32 L_SHELL  = 64; // the egg's shell around a new ship

// ---- sprite layers ----------------------------------------------------------------------
// Back to front, the master PRD's planes. The HUD is the UI's, above all of them.
constexpr i32 Z_DEEP = -30, Z_HIGH = -20, Z_GROUND = -10, Z_CLOUD = 10, Z_AIR = 20, Z_SHIP = 30, Z_SHOT = 25,
              Z_BULLET = 40, Z_FX = 50, Z_MARK = 60;

// ---- components -----------------------------------------------------------------------------

struct Velocity { vec2 v; };                      // world units a tick
struct Plane    { f32 rate; u32 row, col, rows; }; // a parallax tile: its plane's rate, its place in the grid
// The player's ship. `live` is 0 while the egg plays (control does nothing, nothing hits).
struct Ship     { u32 cooldown; u32 live; u32 focus; u32 shots; };
struct Shot     { i32 damage; u32 spent; };       // spent: it has hit something this tick
// What can be shot. `flash` counts the ticks of the white flash left; `score` is paid at the kill.
struct Health   { i32 hp; u32 flash; i32 score; u32 look; };
// A formation member's path, measured, and how far along it is.
struct Mover    { Path path; f32 speed; f32 s; };
// Slice 1's emitter is one rule: fire `count` aimed bullets when the mover passes `at` (a
// fraction of its path), or for a ground row every `cadence` ticks while the ship is above.
struct Emitter  { f32 at; u32 count; u32 cadence; u32 timer; u32 fired; f32 speed; };
struct Ground   { f32 rate; u32 kind; };          // rides the scroll at `rate` times its speed
struct Bullet   { u32 life; u32 grazed; Handle owner; };
// A cosmetic piece of debris or a spark: it drifts, spins, slows and fades over its life.
struct Particle { u32 life, max_life; f32 spin, drag, alpha; u32 pad; };
struct EggBody  { f32 vy; f32 drift; };           // the egg as it falls

// The world row's parts.
enum class Position : u32 { Normal, Stress, Demo };
struct Dials {
    u32 position;          // Position
    f32 bullet_density;    // multiplies how many bullets a rule fires ($rank, later)
    f32 bullet_speed;
    f32 formation_count;   // multiplies a formation's members
    f32 formation_spacing; // multiplies the ticks between them
    u32 particles;         // sparks per event
    u32 plane_density;     // plane tiles per side of a 1024-unit tile: 1 is a 1024 tile, 8 a 128 one
    f32 enemy_health;
    u32 bot;               // the bot flies the ship
    u32 invulnerable;
    u32 loops;             // the stage loops at its end instead of ending
};
struct Stage {
    f32 distance;          // the stage's clock: units scrolled since the stage (or this loop) began
    f32 speed;             // units a tick, now
    f32 ramp_from, ramp_to, ramp_at, ramp_over; // a speed change in progress, by distance
    u32 cursor;            // the next mark
    u32 loops;             // times the stage has looped
    u32 ended;             // the end mark passed and the stage does not loop
    u32 pad;
};
struct Run {
    i64 score;
    i64 stage_score;       // the score when this stage (or loop) began: a restart goes back to it
    i32 eggs;
    u32 restarts;
    u32 epoch;             // +1 at every restart: scheduled spawns of an earlier epoch are dropped
    u32 kills, grazes, deaths;
};
// The egg sequence (egg.cc): which step, its sim ticks, where the ship died.
enum class EggStep : u32 { None, Breaking, Falling, Blooming, Shelled };
struct EggState {
    u32    step;           // EggStep
    u32    t;              // ticks since the death
    point2 at;             // the death point
    u32    shell;          // ticks of shell left
    u32    restart;        // ticks until the restart cover lifts; 0: no restart under way
};
struct Chance { u64 state, inc; }; // the sim's random stream, a PCG32 as Rng keeps it

// ---- events ---------------------------------------------------------------------------------

struct ShotHit    { Handle shot, target; };
struct BulletHit  { Handle bullet; };          // a bullet on the hitbox
struct Rammed     { Handle enemy; };           // an air enemy on the hitbox
struct Graze      { Handle bullet; };
struct ShellHit   { Handle bullet; };
struct Kill       { point2 at; i32 score; u32 pad; }; // no handle: the enemy is gone by the time it is read, and the bus drops stale ones
struct ShipDied   { point2 at; u32 cause; u32 pad; }; // cause 0 a bullet, 1 a ram; `at` is filled by the handler
struct SpawnMember { u32 formation; u32 member; u32 epoch; u32 pad; }; // scheduled by a formation mark
struct StageEnd   { u32 loops; u32 pad; };

// ---- kinds, by id (filled at init) --------------------------------------------------------------
extern u32 g_world_kind, g_plane_kind, g_ship_kind, g_ring_kind, g_mark_kind, g_shot_kind, g_air_kind, g_ground_kind,
    g_bullet_kind, g_particle_kind, g_egg_kind, g_shell_kind;

// ---- the stage as its file says -------------------------------------------------------------
constexpr u32 kMaxPaths = 32, kMaxFormations = 64, kMaxGrounds = 16, kMaxMarks = 128;
enum class Look : u32 { Fighter, Red, Green, Turret, Count }; // what an enemy's sprite is
enum class MarkKind : u32 { Formation, Ground, Speed, End };
struct FormationDef { u32 path; u32 count; u32 spacing; Look look; i32 health; i32 score; f32 fire_at; u32 fire_count; f32 speed; };
struct GroundDef    { f32 x; i32 health; i32 score; u32 cadence; f32 speed; };
struct Mark         { f32 at; MarkKind kind; u32 index; f32 value, over; };
struct StageDef {
    char         name[32] = "";
    f32          speed = 1.0f;  // units a tick at the start
    f32          length = 0.0f; // where the end mark is
    Path         paths[kMaxPaths];
    char         path_names[kMaxPaths][24];
    FormationDef formations[kMaxFormations];
    GroundDef    grounds[kMaxGrounds];
    Mark         marks[kMaxMarks];
    u32          path_count = 0, formation_count = 0, ground_count = 0, mark_count = 0;
};
// Reads a .stage.json. False with `why` filled for a file that does not say a stage.
bool stage_parse(const char* text, usize len, StageDef& out, char* why, usize why_cap);

// ---- the module's state ---------------------------------------------------------------------

enum Tex : u32 { TEX_SHIPS, TEX_TILES, TEX_STARS, TEX_NEBULA, TEX_CLOUD, TEX_BULLETS, TEX_DOT, TEX_RING, TEX_SOFT, TEX_SPARK, TEX_EGG, TEX_COUNT };

// What the game counts for its reports (stats.cc): this tick's numbers and the run's.
struct Stats {
    u32 rows[16];          // live rows per kind, by kind id (stats.cc fills the ones it reports)
    u32 bullets, bullets_spawned, bullets_destroyed;
    u32 sprites, deltas, pairs_tested, colliders;
    f32 cell;
};

struct Game {
    Handle   textures[TEX_COUNT];
    Handle   alpha[TEX_COUNT];    // render.material: each texture, alpha blended
    Handle   additive[TEX_COUNT]; // and added (glows, the hit flash)
    Handle   stage_blob;          // the .stage.json on its way in
    StageDef stage;
    bool     stage_ready = false;
    Handle   ui;                  // ui.doc
    Handle   hud, pause_screen, cover, clear_label;
    u32      clear_ticks = 0;     // the "stage clear" label's time left
    u32      act_move = 0, act_shot = 0, act_focus = 0, act_bomb = 0, act_pause = 0, act_quit = 0;
    bool     demo = false, autoplay = false, stress = false;
    Handle   world;               // the .world row
    Handle   ship, ring, mark;
    Rng      fx;                  // the cosmetic stream: sparks and debris, never sim state
    f32      shake = 0.0f;        // the camera's, in world units, decaying
    u32      bullets_spawned = 0; // this tick, by the emitters (enemies.cc)
    Stats    stats;
    // Reporting (stats.cc): peaks and sums over the run, the wall clock of the last line.
    Stats    peak;
    f64      sum_bullets = 0.0, sum_deltas = 0.0, sum_pairs = 0.0, sum_sprites = 0.0;
    u64      sampled = 0;
    f64      next_report = 0.0;
    u32      last_delta_len = 0;
};

// ---- across files --------------------------------------------------------------------------

// The world row's columns, by name, for ctx.read/ctx.write.
#define ARC_WORLD(c) "example_arcfire.world." #c

// Sprites (module.cc): a texture's material, a sprite of a cell of a sheet.
Sprite sprite_of(const Game& g, Tex tex, rect2 uv, vec2 size, i32 layer, color tint = color_white(), bool additive = false);
rect2  ship_uv(u32 col, u32 row);   // a 32 px cell of the ship sheet
rect2  tile_uv(u32 col, u32 row);   // a 16 px cell of the tile sheet
Sprite enemy_sprite(const Game& g, Look look);

// Dials (dials.cc).
Dials  dials_for(Position p);
void   dials_apply_options(Engine& e, Dials& d); // --dial name=value, each
void   dials_log(const Dials& d);

// Planes (planes.cc).
void   planes_spawn(Engine& e, Game& g, const Dials& d);

// The ship (ship.cc).
Handle ship_spawn(Engine& e, Game& g);
constexpr point2 kShipStart{0.0f, -260.0f};
Collider2 ship_collider();
Collider2 ring_collider();

// Effects (fx.cc): a burst of sparks, debris pieces; called from handlers and systems
// that spawn by command.
void   fx_sparks(TickContext& ctx, Game& g, point2 at, u32 count, color tint, f32 speed = 3.0f);
void   fx_piece(TickContext& ctx, Game& g, point2 at, Tex tex, rect2 uv, vec2 size, vec2 v, f32 spin, u32 life, i32 layer);

// Bullets (bullets.cc): aimed shots from a point, `count` in a fan centered on the ship.
u32    bullets_fire_aimed(Table& bullets, const Game& g, const Dials& d, point2 from, point2 target, u32 count, f32 speed, Handle owner);
Collider2 bullet_collider();

// The stage (stage.cc).
void   stage_reset(Stage& s, const StageDef& def);

// The HUD (hud.cc).
void   hud_open(Engine& e, Game& g);
void   hud_cover(Engine& e, Game& g, bool on);
void   hud_stage_clear(Engine& e, Game& g);

// Reports (stats.cc).
void   stats_summary(Game& g);

} // namespace kv::arcfire
