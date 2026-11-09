// example_arcanum: a JRPG skeleton. Two Tiled maps loaded through the level module, a
// player who walks in eight directions under the "move" action, collides with the map's
// solids (tilemap_move) and with the villagers, pushes rocks, loses a heart for walking
// into a villager, sorts by its feet against trees and houses, and walks through exits to
// the other map; a pause menu with save and load, a level select and the credits; music
// that crossfades between the maps.
#include "korvid/example_arcanum/module.h"
#include "korvid/anim/player.h"
#include "korvid/anim/sheet.h"
#include "korvid/asset/blob.h"
#include "korvid/asset/scope.h"
#include "korvid/asset/sound.h"
#include "korvid/asset/stream.h"
#include "korvid/asset/font.h"
#include "korvid/asset/material.h"
#include "korvid/asset/texture.h"
#include "korvid/audio/audio.h"
#include "korvid/collision/collider.h"
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
#include "korvid/ui/ui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace kv {

namespace {

// ---- data --------------------------------------------------------------------------------------

// A character: which way it faces, whether it is walking, and for a villager a route
// between two points (relative to where it stands). Its sheet's clips animate it.
struct Actor {
    u8     facing;   // 0 down, 1 up, 2 left, 3 right: the sheet's columns
    u8     moving;
    u8     sheet;    // index into Game::sheets
    u8     route_dir;
    point2 home;     // a villager's spawn point
    vec2   route;    // a villager's second point, from home; zero stands still
};
// The player's health: hearts, and the seconds left of the blink after a hurt, during
// which nothing hurts again.
struct Vitals {
    i32 hp;
    i32 hp_max;
    f32 hurt;
};
constinit const FieldDesc k_actor_fields[] = {KV_FIELDS(Actor, facing, moving, sheet, route_dir, home, route)};
// What one villager wants of the player: the old man's errand. An attachment, since one
// villager of many has it; what every villager has (its lines) is in its bag, from the map.
struct Quest {
    u32 rocks_wanted; // broken rocks that would please him
    u32 done;         // 1 once the player has done it
};
constinit const FieldDesc k_vitals_fields[] = {KV_FIELDS(Vitals, hp, hp_max, hurt)};
constinit const FieldDesc k_quest_fields[] = {KV_FIELDS(Quest, rocks_wanted, done)};
constinit const ComponentDesc k_components[] = {KV_COMPONENT(Actor, k_actor_fields), KV_COMPONENT(Vitals, k_vitals_fields), KV_COMPONENT(Quest, k_quest_fields)};

constinit const KindDesc k_kinds[] = {
    // Bodies: the player and the villagers walk by intent (Move2), the rocks are pushed.
    {.name = ".player", .components = "Transform2, Sprite, Actor, Vitals, AnimPlayer, Collider2, Move2, MoveResult2", .capacity = 1},
    {.name = ".npc",    .components = "Transform2, Local2, Parent, Sprite, Actor, AnimPlayer, Collider2, Move2, MoveResult2", .capacity = 32,
     .attachable = "Quest", .props = true},
    // Rocks the player pushes: the level's children, placed by the map's "rock" points.
    {.name = ".rock",   .components = "Transform2, Local2, Parent, Sprite, Collider2, Move2", .capacity = 32},
};
// What a save holds: the player, and what lives in the level and can change.
constinit const char* const k_saved_kinds[] = {"example_arcanum.player", "example_arcanum.npc", "example_arcanum.rock"};

constexpr f32 kWalkSpeed   = 64.0f;   // px per second
constexpr f32 kNpcSpeed    = 24.0f;
constexpr f32 kFeetW = 10.0f, kFeetH = 5.0f;
constexpr f32 kRockW = 26.0f, kRockH = 12.0f; // a boulder's base, from its bottom edge
constexpr f32 kPushPace = 0.5f;              // a pushed rock moves at half the walk
constexpr u32 L_ACTOR = 1;                   // everything solid here is on one layer
constexpr i32 kHpMax = 5;
constexpr f32 kHurtTime = 1.0f;              // seconds of blinking, untouchable
constexpr u32 kSheets = 3;
const char* const k_sheet_paths[kSheets] = {"example_arcanum/actors/NinjaBlue.png", "example_arcanum/actors/OldMan.png", "example_arcanum/actors/ManGreen.png"};
const char* const k_sheet_names[kSheets] = {"NinjaBlue", "OldMan", "ManGreen"};
const char* const k_sheet_files[kSheets] = {"example_arcanum/actors/NinjaBlue.sheet.json", "example_arcanum/actors/OldMan.sheet.json", "example_arcanum/actors/ManGreen.sheet.json"};
const char* const k_facings[4] = {"down", "up", "left", "right"};

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
constexpr color kHurtTint = color{1.0f, 0.35f, 0.35f, 1.0f};

struct Game {
    u32    act_move = 0, act_confirm = 0, act_cancel = 0, act_menu = 0;
    Handle sheet_texture[kSheets], sheet_material[kSheets];
    Handle sheets[kSheets];      // anim.sheet: the walk and idle clips, loaded into each level's scope
    Handle walk_clip[kSheets][4], idle_clip[kSheets][4]; // by facing, once the sheet is Ready
    Handle nature_texture, rock_material; // a rock is a 32 px cell of the nature sheet
    Handle ui;                   // ui.doc: arcanum.ui.json, every screen the game shows
    Handle hud;                  // ui.screen: the hearts, always up
    Handle screen;               // ui.screen: the menu screen up now, if any
    Handle sounds[SND_COUNT];
    Handle credits;              // the credits.json blob
    Handle player;
    Handle map;                  // the current level's map
    Handle music;                // the current level's track (asset.stream)
    bool   exit_armed = false;   // the player has stood outside every exit since arriving
    bool   leaving = false;      // an exit has been taken; the level is on its way
    bool   fainting = false;     // the last heart went: walk takes the player to the start
    Menu   menu = Menu::Closed;
    // Saving and loading. The save is one JSON file in the per-user directory.
    char   save_path[512] = {};
    Handle save_write;           // platform.io: the write on its way out
    Handle load_read;            // platform.io: the file on its way in, the sim held meanwhile
    u8*    load_data = nullptr;  // the file, read, kept until the level it names is built
    usize  load_size = 0;
    bool   demo = false;
    u64    demo_t = 0;           // ticks of the script, not counting the ones a level load holds
};

// ---- sprites ---------------------------------------------------------------------------------------

Sprite actor_sprite(const Game& g, u8 sheet, i32 layer) {
    Sprite s;
    s.material = g.sheet_material[sheet];
    s.uv    = rect2{0.0f, 0.0f, 0.25f, 1.0f / 7.0f}; // facing down, standing, until the idle clip plays
    s.size  = vec2{16, 16};
    s.layer = layer;
    s.sort  = SpriteSort::Feet;
    s.order = -8.0f; // the feet are the sprite's bottom edge
    return s;
}

// The feet box (world units, min corner) of a sprite centered at `p`.
rect2 feet_of(point2 p) { return rect2{p.x - kFeetW * 0.5f, p.y - 8.0f, kFeetW, kFeetH}; }

bool overlaps(const rect2& a, const rect2& b) { return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h; }

// The bodies: a character by its feet, a rock by its base. The player pushes rocks.
Collider2 feet_body(u8 flags) {
    return collider_body(BodyKind::Kinematic, vec2{kFeetW * 0.5f, kFeetH * 0.5f}, vec2{0.0f, -8.0f + kFeetH * 0.5f}, L_ACTOR, L_ACTOR, flags);
}
Collider2 rock_body() {
    Collider2 c = collider_body(BodyKind::Solid, vec2{kRockW * 0.5f, kRockH * 0.5f}, vec2{0.0f, -16.0f + kRockH * 0.5f}, L_ACTOR, L_ACTOR, ColliderPushable);
    c.pace = kPushPace;
    return c;
}

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

// The sheet's walk or idle clip for how the actor moves and faces, started when it
// changes; anim.play does the frames.
void animate(Engine& e, Game& g, const Actor& a, AnimPlayer& p) {
    Handle (&set)[kSheets][4] = a.moving ? g.walk_clip : g.idle_clip;
    Handle& clip = set[a.sheet % kSheets][a.facing & 3];
    if (clip.is_null()) clip = anim_sheet_variant(e, g.sheets[a.sheet % kSheets], a.moving ? "walk" : "idle", k_facings[a.facing & 3]);
    if (p.clip != clip) anim_play(p, clip);
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
// root): logged at a save and after a load, so a run shows the load gave back what was
// saved. A handle counts by its name where it has one (a clip of a sheet the load made
// again is a new handle with the old name).
u64 row_hash(const ComponentDesc& c, const u8* row) {
    u64 h = 0;
    for (const FieldDesc& f : c.fields) {
        const u8* p = row + f.offset;
        for (u32 i = 0; i < f.count; ++i) {
            if (f.type != FieldType::Handle) { h = hash_mix(h ^ hash_bytes(p, (usize)f.count * field_type_size(f.type))); break; }
            Handle hd;
            memcpy(&hd, p + i * sizeof(Handle), sizeof hd);
            const char* name = handle_name(hd);
            h = hash_mix(h ^ (name ? hash_str(name) : hd.bits));
        }
    }
    return h;
}

u64 saved_state_hash(Engine& e) {
    u64 h = 0;
    for (const char* name : k_saved_kinds) {
        const Table* t = engine_table(e, name);
        for (u32 i = 0; t && i < t->rows(); ++i) {
            if (!t->is_alive(i)) continue;
            h = hash_mix(h ^ (t->handle_at(i).bits & 0x000FFFFFFFFFFFFFull));
            for (const Column& c : t->columns)
                if (*c.desc->id != ComponentType<Parent>::id) h = hash_mix(h ^ row_hash(*c.desc, c.data + (usize)i * c.stride));
        }
    }
    return h;
}

// ---- the menu ------------------------------------------------------------------------------------
// Screens of the UI document; what they do comes back as UiEvents (on_ui).

// Under the menu: what saving or loading did.
void note(Engine& e, const char* text) { ui_model_set_str(e, "note", text); }

const char* k_menu_names[] = {"closed", "main", "levels", "credits"};

// The credits as one rich text: the headings gold, a blank line between sections.
void fill_credits(Engine& e, Game& g) {
    const BlobAsset* b = asset_blob(e, g.credits);
    if (!b || b->state != AssetState::Ready) return;
    MemScope scope;
    JsonDoc doc;
    if (!json_parse((const char*)b->data, b->size, mem_scratch(), doc)) return;
    Array<char> text = array_in<char>(mem_scratch());
    auto add = [&](const char* s) { u32 n = (u32)strlen(s); memcpy(text.push_n(n), s, n); };
    add("{c=gold}"); add(json_string(json_get(doc, doc.root(), "title"), "Credits")); add("{/c}\n\n");
    const JsonValue* sections = json_get(doc, doc.root(), "sections");
    for (const JsonValue* sec = sections ? json_first(doc, *sections) : nullptr; sec; sec = json_next(doc, *sec)) {
        add("{c=gold}"); add(json_string(json_get(doc, *sec, "heading"), "")); add("{/c}\n");
        const JsonValue* lines = json_get(doc, *sec, "lines");
        for (const JsonValue* l = lines ? json_first(doc, *lines) : nullptr; l; l = json_next(doc, *l)) { add(json_string(l, "")); add("\n"); }
        add("\n");
    }
    text.push(0);
    ui_set_text(e, g.screen, "text", text.data);
}

// Shows a menu screen (or none), replacing the one up; the first holds the sim.
void open_menu(Engine& e, Game& g, Menu m, const char* focus = nullptr) {
    if (!g.screen.is_null()) ui_close(e, g.screen);
    g.screen = Handle{};
    if (g.menu == Menu::Closed && m != Menu::Closed) note(e, "");
    g.menu = m;
    // Where a screen opens, for --screenshot-frame and the like.
    KV_INFO("arcanum", "menu: %s at frame %llu, tick %llu", k_menu_names[(u32)m], (unsigned long long)e.frame_count, (unsigned long long)e.clock.tick_total);
    if (m == Menu::Closed) return;
    UiOpen o;
    o.layer = 100;
    o.hold = true;
    static const char* const k_screens[] = {"", "pause", "levels", "credits"};
    g.screen = ui_open(e, g.ui, k_screens[(u32)m], o);
    if (m == Menu::Credits) fill_credits(e, g);
    if (focus) ui_focus(e, g.screen, "items", focus);
}

void close_menu(Engine& e, Game& g) { open_menu(e, g, Menu::Closed); }

// ---- saving and loading --------------------------------------------------------------------------

// The save: what level, which entity the player is, and the kinds that change. The bytes
// are copied into the write at submission; the menu says "Saved" when the disk has them.
void save_game(TickContext& ctx, Game& g) {
    Engine& e = *ctx.engine;
    if (!g.save_path[0]) { note(e, "There is nowhere to save"); return; }
    if (level_state(e) != LevelState::Active || !g.save_write.is_null()) { note(e, "Not now"); return; }
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
    note(e, g.save_write.is_null() ? "The save could not be written" : "Saving...");
    KV_INFO("arcanum", "save: %s, %u bytes, state %016llx at tick %llu", g.save_path, w.out.len,
            (unsigned long long)saved_state_hash(e), (unsigned long long)e.clock.tick_total);
    w.free();
}

// The load: the menu closes and the sim holds while the file comes in, so how long the
// disk takes never reaches the state; then the level it names loads (a reload, if it is
// this one) and on_build restores the kinds instead of spawning the map's.
void load_game(TickContext& ctx, Game& g) {
    Engine& e = *ctx.engine;
    if (!g.save_path[0] || !g.load_read.is_null() || g.load_data || level_state(e) != LevelState::Active) { note(e, "Not now"); return; }
    IoRequest req;
    req.path = g.save_path;
    g.load_read = platform_io(e)->read_file(req);
    if (g.load_read.is_null()) { note(e, "The save could not be read"); return; }
    close_menu(e, g);
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
            if (c.status == IoStatus::Done) note(e, "Saved");
            else { note(e, "The save could not be written"); KV_WARN("arcanum", "save: errno %d writing %s", c.error, g.save_path); }
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
            open_menu(e, g, Menu::Main, "load");
            note(e, why);
        }
        // Otherwise the level's transition holds the sim from here and clears the hold when
        // it is done.
    }
}

// Phase PreSim, every tick (paused ones are when it matters): the menu action opens the
// pause menu, or closes whatever menu is up. Everything else is the UI's, and on_ui's.
void menu(TickContext& ctx) {
    static const u32 k_actions_id = ctx.resource_id("input.actions");
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    const InputActions& a = *ctx.resource<const InputActions>(k_actions_id);
    bool held_by_load = (engine_holds(e) & HoldLoading) != 0;
    if (!action_pressed(a, g.act_menu)) return;
    if (g.menu == Menu::Closed) { if (!held_by_load) { open_menu(e, g, Menu::Main); play_sound(ctx, g, SND_ACCEPT); } }
    else { close_menu(e, g); play_sound(ctx, g, SND_CANCEL); }
}

// What the menu screens say: an item chosen, a cancel.
void on_ui(TickContext& ctx, Span<const UiEvent> events) {
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    static const u64 k_select = name_intern("select").hash, k_cancel = name_intern("cancel").hash, k_confirm = name_intern("confirm").hash;
    for (const UiEvent& ev : events) {
        if (ev.screen != g.screen) continue;
        const char* item = name_str(Name{ev.node});
        if (g.menu == Menu::Main) {
            if (ev.event == k_cancel) { close_menu(e, g); continue; }
            if (ev.event != k_select) continue;
            if (!strcmp(item, "resume")) close_menu(e, g);
            else if (!strcmp(item, "save")) save_game(ctx, g);
            else if (!strcmp(item, "load")) load_game(ctx, g);
            else if (!strcmp(item, "level")) open_menu(e, g, Menu::Levels, level_current(e) < KV_COUNTOF(k_levels) ? k_levels[level_current(e)].name : nullptr);
            else if (!strcmp(item, "credits")) open_menu(e, g, Menu::Credits);
            else if (!strcmp(item, "quit")) engine_request_quit(e);
        } else if (g.menu == Menu::Levels) {
            if (ev.event == k_select && level_find(e, item) < KV_LEVELS) {
                close_menu(e, g);
                LevelTransition t;
                t.fade_out = 0.4f; t.fade_in = 0.4f;
                level_request(e, item, t);
            } else open_menu(e, g, Menu::Main, "level"); // back, cancel
        } else if (g.menu == Menu::Credits && (ev.event == k_cancel || ev.event == k_confirm)) {
            open_menu(e, g, Menu::Main, "credits");
        }
    }
}

// ---- walking -------------------------------------------------------------------------------------
// Intent, resolve, react: walk and villagers write Move2, collision.resolve moves everyone
// against the map and each other (the player pushing rocks), react reads the results.

// The player at the map's start, whole again. Called by walk, before the resolve, the tick
// after the heart was lost: a teleport writes the position the resolve reads.
void faint(Game& g, Span<Transform2> xf, Span<Transform2> prev, const TilemapAsset& m) {
    u32 i = g.player.index();
    if (const MapObject* start = tilemap_object_find(m, "player_start")) {
        xf[i] = transform2_at(tilemap_to_world(point2{0, 0}, start->x, start->y));
        prev[i] = xf[i];
    }
    g.exit_armed = exit_at(m, feet_of(xf[i].position)) == nullptr;
    g.fainting = false;
    KV_INFO("arcanum", "fainted; back at the start");
}

// Phase Sim: the player's intent, from the move action.
void walk(TickContext& ctx) {
    static const u32 k_actions_id = ctx.resource_id("input.actions");
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    const InputActions& a = *ctx.resource<const InputActions>(k_actions_id);
    Span<Move2> move = ctx.write<Move2>("example_arcanum.player.Move2");
    Span<Actor> actor = ctx.write<Actor>("example_arcanum.player.Actor");
    u32 i = g.player.index();
    if (i >= move.len || level_state(e) == LevelState::Loading) return;
    if (g.fainting) {
        static const u32 k_players = ctx.kind_id(".player");
        const TilemapAsset* m = tilemap_get(e, g.map);
        Table* players = engine_table(e, k_players); // its Transform2, declared below
        if (m && m->state == AssetState::Ready) faint(g, ctx.write<Transform2>("example_arcanum.player.Transform2"), Span<Transform2>(players->prev<Transform2>(g.player) - i, move.len), *m);
    }
    vec2 dir = action_axis(a, g.act_move);
    f32 dt = (f32)ctx.dt;
    bool going = (dir.x != 0.0f || dir.y != 0.0f) && !g.leaving;
    move[i].delta = going ? vec2{dir.x * kWalkSpeed * dt, dir.y * kWalkSpeed * dt} : vec2{0, 0};
    actor[i].moving = going;
    actor[i].facing = facing_of(dir, actor[i].facing);
}

// Phase Sim, after the resolve: what the move did. A villager that stopped the player, or
// walked into it, costs a heart; then the walk cycle, the blink, and the exits.
void react(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    // The player's columns this system writes, through its table (they are declared one by
    // one: the whole kind would include the intent the resolve reads, a cycle).
    Table* players = (Table*)ctx.read_kind("example_arcanum.player");
    ctx.write<Vitals>("example_arcanum.player.Vitals"); ctx.write<Sprite>("example_arcanum.player.Sprite"); ctx.write<AnimPlayer>("example_arcanum.player.AnimPlayer");
    Span<const MoveResult2> npc_moves = ctx.read<MoveResult2>("example_arcanum.npc.MoveResult2");
    const TilemapAsset* m = tilemap_get(e, g.map);
    Transform2* t = players->get<Transform2>(g.player);
    if (!t || !m || m->state != AssetState::Ready || level_state(e) == LevelState::Loading) return;
    static const u32 k_npc_kind = ctx.kind_id(".npc");
    const u16 npc_handles = engine_table(e, k_npc_kind)->kind;
    const MoveResult2& mine = *players->get<MoveResult2>(g.player);
    bool bumped = !mine.blocker.is_null() && mine.blocker.kind() == npc_handles;
    for (u32 i = 0; i < npc_moves.len && !bumped; ++i) bumped = npc_moves[i].blocker == g.player;
    Vitals* v = players->get<Vitals>(g.player);
    f32 dt = (f32)ctx.dt;
    v->hurt = v->hurt > dt ? v->hurt - dt : 0.0f;
    // Walking into a villager costs a heart, then a second of blinking that nothing
    // hurts through. The last heart faints the player back to the map's start.
    if (bumped && v->hurt == 0.0f && v->hp > 0) {
        --v->hp;
        v->hurt = kHurtTime;
        play_hurt(ctx, g);
        KV_INFO("arcanum", "hurt: %d of %d hearts at tick %llu", v->hp, v->hp_max, (unsigned long long)ctx.tick);
        if (v->hp == 0) { v->hp = v->hp_max; g.fainting = true; } // walk takes the player back next tick
    }
    animate(e, g, *players->get<Actor>(g.player), *players->get<AnimPlayer>(g.player));
    Sprite* s = players->get<Sprite>(g.player);
    s->tint = v->hurt > 0.0f && (u32)(v->hurt * 10.0f) % 2 ? kHurtTint : color_white();

    // Exits: armed once the player has stood clear of them since arriving.
    const MapObject* ex = exit_at(*m, feet_of(t->position));
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

// Phase Sim: villagers walk toward the end of their route they are headed for, turning at
// each end, by intent: one that the player stands in the way of waits.
void villagers(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    Span<const Local2> local = ctx.read<Local2>("example_arcanum.npc.Local2");
    Span<Move2> move = ctx.write<Move2>("example_arcanum.npc.Move2");
    Span<Actor> actor = ctx.write<Actor>("example_arcanum.npc.Actor");
    Span<AnimPlayer> player = ctx.write<AnimPlayer>("example_arcanum.npc.AnimPlayer");
    static const u32 k_npc = ctx.kind_id(".npc");
    const Table* table = engine_table(*ctx.engine, k_npc);
    f32 step = kNpcSpeed * (f32)ctx.dt;
    for (u32 i = 0; i < move.len; ++i) {
        if (!table->is_alive(i)) continue;
        Actor& a = actor[i];
        Move2& mv = move[i];
        point2 at = local[i].position; // the level's root is at the origin
        f32 len = sqrtf(a.route.x * a.route.x + a.route.y * a.route.y);
        mv.delta = vec2{0, 0};
        a.moving = 0;
        if (len > 0.0f) {
            point2 goal = a.route_dir ? a.home : point2{a.home.x + a.route.x, a.home.y + a.route.y};
            vec2 to{goal.x - at.x, goal.y - at.y};
            f32 dist = sqrtf(to.x * to.x + to.y * to.y);
            if (dist <= step) { mv.delta = to; a.route_dir ^= 1; }
            else mv.delta = vec2{to.x / dist * step, to.y / dist * step};
            a.moving = 1;
            a.facing = facing_of(mv.delta, a.facing);
        }
        animate(*ctx.engine, g, a, player[i]);
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

// Phase PostSim, every tick: the hearts, from the player's Vitals, as model values the HUD
// screen shows.
void hud(TickContext& ctx) {
    static const u32 k_vitals = ctx.column_id(".player.Vitals");
    Game& g = ctx.state_as<Game>();
    Span<const Vitals> vitals = ctx.read<Vitals>(k_vitals);
    const Vitals* v = g.player.index() < vitals.len ? &vitals[g.player.index()] : nullptr;
    if (!v) return;
    ui_model_set_int(*ctx.engine, "hp", v->hp);
    ui_model_set_int(*ctx.engine, "hp_max", v->hp_max);
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
            engine_entity_name_set(*ctx.engine, rocks->spawn(transform2_at(at_rock), local2_at(at_rock), p, rock_sprite(g, (i32)objects_layer), rock_body(), Move2{}), name);
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
        Handle npc = npcs->spawn(transform2_at(a.home), local2_at(a.home), p, actor_sprite(g, which, (i32)objects_layer), a, AnimPlayer{}, feet_body(0), Move2{}, MoveResult2{});
        engine_entity_name_set(*ctx.engine, npc, name);
        tilemap_spawn_props(ctx, npc, m, o); // the map's words for it: its lines, its name to show
        if (const MapProperty* q = tilemap_property(m, o.props, "quest_rocks")) npcs->attach(npc, Quest{(u32)q->num, 0});
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
        tilemap_collide(e, b.map, point2{0, 0}); // the map's solids stop every body
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

// A level's scope holds the characters' sheets too, so the level is built (and the sim
// runs) only once their clips are made: a clip that turned up on an I/O-dependent tick
// would start the walk cycles on one, and the hash would follow the disk.
void on_loading(TickContext& ctx, Span<const LevelLoading> loads) {
    Game& g = ctx.state_as<Game>();
    for (const LevelLoading& l : loads)
        for (u32 i = 0; i < kSheets; ++i) g.sheets[i] = anim_sheet_load(*ctx.engine, k_sheet_files[i], l.scope);
    // A reload makes the sheets again: their clips are new handles.
    memset(g.walk_clip, 0, sizeof g.walk_clip);
    memset(g.idle_clip, 0, sizeof g.idle_clip);
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

    g->ui = ui_load(e, "example_arcanum/ui/arcanum.ui.json", asset_scope_global(e));
    UiOpen hud_open;
    hud_open.layer = 90;
    hud_open.input = false;
    g->hud = ui_open(e, g->ui, "hud", hud_open);
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
    g->player = players->spawn(transform2_at(point2{0, 0}), actor_sprite(*g, 0, 2), pa, Vitals{kHpMax, kHpMax, 0.0f}, AnimPlayer{}, feet_body(ColliderPusher), Move2{}, MoveResult2{});
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
    {.name = ".on_build",   KV_ON(LevelBuild, on_build), .writes = ".player, .npc, .rock, tilemap.tile, tilemap.animtile, audio.commands"},
    {.name = ".on_exit",    KV_ON(ExitRequested, on_exit)},
    {.name = ".on_loading", KV_ON(LevelLoading, on_loading)},
    {.name = ".on_ui",      KV_ON(UiEvent, on_ui), .writes = "audio.commands"},
};
constinit const SystemDesc k_systems[] = {
    {.name = ".files", .phase = Phase::PreSim, .fn = files, .after = "level.run", .before = ".menu", .main_thread = true, .runs = RunWhen::Always},
    {.name = ".menu", .phase = Phase::PreSim, .fn = menu, .reads = "input.actions", .writes = "audio.commands",
     .after = "level.run", .main_thread = true, .runs = RunWhen::Always},
    {.name = ".walk", .phase = Phase::Sim, .fn = walk, .reads = "input.actions", .writes = ".player.Move2, .player.Actor, .player.Transform2", .main_thread = true},
    {.name = ".villagers", .phase = Phase::Sim, .fn = villagers, .reads = "^.npc.Local2", .writes = ".npc.Move2, .npc.Actor, .npc.AnimPlayer", .main_thread = true},
    {.name = ".react", .phase = Phase::Sim, .fn = react, .reads = ".player, .npc.MoveResult2",
     .writes = ".player.Vitals, .player.Sprite, .player.AnimPlayer, audio.commands", .main_thread = true},
    {.name = ".camera", .phase = Phase::PostSim, .fn = camera, .reads = "platform_hid.window", .writes = "sprite.camera", .main_thread = true, .runs = RunWhen::Always},
    {.name = ".hud", .phase = Phase::PostSim, .fn = hud, .reads = ".player.Vitals", .before = "ui.draw", .main_thread = true, .runs = RunWhen::Always},
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
