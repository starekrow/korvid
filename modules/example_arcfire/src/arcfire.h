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
#include "korvid/bulletml/bulletml.h"
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
constexpr u32 L_BULLET = 16; // every enemy bullet, plain or running a program
constexpr u32 L_GRAZE  = 32; // the ring around the hitbox
constexpr u32 L_SHELL  = 64; // the egg's shell around a new ship
constexpr u32 L_ITEM   = 128; // chips and pickups: the ring collects them

// ---- sprite layers ----------------------------------------------------------------------
// Back to front, the master PRD's planes. The HUD is the UI's, above all of them.
// The ground map's three tile layers are Z_MAP and the two above it; they scroll by the
// renderer's layer offset (planes.cc), as the planes do, and nothing else is drawn on them.
// Over the ground the sky has two decks of cloud, each a plane on a layer of its own: the
// low clouds (their shadows on the sea and islands) under the air, and the high wisps over
// the air and the bosses but under the ship, its shots and every bullet.
constexpr i32 Z_DEEP = -30, Z_HIGH = -20, Z_MAP = -16, Z_CLOUD_SHADOW = -13, Z_SHADOW = -12, Z_GROUND = -10, Z_CLOUD = 10,
              Z_LOW_CLOUD = 12, Z_BOSS = 16, Z_PART = 17, Z_AIR = 20, Z_HIGH_CLOUD = 23, Z_SHIP = 30, Z_SHOT = 25, Z_ITEM = 35,
              Z_BULLET = 40, Z_FX = 50, Z_MARK = 60;
constexpr u32 kMapLayers = 3;    // sea, land, deco
constexpr f32 kMapScale = 2.0f;  // world units a map pixel

// ---- components -----------------------------------------------------------------------------

struct Velocity { vec2 v; };                      // world units a tick
struct Plane    { f32 rate; f32 tile; u32 row, rows; }; // a parallax tile: its plane's rate, its size, its row of the grid
// The weapons (weapons.cc): a type and a power level, the ship's. Switching the type keeps
// the level; a death keeps both.
enum WeaponType : u32 { WeaponSpread, WeaponBeam, WeaponStorm, WeaponLance, WeaponCount };
constexpr u32 kMaxPower = 5;
// The player's ship. `live` is 0 while the egg plays (control does nothing, nothing hits);
// `guard` counts the ticks a bomb keeps the hitbox out of play; `died`, its deaths;
// `firing`, the shot held this tick (the beam shows while it is).
struct Ship     { u32 cooldown; u32 live; u32 focus; u32 shots; u32 guard; u32 died; u32 weapon; u32 power; u32 firing; u32 pad; };
// A shot: plain (Spread), a bolt that turns toward the nearest enemy (Storm, `turn` radians
// a tick), or a lance that passes through what it kills. `spent`: it has stopped this tick.
enum ShotKind : u32 { ShotPlain, ShotBolt, ShotLance };
struct Shot     { i32 damage; u32 spent; u32 kind; f32 turn; };
// The beam: a child of what fires it, stretched up the field to the first body it meets,
// which it damages every few ticks (`timer`). `length` as cut this tick; `scroll` the uv's.
struct Beam     { u32 timer; f32 length; f32 scroll; u32 pad; };
// What can be shot. `flash` counts the ticks of the white flash left; `score` is paid at the kill.
struct Health   { i32 hp; u32 flash; i32 score; u32 look; };
// A formation member's path, measured, how far along it is, and its formation.
struct Mover    { Path path; f32 speed; f32 s; Handle formation; };
// A formation in the air: its members, how many were shot down and how many got away.
// Shot down to the last one, it drops a bomb where the last one fell.
// `drop`: what its last member shot down leaves (a Drop; 0 none).
struct Formation { u32 count, killed, gone, drop; };
// Something to pick up: a score chip (what a cancelled bullet becomes), a bomb, a weapon
// (it shows a type, which cycles while it falls: `value` the type it started at), power.
enum ItemKind : u32 { ItemChip, ItemBomb, ItemWeapon, ItemPower };
struct Item     { u32 kind; i32 value; u32 life; u32 pad; };
// What an item carrier leaves: none, a weapon item starting at a type (1 + the type), power.
enum Drop : u32 { DropNone, DropWeapon, DropWeaponLast = DropWeapon + WeaponCount - 1, DropPower };
// What an enemy fires: a pattern of the library (patterns.cc), started when an air row's
// mover passes `at` (a fraction of its path) or while the ship is above a ground row,
// and run by the row's Runner. `state`: EmitIdle, EmitRunning, EmitDone; `timer`: ticks
// before a ground row (or the bench) starts its pattern again.
enum EmitState : u32 { EmitIdle, EmitRunning, EmitDone };
struct Emitter  { u32 pattern; f32 at; u32 state; u32 timer; };
// A BulletML runner (the bulletml module's plain data) as a component: on the enemies,
// and on the bullets that run programs of their own (the `active` kind).
using Runner = BmlRunner;
// A ground row: it rides the scroll at `rate` times its speed. A tank also drives back and
// forth along its road (`vx`, within `range` of `home`); a bunker launches its formation
// (`launch`, a FormationDef) every so often (`timer`). The bench's fires whatever the ship does.
enum GroundKind : u32 { GroundTurret, GroundTank, GroundBunker };
struct Ground   { f32 rate; u32 bench; u32 kind; u32 launch; u32 timer; f32 vx; f32 home; f32 range; };
struct Bullet   { u32 life; u32 grazed; Handle owner; };
// A cosmetic piece of debris or a spark: it drifts, spins, slows, falls and fades over its life.
struct Particle { u32 life, max_life; f32 spin, drag, alpha, fall; };
// A boss (boss.cc): which definition, its phase, what it is doing, its clock (ticks since
// it came), how far along its entry it is, where it holds, and which parts are down (a bit
// each, by the part's index in the definition).
enum class BossState : u32 { Entering, Holding, Stomping, Leaving, Dying };
struct Boss     { u32 def; u32 phase; u32 state; u32 clock; f32 s; u32 down; point2 hold; };
// A part of a boss: its definition's index, its pattern's turn in the part's list, the
// ticks to its next pattern or launch, and its boss.
struct Part     { Handle boss; u32 index; u32 cycle; u32 timer; u32 pad; };
struct EggBody  { f32 vy; f32 drift; };           // the egg as it falls

// The world row's parts.
enum class Position : u32 { Normal, Stress, Demo };
struct Dials {
    u32 position;          // Position
    f32 rank;              // BulletML's $rank: how dense the patterns are (Normal 0.5)
    f32 bullet_speed;      // multiplies every bullet's speed
    f32 formation_count;   // multiplies a formation's members
    f32 formation_spacing; // multiplies the ticks between them
    u32 particles;         // sparks per event
    u32 plane_density;     // plane tiles per side of a 1024-unit tile: 1 is a 1024 tile, 8 a 128 one
    f32 enemy_health;
    u32 bot;               // the bot flies the ship
    u32 invulnerable;      // 1: always; 2: once the ship has died (the demo: its egg plays once)
    u32 loops;             // the stage loops at its end instead of ending
    u32 scroll_rows;       // move every plane and map tile each tick (slice 1's way), for measuring the layer scroll
    u32 weapon, power;     // what the ship starts with (a WeaponType, 1..5): Spread at one but to try or measure another
};
struct Stage {
    f32 distance;          // the stage's clock: units scrolled since the stage (or this loop) began
    f32 speed;             // units a tick, now
    f32 ramp_from, ramp_to, ramp_at, ramp_over; // a speed change in progress, by distance
    u32 cursor;            // the next mark
    u32 loops;             // times the stage has looped
    u32 ended;             // the end mark passed and the stage does not loop
    u32 ground_cursor;     // the next ground row of the map to come into view
    // The marks' clock is the distance less `base` (where this loop began) and `held` (what
    // the scroll ran while the stage waited at a boss); the map's is the distance less `base`.
    f32 base, held;
    u32 waiting;           // 1 while a boss mark's boss is up: the marks wait, the scroll does not
};
inline f32 stage_clock(const Stage& s) { return s.distance - s.base - s.held; }
inline f32 map_clock(const Stage& s) { return s.distance - s.base; }
struct Run {
    i64 score;
    i64 stage_score;       // the score when this stage (or loop) began: a restart goes back to it
    i32 eggs;
    u32 restarts;
    u32 epoch;             // +1 at every restart: scheduled spawns of an earlier epoch are dropped
    u32 kills, grazes, deaths;
    i32 bombs;             // in hand: 2 at the start and after every death, at most kMaxBombs
    u32 chips;             // picked up
};
constexpr i32 kBaseBombs = 2, kMaxBombs = 6;
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
struct BeamHit    { Handle target; i32 damage; u32 pad; point2 at; }; // the beam's tick of damage on the first body it met
struct BulletHit  { Handle bullet; };          // a bullet on the hitbox
struct Rammed     { Handle enemy; };           // an air enemy on the hitbox
struct Graze      { Handle bullet; };
struct ShellHit   { Handle bullet; };
// The enemy's own handle is not in it (it is gone when this is read, and the bus drops
// stale handles); its formation's is, which lives until its last member is accounted for.
struct Kill       { point2 at; i32 score; u32 pad; Handle formation; };
struct Bombed     { point2 at; u32 cancelled; u32 pad; };
struct Pickup     { Handle item; };
struct ShipDied   { point2 at; u32 cause; u32 pad; }; // cause 0 a bullet, 1 a ram; `at` is filled by the handler
// Scheduled by a formation mark (or a bunker); `group` its Formation row, `offset` where its
// path starts from (a bunker's launch: the path is relative to the bunker).
struct SpawnMember { u32 formation; u32 member; u32 epoch; u32 pad; Handle group; point2 offset; };
struct StageEnd   { u32 loops; u32 pad; };

// ---- kinds, by id (filled at init) --------------------------------------------------------------
extern u32 g_world_kind, g_plane_kind, g_ship_kind, g_ring_kind, g_mark_kind, g_shot_kind, g_air_kind, g_ground_kind,
    g_bullet_kind, g_active_kind, g_particle_kind, g_egg_kind, g_shell_kind, g_item_kind, g_formation_kind, g_shadow_kind, g_boss_kind, g_part_kind,
    g_boom_kind, g_beam_kind;
extern u32 g_part_down; // the trigger on a part's health
// The two bullet kinds: plain bullets, and bullets that run a program (needles, seeds).
inline bool is_bullet_kind(u32 kind_id) { return kind_id == g_bullet_kind || kind_id == g_active_kind; }

// ---- the stage as its file says -------------------------------------------------------------
constexpr u32 kMaxPaths = 32, kMaxFormations = 64, kMaxGrounds = 64, kMaxMarks = 128, kMaxBosses = 4;
enum class Look : u32 { Fighter, Red, Green, Turret, Tank, Bunker, Count }; // what an enemy's sprite is
enum class MarkKind : u32 { Formation, Ground, Speed, End, Boss };
constexpr u32 kPatternName = 32;
struct FormationDef { u32 path; u32 count; u32 spacing; Look look; i32 health; i32 score; f32 fire_at; f32 speed; char pattern[kPatternName]; u32 entry; u32 drop; };
// A ground row: from a stage mark, or from the map's enemies layer (then `at` is the stage
// distance at which it comes into view and `kind` what it is).
struct GroundDef    { f32 x; i32 health; i32 score; char pattern[kPatternName]; u32 entry; f32 at; GroundKind kind; f32 route; u32 launch; };
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
    // The ground: a Tiled map (its path; "" for none), its enemies once loaded (sorted by
    // `at`), and which planes the stage shows (bits by planes.cc's order; the three of
    // space by default).
    char         map[96] = "";
    char         boss_names[kMaxBosses][24]; // the boss marks' bosses, by mark index; Game::boss_of maps them to definitions
    u32          boss_mark_count = 0;
    GroundDef    map_grounds[kMaxGrounds];
    u32          map_ground_count = 0;
    u32          planes = 7;
};
// Reads a .stage.json. False with `why` filled for a file that does not say a stage.
bool stage_parse(const char* text, usize len, StageDef& out, char* why, usize why_cap);

// ---- a boss as its file says (boss_data.cc) ------------------------------------------------
// A boss is a hull and parts on it, in phases. Shots fly over the hull (a part set back from
// its edge could not be hit otherwise); the hull is a body the ship must not touch. A part
// is a turret, a launch bay or a core; it is armored (shots spark off it and it does not
// fire) until its phase; when every part a phase names is down, the next phase begins; the
// core's death is the boss's. assets/bosses/<name>.boss.json.
constexpr u32 kMaxParts = 32, kMaxPhases = 6, kMaxPartPatterns = 4, kNoFormation = ~0u;
struct PartDef {
    char   name[24];
    rect2  uv, uv_open;    // its cell of the parts sheet, armored and once its phase has come
    vec2   size;           // world units
    point2 at;             // on its parent (the hull, or a part), world units
    f32    radius;         // its collider
    i32    health, score;
    char   patterns[kMaxPartPatterns][kPatternName]; // taken in turn, `rest` ticks apart
    u32    entries[kMaxPartPatterns];
    u32    pattern_count;
    u32    rest;
    u32    armored_until;  // the phase it opens in
    i32    parent;         // a part's index, or -1: the hull
    bool   core;
    u32    launch;         // a formation it launches (the stage's index), or kNoFormation
    u32    every;          // ticks between launches
};
struct PhaseDef { char name[24]; u32 ends; f32 sway, sway_rate; }; // ends: the parts (bits) whose deaths end it
struct BossDef {
    char     name[24];     // its file's
    char     title[32];    // the HUD's
    vec2     size;         // the hull's sprite, world units
    vec2     hull_half, hull_offset; // the hull's collider
    Path     entry;        // from above the field to where it holds
    f32      speed;        // along the entry, units a tick
    u32      timeout;      // ticks from its arrival
    i32      score;        // the kill's bonus, at full time left
    vec2     cell;         // a cell of the parts sheet, as a fraction of it
    PartDef  parts[kMaxParts];
    PhaseDef phases[kMaxPhases];
    u32      part_count, phase_count;
    i32      total;        // its parts' health: the bar's full
    // Its time out: every part still standing fires `stomp` (the bays launch at once) and
    // the hull comes `dive` units down the field and back over `stomp_ticks`; then it leaves.
    char     stomp[kPatternName];
    u32      stomp_entry;
    u32      stomp_ticks;
    f32      dive;
};
// Reads a .boss.json. Its paths and its bays' formations go into the stage (named
// "<boss>.<path>"), so a launch is a formation like any other. False with `why` filled.
bool boss_parse(const char* text, usize len, const char* name, BossDef& out, StageDef& stage, char* why, usize why_cap);

// ---- the module's state ---------------------------------------------------------------------

// The effects (sound.cc), by what they are for.
enum Sfx : u32 { SFX_SHOT, SFX_HIT, SFX_PING, SFX_KILL, SFX_KILL_BIG, SFX_BOSS_DOWN, SFX_RUMBLE, SFX_GRAZE, SFX_CHIP, SFX_BOMB,
                 SFX_EGG_BREAK, SFX_EGG_OPEN, SFX_EGG_BLOOM, SFX_WARNING, SFX_COUNT };

enum Tex : u32 { TEX_SHIPS, TEX_TILES, TEX_STARS, TEX_NEBULA, TEX_CLOUD, TEX_BULLETS, TEX_DOT, TEX_RING, TEX_SOFT, TEX_SPARK, TEX_EGG,
                 TEX_CLOUDS_LOW, TEX_CLOUDS_HIGH, TEX_BEAM, TEX_ITEMS, TEX_COUNT };
constexpr u32 kPlaneCount = 6; // planes.cc's: stars, nebula, cloud, the low clouds' shadows, the low clouds, the high wisps

// What the game counts for its reports (stats.cc): this tick's numbers and the run's.
struct Stats {
    u32 rows[16];          // live rows per kind, by kind id (stats.cc fills the ones it reports)
    u32 bullets, bullets_spawned, bullets_destroyed;
    u32 actives, runners, fires; // bullets that run programs; runners stepped; fires this tick
    u32 items, chips_made, chips_refused; // items alive; chips made, and bullets cancelled past a cancel's cap, this run
    u32 sprites, deltas, pairs_tested, colliders, groups;
};

struct Game {
    Handle   textures[TEX_COUNT];
    Handle   alpha[TEX_COUNT];    // render.material: each texture, alpha blended
    Handle   additive[TEX_COUNT]; // and added (glows, the hit flash)
    Handle   stage_blob;          // the .stage.json on its way in
    StageDef stage;
    bool     stage_parsed = false, stage_ready = false;
    // The patterns: patterns.json names them, each a BulletML document; compiled into
    // the library when all are in. A bullet definition's look, by its label.
    static constexpr u32 kMaxPatterns = 32;
    Handle   pattern_index;
    Handle   pattern_blobs[kMaxPatterns];
    char     pattern_names[kMaxPatterns][kPatternName];
    u32      pattern_count = 0;
    BmlLibrary lib;
    u8       bullet_look[1024];   // 0 round, 1 needle; by bullet definition
    char     bench[kPatternName] = ""; // --bench NAME
    u32      bench_entry = KV_BML_NONE;
    // The bosses (boss.cc): every one the game has is loaded at startup (a fixed list, so
    // their materials' handles are the same every run), parsed at boot.
    BossDef  bosses[kMaxBosses];
    u32      boss_count = 0;
    Handle   boss_blobs[kMaxBosses];
    Handle   boss_tex[kMaxBosses][2];      // hull, parts sheet
    Handle   boss_alpha[kMaxBosses][2], boss_additive[kMaxBosses][2];
    u32      boss_of[kMaxBosses];          // a boss mark's definition
    bool     bosses_parsed = false;
    Handle   stomp_screen;                 // the warning's tint, while a boss stomps
    Handle   sfx[SFX_COUNT];               // asset.sound
    Handle   music_stage, music_boss;
    u32      voices_peak = 0;              // the audio module's live voices, at most (stats.cc)
    Handle   boom_clip;                    // anim.clip: an explosion, Kenney's burst frames
    Handle   map;                 // the stage's ground map (tilemap), and its tiles once spawned
    bool     map_spawned = false;
    point2   map_origin;          // the map's top-left in the world when the distance is 0
    // The layers' scroll (planes.cc): what each plane has moved (its offset wraps), the
    // ground's last offset (a jump back is a snap: a loop, a restart).
    f32      plane_scrolled[kPlaneCount] = {};
    f32      ground_offset = 0.0f;
    Handle   ui;                  // ui.doc
    Handle   hud, pause_screen, cover, clear_label;
    u32      clear_ticks = 0;     // the "stage clear" label's time left
    u32      act_move = 0, act_shot = 0, act_focus = 0, act_bomb = 0, act_pause = 0, act_quit = 0;
    bool     demo = false, autoplay = false, stress = false;
    bool     demo_core_bomb = false;  // the demo's bomb in the carrier's core phase, once
    Handle   world;               // the .world row
    Handle   ship, ring, mark;
    Rng      fx;                  // the cosmetic stream: sparks and debris, never sim state
    f32      shake = 0.0f;        // the camera's, in world units, decaying
    u32      bullets_spawned = 0; // this tick, by the runners (patterns.cc)
    u32      runners_stepped = 0;
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
// The ground map, once its asset is Ready: its tiles spawned (cosmetic, still, scrolled by
// layer) and its enemies read into the stage. False while it is loading.
bool   ground_map_ready(TickContext& ctx, Game& g, char* why, usize why_cap);
// A ground row at `at` (stage.cc): a mark's, the map's, the bench's.
void   ground_spawn(TickContext& ctx, const Game& g, const Dials& d, const GroundDef& gd, point2 at, bool bench = false);

// The ship (ship.cc).
Handle ship_spawn(Engine& e, Game& g, const Dials& d);
constexpr point2 kShipStart{0.0f, -260.0f};
// Where a volley leaves the ship: Spread's outer shots this far to either side of the nose,
// and this far above the ship's middle (weapons.cc; the pilots aim by them).
constexpr f32 kMuzzle = 8.0f, kMuzzleUp = 18.0f;
Collider2 ship_collider();
Collider2 ring_collider();

// Effects (fx.cc): a burst of sparks, debris pieces; called from handlers and systems
// that spawn by command.
void   fx_sparks(TickContext& ctx, Game& g, point2 at, u32 count, color tint, f32 speed = 3.0f);
void   fx_piece(TickContext& ctx, Game& g, point2 at, Tex tex, rect2 uv, vec2 size, vec2 v, f32 spin, u32 life, i32 layer, f32 fall = 0.0f);
void   fx_piece_of(TickContext& ctx, Handle material, point2 at, rect2 uv, vec2 size, vec2 v, f32 spin, u32 life, i32 layer, f32 fall = 0.0f);
// An explosion: Kenney's burst frames as a flipbook (a cosmetic row that goes when it ends).
void   fx_boom(TickContext& ctx, Game& g, point2 at, f32 size);
Handle fx_boom_clip(Engine& e);

// Weapons (weapons.cc): a volley of the weapon at its power from `at`, into the shot
// kind; its next cooldown. The beam is fired by `beams`, not here.
u32    weapon_fire(Game& g, Table& shots, point2 at, u32 weapon, u32 power, bool focus);
const char* weapon_name(u32 weapon);
// The beam that rides on `owner` (the ship, an option).
Handle beam_spawn(Engine& e, Game& g, Handle owner);
// The innermost Spread shot at this power: its muzzle's x and its angle (radians,
// clockwise from up), for the pilots' aim.
void   spread_inner(u32 power, bool focus, f32* muzzle_x, f32* angle);
// A hit of `damage` on an enemy (air, ground, a boss's part) at `at`: absorbed (an armored
// part, a boss's hull: a spark, true), or dealt (a flash; an air or ground row that drops to
// zero is destroyed and pays a Kill; a part's own trigger takes it). `killed` when it died.
bool   hurt(TickContext& ctx, Game& g, Handle target, i32 damage, point2 at, bool* killed);

// Bullets (bullets.cc): one fired by a runner, by command from a parallel chunk; a plain
// bullet, or an active one with its own runner when its definition has actions.
void   bullet_fire(ChunkContext& cc, const Game& g, const Dials& d, point2 at, const BmlFire& f, Handle owner, u64 seed);
Collider2 bullet_collider();
// The Bullet row of a handle of either bullet kind, through the context's declared write.
Bullet* bullet_row(TickContext& ctx, Handle h);
const Transform2* bullet_xf(TickContext& ctx, Handle h);

// Patterns (patterns.cc): the library's loading, an enemy's start.
bool   patterns_load(Engine& e, Game& g);          // the index and the documents, asked for
bool   patterns_ready(Engine& e, Game& g, bool& failed); // all in and compiled
void   emitter_start(const Game& g, Emitter& em, Runner& r, Handle owner, u64 tick);
u64    seed_of(Handle h, u64 tick, u32 salt);

// The stage (stage.cc).
void   stage_reset(Stage& s, const StageDef& def);

// Bosses (boss.cc): their files asked for at startup, parsed at boot; a mark's boss spawned.
void   bosses_load(Engine& e, Game& g);
bool   bosses_ready(Engine& e, Game& g, char* why, usize why_cap); // false while loading; `why` set for a failure
Handle boss_spawn(TickContext& ctx, Game& g, const Dials& d, u32 def);
// A shot on a part (hits.cc): absorbed by its armor (true) or hurt.
bool   part_absorbs(const Game& g, TickContext& ctx, Handle part);
void   boss_ping(TickContext& ctx, Game& g, point2 at);

// Items (items.cc): bullets cancelled into chips (a bomb, a death), at most kChipCap of
// them made a cancel; the bullets past the cap are counted in `overflow` for the caller to
// pay. `radius` <= 0 cancels every bullet on the field.
constexpr u32 kChipCap = 256;
constexpr i32 kChipValue = 10;
constexpr f32 kMagnetY = kFieldTop - 120.0f; // above it, every item comes to the ship
u32    cancel_bullets(TickContext& ctx, Game& g, point2 at, f32 radius, u32* overflow);
void   item_drop(TickContext& ctx, const Game& g, ItemKind kind, point2 at, vec2 v, i32 value = 0);
void   drop_item(TickContext& ctx, const Game& g, u32 drop, point2 at); // an item carrier's Drop
Collider2 item_collider();

// The HUD (hud.cc).
void   hud_open(Engine& e, Game& g);
void   hud_cover(Engine& e, Game& g, bool on);
void   hud_stage_clear(Engine& e, Game& g);
void   hud_flash(Engine& e);
void   hud_boss(Engine& e, Game& g, const char* title, i32 hp, i32 total); // title null: the bar goes
void   hud_stomp(Engine& e, Game& g, bool on);                              // the warning's tint

// Sound (sound.cc): an effect, its volume, priority and pitch's wander from its table; the
// music, the stage's or a boss's. From a system or handler that writes audio.commands.
void   sounds_load(Engine& e, Game& g);
void   sfx(TickContext& ctx, Game& g, Sfx which, f32 pitch = 1.0f);
void   music(TickContext& ctx, Game& g, bool boss);

// Reports (stats.cc).
void   stats_summary(Engine& e, Game& g);

} // namespace kv::arcfire
