// example_arcanum: a JRPG skeleton. Two Tiled maps loaded through the level module, a
// player who walks in eight directions under the "move" action, collides with the map's
// solids (tilemap_move) and with the villagers, sorts by its feet against trees and
// houses, and walks through exits to the other map; a pause menu with a level select and
// the credits; music that crossfades between the maps.
#include "korvid/example_arcanum/module.h"
#include "korvid/asset/blob.h"
#include "korvid/asset/scope.h"
#include "korvid/asset/sound.h"
#include "korvid/asset/stream.h"
#include "korvid/asset/font.h"
#include "korvid/asset/texture.h"
#include "korvid/audio/audio.h"
#include "korvid/core/engine.h"
#include "korvid/core/json.h"
#include "korvid/core/log.h"
#include "korvid/core/utf8.h"
#include "korvid/frameloop/frameloop.h"
#include "korvid/level/level.h"
#include "korvid/math/fields.h"
#include "korvid/platform/actions.h"
#include "korvid/platform/host_window.h"
#include "korvid/render/renderer.h"
#include "korvid/scene/parent.h"
#include "korvid/sprite/camera.h"
#include "korvid/sprite/sprite.h"
#include "korvid/sprite/transform2.h"
#include "korvid/text/text.h"
#include "korvid/tilemap/tilemap.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace kv {

namespace {

// ---- data --------------------------------------------------------------------------------------

// A character: which way it faces, whether it is walking, its walk cycle, and for a
// villager a route between two points (relative to where it stands).
struct Actor {
    u8     facing;   // 0 down, 1 up, 2 left, 3 right: the sheet's columns
    u8     moving;
    u8     sheet;    // index into Game::sheets
    u8     route_dir;
    f32    walk;     // seconds into the walk cycle
    point2 home;     // a villager's spawn point
    vec2   route;    // a villager's second point, from home; zero stands still
    f32    route_t;  // 0 at home, 1 at home + route
};
constinit const FieldDesc k_actor_fields[] = {KV_FIELDS(Actor, facing, moving, sheet, route_dir, walk, home, route, route_t)};
constinit const ComponentDesc k_components[] = {KV_COMPONENT(Actor, k_actor_fields)};

constinit const KindDesc k_kinds[] = {
    {.name = ".player", .components = "Transform2, Sprite, Actor", .capacity = 1},
    {.name = ".npc",    .components = "Transform2, Local2, Parent, Sprite, Actor", .capacity = 32},
    // The menu and the credits: presentation, never hashed.
    {.name = ".ui",     .components = "Transform2, Text", .capacity = 128, .cosmetic = true},
    {.name = ".dim",    .components = "Transform2, Sprite", .capacity = 2, .cosmetic = true},
};

constexpr f32 kWalkSpeed   = 64.0f;   // px per second
constexpr f32 kNpcSpeed    = 24.0f;
constexpr f32 kFrameTime   = 0.15f;   // seconds per walk frame
constexpr f32 kFeetW = 10.0f, kFeetH = 5.0f;
constexpr u32 kSheets = 3;
const char* const k_sheet_paths[kSheets] = {"example_arcanum/actors/NinjaBlue.png", "example_arcanum/actors/OldMan.png", "example_arcanum/actors/ManGreen.png"};
const char* const k_sheet_names[kSheets] = {"NinjaBlue", "OldMan", "ManGreen"};

constinit const LevelDesc k_levels[] = {
    {"village", "example_arcanum/maps/village.tmj"},
    {"field",   "example_arcanum/maps/field.tmj"},
};

constinit const InputActionDesc k_actions[] = {
    {"move",    ActionKind::Axis2,  "keys W S A D, keys Up Down Left Right, stick left, dpad"},
    {"confirm", ActionKind::Button, "key Enter, key Space, pad South"},
    {"cancel",  ActionKind::Button, "key Backspace, pad East"},
    {"menu",    ActionKind::Button, "key Escape, pad Start"},
};

enum class Menu : u8 { Closed, Main, Levels, Credits };
enum Snd : u8 { SND_MOVE, SND_ACCEPT, SND_CANCEL, SND_COUNT };

struct Game {
    u32    act_move = 0, act_confirm = 0, act_cancel = 0, act_menu = 0;
    Handle sheet_texture[kSheets], sheet_material[kSheets];
    Handle white_texture, white_material;
    Handle font;
    Handle sounds[SND_COUNT];
    Handle credits;              // the credits.json blob
    Handle player;
    Handle map;                  // the current level's map
    Handle music;                // the current level's track (asset.stream)
    bool   exit_armed = false;   // the player has stood outside every exit since arriving
    bool   leaving = false;      // an exit has been taken; the level is on its way
    Menu   menu = Menu::Closed;
    u32    cursor = 0;
    i32    credits_scroll = 0;
    bool   menu_dirty = false;
    bool   demo = false;
    u64    demo_t = 0;           // ticks of the script, not counting the ones a level load holds
};

// ---- sprites ---------------------------------------------------------------------------------------

// The sheet's cell for a facing and a frame: 4 columns of facings, rows of walk frames.
rect2 sheet_uv(u8 facing, u32 row) { return rect2{(f32)facing / 4.0f, (f32)row / 7.0f, 0.25f, 1.0f / 7.0f}; }

Sprite actor_sprite(const Game& g, u8 sheet, i32 layer) {
    Sprite s;
    s.material = g.sheet_material[sheet];
    s.uv    = sheet_uv(0, 0);
    s.size  = vec2{16, 16};
    s.layer = layer;
    s.sort  = SpriteSort::Feet;
    s.order = -8.0f; // the feet are the sprite's bottom edge
    return s;
}

// The feet box (world units, min corner) of a sprite centered at `p`.
rect2 feet_of(point2 p) { return rect2{p.x - kFeetW * 0.5f, p.y - 8.0f, kFeetW, kFeetH}; }
point2 center_of_feet(const rect2& b) { return point2{b.x + kFeetW * 0.5f, b.y + 8.0f}; }

bool overlaps(const rect2& a, const rect2& b) { return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h; }

// The facing a direction is mostly in.
u8 facing_of(vec2 d, u8 was) {
    if (d.x == 0.0f && d.y == 0.0f) return was;
    if (fabsf(d.x) > fabsf(d.y)) return d.x < 0 ? 2 : 3;
    return d.y < 0 ? 0 : 1;
}

// The walk cycle into the sprite: rows 0..3 while walking, row 0 standing.
void animate(Actor& a, Sprite& s, f32 dt) {
    if (a.moving) a.walk += dt; else a.walk = 0.0f;
    u32 row = a.moving ? (u32)(a.walk / kFrameTime) % 4 : 0;
    s.uv = sheet_uv(a.facing, row);
}

// A map's exits that contain a feet box, in world units (the map's origin at 0, 0).
const MapObject* exit_at(const TilemapAsset& m, const rect2& feet) {
    const MapLayer* exits = tilemap_layer_find(m, "exits");
    if (!exits) return nullptr;
    for (u32 i = 0; i < exits->objects.count; ++i) {
        const MapObject& o = m.objects[exits->objects.first + i];
        rect2 r{o.x, -(o.y + o.height), o.width, o.height};
        if (overlaps(feet, r)) return &o;
    }
    return nullptr;
}

void play_sound(TickContext& ctx, Game& g, Snd snd) {
    SoundPlay p;
    p.sound = g.sounds[snd];
    p.bus = AudioBus::Ui;
    p.pause_with_sim = false;
    p.volume = 0.6f;
    audio_play(ctx, p);
}

// ---- the menu ------------------------------------------------------------------------------------

u32 item_count(Engine& e, const Game& g) {
    switch (g.menu) {
    case Menu::Main:   return 4;                 // Resume, Level, Credits, Quit
    case Menu::Levels: return KV_COUNTOF(k_levels) + 1; // the levels, Back
    default:           return 0;
    }
    (void)e;
}

void label(Table& ui, const Game& g, const char* text, f32 x, f32 y, f32 scale, color tint) {
    Text t;
    t.font  = g.font;
    utf8_copy(t.str, sizeof t.str, text);
    t.scale = scale;
    t.tint  = tint;
    t.layer = 110;
    t.space = SpriteSpace::Screen;
    t.align = TextAlign::Center;
    ui.spawn(transform2_at(point2{x, y}), t);
}

// The menu's labels, made again whenever what it shows changes.
void build_menu(TickContext& ctx, Game& g) {
    Engine& e = *ctx.engine;
    Table* ui = ctx.write_kind("example_arcanum.ui");
    Table* dim = ctx.write_kind("example_arcanum.dim");
    ui->destroy_all();
    dim->destroy_all();
    if (g.menu == Menu::Closed) return;
    Sprite shade;
    shade.material = g.white_material;
    shade.size  = vec2{100000.0f, 100000.0f};
    shade.tint  = color_rgba(0.0f, 0.0f, 0.04f, 0.65f);
    shade.layer = 100;
    shade.space = SpriteSpace::Screen;
    dim->spawn(transform2_at(point2{0, 0}), shade);
    const color white = color_white(), gold = color_rgba(1.0f, 0.78f, 0.25f, 1.0f), grey = color_rgba(0.6f, 0.6f, 0.65f, 1.0f);
    char line[64];
    if (g.menu == Menu::Main || g.menu == Menu::Levels) {
        const char* const main_items[] = {"Resume", "Level", "Credits", "Quit"};
        label(*ui, g, g.menu == Menu::Main ? "Paused" : "Go to", 0, 150, 1.4f, gold);
        u32 n = item_count(e, g);
        for (u32 i = 0; i < n; ++i) {
            const char* name;
            char tmp[48];
            if (g.menu == Menu::Main) name = main_items[i];
            else if (i < KV_COUNTOF(k_levels)) {
                bool here = level_current(e) == i;
                snprintf(tmp, sizeof tmp, "%s%s", k_levels[i].name, here ? "  (here)" : "");
                name = tmp;
            } else name = "Back";
            snprintf(line, sizeof line, "%s%s", i == g.cursor ? "\xe2\x80\xba " : "", name); // U+203A
            label(*ui, g, line, 0, 70.0f - 44.0f * (f32)i, 1.0f, i == g.cursor ? gold : white);
        }
        return;
    }
    // Credits: headings and lines from credits.json, scrolled.
    const BlobAsset* b = asset_blob(e, g.credits);
    if (!b || b->state != AssetState::Ready) { label(*ui, g, "...", 0, 0, 1.0f, grey); return; }
    MemScope scope;
    JsonDoc doc;
    if (!json_parse((const char*)b->data, b->size, mem_scratch(), doc)) { label(*ui, g, "credits.json does not parse", 0, 0, 0.8f, grey); return; }
    f32 y = 240.0f + 34.0f * (f32)g.credits_scroll;
    label(*ui, g, json_string(json_get(doc, doc.root(), "title"), "Credits"), 0, y, 1.5f, gold);
    y -= 60.0f;
    const JsonValue* sections = json_get(doc, doc.root(), "sections");
    for (const JsonValue* sec = sections ? json_first(doc, *sections) : nullptr; sec; sec = json_next(doc, *sec)) {
        label(*ui, g, json_string(json_get(doc, *sec, "heading"), ""), 0, y, 0.9f, gold);
        y -= 32.0f;
        const JsonValue* lines = json_get(doc, *sec, "lines");
        for (const JsonValue* l = lines ? json_first(doc, *lines) : nullptr; l; l = json_next(doc, *l)) {
            label(*ui, g, json_string(l, ""), 0, y, 0.8f, white);
            y -= 28.0f;
        }
        y -= 16.0f;
    }
}

void open_menu(TickContext& ctx, Game& g, Menu m) {
    if (g.menu == Menu::Closed) engine_pause(*ctx.engine, true);
    g.menu = m;
    g.cursor = 0;
    g.credits_scroll = 0;
    g.menu_dirty = true;
}

void close_menu(TickContext& ctx, Game& g) {
    g.menu = Menu::Closed;
    g.menu_dirty = true;
    engine_pause(*ctx.engine, false);
}

// Phase PreSim, every tick (paused ones are when it matters): the menu.
void menu(TickContext& ctx) {
    static const u32 k_actions_id = ctx.resource_id("input.actions");
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    const InputActions& a = *ctx.resource<const InputActions>(k_actions_id);
    bool held_by_load = (engine_holds(e) & HoldLoading) != 0;
    if (g.menu == Menu::Closed) {
        if (action_pressed(a, g.act_menu) && !held_by_load) { open_menu(ctx, g, Menu::Main); play_sound(ctx, g, SND_ACCEPT); }
    } else {
        vec2 mv = action_axis(a, g.act_move);
        bool step = action_repeat(a, g.act_move, 18, 6);
        if (g.menu == Menu::Credits) {
            if (step && mv.y != 0.0f) { g.credits_scroll += mv.y > 0 ? -1 : 1; if (g.credits_scroll < 0) g.credits_scroll = 0; g.menu_dirty = true; }
            if (action_pressed(a, g.act_confirm) || action_pressed(a, g.act_cancel)) { open_menu(ctx, g, Menu::Main); g.cursor = 2; play_sound(ctx, g, SND_CANCEL); }
        } else {
            u32 n = item_count(e, g);
            if (step && fabsf(mv.y) > 0.5f) {
                g.cursor = mv.y < 0 ? (g.cursor + 1) % n : (g.cursor + n - 1) % n;
                g.menu_dirty = true;
                play_sound(ctx, g, SND_MOVE);
            }
            if (action_pressed(a, g.act_menu)) { close_menu(ctx, g); play_sound(ctx, g, SND_CANCEL); }
            else if (action_pressed(a, g.act_cancel)) {
                if (g.menu == Menu::Levels) { open_menu(ctx, g, Menu::Main); g.cursor = 1; }
                else close_menu(ctx, g);
                play_sound(ctx, g, SND_CANCEL);
            } else if (action_pressed(a, g.act_confirm)) {
                play_sound(ctx, g, SND_ACCEPT);
                if (g.menu == Menu::Main) {
                    switch (g.cursor) {
                    case 0: close_menu(ctx, g); break;
                    case 1: open_menu(ctx, g, Menu::Levels); g.cursor = level_current(e) < KV_COUNTOF(k_levels) ? level_current(e) : 0; break;
                    case 2: open_menu(ctx, g, Menu::Credits); break;
                    case 3: engine_request_quit(e); break;
                    }
                } else if (g.cursor < KV_COUNTOF(k_levels)) {
                    close_menu(ctx, g);
                    LevelTransition t;
                    t.fade_out = 0.4f; t.fade_in = 0.4f;
                    level_request(e, k_levels[g.cursor].name, t);
                } else { open_menu(ctx, g, Menu::Main); g.cursor = 1; }
            }
        }
    }
    if (g.menu_dirty) { build_menu(ctx, g); g.menu_dirty = false; }
}

// ---- walking -------------------------------------------------------------------------------------

// Phase Sim: the player walks, collides, and takes exits.
void walk(TickContext& ctx) {
    static const u32 k_actions_id = ctx.resource_id("input.actions");
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    const InputActions& a = *ctx.resource<const InputActions>(k_actions_id);
    Table* players = ctx.write_kind("example_arcanum.player");
    const Table* npcs = ctx.read_kind("example_arcanum.npc");
    Transform2* t = players->get<Transform2>(g.player);
    Sprite* s = players->get<Sprite>(g.player);
    Actor* act = players->get<Actor>(g.player);
    const TilemapAsset* m = tilemap_get(e, g.map);
    if (!t || !m || m->state != AssetState::Ready || level_state(e) == LevelState::Loading) return;
    vec2 dir = action_axis(a, g.act_move);
    f32 dt = (f32)ctx.dt;
    vec2 d{dir.x * kWalkSpeed * dt, dir.y * kWalkSpeed * dt};
    act->moving = (dir.x != 0.0f || dir.y != 0.0f) && !g.leaving;
    act->facing = facing_of(dir, act->facing);
    rect2 before = feet_of(t->position);
    TileMove mv = tilemap_move(*m, point2{0, 0}, before, d);
    rect2 after = mv.box;
    // The villagers block, as of their last placement: try the move whole, then each axis.
    auto blocked = [&](const rect2& box) {
        for (u32 i = 0; i < npcs->rows(); ++i)
            if (npcs->is_alive(i) && overlaps(box, feet_of(npcs->rows_of<Transform2>()[i].position))) return true;
        return false;
    };
    if (blocked(after)) {
        rect2 xonly{after.x, before.y, after.w, after.h}, yonly{before.x, after.y, after.w, after.h};
        after = !blocked(xonly) ? xonly : !blocked(yonly) ? yonly : before;
    }
    t->position = center_of_feet(after);
    animate(*act, *s, dt);
    // Exits: armed once the player has stood clear of them since arriving.
    const MapObject* ex = exit_at(*m, after);
    if (!ex) g.exit_armed = true;
    else if (g.exit_armed && !g.leaving) {
        const MapProperty* target = tilemap_property(*m, ex->props, "target");
        const MapProperty* entry = tilemap_property(*m, ex->props, "entry");
        LevelTransition tr;
        tr.fade_out = 0.4f; tr.fade_in = 0.4f;
        tr.entry = entry ? entry->str : "";
        if (target && level_request(e, target->str, tr)) g.leaving = true;
    }
}

// Phase Sim: villagers pace their routes and turn to face where they go.
void villagers(TickContext& ctx) {
    Table* npcs = ctx.write_kind("example_arcanum.npc");
    f32 dt = (f32)ctx.dt;
    for (u32 i = 0; i < npcs->rows(); ++i) {
        if (!npcs->is_alive(i)) continue;
        Actor& a = npcs->rows_of<Actor>()[i];
        Local2& l = npcs->rows_of<Local2>()[i];
        Sprite& s = npcs->rows_of<Sprite>()[i];
        f32 len = sqrtf(a.route.x * a.route.x + a.route.y * a.route.y);
        if (len > 0.0f) {
            f32 dir = a.route_dir ? -1.0f : 1.0f;
            a.route_t += dir * kNpcSpeed * dt / len;
            if (a.route_t >= 1.0f) { a.route_t = 1.0f; a.route_dir = 1; }
            if (a.route_t <= 0.0f) { a.route_t = 0.0f; a.route_dir = 0; }
            l.position = point2{a.home.x + a.route.x * a.route_t, a.home.y + a.route.y * a.route_t};
            a.moving = 1;
            a.facing = facing_of(vec2{a.route.x * dir, a.route.y * dir}, a.facing);
        }
        animate(a, s, dt);
    }
}

// Phase PostSim, every tick: the camera follows the player inside the map, at an integer zoom.
void camera(TickContext& ctx) {
    static const u32 k_camera = ctx.resource_id("sprite.camera");
    static const u32 k_window = ctx.resource_id("platform.window");
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    Camera2* cam = ctx.resource<Camera2>(k_camera);
    const TilemapAsset* m = tilemap_get(e, g.map);
    const HostWindow* w = ctx.resource<const HostWindow>(k_window);
    u32 h = w ? w->height() : 720;
    cam->zoom = (f32)(h / 240 ? h / 240 : 1);
    cam->follow = g.player;
    cam->pixel_snap = true;
    if (m && m->state == AssetState::Ready) cam->bounds = rect2{0.0f, -(f32)(m->height * m->tile_h), (f32)(m->width * m->tile_w), (f32)(m->height * m->tile_h)};
}

// ---- levels -------------------------------------------------------------------------------------

// The level is loaded: its tiles, the villagers it names, the player at its entry.
void on_build(TickContext& ctx, Span<const LevelBuild> builds) {
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    for (const LevelBuild& b : builds) {
        const TilemapAsset* m = tilemap_get(e, b.map);
        if (!m || m->state != AssetState::Ready) { KV_ERROR("arcanum", "level %s has no map", level_name(e, b.level)); continue; }
        g.map = b.map;
        u32 objects_layer = 0;
        tilemap_layer_find(*m, "objects", &objects_layer);
        TilemapPlace place;
        place.owner = b.root;
        tilemap_spawn(ctx, b.map, place);
        // The player, at the entry it came by or the map's start.
        const MapLayer* spawns = tilemap_layer_find(*m, "spawns");
        const MapObject* at = nullptr;
        for (u32 i = 0; spawns && i < spawns->objects.count; ++i) {
            const MapObject& o = m->objects[spawns->objects.first + i];
            if (b.entry[0] && !strcmp(o.name, b.entry)) at = &o;
        }
        if (!at) at = tilemap_object_find(*m, "player_start");
        Table* players = ctx.write_kind("example_arcanum.player");
        if (Transform2* t = players->get<Transform2>(g.player)) {
            if (at && strcmp(b.entry, "reload") != 0) {
                *t = transform2_at(tilemap_to_world(point2{0, 0}, at->x, at->y));
                *players->prev<Transform2>(g.player) = *t; // a teleport: no sweep across the screen
            }
            players->get<Sprite>(g.player)->layer = (i32)objects_layer;
            g.exit_armed = exit_at(*m, feet_of(t->position)) == nullptr;
        }
        g.leaving = false;
        // The villagers the map names, children of the level's root.
        Table* npcs = ctx.write_kind("example_arcanum.npc");
        const MapLayer* objs = tilemap_layer_find(*m, "objects");
        for (u32 i = 0; objs && i < objs->objects.count; ++i) {
            const MapObject& o = m->objects[objs->objects.first + i];
            const MapProperty* sheet = tilemap_property(*m, o.props, "sheet");
            if (!o.point || !sheet) continue;
            u8 which = 0;
            for (u32 k = 0; k < kSheets; ++k) if (strstr(sheet->str, k_sheet_names[k])) which = (u8)k;
            Actor a{};
            a.sheet = which;
            a.home = tilemap_to_world(point2{0, 0}, o.x, o.y);
            if (const MapProperty* route = tilemap_property(*m, o.props, "route")) {
                f32 x0, y0, x1, y1;
                if (sscanf(route->str, "%f,%f %f,%f", &x0, &y0, &x1, &y1) == 4) a.route = vec2{x1 - x0, -(y1 - y0)};
            }
            Parent p;
            p.parent = b.root;
            npcs->spawn(transform2_at(a.home), local2_at(a.home), p, actor_sprite(g, which, (i32)objects_layer), a);
        }
        // The map's music joins the level's scope; it plays when the level is entered.
        g.music = Handle{};
        if (const MapProperty* music = tilemap_map_property(*m, "music")) g.music = asset_load_stream(e, music->str, b.scope);
    }
}

void on_entered(TickContext& ctx, Span<const LevelEntered>) {
    Game& g = ctx.state_as<Game>();
    MusicPlay p;
    p.fade_out = 1.5f; p.fade_in = 1.5f; p.overlap = 1.0f; p.volume = 0.7f;
    if (!g.music.is_null()) music_play(ctx, g.music, p);
}

void on_exit(TickContext& ctx, Span<const ExitRequested>) { engine_request_quit(*ctx.engine); }

// ---- the demo -----------------------------------------------------------------------------------

// The script, by its own clock (every tick but those a level load holds): the steps say
// what the actions read on the ticks they cover.
struct Step { u64 from, to; const char* action; f32 x, y; };
constinit const Step k_script[] = {
    {30, 340, "move", 1, 0},       // east along the village road, out through the exit
    {340, 420, "move", 1, 0},      // on into the field
    {420, 470, "move", 0.7f, 0.7f},
    {520, 521, "menu", 0, 0},      // the menu
    {540, 541, "move", 0, -1},     // Level
    {560, 561, "confirm", 0, 0},   // the level list: the cursor starts on the field, where we are
    {580, 581, "move", 0, 1},      // up to the village
    {600, 601, "confirm", 0, 0},
    {760, 900, "move", -1, 0},     // west along the road, back towards the lane
    {900, 980, "move", 0, 1},      // up the lane, into a house's wall
    {1000, 1001, "menu", 0, 0},    // the credits
    {1020, 1021, "move", 0, -1},
    {1040, 1041, "move", 0, -1},
    {1060, 1061, "confirm", 0, 0},
    {1080, 1081, "move", 0, -1},   // scroll
    {1120, 1121, "cancel", 0, 0},  // back to the menu
    {1140, 1141, "cancel", 0, 0},  // closed
};
constexpr u64 kReloadAt = 1200, kQuitAt = 1400;

// Phase PostSim, every tick: the next tick's actions, and the reload and the end.
void demo(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    if (!g.demo) return;
    if (engine_holds(e) & HoldLoading) return;
    u64 t = g.demo_t++;
    for (const Step& s : k_script)
        if (t >= s.from && t < s.to) input_action_override(e, s.action, true, vec2{s.x, s.y});
    if (t == kReloadAt) level_reload(e);
    if (t == kQuitAt) engine_request_quit(e);
}

// ---- startup ------------------------------------------------------------------------------------

bool arcanum_startup(Engine& e, ModuleInstance& self) {
    Game* g = arena_push<Game>(self.arena);
    if (!g) return false;
    self.set_state(g);
    if (!input_actions(e, k_actions)) return false;
    const InputActions* a = input_actions_get(e);
    g->act_move = a->find("move"); g->act_confirm = a->find("confirm"); g->act_cancel = a->find("cancel"); g->act_menu = a->find("menu");
    g->demo = frameloop_flag(e, "--demo");
    Renderer* r = renderer_get(e);
    for (u32 i = 0; i < kSheets; ++i) {
        g->sheet_texture[i] = asset_load_texture(e, k_sheet_paths[i]);
        MaterialDesc md;
        if (const TextureAsset* t = asset_texture(e, g->sheet_texture[i])) md.texture = t->texture;
        md.blend = BlendMode::Alpha;
        g->sheet_material[i] = r ? renderer_material_create(r, md) : Handle{};
    }
    if (r) {
        TextureDesc td;
        td.name = "arcanum.white";
        g->white_texture = renderer_texture_create(r, td);
        const u8 white[4] = {255, 255, 255, 255};
        renderer_texture_upload(r, g->white_texture, 1, 1, white, sizeof white);
        MaterialDesc md;
        md.texture = g->white_texture;
        g->white_material = renderer_material_create(r, md);
    }
    g->font = asset_load_font(e, "text/DejaVuSans.ttf", 26.0f);
    g->sounds[SND_MOVE]   = asset_load_sound(e, "example_arcanum/sounds/move.wav");
    g->sounds[SND_ACCEPT] = asset_load_sound(e, "example_arcanum/sounds/accept.wav");
    g->sounds[SND_CANCEL] = asset_load_sound(e, "example_arcanum/sounds/cancel.wav");
    g->credits = asset_load_blob(e, "example_arcanum/credits.json");
    Table* players = engine_table(e, "example_arcanum.player");
    Actor pa{};
    g->player = players->spawn(transform2_at(point2{0, 0}), actor_sprite(*g, 0, 2), pa);
    LevelConfig cfg;
    cfg.levels = k_levels;
    cfg.load = [](Engine& en, const char* map, Handle scope) { return tilemap_load(en, map, scope); };
    if (!level_catalog(e, cfg)) return false;
    level_request(e, "village");
    KV_INFO("arcanum", "Arrows or WASD (or a pad's stick) walk, Escape or Start opens the menu, Enter or South chooses, Backspace or East goes back.");
    return true;
}

constinit const HandlerDesc k_handlers[] = {
    {.name = ".on_build",   KV_ON(LevelBuild, on_build), .writes = ".player, .npc, tilemap.tile"},
    {.name = ".on_entered", KV_ON(LevelEntered, on_entered), .writes = "audio.commands"},
    {.name = ".on_exit",    KV_ON(ExitRequested, on_exit)},
};
constinit const SystemDesc k_systems[] = {
    {.name = ".menu", .phase = Phase::PreSim, .fn = menu, .reads = "input.actions", .writes = ".ui, .dim, audio.commands",
     .after = "level.run", .main_thread = true, .runs = RunWhen::Always},
    {.name = ".walk", .phase = Phase::Sim, .fn = walk, .reads = "input.actions, .npc", .writes = ".player", .main_thread = true},
    {.name = ".villagers", .phase = Phase::Sim, .fn = villagers, .writes = ".npc"},
    {.name = ".camera", .phase = Phase::PostSim, .fn = camera, .reads = "platform.window", .writes = "sprite.camera", .main_thread = true, .runs = RunWhen::Always},
    {.name = ".demo", .phase = Phase::PostSim, .fn = demo, .main_thread = true, .runs = RunWhen::Always},
};

} // namespace

KV_MODULE_DEFINE(example_arcanum_module) = {
    .name       = "example_arcanum",
    .components = k_components,
    .kinds      = k_kinds,
    .systems    = k_systems,
    .handlers   = k_handlers,
    .startup    = arcanum_startup,
    .memory     = {.reserve = 64 * 1024},
};

} // namespace kv
