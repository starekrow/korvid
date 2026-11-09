// example_arcanum: a JRPG skeleton. Two Tiled maps loaded through the level module, a
// player who walks in eight directions under the "move" action, collides with the map's
// solids (tilemap_move) and with the villagers, pushes rocks, loses a heart for walking
// into a villager, sorts by its feet against trees and houses, and walks through exits to
// the other map; a pause menu with save and load, a level select and the credits; music
// that crossfades between the maps.
#include "korvid/example_arcanum/module.h"
#include "korvid/asset/blob.h"
#include "korvid/asset/scope.h"
#include "korvid/asset/sound.h"
#include "korvid/asset/stream.h"
#include "korvid/asset/font.h"
#include "korvid/asset/material.h"
#include "korvid/asset/texture.h"
#include "korvid/audio/audio.h"
#include "korvid/core/engine.h"
#include "korvid/core/hash.h"
#include "korvid/core/json.h"
#include "korvid/core/log.h"
#include "korvid/core/utf8.h"
#include "korvid/frameloop/frameloop.h"
#include "korvid/level/level.h"
#include "korvid/math/fields.h"
#include "korvid/platform/io.h"
#include "korvid/platform_hid/actions.h"
#include "korvid/platform_hid/host_window.h"
#include "korvid/render/renderer.h"
#include "korvid/save/save.h"
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
// The player's health: hearts, and the seconds left of the blink after a hurt, during
// which nothing hurts again.
struct Vitals {
    i32 hp;
    i32 hp_max;
    f32 hurt;
};
constinit const FieldDesc k_actor_fields[] = {KV_FIELDS(Actor, facing, moving, sheet, route_dir, walk, home, route, route_t)};
constinit const FieldDesc k_vitals_fields[] = {KV_FIELDS(Vitals, hp, hp_max, hurt)};
constinit const ComponentDesc k_components[] = {KV_COMPONENT(Actor, k_actor_fields), KV_COMPONENT(Vitals, k_vitals_fields)};

constinit const KindDesc k_kinds[] = {
    {.name = ".player", .components = "Transform2, Sprite, Actor, Vitals", .capacity = 1},
    {.name = ".npc",    .components = "Transform2, Local2, Parent, Sprite, Actor", .capacity = 32},
    // Rocks the player pushes: the level's children, placed by the map's "rock" points.
    {.name = ".rock",   .components = "Transform2, Local2, Parent, Sprite", .capacity = 32},
    // The menu, the credits and the hearts: presentation, never hashed.
    {.name = ".ui",     .components = "Transform2, Text", .capacity = 128, .cosmetic = true},
    {.name = ".dim",    .components = "Transform2, Sprite", .capacity = 2, .cosmetic = true},
    {.name = ".hud",    .components = "Transform2, Sprite", .capacity = 16, .cosmetic = true},
    {.name = ".hudtext", .components = "Transform2, Text", .capacity = 2, .cosmetic = true},
};
// What a save holds: the player, and what lives in the level and can change.
constinit const char* const k_saved_kinds[] = {"example_arcanum.player", "example_arcanum.npc", "example_arcanum.rock"};

constexpr f32 kWalkSpeed   = 64.0f;   // px per second
constexpr f32 kNpcSpeed    = 24.0f;
constexpr f32 kFrameTime   = 0.15f;   // seconds per walk frame
constexpr f32 kFeetW = 10.0f, kFeetH = 5.0f;
constexpr f32 kRockW = 26.0f, kRockH = 12.0f; // a boulder's base, from its bottom edge
constexpr f32 kPushPace = 0.5f;              // a pushed rock moves at half the walk
constexpr i32 kHpMax = 5;
constexpr f32 kHurtTime = 1.0f;              // seconds of blinking, untouchable
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
enum MainItem : u32 { ITEM_RESUME, ITEM_SAVE, ITEM_LOAD, ITEM_LEVEL, ITEM_CREDITS, ITEM_QUIT, ITEM_COUNT };
enum Snd : u8 { SND_MOVE, SND_ACCEPT, SND_CANCEL, SND_COUNT };
constexpr color kHurtTint = color{1.0f, 0.35f, 0.35f, 1.0f};

struct Game {
    u32    act_move = 0, act_confirm = 0, act_cancel = 0, act_menu = 0;
    Handle sheet_texture[kSheets], sheet_material[kSheets];
    Handle white_texture, white_material;
    Handle nature_texture, rock_material; // a rock is a 32 px cell of the nature sheet
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
    char   note[48] = {};        // under the menu: what saving or loading did
    // Saving and loading. The save is one JSON file in the per-user directory.
    char   save_path[512] = {};
    Handle save_write;           // platform.io: the write on its way out
    Handle load_read;            // platform.io: the file on its way in, the sim held meanwhile
    u8*    load_data = nullptr;  // the file, read, kept until the level it names is built
    usize  load_size = 0;
    Handle hud_back, hud_pips[kHpMax], hud_label;
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

// A rock's base (world units, min corner) for a rock centered at `p`, and back.
rect2 rock_box(point2 p) { return rect2{p.x - kRockW * 0.5f, p.y - 16.0f, kRockW, kRockH}; }
point2 center_of_rock(const rect2& b) { return point2{b.x + kRockW * 0.5f, b.y + 16.0f}; }

Sprite rock_sprite(const Game& g, i32 layer) {
    Sprite s;
    s.material = g.rock_material;
    s.uv    = rect2{256.0f / 384.0f, 128.0f / 336.0f, 32.0f / 384.0f, 32.0f / 336.0f}; // the grey boulder
    s.size  = vec2{32, 32};
    s.layer = layer;
    s.sort  = SpriteSort::Feet;
    s.order = -16.0f;
    return s;
}

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

// The hurt: the cancel blip, lower, in the world's bus.
void play_hurt(TickContext& ctx, Game& g) {
    SoundPlay p;
    p.sound = g.sounds[SND_CANCEL];
    p.pitch = 0.6f;
    p.volume = 0.8f;
    audio_play(ctx, p);
}

// The saved kinds' state, less their Parent links (a load parents them to the level's new
// root): logged at a save and after a load, so a run shows the load gave back what was saved.
u64 saved_state_hash(Engine& e) {
    u64 h = 0;
    for (const char* name : k_saved_kinds) {
        const Table* t = engine_table(e, name);
        for (u32 i = 0; t && i < t->rows(); ++i) {
            if (!t->is_alive(i)) continue;
            h = hash_mix(h ^ (t->handle_at(i).bits & 0x000FFFFFFFFFFFFFull));
            for (const Column& c : t->columns)
                if (*c.desc->id != ComponentType<Parent>::id) h = hash_mix(h ^ component_hash(*c.desc, c.data + (usize)i * c.stride));
        }
    }
    return h;
}

// ---- the menu ------------------------------------------------------------------------------------

u32 item_count(Engine& e, const Game& g) {
    switch (g.menu) {
    case Menu::Main:   return ITEM_COUNT;
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
    t.shadow = color_rgba(0.0f, 0.0f, 0.0f, 0.85f);
    t.shadow_offset = vec2{2.0f, -2.0f};
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
        const char* const main_items[ITEM_COUNT] = {"Resume", "Save", "Load", "Level", "Credits", "Quit"};
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
        if (g.note[0]) label(*ui, g, g.note, 0, 70.0f - 44.0f * (f32)n - 16.0f, 0.75f, grey);
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
    if (g.menu == Menu::Closed) { engine_pause(*ctx.engine, true); g.note[0] = 0; }
    static const char* const names[] = {"closed", "main", "levels", "credits"};
    // Where a screen opens, for --screenshot-frame and the like.
    KV_INFO("arcanum", "menu: %s at frame %llu, tick %llu", names[(u32)m], (unsigned long long)ctx.engine->frame_count, (unsigned long long)ctx.engine->clock.tick_total);
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

// ---- saving and loading --------------------------------------------------------------------------

// The save: what level, which entity the player is, and the kinds that change. The bytes
// are copied into the write at submission; the menu says "Saved" when the disk has them.
void save_game(TickContext& ctx, Game& g) {
    Engine& e = *ctx.engine;
    if (!g.save_path[0]) { utf8_copy(g.note, sizeof g.note, "There is nowhere to save"); return; }
    if (level_state(e) != LevelState::Active || !g.save_write.is_null()) { utf8_copy(g.note, sizeof g.note, "Not now"); return; }
    SaveWriter w;
    w.init(mem_general());
    w.begin_object();
    w.key("game");       w.string_value("example_arcanum");
    w.key("version");    w.int_value(1);
    w.key("tick");       w.uint_value(e.clock.tick_total);
    w.key("level");      w.string_value(level_name(e, level_current(e)));
    w.key("player");     save_handle(w, g.player);
    w.key("exit_armed"); w.bool_value(g.exit_armed);
    w.key("kinds");      save_kinds(w, e, k_saved_kinds);
    w.end_object();
    if (w.finish()) {
        IoWriteRequest req;
        req.path = g.save_path; req.data = w.out.data; req.size = w.out.len;
        g.save_write = platform_io(e)->write_file(req);
    }
    utf8_copy(g.note, sizeof g.note, g.save_write.is_null() ? "The save could not be written" : "Saving...");
    KV_INFO("arcanum", "save: %s, %u bytes, state %016llx at tick %llu", g.save_path, w.out.len,
            (unsigned long long)saved_state_hash(e), (unsigned long long)e.clock.tick_total);
    w.free();
}

// The load: the menu closes and the sim holds while the file comes in, so how long the
// disk takes never reaches the state; then the level it names loads (a reload, if it is
// this one) and on_build restores the kinds instead of spawning the map's.
void load_game(TickContext& ctx, Game& g) {
    Engine& e = *ctx.engine;
    if (!g.save_path[0] || !g.load_read.is_null() || g.load_data || level_state(e) != LevelState::Active) { utf8_copy(g.note, sizeof g.note, "Not now"); return; }
    IoRequest req;
    req.path = g.save_path;
    g.load_read = platform_io(e)->read_file(req);
    if (g.load_read.is_null()) { utf8_copy(g.note, sizeof g.note, "The save could not be read"); return; }
    close_menu(ctx, g);
    engine_hold(e, HoldLoading, true);
}

void free_load(Game& g) {
    free_in(mem_general(), g.load_data, g.load_size ? g.load_size : 1);
    g.load_data = nullptr;
    g.load_size = 0;
}

// Phase PreSim, main thread, every tick: the save's write and read as they complete.
void files(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    IoQueue* io = platform_io(e);
    if (g.save_write.is_null() && g.load_read.is_null()) return;
    for (u32 i = 0; i < io->completions().len; ++i) {
        IoCompletion c = io->completions()[i];
        if (c.request != g.save_write && c.request != g.load_read) continue;
        io->reap(c.request);
        --i;
        if (c.request == g.save_write) {
            g.save_write = Handle{};
            if (c.status == IoStatus::Done) utf8_copy(g.note, sizeof g.note, "Saved");
            else { utf8_copy(g.note, sizeof g.note, "The save could not be written"); KV_WARN("arcanum", "save: errno %d writing %s", c.error, g.save_path); }
            g.menu_dirty = true;
            continue;
        }
        g.load_read = Handle{};
        g.load_data = (u8*)c.data;
        g.load_size = c.size;
        const char* why = nullptr;
        if (c.status != IoStatus::Done) why = c.error == 2 ? "There is no saved game" : "The save could not be read";
        else {
            MemScope scope;
            JsonDoc doc;
            const char* level = nullptr;
            if (!save_parse(SaveFormat::Json, g.load_data, g.load_size, mem_scratch(), doc)) KV_WARN("arcanum", "load: %s at byte %u", doc.error, doc.error_at);
            else if (strcmp(json_string(json_get(doc, doc.root(), "game"), ""), "example_arcanum") != 0) KV_WARN("arcanum", "load: not an arcanum save");
            else level = json_string(json_get(doc, doc.root(), "level"), "");
            LevelTransition t;
            t.fade_out = 0.3f; t.fade_in = 0.3f; t.entry = "load";
            if (!level || !level_request(e, level, t)) why = "The save does not load";
            else KV_INFO("arcanum", "load: %s, %u bytes, level %s", g.save_path, (u32)g.load_size, level);
        }
        if (why) {
            // Back to the menu, which says why; the sim was held for nothing.
            free_load(g);
            engine_hold(e, HoldLoading, false);
            open_menu(ctx, g, Menu::Main);
            g.cursor = ITEM_LOAD;
            utf8_copy(g.note, sizeof g.note, why);
        }
        // Otherwise the level's transition holds the sim from here and clears the hold when
        // it is done.
    }
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
            if (action_pressed(a, g.act_confirm) || action_pressed(a, g.act_cancel)) { open_menu(ctx, g, Menu::Main); g.cursor = ITEM_CREDITS; play_sound(ctx, g, SND_CANCEL); }
        } else {
            u32 n = item_count(e, g);
            if (step && fabsf(mv.y) > 0.5f) {
                g.cursor = mv.y < 0 ? (g.cursor + 1) % n : (g.cursor + n - 1) % n;
                g.menu_dirty = true;
                play_sound(ctx, g, SND_MOVE);
            }
            if (action_pressed(a, g.act_menu)) { close_menu(ctx, g); play_sound(ctx, g, SND_CANCEL); }
            else if (action_pressed(a, g.act_cancel)) {
                if (g.menu == Menu::Levels) { open_menu(ctx, g, Menu::Main); g.cursor = ITEM_LEVEL; }
                else close_menu(ctx, g);
                play_sound(ctx, g, SND_CANCEL);
            } else if (action_pressed(a, g.act_confirm)) {
                play_sound(ctx, g, SND_ACCEPT);
                if (g.menu == Menu::Main) {
                    switch (g.cursor) {
                    case ITEM_RESUME:  close_menu(ctx, g); break;
                    case ITEM_SAVE:    save_game(ctx, g); g.menu_dirty = true; break;
                    case ITEM_LOAD:    load_game(ctx, g); g.menu_dirty = true; break;
                    case ITEM_LEVEL:   open_menu(ctx, g, Menu::Levels); g.cursor = level_current(e) < KV_COUNTOF(k_levels) ? level_current(e) : 0; break;
                    case ITEM_CREDITS: open_menu(ctx, g, Menu::Credits); break;
                    case ITEM_QUIT:    engine_request_quit(e); break;
                    }
                } else if (g.cursor < KV_COUNTOF(k_levels)) {
                    close_menu(ctx, g);
                    LevelTransition t;
                    t.fade_out = 0.4f; t.fade_in = 0.4f;
                    level_request(e, k_levels[g.cursor].name, t);
                } else { open_menu(ctx, g, Menu::Main); g.cursor = ITEM_LEVEL; }
            }
        }
    }
    if (g.menu_dirty) { build_menu(ctx, g); g.menu_dirty = false; }
}

// ---- walking -------------------------------------------------------------------------------------

// A min-corner box at least `eps` deep into another on both axes: touching, or a hair of
// rounding, does not count.
bool sunk(const rect2& a, const rect2& b, f32 eps) {
    return a.x + eps < b.x + b.w && b.x + eps < a.x + a.w && a.y + eps < b.y + b.h && b.y + eps < a.y + a.h;
}

vec2 along(u32 axis, f32 v) { return axis ? vec2{0.0f, v} : vec2{v, 0.0f}; }

// The player at the map's start, whole again.
void faint(Game& g, Table& players, const TilemapAsset& m) {
    Vitals* v = players.get<Vitals>(g.player);
    Transform2* t = players.get<Transform2>(g.player);
    v->hp = v->hp_max;
    if (const MapObject* start = tilemap_object_find(m, "player_start")) {
        *t = transform2_at(tilemap_to_world(point2{0, 0}, start->x, start->y));
        *players.prev<Transform2>(g.player) = *t;
    }
    g.exit_armed = exit_at(m, feet_of(t->position)) == nullptr;
    KV_INFO("arcanum", "fainted; back at the start");
}

// Phase Sim: the player walks, collides, pushes rocks, is hurt by villagers, and takes exits.
void walk(TickContext& ctx) {
    static const u32 k_actions_id = ctx.resource_id("input.actions");
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    const InputActions& a = *ctx.resource<const InputActions>(k_actions_id);
    Table* players = ctx.write_kind("example_arcanum.player");
    Table* rocks = ctx.write_kind("example_arcanum.rock");
    const Table* npcs = ctx.read_kind("example_arcanum.npc");
    Transform2* t = players->get<Transform2>(g.player);
    Sprite* s = players->get<Sprite>(g.player);
    Actor* act = players->get<Actor>(g.player);
    Vitals* v = players->get<Vitals>(g.player);
    const TilemapAsset* m = tilemap_get(e, g.map);
    if (!t || !m || m->state != AssetState::Ready || level_state(e) == LevelState::Loading) return;
    vec2 dir = action_axis(a, g.act_move);
    f32 dt = (f32)ctx.dt;
    vec2 d{dir.x * kWalkSpeed * dt, dir.y * kWalkSpeed * dt};
    act->moving = (dir.x != 0.0f || dir.y != 0.0f) && !g.leaving;
    act->facing = facing_of(dir, act->facing);
    v->hurt = v->hurt > dt ? v->hurt - dt : 0.0f;

    // The villagers block as of their last placement; the rocks as they are now.
    auto npc_in = [&](const rect2& box) {
        for (u32 i = 0; i < npcs->rows(); ++i)
            if (npcs->is_alive(i) && overlaps(box, feet_of(npcs->rows_of<Transform2>()[i].position))) return true;
        return false;
    };
    Span<Local2> rock_at = rocks->rows_of<Local2>();
    // The first rock `box` runs into, skipping `skip` and any rock `from` is already inside
    // (one placed on the player by a map edit walks free rather than dragged along).
    auto rock_in = [&](const rect2& box, const rect2* from, u32 skip) -> u32 {
        for (u32 i = 0; i < rocks->rows(); ++i) {
            if (i == skip || !rocks->is_alive(i)) continue;
            rect2 rb = rock_box(rock_at[i].position);
            if (overlaps(box, rb) && !(from && sunk(*from, rb, 0.5f))) return i;
        }
        return KV_POOL_NONE;
    };

    // An axis at a time, so a blocked diagonal slides along what blocks it.
    rect2 box = feet_of(t->position);
    bool bumped = false;
    for (u32 axis = 0; axis < 2; ++axis) {
        f32 step = axis ? d.y : d.x;
        if (step == 0.0f) continue;
        rect2 want = tilemap_move(*m, point2{0, 0}, box, along(axis, step)).box;
        // Against a rock the player slows to the rock's pace, and each rock in the way is
        // pushed clear of where the player would be if the map, the other rocks and the
        // villagers leave it room. The player stops at whatever did not give.
        if (rock_in(want, &box, KV_POOL_NONE) != KV_POOL_NONE) {
            step *= kPushPace;
            want = tilemap_move(*m, point2{0, 0}, box, along(axis, step)).box;
        }
        for (u32 tries = 0; tries < 4; ++tries) {
            u32 r = rock_in(want, &box, KV_POOL_NONE);
            if (r == KV_POOL_NONE) break;
            rect2 rb = rock_box(rock_at[r].position);
            f32 lo = axis ? want.y : want.x, hi = axis ? want.y + want.h : want.x + want.w;
            f32 rlo = axis ? rb.y : rb.x, rhi = axis ? rb.y + rb.h : rb.x + rb.w;
            f32 need = step > 0.0f ? hi - rlo : lo - rhi;
            rect2 moved = tilemap_move(*m, point2{0, 0}, rb, along(axis, need)).box;
            if (rock_in(moved, nullptr, r) != KV_POOL_NONE || npc_in(moved)) moved = rb;
            if (moved.x != rb.x || moved.y != rb.y) {
                rock_at[r].position = center_of_rock(moved);
                rocks->rows_of<Transform2>()[r].position = rock_at[r].position; // the root is at the origin
            }
            if (!overlaps(want, moved)) continue;
            if (axis == 0) want.x = step > 0.0f ? fmaxf(box.x, moved.x - want.w) : fminf(box.x, moved.x + moved.w);
            else           want.y = step > 0.0f ? fmaxf(box.y, moved.y - want.h) : fminf(box.y, moved.y + moved.h);
        }
        if (npc_in(want)) { bumped = true; continue; }
        box = want;
    }
    t->position = center_of_feet(box);

    // Walking into a villager costs a heart, then a second of blinking that nothing
    // hurts through. The last heart faints the player back to the map's start.
    if (bumped && v->hurt == 0.0f && v->hp > 0) {
        --v->hp;
        v->hurt = kHurtTime;
        play_hurt(ctx, g);
        if (v->hp == 0) faint(g, *players, *m);
    }
    animate(*act, *s, dt);
    s->tint = v->hurt > 0.0f && (u32)(v->hurt * 10.0f) % 2 ? kHurtTint : color_white();

    // Exits: armed once the player has stood clear of them since arriving.
    const MapObject* ex = exit_at(*m, box);
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
    static const u32 k_window = ctx.resource_id("platform_hid.window");
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

// Phase PostSim, every tick: the hearts, top left, from the player's Vitals. Made once,
// placed every tick (the window may have changed size).
void hud(TickContext& ctx) {
    static const u32 k_window = ctx.resource_id("platform_hid.window");
    static const u32 k_vitals = ctx.column_id(".player.Vitals");
    Game& g = ctx.state_as<Game>();
    Table* hud = ctx.write_kind("example_arcanum.hud");
    Table* text = ctx.write_kind("example_arcanum.hudtext");
    Span<const Vitals> vitals = ctx.read<Vitals>(k_vitals);
    const Vitals* v = g.player.index() < vitals.len ? &vitals[g.player.index()] : nullptr;
    const HostWindow* w = ctx.resource<const HostWindow>(k_window);
    if (!v || !w || g.white_material.is_null()) return;
    constexpr f32 kPipW = 18.0f, kPipH = 12.0f, kPipStep = 24.0f, kLabelW = 40.0f, kPad = 8.0f;
    if (g.hud_back.is_null()) {
        Sprite sp;
        sp.material = g.white_material;
        sp.space = SpriteSpace::Screen;
        sp.layer = 90;
        sp.tint = color_rgba(0.0f, 0.0f, 0.04f, 0.5f);
        sp.size = vec2{kPad * 2.0f + kLabelW + kPipStep * (f32)kHpMax - (kPipStep - kPipW), 30.0f};
        g.hud_back = hud->spawn(transform2_at(point2{0, 0}), sp);
        sp.layer = 91;
        sp.size = vec2{kPipW, kPipH};
        for (Handle& h : g.hud_pips) h = hud->spawn(transform2_at(point2{0, 0}), sp);
        Text t;
        t.font  = g.font;
        utf8_copy(t.str, sizeof t.str, "HP");
        t.scale = 0.7f;
        t.tint  = color_rgba(1.0f, 0.78f, 0.25f, 1.0f);
        t.layer = 91;
        t.space = SpriteSpace::Screen;
        t.align = TextAlign::Left;
        t.shadow = color_rgba(0.0f, 0.0f, 0.0f, 0.85f);
        t.shadow_offset = vec2{1.0f, -1.0f};
        g.hud_label = text->spawn(transform2_at(point2{0, 0}), t);
    }
    // The bar's middle, 20 px in from the window's top-left corner.
    f32 left = -(f32)w->width() * 0.5f + 20.0f, mid = (f32)w->height() * 0.5f - 20.0f - 15.0f;
    const Sprite* back = hud->get<Sprite>(g.hud_back);
    hud->get<Transform2>(g.hud_back)->position = point2{left + back->size.x * 0.5f, mid};
    text->get<Transform2>(g.hud_label)->position = point2{left + kPad, mid - 5.0f}; // its baseline, so the capitals sit centered
    for (i32 i = 0; i < kHpMax; ++i) {
        hud->get<Transform2>(g.hud_pips[i])->position = point2{left + kPad + kLabelW + kPipStep * (f32)i + kPipW * 0.5f, mid};
        bool full = i < v->hp && i < v->hp_max;
        hud->get<Sprite>(g.hud_pips[i])->tint = full ? color_rgba(0.86f, 0.18f, 0.22f, 1.0f) : color_rgba(0.3f, 0.3f, 0.35f, 0.8f);
    }
}

// ---- levels -------------------------------------------------------------------------------------

// The saved kinds back, in place of the map's villagers and rocks. Parent links name the
// level's root ("level.root@level.root"), so they find the new one; materials name their
// textures; the rest comes back by kind name and slot.
void restore(TickContext& ctx, Game& g, const LevelBuild& b) {
    Engine& e = *ctx.engine;
    MemScope scope;
    JsonDoc doc;
    if (!save_parse(SaveFormat::Json, g.load_data, g.load_size, mem_scratch(), doc)) {
        KV_ERROR("arcanum", "load: %s at byte %u", doc.error, doc.error_at); // it parsed when it arrived
        return;
    }
    (void)b;
    const JsonValue* kinds = json_get(doc, doc.root(), "kinds");
    u32 n = kinds ? load_kinds(doc, *kinds, e) : 0;
    Handle player = load_handle(json_get(doc, doc.root(), "player"));
    if (!player.is_null()) g.player = player;
    g.exit_armed = json_bool(json_get(doc, doc.root(), "exit_armed"), true);
    KV_INFO("arcanum", "load: %u kinds, state %016llx, saved at tick %llu", n, (unsigned long long)saved_state_hash(e),
            (unsigned long long)json_u64(json_get(doc, doc.root(), "tick"), 0));
}


// A level as its map has it: the player at its entry, and the villagers and the rocks
// the `objects` layer names, children of the level's root.
void spawn_level(TickContext& ctx, Game& g, const LevelBuild& b, const TilemapAsset& m, u32 objects_layer) {
    // The player, at the entry it came by or the map's start; where it stood, for a reload.
    const MapLayer* spawns = tilemap_layer_find(m, "spawns");
    const MapObject* at = nullptr;
    for (u32 i = 0; spawns && i < spawns->objects.count; ++i) {
        const MapObject& o = m.objects[spawns->objects.first + i];
        if (b.entry[0] && !strcmp(o.name, b.entry)) at = &o;
    }
    if (!at) at = tilemap_object_find(m, "player_start");
    Table* players = ctx.write_kind("example_arcanum.player");
    if (Transform2* t = players->get<Transform2>(g.player)) {
        if (at && strcmp(b.entry, "reload") != 0) {
            *t = transform2_at(tilemap_to_world(point2{0, 0}, at->x, at->y));
            *players->prev<Transform2>(g.player) = *t; // a teleport: no sweep across the screen
        }
        players->get<Sprite>(g.player)->layer = (i32)objects_layer;
        g.exit_armed = exit_at(m, feet_of(t->position)) == nullptr;
    }
    Table* npcs = ctx.write_kind("example_arcanum.npc");
    Table* rocks = ctx.write_kind("example_arcanum.rock");
    const MapLayer* objs = tilemap_layer_find(m, "objects");
    for (u32 i = 0; objs && i < objs->objects.count; ++i) {
        const MapObject& o = m.objects[objs->objects.first + i];
        Parent p;
        p.parent = b.root;
        char name[64];
        tilemap_object_name(m, o, name, sizeof name);
        if (o.point && !strcmp(o.name, "rock")) {
            point2 at_rock = tilemap_to_world(point2{0, 0}, o.x, o.y);
            engine_entity_name_set(*ctx.engine, rocks->spawn(transform2_at(at_rock), local2_at(at_rock), p, rock_sprite(g, (i32)objects_layer)), name);
            continue;
        }
        const MapProperty* sheet = tilemap_property(m, o.props, "sheet");
        if (!o.point || !sheet) continue;
        u8 which = 0;
        for (u32 k = 0; k < kSheets; ++k) if (strstr(sheet->str, k_sheet_names[k])) which = (u8)k;
        Actor a{};
        a.sheet = which;
        a.home = tilemap_to_world(point2{0, 0}, o.x, o.y);
        if (const MapProperty* route = tilemap_property(m, o.props, "route")) {
            f32 x0, y0, x1, y1;
            if (sscanf(route->str, "%f,%f %f,%f", &x0, &y0, &x1, &y1) == 4) a.route = vec2{x1 - x0, -(y1 - y0)};
        }
        engine_entity_name_set(*ctx.engine, npcs->spawn(transform2_at(a.home), local2_at(a.home), p, actor_sprite(g, which, (i32)objects_layer), a), name);
    }
}

// The level is loaded: its tiles, and then either what a load brought or what the map names.
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
        g.leaving = false;
        if (g.load_data) {
            restore(ctx, g, b);
            free_load(g);
        } else spawn_level(ctx, g, b, *m, objects_layer);
        // The map's music joins the level's scope and crossfades in with the picture: the
        // old track goes down while the new one comes up, over the screen's fade-in.
        g.music = Handle{};
        if (const MapProperty* music = tilemap_map_property(*m, "music")) g.music = asset_load_stream(e, music->str, b.scope);
        MusicPlay p;
        p.fade_out = 0.5f; p.fade_in = 0.5f; p.overlap = 0.5f; p.volume = 0.7f;
        if (!g.music.is_null()) music_play(ctx, g.music, p);
    }
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
    {540, 541, "move", 0, -1},     // down to Level
    {550, 551, "move", 0, -1},
    {560, 561, "move", 0, -1},
    {580, 581, "confirm", 0, 0},   // the level list: the cursor starts on the field, where we are
    {600, 601, "move", 0, 1},      // up to the village
    {620, 621, "confirm", 0, 0},
    {780, 880, "move", 0, 1},      // up the lane from the start, pushing its rock
    {880, 1010, "move", 0, -1},    // back down across the road
    {1010, 1160, "move", -1, 0},   // west into the villager pacing there: hearts lost
    {1180, 1181, "menu", 0, 0},    // save
    {1200, 1201, "move", 0, -1},
    {1220, 1221, "confirm", 0, 0},
    {1240, 1241, "menu", 0, 0},
    {1250, 1254, "move", 0, 1},    // onto the villager's line
    {1260, 1440, "move", 1, 0},    // east after the villager, into it again and again
    {1460, 1461, "menu", 0, 0},    // load: back to where the save was
    {1480, 1481, "move", 0, -1},
    {1490, 1491, "move", 0, -1},
    {1500, 1501, "confirm", 0, 0},
    {1560, 1561, "menu", 0, 0},    // the credits
    {1580, 1581, "move", 0, 1},    // up past Resume to Quit, Credits
    {1590, 1591, "move", 0, 1},
    {1610, 1611, "confirm", 0, 0},
    {1630, 1631, "move", 0, -1},   // scroll
    {1670, 1671, "cancel", 0, 0},  // back to the menu
    {1690, 1691, "cancel", 0, 0},  // closed
};
constexpr u64 kReloadAt = 1750, kQuitAt = 1950;

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
    // Materials named for their textures ("<path>:alpha"), so a save names the look it
    // means whatever order they were made in.
    auto material_of = [&](Handle texture) {
        const MaterialAsset* m = asset_material(e, asset_material_for_texture(e, texture, BlendMode::Alpha, TextureFilter::Nearest, asset_scope_global(e)));
        return m ? m->material : Handle{};
    };
    for (u32 i = 0; i < kSheets; ++i) {
        g->sheet_texture[i] = asset_load_texture(e, k_sheet_paths[i]);
        g->sheet_material[i] = material_of(g->sheet_texture[i]);
    }
    // The rocks' sheet: the map loads it too, into the level's scope; this reference keeps
    // it for the material, which lives as long as the game.
    g->nature_texture = asset_load_texture(e, "example_arcanum/tiles/TilesetNature.png");
    g->rock_material = material_of(g->nature_texture);
    if (r) {
        TextureDesc td;
        td.name = "example_arcanum.white";
        g->white_texture = renderer_texture_create(r, td);
        const u8 white[4] = {255, 255, 255, 255};
        renderer_texture_upload(r, g->white_texture, 1, 1, white, sizeof white);
        MaterialDesc md;
        md.name = "example_arcanum.white";
        md.texture = g->white_texture;
        g->white_material = renderer_material_create(r, md);
    }
    g->font = asset_load_font(e, "text/DejaVuSans.ttf", 26.0f);
    g->sounds[SND_MOVE]   = asset_load_sound(e, "example_arcanum/sounds/move.wav");
    g->sounds[SND_ACCEPT] = asset_load_sound(e, "example_arcanum/sounds/accept.wav");
    g->sounds[SND_CANCEL] = asset_load_sound(e, "example_arcanum/sounds/cancel.wav");
    g->credits = asset_load_blob(e, "example_arcanum/credits.json");
    // The save, in the per-user directory; the demo keeps its own so a scripted run never
    // overwrites a player's game.
    const char* pref = platform_pref_path(e, "korvid", "arcanum");
    if (pref[0]) snprintf(g->save_path, sizeof g->save_path, "%ssave%s.json", pref, g->demo ? "-demo" : "");
    Table* players = engine_table(e, "example_arcanum.player");
    Actor pa{};
    g->player = players->spawn(transform2_at(point2{0, 0}), actor_sprite(*g, 0, 2), pa, Vitals{kHpMax, kHpMax, 0.0f});
    engine_entity_name_set(e, g->player, "player");
    LevelConfig cfg;
    cfg.levels = k_levels;
    cfg.load = [](Engine& en, const char* map, Handle scope) { return tilemap_load(en, map, scope); };
    if (!level_catalog(e, cfg)) return false;
    level_request(e, "village");
    KV_INFO("arcanum", "Arrows or WASD (or a pad's stick) walk, Escape or Start opens the menu, Enter or South chooses, Backspace or East goes back.");
    return true;
}

void arcanum_shutdown(Engine&, ModuleInstance& self) {
    Game* g = (Game*)self.state;
    if (g) free_load(*g); // a load the quit overtook
}

constinit const HandlerDesc k_handlers[] = {
    {.name = ".on_build",   KV_ON(LevelBuild, on_build), .writes = ".player, .npc, .rock, tilemap.tile, audio.commands"},
    {.name = ".on_exit",    KV_ON(ExitRequested, on_exit)},
};
constinit const SystemDesc k_systems[] = {
    {.name = ".files", .phase = Phase::PreSim, .fn = files, .after = "level.run", .before = ".menu", .main_thread = true, .runs = RunWhen::Always},
    {.name = ".menu", .phase = Phase::PreSim, .fn = menu, .reads = "input.actions, .player, .npc, .rock", .writes = ".ui, .dim, audio.commands",
     .after = "level.run", .main_thread = true, .runs = RunWhen::Always},
    {.name = ".walk", .phase = Phase::Sim, .fn = walk, .reads = "input.actions, .npc", .writes = ".player, .rock, audio.commands", .main_thread = true},
    {.name = ".villagers", .phase = Phase::Sim, .fn = villagers, .writes = ".npc"},
    {.name = ".camera", .phase = Phase::PostSim, .fn = camera, .reads = "platform_hid.window", .writes = "sprite.camera", .main_thread = true, .runs = RunWhen::Always},
    {.name = ".hud", .phase = Phase::PostSim, .fn = hud, .reads = ".player.Vitals, platform_hid.window", .writes = ".hud, .hudtext",
     .before = "text.layout, scene.compose", .main_thread = true, .runs = RunWhen::Always},
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
    .shutdown   = arcanum_shutdown,
    .memory     = {.reserve = 64 * 1024},
};

} // namespace kv
