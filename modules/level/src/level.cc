#include "korvid/level/module.h"
#include "korvid/level/level.h"
#include "korvid/asset/blob.h"
#include "korvid/asset/scope.h"
#include "korvid/core/cli.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/utf8.h"
#include "korvid/render/renderer.h"
#include "korvid/save/save.h"
#include "korvid/sprite/sprite.h"
#include "korvid/sprite/transform2.h"
#include <string.h>

namespace kv {

namespace {

struct Entry { char name[KV_LEVEL_NAME_MAX]; char map[128]; u64 user; };

struct Request {
    bool            pending = false;
    u32             level   = 0;
    LevelTransition t;
    char            entry[KV_LEVEL_ENTRY_MAX] = {};
};

struct LevelState_ {
    Entry          levels[KV_LEVELS];
    u32            count   = 0;
    LevelMapLoader load    = nullptr;
    LevelState     state   = LevelState::None;
    Request        now, next;           // the transition under way, the one waiting
    u32            current = KV_LEVELS; // live, or being entered
    u32            leaving = KV_LEVELS; // the level the transition started from
    bool           reload  = false;
    Handle         root, scope, map;    // the current level's
    Handle         old_root, old_scope; // the level being replaced, until Building
    Handle         new_scope, new_map;  // the level being loaded
    u32            ticks_in_state = 0;
    f32            fade = 0.0f;         // 0 clear, 1 covered
    u32            root_kind = 0, fade_kind = 0;
    Handle         fade_entity;
    Handle         white_texture, white_material;
    // Per-level state: the kinds whose named entities a level remembers, and per level what
    // it remembers ({"rows": {name: row}, "gone": [name]}, JSON) and the names its build made.
    char           persist[8][KV_KIND_NAME_MAX] = {};
    u32            persist_count = 0;
    Array<char>    record[KV_LEVELS];
    Array<u64>     built[KV_LEVELS];
    bool           forgotten[KV_LEVELS] = {}; // level_forget: the next leave records nothing
    Allocator*     general = nullptr;
};

LevelState_* state(Engine& e) { return engine_module_state_as<LevelState_>(e, "level"); }

void enter_state(LevelState_& s, LevelState st) { s.state = st; s.ticks_in_state = 0; }

const char* state_name(LevelState st) {
    static const char* const names[] = {"none", "leaving", "loading", "building", "entering", "active"};
    return names[(u32)st];
}

// The quad's white material, made the first time a renderer is there: the frame loop
// creates the renderer after the modules start, this one included.
void ensure_white(Engine& e, LevelState_& s) {
    if (!s.white_material.is_null()) return;
    Renderer* r = renderer_get(e);
    if (!r) return;
    TextureDesc td;
    td.name = "level.white";
    s.white_texture = renderer_texture_create(r, td);
    const u8 white[4] = {255, 255, 255, 255};
    renderer_texture_upload(r, s.white_texture, 1, 1, white, sizeof white);
    MaterialDesc md;
    md.texture = s.white_texture;
    md.blend   = BlendMode::Alpha;
    s.white_material = renderer_material_create(r, md);
}

// The fade quad: one screen-space sprite at the top layer, far larger than any window.
void set_fade(TickContext& ctx, LevelState_& s) {
    ensure_white(*ctx.engine, s);
    Table* t = ctx.write_kind(s.fade_kind);
    if (!t) return;
    if (s.fade_entity.is_null() && s.fade > 0.0f && !s.white_material.is_null()) {
        Sprite sp;
        sp.material = s.white_material;
        sp.size  = vec2{100000.0f, 100000.0f};
        sp.layer = 127;
        sp.space = SpriteSpace::Screen;
        s.fade_entity = t->spawn(transform2_at(point2{0.0f, 0.0f}), sp);
    }
    if (Sprite* sp = t->get<Sprite>(s.fade_entity)) {
        color c = s.now.t.tint;
        c.a *= s.fade;
        sp->tint = c;
        if (s.fade <= 0.0f) { t->destroy(s.fade_entity); s.fade_entity = Handle{}; }
    }
}

// Seconds to a fraction per tick; 0 seconds is all at once.
// ---- per-level state -------------------------------------------------------------------------------

bool persisted(const LevelState_& s, const Table& t) {
    for (u32 i = 0; i < s.persist_count; ++i) if (!strcmp(s.persist[i], t.name)) return true;
    return false;
}

// What the level is now, into its record: every named entity of the persisted kinds, and the
// names it once had and has no longer (its build's, and those the record already said).
void snapshot(Engine& e, LevelState_& s, u32 level) {
    if (level >= s.count || !s.persist_count) return;
    if (s.forgotten[level]) { s.forgotten[level] = false; return; }
    MemScope scope;
    Array<u64> gone = array_in<u64>(mem_scratch());
    for (u64 n : s.built[level]) if (engine_entity_find(e, Name{n}).is_null()) gone.push(n);
    if (s.record[level].len) {
        JsonDoc doc;
        if (json_parse(s.record[level].data, s.record[level].len, mem_scratch(), doc)) {
            const JsonValue* old = json_get(doc, doc.root(), "gone");
            for (const JsonValue* g = old ? json_first(doc, *old) : nullptr; g; g = json_next(doc, *g)) {
                Name n = name_intern(json_string(g, ""));
                bool have = false;
                for (u64 x : gone) have |= x == n.hash;
                if (!have && engine_entity_find(e, n).is_null()) gone.push(n.hash);
            }
        }
    }
    SaveWriter w;
    w.init(mem_scratch(), SaveFormat::Json, false);
    w.begin_object();
    w.key("rows");
    w.begin_object();
    for (Table& t : e.tables) {
        if (!persisted(s, t)) continue;
        for (u32 i = 0; i < t.names.count(); ++i) {
            if (!t.sparse_live(t.names, i)) continue;
            Handle h = t.handle_at(t.names.slots[i]);
            w.key(name_str(*(const Name*)t.names.at(i)));
            save_row(w, t, h);
        }
    }
    w.end_object();
    w.key("gone");
    w.begin_array(true);
    for (u64 n : gone) w.string_value(name_str(Name{n}));
    w.end_array();
    w.end_object();
    if (!w.finish()) return;
    s.record[level].clear();
    memcpy(s.record[level].push_n(w.out.len), w.out.data, w.out.len);
}

// After the build's handlers have spawned the level from its map: the names it made, then
// what the record says: the gone destroyed, the rest given their rows back.
void apply(Engine& e, LevelState_& s, u32 level) {
    if (level >= s.count || !s.persist_count) return;
    s.built[level].clear();
    for (Table& t : e.tables) {
        if (!persisted(s, t)) continue;
        for (u32 i = 0; i < t.names.count(); ++i)
            if (t.sparse_live(t.names, i)) s.built[level].push(((const Name*)t.names.at(i))->hash);
    }
    if (!s.record[level].len) return;
    MemScope scope;
    JsonDoc doc;
    if (!json_parse(s.record[level].data, s.record[level].len, mem_scratch(), doc)) return;
    u32 destroyed = 0, restored = 0;
    const JsonValue* gone = json_get(doc, doc.root(), "gone");
    for (const JsonValue* g = gone ? json_first(doc, *gone) : nullptr; g; g = json_next(doc, *g)) {
        Handle h = engine_entity_find(e, json_string(g, ""));
        Table* t = h.is_null() ? nullptr : engine_table_of(e, h);
        if (t && persisted(s, *t) && t->destroy(h)) ++destroyed;
    }
    const JsonValue* rows = json_get(doc, doc.root(), "rows");
    for (const JsonValue* r = rows ? json_first(doc, *rows) : nullptr; r; r = json_next(doc, *r)) {
        Handle h = engine_entity_find(e, r->key);
        Table* t = h.is_null() ? nullptr : engine_table_of(e, h);
        if (t && persisted(s, *t) && load_row(doc, *r, *t, h)) ++restored;
    }
    KV_INFO("level", "'%s' remembers: %u restored, %u gone", s.levels[level].name, restored, destroyed);
}

f32 fade_step(f64 dt, f32 seconds) { return seconds > 0.0f ? (f32)(dt / seconds) : 1.0f; }

void begin(Engine& e, LevelState_& s) {
    snapshot(e, s, s.current); // the level being left, as it is
    s.now = s.next;
    s.next.pending = false;
    s.reload  = s.now.level == s.current && s.current != KV_LEVELS;
    s.leaving = s.current;
    if (s.reload) utf8_copy(s.now.entry, sizeof s.now.entry, "reload");
    enter_state(s, LevelState::Leaving);
}

// The sim stands still from the moment the transition starts (the default), or from when
// the screen is covered.
void hold_from_start(Engine& e, const LevelState_& s) {
    if (s.now.t.freeze) engine_hold(e, HoldLoading, true);
}

// Phase PreSim, main thread, every tick: one step of the transition.
void level_run(TickContext& ctx) {
    LevelState_& s = ctx.state_as<LevelState_>();
    Engine& e = *ctx.engine;
    ++s.ticks_in_state;
    switch (s.state) {
    case LevelState::None:
    case LevelState::Active:
        if (s.next.pending) {
            begin(e, s);
            hold_from_start(e, s);
            ctx.emit(LevelLeaving{s.leaving});
            if (s.leaving == KV_LEVELS) s.fade = 1.0f; // the first level comes out of black
        }
        break;
    case LevelState::Leaving:
        s.fade += fade_step(ctx.dt, s.now.t.fade_out);
        if (s.fade < 1.0f) break;
        s.fade = 1.0f;
        engine_hold(e, HoldLoading, true);
        if (s.reload) {
            // Everything of the level goes first, so the files are read again; the new
            // scope is made on the next tick, after the asset pump has retired them.
            if (Table* roots = ctx.write_kind(s.root_kind)) roots->destroy(s.root);
            asset_scope_release(e, s.scope);
            s.root = s.scope = s.map = Handle{};
        }
        enter_state(s, LevelState::Loading);
        break;
    case LevelState::Loading:
        if (s.new_scope.is_null()) {
            if (s.reload && s.ticks_in_state < 2) break; // let one asset pump run between release and load
            const Entry& lv = s.levels[s.now.level];
            s.new_scope = asset_scope_create(e, lv.name);
            s.new_map   = s.load ? s.load(e, lv.map, s.new_scope) : asset_load_blob(e, lv.map, s.new_scope);
            ctx.emit(LevelLoading{s.now.level, s.new_scope});
            KV_INFO("level", "loading '%s' from '%s'", lv.name, lv.map);
            break;
        }
        if (asset_scope_progress(e, s.new_scope).pending) break;
        enter_state(s, LevelState::Building);
        [[fallthrough]];
    case LevelState::Building: {
        Table* roots = ctx.write_kind(s.root_kind);
        // Two ticks. The first takes the old level down: its root (the scene module takes
        // the children at the end of this tick, the tilemap its tiles at its next pump) and
        // its scope (after the new scope holds what both use). The second builds the new
        // one. No tick ever shows a new level's entities beside an old level's.
        if (!s.root.is_null() || !s.scope.is_null()) {
            if (roots && !s.root.is_null()) roots->destroy(s.root);
            if (!s.scope.is_null()) asset_scope_release(e, s.scope);
            s.root = s.scope = s.map = Handle{};
            break;
        }
        s.scope = s.new_scope; s.map = s.new_map;
        s.new_scope = s.new_map = Handle{};
        s.root = roots ? roots->spawn(transform2_at(point2{0.0f, 0.0f})) : Handle{};
        // Named, so a handle to it saved by one run means the root of another (the old one
        // is gone by now, two ticks back).
        if (!s.root.is_null()) engine_entity_name_set(*ctx.engine, s.root, "level.root");
        s.current = s.now.level;
        LevelBuild b{s.now.level, s.root, s.map, s.scope, {}};
        utf8_copy(b.entry, sizeof b.entry, s.now.entry);
        ctx.emit(b);
        AssetScopeProgress p = asset_scope_progress(e, s.scope);
        KV_INFO("level", "built '%s': %u assets (%u failed), %llu KB, at tick %llu", s.levels[s.current].name, p.members, p.failed,
                (unsigned long long)(p.bytes / 1024), (unsigned long long)ctx.tick);
        enter_state(s, LevelState::Entering);
        break;
    }
    case LevelState::Entering:
        if (s.ticks_in_state == 1) { // the build's handlers ran on a held tick
            apply(e, s, s.current);
            engine_hold(e, HoldLoading, false);
            break;
        }
        s.fade -= fade_step(ctx.dt, s.now.t.fade_in);
        if (s.fade > 0.0f) break;
        s.fade = 0.0f;
        enter_state(s, LevelState::Active);
        ctx.emit(LevelEntered{s.current});
        break;
    }
    set_fade(ctx, s);
}

// ---- CLI -----------------------------------------------------------------------------------------

void cmd_list(CliContext& ctx, Span<const char* const>) {
    LevelState_* s = state(*ctx.engine);
    if (!s) { ctx.fail("the level module is not up"); return; }
    for (u32 i = 0; i < s->count; ++i)
        ctx.print("%s%-20s %s", i == s->current ? "* " : "  ", s->levels[i].name, s->levels[i].map);
    ctx.print("state %s", state_name(s->state));
}

void cmd_load(CliContext& ctx, Span<const char* const> args) {
    if (args.len != 1) { ctx.fail("level load <name>"); return; }
    if (!level_request(*ctx.engine, args[0])) { ctx.fail("no level named '%s'", args[0]); return; }
    ctx.print("loading '%s'", args[0]);
}

void cmd_reload(CliContext& ctx, Span<const char* const>) {
    if (!level_reload(*ctx.engine)) { ctx.fail("no level is loaded"); return; }
    ctx.print("reloading '%s'", level_name(*ctx.engine, level_current(*ctx.engine)));
}

bool level_startup(Engine& e, ModuleInstance& self) {
    LevelState_* s = arena_push<LevelState_>(self.arena);
    if (!s) return false;
    s->root_kind = engine_kind_id(e, "level.root");
    s->general = &self.general;
    for (u32 i = 0; i < KV_LEVELS; ++i) { s->record[i].alloc = s->built[i].alloc = &self.general; }
    s->fade_kind = engine_kind_id(e, "level.fade");
    self.set_state(s);
    return true;
}

void level_shutdown(Engine& e, ModuleInstance& self) {
    LevelState_* s = self.state_as<LevelState_>();
    if (!s) return;
    for (u32 i = 0; i < KV_LEVELS; ++i) { s->record[i].free(); s->built[i].free(); }
    if (!s->new_scope.is_null()) asset_scope_release(e, s->new_scope);
    if (!s->scope.is_null()) asset_scope_release(e, s->scope);
}

constinit const KindDesc k_kinds[] = {
    {.name = ".root", .components = "Transform2", .capacity = 4},
    // The fade is cosmetic: its alpha follows wall-clock ticks, held ones included.
    {.name = ".fade", .components = "Transform2, Sprite", .capacity = 2, .cosmetic = true},
};
constinit const EventTypeDesc k_events[] = {KV_EVENT(LevelLeaving), KV_EVENT(LevelLoading), KV_EVENT(LevelBuild), KV_EVENT(LevelEntered)};
constinit const SystemDesc k_systems[] = {
    // Main thread, after the asset pumps: loads and releases go through the asset module's
    // functions, as any main-thread code's do, so its tables are not this system's to declare.
    {.name = ".run", .phase = Phase::PreSim, .fn = level_run, .writes = ".root, .fade",
     .emits = "LevelLeaving, LevelLoading, LevelBuild, LevelEntered", .after = "asset.pump_scopes, asset.pump, asset.pump_materials, asset.pump_fonts, asset.pump_sounds, asset.pump_blobs",
     .main_thread = true, .runs = RunWhen::Always},
};
constinit const CliCommandDesc k_commands[] = {
    {.name = "level list", .help = "the catalog, the current level marked", .fn = cmd_list},
    {.name = "level load", .help = "<name>: go there", .fn = cmd_load},
    {.name = "level reload", .help = "read the current level's files again and rebuild it", .fn = cmd_reload},
};

} // namespace

KV_MODULE_DEFINE(level_module) = {
    .name     = "level",
    .events   = k_events,
    .kinds    = k_kinds,
    .systems  = k_systems,
    .commands = k_commands,
    .startup  = level_startup,
    .shutdown = level_shutdown,
    .memory   = {.reserve = 64 * 1024},
};

bool level_catalog(Engine& e, const LevelConfig& config) {
    LevelState_* s = state(e);
    if (!s) return false;
    if (config.levels.len > KV_LEVELS) { KV_ERROR("level", "%u levels, more than %u", (u32)config.levels.len, KV_LEVELS); return false; }
    s->count = 0;
    for (const LevelDesc& d : config.levels) {
        Entry& en = s->levels[s->count++];
        utf8_copy(en.name, sizeof en.name, d.name);
        utf8_copy(en.map, sizeof en.map, d.map);
        en.user = d.user;
    }
    s->load = config.load;
    return true;
}

u32 level_find(Engine& e, const char* name) {
    LevelState_* s = state(e);
    for (u32 i = 0; s && name && i < s->count; ++i)
        if (strcmp(s->levels[i].name, name) == 0) return i;
    return KV_LEVELS;
}

bool level_request(Engine& e, const char* name, const LevelTransition& t) {
    LevelState_* s = state(e);
    u32 i = level_find(e, name);
    if (!s || i == KV_LEVELS) return false;
    s->next.pending = true;
    s->next.level   = i;
    s->next.t       = t;
    utf8_copy(s->next.entry, sizeof s->next.entry, t.entry ? t.entry : "");
    s->next.t.entry = nullptr;
    return true;
}

bool level_reload(Engine& e, const LevelTransition& t) {
    LevelState_* s = state(e);
    if (!s || s->current == KV_LEVELS) return false;
    return level_request(e, s->levels[s->current].name, t);
}

bool level_persist(Engine& e, Span<const char* const> kinds) {
    LevelState_* s = state(e);
    if (!s || kinds.len > KV_COUNTOF(s->persist)) return false;
    s->persist_count = 0;
    for (const char* k : kinds) {
        if (!engine_table(e, k)) { KV_ERROR("level", "no kind '%s' to remember", k); return false; }
        utf8_copy(s->persist[s->persist_count++], KV_KIND_NAME_MAX, k);
    }
    return true;
}

void level_save(SaveWriter& w, Engine& e) {
    LevelState_* s = state(e);
    w.begin_object();
    if (s) {
        if (s->state == LevelState::Active) snapshot(e, *s, s->current); // the live level, as it is now
        for (u32 i = 0; i < s->count; ++i) {
            if (!s->record[i].len) continue;
            MemScope scope;
            JsonDoc doc;
            if (!json_parse(s->record[i].data, s->record[i].len, mem_scratch(), doc)) continue;
            w.key(s->levels[i].name);
            save_json(w, doc, doc.root());
        }
    }
    w.end_object();
}

u32 level_load(Engine& e, const JsonDoc& doc, const JsonValue& v) {
    LevelState_* s = state(e);
    if (!s || !v.is_object()) return 0;
    u32 n = 0;
    for (u32 i = 0; i < s->count; ++i) s->record[i].clear();
    if (s->current < KV_LEVELS) s->forgotten[s->current] = true; // what the save says, not what is here
    for (const JsonValue* lv = json_first(doc, v); lv; lv = json_next(doc, *lv)) {
        u32 i = level_find(e, lv->key);
        if (i == KV_LEVELS) { KV_WARN("level", "the save remembers a level '%s' this game lacks", lv->key); continue; }
        MemScope scope;
        SaveWriter w;
        w.init(mem_scratch(), SaveFormat::Json, false);
        save_json(w, doc, *lv);
        if (!w.finish()) continue;
        memcpy(s->record[i].push_n(w.out.len), w.out.data, w.out.len);
        ++n;
    }
    return n;
}

void level_forget(Engine& e, const char* name) {
    LevelState_* s = state(e);
    u32 i = level_find(e, name);
    if (s && i < KV_LEVELS) { s->record[i].clear(); s->built[i].clear(); s->forgotten[i] = i == s->current; }
}

u32         level_current(Engine& e) { LevelState_* s = state(e); return s ? s->current : KV_LEVELS; }
const char* level_name(Engine& e, u32 level) { LevelState_* s = state(e); return s && level < s->count ? s->levels[level].name : nullptr; }
LevelState  level_state(Engine& e) { LevelState_* s = state(e); return s ? s->state : LevelState::None; }
Handle      level_root(Engine& e) { LevelState_* s = state(e); return s ? s->root : Handle{}; }
Handle      level_scope(Engine& e) { LevelState_* s = state(e); return s ? s->scope : Handle{}; }
Handle      level_map(Engine& e) { LevelState_* s = state(e); return s ? s->map : Handle{}; }
u64         level_user(Engine& e, u32 level) { LevelState_* s = state(e); return s && level < s->count ? s->levels[level].user : 0; }

} // namespace kv
