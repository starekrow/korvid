#include "korvid/level/module.h"
#include "korvid/level/level.h"
#include "korvid/asset/blob.h"
#include "korvid/asset/scope.h"
#include "korvid/core/cli.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/utf8.h"
#include "korvid/render/renderer.h"
#include "korvid/sprite/sprite.h"
#include "korvid/sprite/transform2.h"
#include <stdio.h>
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
};

LevelState_* state(Engine& e) { return engine_module_state_as<LevelState_>(e, "level"); }

void enter_state(LevelState_& s, LevelState st) { s.state = st; s.ticks_in_state = 0; }

const char* state_name(LevelState st) {
    static const char* const names[] = {"none", "leaving", "loading", "building", "entering", "active"};
    return names[(u32)st];
}

// The fade quad: one screen-space sprite at the top layer, far larger than any window.
void set_fade(TickContext& ctx, LevelState_& s) {
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
f32 fade_step(f64 dt, f32 seconds) { return seconds > 0.0f ? (f32)(dt / seconds) : 1.0f; }

void begin(LevelState_& s) {
    s.now = s.next;
    s.next.pending = false;
    s.reload  = s.now.level == s.current && s.current != KV_LEVELS;
    s.leaving = s.current;
    if (s.reload) utf8_copy(s.now.entry, sizeof s.now.entry, "reload");
    enter_state(s, LevelState::Leaving);
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
            begin(s);
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
        if (roots && !s.root.is_null()) roots->destroy(s.root); // its children go with it (scene)
        if (!s.scope.is_null()) asset_scope_release(e, s.scope); // after the new scope holds what both use
        s.scope = s.new_scope; s.map = s.new_map;
        s.new_scope = s.new_map = Handle{};
        s.root = roots ? roots->spawn(transform2_at(point2{0.0f, 0.0f})) : Handle{};
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
        if (s.ticks_in_state == 1) { engine_hold(e, HoldLoading, false); break; } // the build's handlers ran on a held tick
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
    s->fade_kind = engine_kind_id(e, "level.fade");
    if (Renderer* r = renderer_get(e)) {
        TextureDesc td;
        td.name = "level.white";
        s->white_texture = renderer_texture_create(r, td);
        const u8 white[4] = {255, 255, 255, 255};
        renderer_texture_upload(r, s->white_texture, 1, 1, white, sizeof white);
        MaterialDesc md;
        md.texture = s->white_texture;
        md.blend   = BlendMode::Alpha;
        s->white_material = renderer_material_create(r, md);
    }
    self.set_state(s);
    return true;
}

void level_shutdown(Engine& e, ModuleInstance& self) {
    LevelState_* s = self.state_as<LevelState_>();
    if (!s) return;
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
     .after = "asset.pump_scopes, asset.pump, asset.pump_materials, asset.pump_fonts, asset.pump_sounds, asset.pump_blobs",
     .main_thread = true, .runs = RunWhen::Always},
};
constinit const CliCommandDesc k_commands[] = {
    {.name = "level list", .help = "level list: the catalog, the current level marked", .fn = cmd_list},
    {.name = "level load", .help = "level load <name>: go there", .fn = cmd_load},
    {.name = "level reload", .help = "level reload: read the current level's files again and rebuild it", .fn = cmd_reload},
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

u32         level_current(Engine& e) { LevelState_* s = state(e); return s ? s->current : KV_LEVELS; }
const char* level_name(Engine& e, u32 level) { LevelState_* s = state(e); return s && level < s->count ? s->levels[level].name : nullptr; }
LevelState  level_state(Engine& e) { LevelState_* s = state(e); return s ? s->state : LevelState::None; }
Handle      level_root(Engine& e) { LevelState_* s = state(e); return s ? s->root : Handle{}; }
Handle      level_scope(Engine& e) { LevelState_* s = state(e); return s ? s->scope : Handle{}; }
Handle      level_map(Engine& e) { LevelState_* s = state(e); return s ? s->map : Handle{}; }
u64         level_user(Engine& e, u32 level) { LevelState_* s = state(e); return s && level < s->count ? s->levels[level].user : 0; }

} // namespace kv
