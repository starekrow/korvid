// arcanum: a JRPG skeleton. Two Tiled maps loaded through the level module, a
// player who walks in eight directions under the "move" action, collides with the map's
// solids (tilemap_move) and with the villagers, pushes rocks, loses a heart for walking
// into a villager, sorts by its feet against trees and houses, and walks through exits to
// the other map; a pause menu with save and load, a level select and the credits; music
// that crossfades between the maps.
#include "korvid/arcanum/module.h"
#include "korvid/core/format.h"
#include "korvid/anim/player.h"
#include "korvid/anim/sheet.h"
#include "korvid/asset/blob.h"
#include "korvid/asset/scope.h"
#include "korvid/asset/sound.h"
#include "korvid/asset/stream.h"
#include "korvid/asset/font.h"
#include "korvid/asset_render/material.h"
#include "korvid/asset_render/texture.h"
#include "korvid/audio/audio.h"
#include "korvid/collision/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/hash.h"
#include "korvid/core/json.h"
#include "korvid/core/random.h"
#include "korvid/core/log.h"
#include "korvid/core/utf8.h"
#include "korvid/frameloop/frameloop.h"
#include "korvid/level/level.h"
#include "korvid/math/fields.h"
#include "korvid/platform/io.h"
#include "korvid/platform/paths.h"
#include "korvid/platform_hid/actions.h"
#include "korvid/platform_hid/host_window.h"
#include "korvid/platform_hid/module.h"
#include "korvid/render/renderer.h"
#include "korvid/save/save.h"
#include "korvid/scene/parent.h"
#include "korvid/sprite/camera.h"
#include "korvid/sprite/sprite.h"
#include "korvid/sprite/transform2.h"
#include "korvid/sprite_text/sprite_text.h"
#include "korvid/tilemap/tilemap.h"
#include "korvid/sprite_ui/sprite_ui.h"
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
    u8     hostile;  // a villager the hammer has offended: it chases until the level is left
    u8     pad[3];
    point2 home;     // a villager's spawn point
    vec2   route;    // a villager's second point, from home; zero stands still
    f32    swing;    // seconds left of a hammer swing (the attack pose)
    point2 goal;     // a hostile villager's next waypoint
    f32    repath;   // seconds until it asks the map for a path again
};
// The player's health: hearts, and the seconds left of the blink after a hurt, during
// which nothing hurts again.
struct Vitals {
    i32 hp;
    i32 hp_max;
    f32 hurt;
    u32 pad;
    u64 luck;  // the hammer's dice: a PCG32 state, sim state like the rest
};
constinit const FieldDesc k_actor_fields[] = {KV_FIELDS(Actor, facing, moving, sheet, route_dir, hostile, pad, home, route, swing, goal, repath)};
// What one villager wants of the player: the old man's errand. An attachment, since one
// villager of many has it; what every villager has (its lines) is in its bag, from the map.
struct Quest {
    u32 rocks_wanted; // broken rocks that would please him
    u32 done;         // 1 once the player has done it
};
constinit const FieldDesc k_vitals_fields[] = {KV_FIELDS(Vitals, hp, hp_max, hurt, pad, luck)};
constinit const FieldDesc k_quest_fields[] = {KV_FIELDS(Quest, rocks_wanted, done)};
constinit const ComponentDesc k_components[] = {KV_COMPONENT(Actor, k_actor_fields), KV_COMPONENT(Vitals, k_vitals_fields), KV_COMPONENT(Quest, k_quest_fields)};

constinit const KTableDesc k_ktables[] = {
    // Bodies: the player and the villagers walk by intent (Move2), the rocks are pushed.
    {.name = ".player", .components = "Transform2, Sprite, Actor, Vitals, AnimPlayer, Collider2, Move2, MoveResult2", .capacity = 1},
    {.name = ".npc",    .components = "Transform2, Local2, Parent, Sprite, Actor, AnimPlayer, Collider2, Move2, MoveResult2", .capacity = 32,
     .attachable = "Quest", .props = true},
    // Rocks the player pushes: the level's children, placed by the map's "rock" points.
    {.name = ".rock",   .components = "Transform2, Local2, Parent, Sprite, Collider2, Move2", .capacity = 32},
};
// What a save holds: the player, and what lives in the level and can change.
constinit const char* const k_saved_ktables[] = {"arcanum.player", "arcanum.npc", "arcanum.rock"};

constexpr f32 kWalkSpeed   = 64.0f;   // px per second
constexpr f32 kNpcSpeed    = 24.0f;
constexpr f32 kFeetW = 10.0f, kFeetH = 5.0f;
constexpr f32 kRockW = 26.0f, kRockH = 12.0f; // a boulder's base, from its bottom edge
constexpr f32 kPushPace = 0.5f;              // a pushed rock moves at half the walk
constexpr u32 L_ACTOR = 1;                   // everything solid here is on one layer
constexpr i32 kHpMax = 5;
constexpr f32 kHurtTime = 1.0f;              // seconds of blinking, untouchable
constexpr u32 kSheets = 3;
const char* const k_sheet_paths[kSheets] = {"arcanum/actors/NinjaBlue.png", "arcanum/actors/OldMan.png", "arcanum/actors/ManGreen.png"};
const char* const k_sheet_names[kSheets] = {"NinjaBlue", "OldMan", "ManGreen"};
const char* const k_sheet_files[kSheets] = {"arcanum/actors/NinjaBlue.sheet.json", "arcanum/actors/OldMan.sheet.json", "arcanum/actors/ManGreen.sheet.json"};
const char* const k_facings[4] = {"down", "up", "left", "right"};

constinit const LevelDesc k_levels[] = {
    {"village", "arcanum/maps/village.tmj"},
    {"field",   "arcanum/maps/field.tmj"},
};

constinit const InputActionDesc k_actions[] = {
    {"move",    ActionKind::Axis2,  "keys W S A D, keys Up Down Left Right, stick left, dpad"},
    {"confirm", ActionKind::Button, "key Enter, key Space, pad South"},
    {"cancel",  ActionKind::Button, "key Backspace, pad East"},
    {"menu",    ActionKind::Button, "key Escape, pad Start"},
    {"hammer",  ActionKind::Button, "key H, key X, pad West"},
};

enum class Menu : u8 { Closed, Main, Levels, Credits };
// Where the game is: the title (and naming the hero), or playing.
enum class Mode : u8 { Title, Play, Aim, Dead };
// What a cover is for, in its user word.
constexpr u64 COVER_START = 1;             // the iris from the name to the village
constexpr u64 COVER_AIM_IN = 2, COVER_AIM_OUT = 3; // into the hammer's minigame and out
constexpr u64 COVER_DEAD = 4;              // the last heart: to the game-over screen
constexpr u64 COVER_RESTART = 5;           // back to the village from the game-over screen
constexpr f32 kChaseSpeed = 128.0f;        // a hostile villager: twice the walk
constexpr f32 kAimSpeed = 3.2f;            // the needle, radians a second
enum Snd : u8 { SND_MOVE, SND_ACCEPT, SND_CANCEL, SND_BREAK, SND_HIT, SND_ALERT, SND_COUNT };
constexpr color kHurtTint = color{1.0f, 0.35f, 0.35f, 1.0f};

struct Game {
    u32    act_move = 0, act_confirm = 0, act_cancel = 0, act_menu = 0, act_hammer = 0;
    Handle sheet_texture[kSheets], sheet_material[kSheets];
    Handle sheets[kSheets];      // anim.sheet: the walk and idle clips, loaded into each level's scope
    Handle walk_clip[kSheets][4], idle_clip[kSheets][4]; // by facing, once the sheet is Ready
    Handle nature_texture, rock_material; // a rock is a 32 px cell of the nature sheet
    Handle ui;                   // ui.doc: arcanum.spui.json, every screen the game shows
    Handle hud;                  // ui.screen: the hearts, always up
    Handle screen;               // ui.screen: the menu screen up now, if any
    Handle sounds[SND_COUNT];
    Handle credits;              // the credits.json blob
    Handle player;
    Handle map;                  // the current level's map
    Handle music;                // the current level's track (asset.stream)
    bool   exit_armed = false;   // the player has stood outside every exit since arriving
    bool   leaving = false;      // an exit has been taken; the level is on its way
    Menu   menu = Menu::Closed;
    Mode   mode = Mode::Title;
    Handle title, naming;        // ui.screen: the title, the name entry
    Handle dialogue;             // ui.screen: a villager talking, if one is
    char   pages[512] = {};      // the villager's lines, pages split by '|'
    u32    page = 0, page_count = 0;
    char   hero[32] = "Rowan";
    // The hammer: the rock it is aimed at, the minigame's clock and how it went.
    Handle aim, aim_rock;
    f32    aim_t = 0.0f, aim_done = 0.0f; // seconds swinging; seconds the result has shown
    bool   aim_struck = false, aim_broke = false;
    u32    rocks_broken = 0;
    Handle provoked;             // a villager just hammered: the villagers system turns it
    Handle shout;                // ui.screen: its cry, for a moment
    f32    shout_t = 0.0f;
    Handle gameover;             // ui.screen
    bool   chased = false;       // a villager has turned on the player (the demo stops swinging)
    u32    demo_wait = 0;        // the demo's ticks on the game-over screen, then after the load
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
    if (kv::fabs(d.x) > kv::fabs(d.y)) return d.x < 0 ? 2 : 3;
    return d.y < 0 ? 0 : 1;
}

// The sheet's walk or idle clip for how the actor moves and faces, started when it
// changes; anim.play does the frames.
void animate(Engine& e, Game& g, const Actor& a, AnimPlayer& p) {
    if (a.swing > 0.0f) { // the hammer's pose, held while it lasts
        Handle swing = anim_sheet_variant(e, g.sheets[a.sheet % kSheets], "attack", k_facings[a.facing & 3]);
        if (p.clip != swing) anim_play(p, swing);
        return;
    }
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

// The saved ktables' state, less their Parent links (a load parents them to the level's new
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
    for (const char* name : k_saved_ktables) {
        const KTable* t = engine_ktable(e, name);
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
// Screens of the UI document; what they do comes back as SpuiEvents (on_ui).

// Under the menu: what saving or loading did.
void note(Engine& e, const char* text) { spui_model_set_str(e, "note", text); }

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
    add("{*c=gold}"); add(json_string(json_get(doc, doc.root(), "title"), "Credits")); add("{/c}\n\n");
    const JsonValue* sections = json_get(doc, doc.root(), "sections");
    for (const JsonValue* sec = sections ? json_first(doc, *sections) : nullptr; sec; sec = json_next(doc, *sec)) {
        add("{*c=gold}"); add(json_string(json_get(doc, *sec, "heading"), "")); add("{/c}\n");
        const JsonValue* lines = json_get(doc, *sec, "lines");
        for (const JsonValue* l = lines ? json_first(doc, *lines) : nullptr; l; l = json_next(doc, *l)) { add(json_string(l, "")); add("\n"); }
        add("\n");
    }
    text.push(0);
    spui_set_text(e, g.screen, "text", text.data);
}

// Shows a menu screen (or none), replacing the one up; the first holds the sim.
void open_menu(Engine& e, Game& g, Menu m, const char* focus = nullptr) {
    if (!g.screen.is_null()) spui_close(e, g.screen);
    g.screen = Handle{};
    if (g.menu == Menu::Closed && m != Menu::Closed) note(e, "");
    g.menu = m;
    // Where a screen opens, for --screenshot-frame and the like.
    KV_INFO("arcanum", "menu: %s at frame %llu, tick %llu", k_menu_names[(u32)m], (unsigned long long)e.frame_count, (unsigned long long)e.clock.tick_total);
    if (m == Menu::Closed) return;
    SpuiOpen o;
    o.layer = g.mode == Mode::Title ? 125 : 100; // over the title, when it is up
    o.hold = true;
    static const char* const k_screens[] = {"", "pause", "levels", "credits"};
    g.screen = spui_open(e, g.ui, k_screens[(u32)m], o);
    if (m == Menu::Credits) fill_credits(e, g);
    if (focus) spui_focus(e, g.screen, "items", focus);
}

void close_menu(Engine& e, Game& g) { open_menu(e, g, Menu::Closed); }

void save_game(TickContext& ctx, Game& g);
void load_game(TickContext& ctx, Game& g);

// ---- the title ------------------------------------------------------------------------------------

void restart(Engine& e, Game& g, bool to_title);
void strike(TickContext& ctx, Game& g);
void start_aim(Engine& e, Game& g, Handle rock);
void shout(TickContext& ctx, Game& g, const char* text);

void title_choice(TickContext& ctx, Game& g, const char* item) {
    Engine& e = *ctx.engine;
    if (!strcmp(item, "new")) {
        SpuiOpen o;
        o.layer = 120;
        o.hold = true;
        o.enter.fx = SpuiFx::Slide;
        o.enter.from = SpuiPoint::Bottom;
        g.naming = spui_open(e, g.ui, "name", o);
    } else if (!strcmp(item, "continue")) {
        note(e, "");
        load_game(ctx, g); // the title goes when the level it names is built
    } else if (!strcmp(item, "credits")) open_menu(e, g, Menu::Credits);
    else if (!strcmp(item, "quit")) engine_request_quit(e);
}

// The name is given: an iris closes on it, and opens on the village.
void begin_play(Engine& e, Game& g) {
    const char* typed = spui_field_text(e, g.naming, "entry");
    utf8_copy(g.hero, sizeof g.hero, typed[0] ? typed : "Rowan");
    spui_model_set_str(e, "hero", g.hero);
    SpuiCover c;
    c.fx = SpuiFx::Iris;
    c.seconds = 0.6f;
    c.user = COVER_START;
    spui_cover(e, c);
}

// ---- dialogue -------------------------------------------------------------------------------------

// The current page of the lines into the box, typed out.
void show_page(Engine& e, Game& g) {
    const char* p = g.pages;
    for (u32 i = 0; i < g.page && p; ++i) { p = strchr(p, '|'); if (p) ++p; }
    char page[256];
    const char* end = p ? strchr(p, '|') : nullptr;
    usize n = p ? (end ? (usize)(end - p) : strlen(p)) : 0;
    if (n >= sizeof page) n = sizeof page - 1;
    if (p) memcpy(page, p, n);
    page[n] = 0;
    spui_set_text(e, g.dialogue, "text", page);
    spui_set_visible(e, g.dialogue, "more", g.page + 1 < g.page_count);
}

void next_page(Engine& e, Game& g) {
    if (++g.page < g.page_count) { show_page(e, g); return; }
    SpuiMotion out;
    out.fx = SpuiFx::Slide;
    out.seconds = 0.15f;
    spui_close_with(e, g.dialogue, out);
    g.dialogue = Handle{};
}

// A villager speaks: its face, its name to show and its lines, from its bag.
void talk(TickContext& ctx, Game& g, Handle npc) {
    Engine& e = *ctx.engine;
    if (!ctx.alive(npc)) return;
    const Actor* a = &ctx.read<Actor>("arcanum.npc.Actor")[npc.index()];
    Quest* q = ctx.attached<Quest>(npc);
    if (q && !q->done && g.rocks_broken >= q->rocks_wanted) { q->done = 1; KV_INFO("arcanum", "quest: done"); }
    const char* lines = ctx.prop_str(npc, q && q->done ? "dialogue_done" : "dialogue", "...");
    utf8_copy(g.pages, sizeof g.pages, lines);
    g.page_count = 1;
    for (const char* c = g.pages; *c; ++c) g.page_count += *c == '|';
    g.page = 0;
    SpuiOpen o;
    o.layer = 105;
    o.hold = true;
    o.enter.fx = SpuiFx::Slide;
    o.enter.seconds = 0.15f;
    g.dialogue = spui_open(e, g.ui, "dialogue", o);
    char face[96];
    format(face, sizeof face, "arcanum/faces/%s.png", k_sheet_names[a ? a->sheet % kSheets : 0]);
    spui_set_texture(e, g.dialogue, "face", face, rect2{0, 0, 38, 38});
    spui_set_text(e, g.dialogue, "who", ctx.prop_str(npc, "display", "Villager"));
    show_page(e, g);
    KV_INFO("arcanum", "talk: %s, %u pages", ctx.prop_str(npc, "display", "?"), g.page_count);
}

// ---- saving and loading --------------------------------------------------------------------------

// The save's format, written as "version". 1: ktables named "example_arcanum.*" and the game
// "example_arcanum", from when the module was named so. 2: the module is "arcanum", and so
// are its ktables and the game.
constexpr i64 kSaveVersion = 3;

// An older save brought up to kSaveVersion in place, or false when it cannot be. Version 2
// kept its ktables under "kinds", from before they were ktables: the member is renamed. A
// version 1 save names ktables that are not there any more, so it is refused.
bool upgrade_save(JsonDoc& doc, i64 version) {
    if (version != 2 || !doc.count || !doc.nodes[0].is_object()) return false;
    const JsonValue& root = doc.nodes[0];
    for (u32 i = 0; i < root.as.kids.count; ++i) {
        JsonValue& m = doc.nodes[root.as.kids.first + i];
        if (m.key && !strcmp(m.key, "kinds")) m.key = "ktables";
    }
    return true;
}

// The save read back: parsed, arcanum's, and at kSaveVersion (upgraded when older). Null
// when it is good, else what the menu says.
const char* read_save(const u8* data, usize size, JsonDoc& doc) {
    if (!save_parse(SaveFormat::Json, data, size, mem_scratch(), doc)) {
        KV_WARN("arcanum", "load: %s at byte %u", doc.error, doc.error_at);
        return "The save does not load";
    }
    // Version 1 named the game for the module it was then.
    const char* game = json_string(json_get(doc, doc.root(), "game"), "");
    if (strcmp(game, "arcanum") != 0 && strcmp(game, "example_arcanum") != 0) {
        KV_WARN("arcanum", "load: not an arcanum save");
        return "The save does not load";
    }
    i64 version = json_i64(json_get(doc, doc.root(), "version"), 0);
    if (version > kSaveVersion) {
        KV_WARN("arcanum", "load: save version %lld, newer than this game's %lld", (long long)version, (long long)kSaveVersion);
        return "The save is from a newer version";
    }
    if (version < kSaveVersion && !upgrade_save(doc, version)) {
        KV_WARN("arcanum", "load: save version %lld cannot be upgraded to %lld", (long long)version, (long long)kSaveVersion);
        return "The save is from an older version";
    }
    return nullptr;
}

// The save: what level, which entity the player is, and the ktables that change. The bytes
// are copied into the write at submission; the menu says "Saved" when the disk has them.
void save_game(TickContext& ctx, Game& g) {
    Engine& e = *ctx.engine;
    if (!g.save_path[0]) { note(e, "There is nowhere to save"); return; }
    if (level_state(e) != LevelState::Active || !g.save_write.is_null()) { note(e, "Not now"); return; }
    SaveWriter w;
    w.init(mem_general());
    w.begin_object();
    w.key("game");       w.string_value("arcanum");
    w.key("version");    w.int_value(kSaveVersion);
    w.key("tick");       w.uint_value(e.clock.tick_total);
    w.key("level");      w.string_value(level_name(e, level_current(e)));
    w.key("player");     save_handle(w, g.player);
    w.key("exit_armed"); w.bool_value(g.exit_armed);
    w.key("hero");       w.string_value(g.hero);
    w.key("broken");     w.uint_value(g.rocks_broken);
    w.key("levels");     level_save(w, e); // what each level remembers: the rocks
    w.key("ktables");    save_ktables(w, e, k_saved_ktables);
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
// this one) and on_build restores the ktables instead of spawning the map's.
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
    free_in(mem_general(), g.load_data, g.load_size + 1); // a read's bytes: the file and its NUL
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
        g.load_data = (u8*)c.bytes.data;
        g.load_size = c.bytes.size;
        const char* why = nullptr;
        if (c.status != IoStatus::Done) why = c.not_found() ? "There is no saved game" : "The save could not be read";
        else {
            MemScope scope;
            JsonDoc doc;
            why = read_save(g.load_data, g.load_size, doc);
            if (!why) {
                const char* level = json_string(json_get(doc, doc.root(), "level"), "");
                if (level[0]) if (const JsonValue* lv = json_get(doc, doc.root(), "levels")) level_load(e, doc, *lv);
                g.rocks_broken = (u32)json_u64(json_get(doc, doc.root(), "broken"), 0);
                LevelTransition t;
                t.fade_out = 0.3f; t.fade_in = 0.3f; t.entry = "load";
                if (!level[0] || !level_request(e, level, t)) why = "The save does not load";
                else KV_INFO("arcanum", "load: %s, %u bytes, level %s", g.save_path, (u32)g.load_size, level);
            }
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
    if (!action_pressed(a, g.act_menu) || g.mode == Mode::Title || !g.dialogue.is_null()) return;
    if (g.menu == Menu::Closed) { if (!held_by_load) { open_menu(e, g, Menu::Main); play_sound(ctx, g, SND_ACCEPT); } }
    else { close_menu(e, g); play_sound(ctx, g, SND_CANCEL); }
}

// What the menu screens say: an item chosen, a cancel.
void on_ui(TickContext& ctx, Span<const SpuiEvent> events) {
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    static const u64 k_select = name_intern("select").hash, k_cancel = name_intern("cancel").hash, k_confirm = name_intern("confirm").hash;
    for (const SpuiEvent& ev : events) {
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
            if (g.mode == Mode::Title) close_menu(e, g); // back to the title under it
            else open_menu(e, g, Menu::Main, "credits");
        }
    }
    for (const SpuiEvent& ev : events) {
        const char* item = name_str(Name{ev.node});
        if (ev.screen == g.title && g.menu == Menu::Closed && g.naming.is_null() && ev.event == k_select) title_choice(ctx, g, item);
        else if (ev.screen == g.naming) {
            static const u64 k_submit = name_intern("submit").hash;
            if (ev.event == k_submit) begin_play(e, g);
            else if (ev.event == k_cancel) { spui_close(e, g.naming); g.naming = Handle{}; }
        } else if (ev.screen == g.dialogue && ev.event == k_confirm) next_page(e, g);
        else if (ev.screen == g.aim && ev.event == k_confirm && !g.aim_struck) strike(ctx, g);
        else if (ev.screen == g.gameover && ev.event == k_select) {
            spui_close(e, g.gameover);
            g.gameover = Handle{};
            if (!strcmp(item, "continue")) { load_game(ctx, g); if (g.load_read.is_null()) restart(e, g, false); }
            else restart(e, g, true);
        }
    }
}

// Back to the village as the map has it, the hero whole: from the game-over screen when
// there is no save, or to the title.
void restart(Engine& e, Game& g, bool to_title) {
    KTable* players = engine_ktable(e, "arcanum.player");
    Vitals* v = players->get<Vitals>(g.player);
    v->hp = v->hp_max;
    v->hurt = 0.0f;
    for (u32 i = 0; i < KV_COUNTOF(k_levels); ++i) level_forget(e, k_levels[i].name);
    g.rocks_broken = 0;
    LevelTransition t;
    t.fade_out = 0.0f; t.fade_in = 0.4f;
    level_request(e, "village", t);
    g.mode = to_title ? Mode::Title : Mode::Play;
    if (to_title) {
        SpuiOpen o;
        o.layer = 110;
        o.hold = true;
        g.title = spui_open(e, g.ui, "title", o);
        spui_set_visible(e, g.hud, "", false);
    } else spui_set_visible(e, g.hud, "", true);
}

// The iris has closed on the name screen: the title goes, the village shows as it opens.
void on_covered(TickContext& ctx, Span<const SpuiCovered> covers) {
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    for (const SpuiCovered& c : covers) {
        if (c.user == COVER_AIM_IN) { // the minigame, where the world was
            SpuiOpen o;
            o.layer = 106;
            o.hold = true;
            g.aim = spui_open(e, g.ui, "aim", o);
            spui_set_visible(e, g.hud, "", false);
            continue;
        }
        if (c.user == COVER_AIM_OUT) { // the world again; the rock gone if it broke
            spui_close(e, g.aim);
            g.aim = Handle{};
            spui_set_visible(e, g.hud, "", true);
            if (g.aim_broke) {
                if (KTable* rocks = ctx.write_ktable("arcanum.rock")) rocks->destroy(g.aim_rock);
                ++g.rocks_broken;
                KV_INFO("arcanum", "broken: %u rocks", g.rocks_broken);
            }
            g.aim_rock = Handle{};
            g.mode = Mode::Play;
            continue;
        }
        if (c.user == COVER_DEAD) { // the game-over screen
            SpuiOpen o;
            o.layer = 130;
            o.hold = true;
            g.gameover = spui_open(e, g.ui, "gameover", o);
            spui_set_visible(e, g.hud, "", false);
            continue;
        }
        if (c.user != COVER_START) continue;
        if (!g.naming.is_null()) spui_close(e, g.naming);
        if (!g.title.is_null()) spui_close(e, g.title);
        g.naming = g.title = Handle{};
        g.mode = Mode::Play;
        spui_set_visible(e, g.hud, "", true);
        KV_INFO("arcanum", "play: %s sets out", g.hero);
    }
}

// ---- walking -------------------------------------------------------------------------------------
// Intent, resolve, react: walk and villagers write Move2, collision.resolve moves everyone
// against the map and each other (the player pushing rocks), react reads the results.

// ---- the hammer -----------------------------------------------------------------------------------

// A rock in reach of the hammer: an iris closes on the player, and the minigame opens.
void start_aim(Engine& e, Game& g, Handle rock) {
    g.aim_rock = rock;
    g.aim_t = 0.0f;
    g.aim_done = 0.0f;
    g.aim_struck = g.aim_broke = false;
    g.mode = Mode::Aim;
    SpuiCover c;
    c.fx = SpuiFx::Iris;
    c.seconds = 0.4f;
    c.user = COVER_AIM_IN;
    spui_cover(e, c);
    KV_INFO("arcanum", "aim: at %s", engine_entity_name(e, rock));
}

// A villager's cry, on top of everything for a second and a half.
void shout(TickContext& ctx, Game& g, const char* text) {
    Engine& e = *ctx.engine;
    if (g.shout.is_null()) {
        SpuiOpen o;
        o.layer = 108;
        o.input = false;
        g.shout = spui_open(e, g.ui, "shout", o);
    }
    spui_set_text(e, g.shout, "", text);
    g.shout_t = 1.5f;
    play_sound(ctx, g, SND_ALERT);
}

// Phase PreSim, every tick: the needle sweeps; a struck result shows a moment, then the
// iris takes the minigame away; the cry fades.
void modes(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    f32 dt = (f32)ctx.dt;
    if (!g.shout.is_null() && (g.shout_t -= dt) <= 0.0f) { spui_close(e, g.shout); g.shout = Handle{}; }
    if (g.aim.is_null() || spui_covering(e)) return;
    if (!g.aim_struck) {
        g.aim_t += dt;
        f32 x = 0.5f - 0.5f * kv::cos(g.aim_t * kAimSpeed); // 0 to 1 and back, from the left
        spui_set_offset(e, g.aim, "needle", vec2{(x - 0.5f) * 200.0f, 0.0f});
        return;
    }
    if ((g.aim_done += dt) < 0.9f || spui_covering(e)) return;
    SpuiCover c;
    c.fx = SpuiFx::Iris;
    c.seconds = 0.4f;
    c.user = COVER_AIM_OUT;
    spui_cover(e, c);
    g.aim_done = -1e6f; // once
}

// The swing: on the gold mark (a tenth of the bar) the rock breaks; off it, the nearer the
// middle, the likelier. The dice are the
// player's, sim state, so a demo breaks the same rock in every run.
void strike(TickContext& ctx, Game& g) {
    Engine& e = *ctx.engine;
    KTable* players = engine_ktable(e, "arcanum.player");
    Vitals* v = players->get<Vitals>(g.player);
    f32 x = 0.5f - 0.5f * kv::cos(g.aim_t * kAimSpeed);
    f32 close = 1.0f - kv::fabs(x - 0.5f) * 2.0f;
    Rng r;
    r.state = v->luck;
    f32 roll = r.unit();
    v->luck = r.state;
    g.aim_struck = true;
    g.aim_broke = close >= 0.9f || roll < close * close; // the gold mark always breaks it
    spui_set_text(e, g.aim, "result", g.aim_broke ? "{*c=gold}{*wave}Crack!{/wave}{/c} The boulder splits." : "{*shake}Clang!{/shake} It holds.");
    play_sound(ctx, g, g.aim_broke ? SND_BREAK : SND_HIT);
    KV_INFO("arcanum", "strike: needle %.3f, near %.3f, roll %.3f: %s", x, close, roll, g.aim_broke ? "breaks" : "holds");
}

// Phase Sim: the player's intent, from the move action.
void walk(TickContext& ctx) {
    static const u32 k_actions_id = ctx.resource_id("input.actions");
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    const InputActions& a = *ctx.resource<const InputActions>(k_actions_id);
    Span<Move2> move = ctx.write<Move2>("arcanum.player.Move2");
    Span<Actor> actor = ctx.write<Actor>("arcanum.player.Actor");
    u32 i = g.player.index();
    if (i >= move.len || level_state(e) == LevelState::Loading) return;
    actor[i].swing = actor[i].swing > (f32)ctx.dt ? actor[i].swing - (f32)ctx.dt : 0.0f;
    // Confirm facing a villager within a step: it talks. The hammer, facing a rock: the
    // minigame; facing a villager: trouble. The query sees the bodies where the last tick
    // left them (the reads are early: this system writes the intent).
    bool talking = action_pressed(a, g.act_confirm), hammering = action_pressed(a, g.act_hammer);
    if ((talking || hammering) && g.mode == Mode::Play && g.dialogue.is_null() && g.aim.is_null()) {
        static const u32 k_xf = ctx.column_id(".player.Transform2");
        point2 at = ctx.read<Transform2>(k_xf)[i].position;
        static const vec2 k_ahead[4] = {{0, -12}, {0, 12}, {-12, 0}, {12, 0}};
        vec2 ahead = k_ahead[actor[i].facing & 3];
        MemScope scope;
        Array<Handle> nearby = array_in<Handle>(mem_scratch());
        collision_query(ctx, rect2{at.x + ahead.x - 8.0f, at.y - 9.0f + ahead.y, 16.0f, 10.0f}, L_ACTOR, nearby);
        static const u32 k_npc = ctx.ktable_id(".npc");
        static const u32 k_rock = ctx.ktable_id(".rock");
        u16 npc_hk = engine_ktable_now(e, k_npc)->handle_kind, rock_hk = engine_ktable_now(e, k_rock)->handle_kind; // handle kinds, which never change
        if (hammering) {
            actor[i].swing = 0.3f;
            play_sound(ctx, g, SND_HIT);
            for (Handle h : nearby) {
                if (h.kind() == rock_hk) { start_aim(e, g, h); break; }
                if (h.kind() == npc_hk) { g.provoked = h; break; }
            }
        } else for (Handle h : nearby) if (h.kind() == npc_hk) { talk(ctx, g, h); break; }
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
    // The player's columns, by its row: read as the ktable is declared, and the three it
    // writes declared one by one (the whole ktable would include the intent the resolve
    // reads, a cycle).
    Span<const Transform2>  transforms = ctx.read<Transform2>(".player.Transform2");
    Span<const MoveResult2> results    = ctx.read<MoveResult2>(".player.MoveResult2");
    Span<const Actor>       actors     = ctx.read<Actor>(".player.Actor");
    Span<Vitals>            vitals     = ctx.write<Vitals>(".player.Vitals");
    Span<Sprite>            sprites    = ctx.write<Sprite>(".player.Sprite");
    Span<AnimPlayer>        anims      = ctx.write<AnimPlayer>(".player.AnimPlayer");
    Span<const MoveResult2> npc_moves  = ctx.read<MoveResult2>(".npc.MoveResult2");
    const TilemapAsset* m = tilemap_get(e, g.map);
    if (!ctx.alive(g.player) || !m || m->state != AssetState::Ready || level_state(e) == LevelState::Loading) return;
    const u32 me = g.player.index();
    const Transform2* t = &transforms[me];
    static const u32 k_npc_ktable = ctx.ktable_id(".npc");
    const u16 npc_handles = engine_ktable(e, k_npc_ktable)->handle_kind;
    const MoveResult2& mine = results[me];
    bool bumped = !mine.blocker.is_null() && mine.blocker.kind() == npc_handles;
    for (u32 i = 0; i < npc_moves.len && !bumped; ++i) bumped = npc_moves[i].blocker == g.player;
    Vitals* v = &vitals[me];
    f32 dt = (f32)ctx.dt;
    v->hurt = v->hurt > dt ? v->hurt - dt : 0.0f;
    // Walking into a villager costs a heart, then a second of blinking that nothing hurts
    // through. The last heart is the end.
    if (bumped && v->hurt == 0.0f && v->hp > 0) {
        --v->hp;
        v->hurt = kHurtTime;
        play_hurt(ctx, g);
        KV_INFO("arcanum", "hurt: %d of %d hearts at tick %llu", v->hp, v->hp_max, (unsigned long long)ctx.tick);
        if (v->hp == 0 && g.mode == Mode::Play) { // the last heart: the game-over screen, behind a red fade
            g.mode = Mode::Dead;
            SpuiCover c;
            c.fx = SpuiFx::Fade;
            c.seconds = 0.8f;
            c.tint = color_rgba(0.12f, 0.0f, 0.01f, 1.0f);
            c.user = COVER_DEAD;
            spui_cover(e, c);
            KV_INFO("arcanum", "dead: at tick %llu", (unsigned long long)ctx.tick);
        }
    }
    animate(e, g, actors[me], anims[me]);
    Sprite* s = &sprites[me];
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
    Span<const Local2> local = ctx.read<Local2>("arcanum.npc.Local2");
    Span<Move2> move = ctx.write<Move2>("arcanum.npc.Move2");
    Span<Actor> actor = ctx.write<Actor>("arcanum.npc.Actor");
    Span<AnimPlayer> player = ctx.write<AnimPlayer>("arcanum.npc.AnimPlayer");
    Span<Sprite> sprite = ctx.write<Sprite>("arcanum.npc.Sprite");
    Span<const MoveResult2> last = ctx.read<MoveResult2>("arcanum.npc.MoveResult2"); // last tick's: an early read
    static const u32 k_player_xf = ctx.column_id(".player.Transform2");
    Span<const Transform2> pxf = ctx.read<Transform2>(k_player_xf);
    const point2* player_at = g.player.index() < pxf.len ? &pxf[g.player.index()].position : nullptr;
    const TilemapAsset* map = tilemap_get(*ctx.engine, g.map);
    if (map && map->state != AssetState::Ready) map = nullptr;
    static const u32 k_npc = ctx.ktable_id(".npc");
    KTableRows table = ctx.rows(k_npc);
    f32 step = kNpcSpeed * (f32)ctx.dt;
    for (u32 i = 0; i < move.len; ++i) {
        if (!table.alive(i)) continue;
        Actor& a = actor[i];
        Move2& mv = move[i];
        point2 at = local[i].position; // the level's root is at the origin
        f32 len = kv::sqrt(a.route.x * a.route.x + a.route.y * a.route.y);
        mv.delta = vec2{0, 0};
        a.moving = 0;
        if (!g.provoked.is_null() && table.handle_at(i) == g.provoked && !a.hostile) {
            // Hammered: it turns on the player, says so, and its color goes.
            a.hostile = 1;
            a.repath = 0.0f;
            g.chased = true;
            g.provoked = Handle{};
            sprite[i].tint = color_rgba(1.0f, 0.55f, 0.55f, 1.0f);
            shout(ctx, g, "{*shake}How DARE you!{/shake}");
            KV_INFO("arcanum", "hostile: %s at tick %llu", engine_entity_name(*ctx.engine, table.handle_at(i)), (unsigned long long)ctx.tick);
        }
        if (a.hostile && player_at && map) {
            // The chase: the map's path toward the player, a waypoint at a time, at twice the
            // player's pace. The path is asked for again every few tenths of a second.
            a.repath -= (f32)ctx.dt;
            vec2 to_goal{a.goal.x - at.x, a.goal.y - at.y};
            if (a.repath <= 0.0f || to_goal.x * to_goal.x + to_goal.y * to_goal.y < 4.0f) {
                MemScope scope;
                Array<point2> path = array_in<point2>(mem_scratch());
                PathQuery q;
                q.box = vec2{kFeetW, kFeetH};
                point2 from{at.x, at.y - 5.5f}, to{player_at->x, player_at->y - 5.5f};
                a.goal = tilemap_path(*map, point2{0, 0}, from, to, q, path) ? point2{path[0].x, path[0].y + 5.5f} : *player_at;
                if (path.len == 1) a.goal = *player_at; // the same cell: straight at it
                a.repath = 0.3f;
            }
            vec2 d{a.goal.x - at.x, a.goal.y - at.y};
            f32 dist = kv::sqrt(d.x * d.x + d.y * d.y);
            f32 chase = kChaseSpeed * (f32)ctx.dt;
            if (dist > 0.01f) mv.delta = dist <= chase ? d : vec2{d.x / dist * chase, d.y / dist * chase};
            a.moving = dist > 0.01f;
            a.facing = facing_of(mv.delta, a.facing);
        } else if (len > 0.0f) {
            // Stopped by something on the way (a bush, the player): turn back.
            if ((last[i].hit_x || last[i].hit_y) && last[i].blocker != g.player) a.route_dir ^= 1;
            point2 goal = a.route_dir ? a.home : point2{a.home.x + a.route.x, a.home.y + a.route.y};
            vec2 to{goal.x - at.x, goal.y - at.y};
            f32 dist = kv::sqrt(to.x * to.x + to.y * to.y);
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
    spui_model_set_int(*ctx.engine, "hp", v->hp);
    spui_model_set_int(*ctx.engine, "hp_max", v->hp_max);
}

// ---- levels -------------------------------------------------------------------------------------

// The saved ktables back, in place of the map's villagers and rocks. Parent links name the
// level's root ("level.root@level.root"), so they find the new one; materials name their
// textures; the rest comes back by ktable name and slot.
void restore(TickContext& ctx, Game& g, const LevelBuild& b) {
    Engine& e = *ctx.engine;
    MemScope scope;
    JsonDoc doc;
    if (read_save(g.load_data, g.load_size, doc)) {
        KV_ERROR("arcanum", "load: the save read when it arrived and does not now");
        return;
    }
    (void)b;
    const JsonValue* ktables = json_get(doc, doc.root(), "ktables");
    u32 n = ktables ? load_ktables(doc, *ktables, e) : 0;
    Handle player = load_handle(json_get(doc, doc.root(), "player"));
    if (!player.is_null()) g.player = player;
    g.exit_armed = json_bool(json_get(doc, doc.root(), "exit_armed"), true);
    utf8_copy(g.hero, sizeof g.hero, json_string(json_get(doc, doc.root(), "hero"), g.hero));
    spui_model_set_str(e, "hero", g.hero);
    if (g.mode == Mode::Dead) { g.mode = Mode::Play; spui_set_visible(e, g.hud, "", true); }
    if (g.mode == Mode::Title) { // continued from the title: it goes
        if (!g.title.is_null()) spui_close(e, g.title);
        g.title = Handle{};
        g.mode = Mode::Play;
        spui_set_visible(e, g.hud, "", true);
    }
    KV_INFO("arcanum", "load: %u ktables, state %016llx, saved at tick %llu", n, (unsigned long long)saved_state_hash(e),
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
    KTable* players = ctx.write_ktable("arcanum.player");
    if (Transform2* t = players->get<Transform2>(g.player)) {
        if (at && strcmp(b.entry, "reload") != 0) {
            *t = transform2_at(tilemap_to_world(point2{0, 0}, at->x, at->y));
            *players->prev<Transform2>(g.player) = *t; // a teleport: no sweep across the screen
        }
        players->get<Sprite>(g.player)->layer = (i32)objects_layer;
        g.exit_armed = exit_at(m, feet_of(t->position)) == nullptr;
    }
    KTable* npcs = ctx.write_ktable("arcanum.npc");
    KTable* rocks = ctx.write_ktable("arcanum.rock");
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
            // "x0,y0 x1,y1": four numbers, a comma inside each pair (strtof skips the space).
            f32 v[4];
            const char* rp = route->str;
            bool ok = true;
            for (u32 n = 0; n < 4 && ok; ++n) {
                char* end = nullptr;
                v[n] = strtof(rp, &end);
                ok = end != rp && (n % 2 == 1 || *end == ',');
                rp = n % 2 == 0 ? end + 1 : end;
            }
            if (ok) a.route = vec2{v[2] - v[0], -(v[3] - v[1])};
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
// The title: New game (the list starts on it), the name typed and entered, the iris.
constexpr u64 kTitle = 130;     // where the walk below starts, after the opening
constexpr u64 kTypeAt = 50;     // the name, with the newline that submits it
constinit const Step k_opening[] = {
    {20, 21, "confirm", 0, 0},
};
constinit const Step k_script[] = {
    {0, 90, "move", 0, 1},         // up the lane, pushing its boulder
    {90, 150, "move", 0, -1},      // back down to the road
    {150, 480, "move", 1, 0},      // east along the road, out to the field
    {480, 560, "move", -1, 0},     // and straight back: the boulder is where it was left
    {560, 857, "move", -1, 0},     // west along the road
    {857, 971, "move", 0, 1},      // north, beside the old man
    {980, 981, "move", -1, 0},     // facing him
    {990, 991, "confirm", 0, 0},   // he talks: the boulders
    {1090, 1091, "confirm", 0, 0},
    {1220, 1221, "confirm", 0, 0},
    {1230, 1279, "move", 0, -1},   // south, beside a boulder
    {1285, 1286, "move", -1, 0},   // facing it
    {1295, 1296, "hammer", 0, 0},  // the minigame
    {1373, 1374, "confirm", 0, 0}, // swing, on the mark: it breaks
    {1490, 1539, "move", 0, 1},    // back up to the old man
    {1545, 1546, "move", -1, 0},
    {1555, 1556, "confirm", 0, 0}, // the errand is done
    {1680, 1681, "confirm", 0, 0},
    {1810, 1811, "confirm", 0, 0},
    {1830, 1831, "menu", 0, 0},    // save
    {1840, 1841, "move", 0, -1},
    {1850, 1851, "confirm", 0, 0},
    {1870, 1871, "menu", 0, 0},
    {1880, 1910, "move", 1, 0},    // east of the bush the farmer paces by, west of the lane boulder
    {1915, 2063, "move", 0, -1},   // south, a row above his walk
};
// The hammer, every third of a second, for as long as the farmer takes to pace by.
constexpr u64 kHammerFrom = 2075, kHammerTo = 2700;
constexpr u64 kReloadAt = 100000, kQuitAt = 4000; // the quit, should the ending not come

// Phase PostSim, every tick: the next tick's actions, and the reload and the end.
void demo(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    if (!g.demo) return;
    if (engine_holds(e) & HoldLoading) return;
    u64 t = g.demo_t++;
    for (const Step& s : k_opening)
        if (t >= s.from && t < s.to) input_action_override(e, s.action, true, vec2{s.x, s.y});
    if (t == kTypeAt) input_text_override(e, "Rowan\n");
    if (t < kTitle) return;
    t -= kTitle;
    for (const Step& s : k_script)
        if (t >= s.from && t < s.to) input_action_override(e, s.action, true, vec2{s.x, s.y});
    if (t >= kHammerFrom && t < kHammerTo && (t - kHammerFrom) % 20 == 0 && g.mode == Mode::Play && !g.chased) input_action_override(e, "hammer", true);
    // The end: a second and a half on the game-over screen, Continue (the save: the rock
    // still broken, the farmer calm), four seconds of the village, quit.
    if (!g.gameover.is_null() && ++g.demo_wait == 90) input_action_override(e, "confirm", true);
    if (g.demo_wait >= 90 && g.mode == Mode::Play && ++g.demo_wait == 330) engine_request_quit(e);
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
    g->act_hammer = a->find("hammer");
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
    g->nature_texture = asset_load_texture(e, "arcanum/tiles/TilesetNature.png");
    g->rock_material = material_of(g->nature_texture);

    g->ui = spui_load(e, "arcanum/ui/arcanum.spui.json", asset_scope_global(e));
    SpuiOpen hud_open;
    hud_open.layer = 90;
    hud_open.input = false;
    g->hud = spui_open(e, g->ui, "hud", hud_open);
    spui_set_visible(e, g->hud, "", false); // until the hero sets out
    spui_model_set_str(e, "hero", g->hero);
    SpuiOpen title_open;
    title_open.layer = 110;
    title_open.hold = true;
    g->title = spui_open(e, g->ui, "title", title_open);
    g->sounds[SND_MOVE]   = asset_load_sound(e, "arcanum/sounds/move.wav");
    g->sounds[SND_ACCEPT] = asset_load_sound(e, "arcanum/sounds/accept.wav");
    g->sounds[SND_CANCEL] = asset_load_sound(e, "arcanum/sounds/cancel.wav");
    g->sounds[SND_BREAK]  = asset_load_sound(e, "arcanum/sounds/break.wav");
    g->sounds[SND_HIT]    = asset_load_sound(e, "arcanum/sounds/hit.wav");
    g->sounds[SND_ALERT]  = asset_load_sound(e, "arcanum/sounds/alert.wav");
    g->credits = asset_load_blob(e, "arcanum/credits.json");
    // The save, in the per-user directory; the demo keeps its own so a scripted run never
    // overwrites a player's game.
    const char* pref = platform_user_dir(e, UserDir::Data, "korvid", "arcanum");
    if (pref[0]) format(g->save_path, sizeof g->save_path, "%ssave%s.json", pref, g->demo ? "-demo" : "");
    KTable* players = engine_ktable(e, "arcanum.player");
    Actor pa{};
    g->player = players->spawn(transform2_at(point2{0, 0}), actor_sprite(*g, 0, 2), pa, Vitals{kHpMax, kHpMax, 0.0f, 0, Rng::seeded(0x4152434e).state}, AnimPlayer{}, feet_body(ColliderPusher), Move2{}, MoveResult2{});
    engine_entity_name_set(e, g->player, "player");
    LevelConfig cfg;
    cfg.levels = k_levels;
    cfg.load = [](Engine& en, const char* map, Handle scope) { return tilemap_load(en, map, scope); };
    if (!level_catalog(e, cfg)) return false;
    const char* const remembered[] = {"arcanum.rock"}; // a level keeps its rocks as they were left
    if (!level_persist(e, remembered)) return false;
    level_request(e, "village");
    KV_INFO("arcanum", "Arrows or WASD (or a pad's stick) walk, Escape or Start opens the menu, Enter or South chooses, Backspace or East goes back.");
    return true;
}

void arcanum_shutdown(Engine&, ModuleInstance& self) {
    Game* g = (Game*)self.state;
    if (g) free_load(*g); // a load the quit overtook
}

constinit const HandlerDesc k_handlers[] = {
    {.name = ".on_build",   KV_ON(LevelBuild, on_build), .phase = Phase::PreSim, .writes = ".player, .npc, .rock, tilemap.tile, tilemap.animtile", .appends = "audio.commands", .after = "tilemap.pump"},
    {.name = ".on_exit",    KV_ON(ExitRequested, on_exit), .phase = Phase::PreSim},
    {.name = ".on_loading", KV_ON(LevelLoading, on_loading), .phase = Phase::PreSim},
    {.name = ".on_ui",      KV_ON(SpuiEvent, on_ui), .phase = Phase::PreSim, .writes = ".player.Vitals", .appends = "audio.commands", .after = ".on_build"},
    {.name = ".on_covered", KV_ON(SpuiCovered, on_covered), .phase = Phase::PreSim, .writes = ".rock", .after = ".on_build"},
};
constinit const SystemDesc k_systems[] = {
    {.name = ".files", .phase = Phase::PreSim, .fn = files, .after = "level.run", .before = ".menu", .main_thread = true, .runs = RunWhen::Always},
    {.name = ".menu", .phase = Phase::PreSim, .fn = menu, .reads = "input.actions", .appends = "audio.commands",
     .after = "level.run", .main_thread = true, .runs = RunWhen::Always},
    {.name = ".modes", .phase = Phase::PreSim, .fn = modes, .appends = "audio.commands", .after = ".menu", .main_thread = true, .runs = RunWhen::Always},
    // Before the resolve (it writes the intent): bodies where the phase found them; of the
    // villager it faces, whether it lives, how it stands (villagers turns it) and its bag.
    {.name = ".walk", .phase = Phase::Sim, .fn = walk, .reads = "input.actions, ^*[Collider2].Transform2, *[Collider2].Collider2, .npc.@alive, .npc.Actor, .npc.@props",
     .writes = ".player.Move2, .player.Actor, .npc.@Quest", .appends = "audio.commands", .main_thread = true},
    {.name = ".villagers", .phase = Phase::Sim, .fn = villagers, .reads = "^.npc.@alive, ^.npc.Local2, ^.player.Transform2, ^.npc.MoveResult2", .writes = ".npc.Move2, .npc.Actor, .npc.AnimPlayer, .npc.Sprite", .appends = "audio.commands", .main_thread = true},
    {.name = ".react", .phase = Phase::Sim, .fn = react, .reads = ".player, .npc.MoveResult2",
     .writes = ".player.Vitals, .player.Sprite, .player.AnimPlayer", .appends = "audio.commands", .main_thread = true},
    {.name = ".camera", .phase = Phase::PostSim, .fn = camera, .reads = "platform_hid.window", .writes = "sprite.camera", .main_thread = true, .runs = RunWhen::Always},
    {.name = ".hud", .phase = Phase::PostSim, .fn = hud, .reads = ".player.Vitals", .before = "sprite_ui.draw", .main_thread = true, .runs = RunWhen::Always},
    {.name = ".demo", .phase = Phase::PostSim, .fn = demo, .main_thread = true, .runs = RunWhen::Always},
};

} // namespace

KV_MODULE_DEFINE(arcanum_module) = {
    .name       = "arcanum",
    .components = k_components,
    .ktables    = k_ktables,
    .systems    = k_systems,
    .handlers   = k_handlers,
    .startup    = arcanum_startup,
    .shutdown   = arcanum_shutdown,
    .memory     = {.reserve = 64 * 1024},
};

} // namespace kv
