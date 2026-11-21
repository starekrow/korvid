// arcfire: the module record. Velocity, which every file moves by, and the world ktable;
// the list of the files' parts (each file declares its own components, ktables, events,
// systems and handlers at its end, and loads its own art and files in its part's startup);
// the startup that makes the game's state and reads the flags, before any part's; and the
// textures and sprite helpers every file draws with.
#include "arcfire.h"
#include "korvid/anim/player.h"
#include "korvid/core/format.h"
#include "korvid/asset/blob.h"
#include "korvid/asset_render/material.h"
#include "korvid/asset/scope.h"
#include "korvid/asset_render/texture.h"
#include "korvid/core/log.h"
#include "korvid/frameloop/frameloop.h"
#include "korvid/platform_hid/actions.h"
#include "korvid/sprite_ui/sprite_ui.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

namespace kv::arcfire {

constinit u32 g_world_ktable = 0, g_plane_ktable = 0, g_ship_ktable = 0, g_ring_ktable = 0, g_mark_ktable = 0, g_shot_ktable = 0, g_air_ktable = 0,
              g_ground_ktable = 0, g_bullet_ktable = 0, g_active_ktable = 0, g_particle_ktable = 0, g_egg_ktable = 0, g_shell_ktable = 0,
              g_item_ktable = 0, g_formation_ktable = 0, g_shadow_ktable = 0, g_boss_ktable = 0, g_part_ktable = 0, g_boom_ktable = 0, g_beam_ktable = 0, g_option_ktable = 0, g_glow_ktable = 0, g_aglow_ktable = 0, g_fleet_ktable = 0, g_tracer_ktable = 0, g_capital_ktable = 0;
constinit u32 g_part_down = 0;

namespace {

// ---- schemas -------------------------------------------------------------------------------------
// Velocity alone is the module's: eight files move rows by it. Every other component is
// declared in the file that owns it (Health in hits.cc, the world row's in dials.cc,
// stage.cc, scoring.cc, egg.cc and flow.cc, and so on).
constinit const FieldDesc k_velocity_fields[] = {KV_FIELDS(Velocity, v)};
constinit const ComponentDesc k_components[] = {
    KV_COMPONENT(Velocity, k_velocity_fields),
};

// ---- ktables -------------------------------------------------------------------------------------
// The world: the run's one row of dials, stage, score and the egg's state, which every file
// reads (its components are declared by the files that own them). Every other ktable is
// declared beside its code, in its file's part.
constinit const KTableDesc k_ktables[] = {
    {.name = ".world", .components = "Dials, Stage, Run, EggState, Chance", .capacity = 1, .id = &g_world_ktable},
};

// ---- the parts -----------------------------------------------------------------------------------
// Each file declares what it brings at its end: its components and ktables, its events, its
// systems and handlers. The engine reads them in this order, and nearly every system and
// handler writes the game's state, so within a phase this order is the order they run in
// wherever their sets leave it free. It follows the game. The hits come first: their handlers
// run in PostSim before the tick's systems, a bullet on the hitbox before the grazes (it
// cancels its own graze), then the kills. In Sim: the bot, the scroll and the stage's marks,
// the planes, the ship, its shots, the enemies and their bullets, the egg, the particles;
// then the collision module's broadphase, whose overlaps the hits' handlers sort into hits.
// The parts' startups run in this order too, after the module's; their shutdowns in reverse
// (the stats' summary, then the patterns' library). The dials and the sound bring no code
// that runs in a phase: the world row's dials, the effects and the music.
constinit const ModulePart* const k_parts[] = {
    &arcfire_hits,     &arcfire_pilot,    &arcfire_dials,    &arcfire_stage,    &arcfire_scoring,  &arcfire_records,
    &arcfire_screens,  &arcfire_flow,     &arcfire_planes,   &arcfire_ship,     &arcfire_weapons,  &arcfire_options,
    &arcfire_enemies,  &arcfire_boss,     &arcfire_patterns, &arcfire_bullets,  &arcfire_egg,      &arcfire_bomb,
    &arcfire_items,    &arcfire_sound,    &arcfire_fx,       &arcfire_battle,   &arcfire_hud,      &arcfire_stats,
};

// The actions the game reads, by name (arcfire_startup hands them to platform_hid).
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

// ---- startup -------------------------------------------------------------------------------------

// The sheets every file draws from; each file's part loads the rest of the art it owns.
constinit const TexFile k_textures[] = {
    {TEX_SHIPS, "arcfire/kenney/ships.png"},
    {TEX_TILES, "arcfire/kenney/tiles.png"},
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
    if (const char* bench = frameloop_option(e, "--bench")) format(g->bench, sizeof g->bench, "%s", bench);
    // The run's shape: run plainly, the title; a flag that names a run goes straight into
    // it: arcade unless a stage is named, and the demo, a stress run and the bench are runs
    // of their own. --difficulty is where arcade or the stage starts.
    bool stage_named = false;
    u32 stage_index = 0;
    if (const char* name = frameloop_option(e, "--stage")) {
        stage_index = arcfire::stage_named(name);
        if (stage_index == kStageCount) { KV_WARN("arcfire", "--stage %s: shoreline or orbit; Shoreline it is", name); stage_index = 0; }
        format(g->stage_name, sizeof g->stage_name, "%s", k_stage_names[stage_index]);
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
    g->plan = RunPlan{!stage_named && !g->demo && !g->stress && !g->bench[0], stage_named, g->start, stage_index};
    textures_load(e, *g, k_textures);
    KV_INFO("arcfire", "%s: %s at %s", g->front_door ? "the title" : g->plan.arcade ? "arcade" : g->plan.stage_select ? "stage select" : "run",
            g->stage_name, position_name((u32)g->start));
    engine_hold(e, HoldLoading, true); // until boot has what the parts' startups asked for
    KV_INFO("arcfire", "Arrows or WASD (or a pad's stick) fly, Z or Space (South) fires, Shift (West) focuses, Escape (Start) pauses, Q quits.");
    return true;
}

} // namespace

// ---- textures and sprites ------------------------------------------------------------------------

void textures_load(Engine& e, Game& g, Span<const TexFile> files) {
    for (const TexFile& t : files) {
        KV_VERIFY(g.textures[t.tex].is_null()); // one file loads each
        g.textures[t.tex] = asset_load_texture(e, t.path);
        g.alpha[t.tex] = material_of(e, g.textures[t.tex], BlendMode::Alpha);
        g.additive[t.tex] = material_of(e, g.textures[t.tex], BlendMode::Additive);
    }
}


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
    case Look::Heavy:  return sprite_of(g, TEX_SHIPS, ship_uv(3, 0), vec2{80, 80}, Z_AIR);
    case Look::Boat:   return sprite_of(g, TEX_BOAT, rect2_unit(), vec2{64, 144}, Z_AIR);
    case Look::Debris: return sprite_of(g, TEX_DEBRIS, rect2{0.0f, 0.0f, 1.0f / 3.0f, 1.0f}, vec2{48, 48}, Z_AIR);
    default:           return sprite_of(g, TEX_SHIPS, ship_uv(1, 0), vec2{64, 64}, Z_AIR);
    }
}

} // namespace kv::arcfire

namespace kv {
KV_MODULE_DEFINE(arcfire_module) = {
    .name       = "arcfire",
    .components = arcfire::k_components,
    .ktables    = arcfire::k_ktables,
    .parts      = arcfire::k_parts,
    .startup    = arcfire::arcfire_startup,
    .memory     = {.reserve = 1024 * 1024},
};
} // namespace kv
