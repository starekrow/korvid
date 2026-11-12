// Levels without a renderer (no fade quad; every step and hold the same): the first
// level comes up; a switch keeps what both levels load (one read) and retires the rest;
// the old level's children go with its root; no sim tick runs while the level loads;
// reload reads the files again; a request mid-transition waits; the CLI's commands.
#include "korvid/level/level.h"
#include "korvid/level/module.h"
#include "korvid/asset/blob.h"
#include "korvid/asset/module.h"
#include "korvid/asset/scope.h"
#include "korvid/cli/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include "korvid/platform/io.h"
#include "korvid/platform/module.h"
#include "korvid/save/save.h"
#include "korvid/scene/module.h"
#include "korvid/scene/parent.h"
#include "korvid/sprite/module.h"
#include "korvid/sprite/transform2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

using namespace kv;

struct Seen {
    u32    leaving = 0, loading = 0, built = 0, entered = 0;
    u32    last_level = 99;
    char   entry[32] = {};
    Handle root, map, shared, child;
    u64    sim_ticks = 0, loading_at = 0, built_at = 0, sim_while_loading = 0;
    bool   load_shared = true;
};
Seen g;

void on_leaving(TickContext&, Span<const LevelLeaving> es) { g.leaving += es.len; }
void on_loading(TickContext& ctx, Span<const LevelLoading> es) {
    for (const LevelLoading& l : es) {
        ++g.loading;
        g.loading_at = ctx.engine->clock.tick_count;
        if (g.load_shared) g.shared = asset_load_blob(*ctx.engine, "level_test/shared.bin", l.scope);
    }
}
void on_build(TickContext& ctx, Span<const LevelBuild> es) {
    for (const LevelBuild& b : es) {
        ++g.built;
        g.last_level = b.level; g.root = b.root; g.map = b.map;
        snprintf(g.entry, sizeof g.entry, "%s", b.entry);
        g.built_at = ctx.engine->clock.tick_count;
        g.sim_while_loading += g.built_at - g.loading_at; // the sim stood still from the load to the build
        Table* things = ctx.write_kind("lt.thing");
        Parent p; p.parent = b.root;
        g.child = things->spawn(transform2_at(point2{0, 0}), local2_at(point2{1, 2}), p);
        if (b.level == 0) { // the town's two named stones, as a map would make them
            engine_entity_name_set(*ctx.engine, things->spawn(transform2_at(point2{0, 0}), local2_at(point2{10, 0}), p), "town#a");
            engine_entity_name_set(*ctx.engine, things->spawn(transform2_at(point2{0, 0}), local2_at(point2{20, 0}), p), "town#b");
        }
    }
}
void on_entered(TickContext&, Span<const LevelEntered> es) { g.entered += es.len; }
void sim_count(TickContext&) { ++g.sim_ticks; }

constinit const KindDesc k_kinds[] = {{.name = ".thing", .components = "Transform2, Local2, Parent", .capacity = 8}};
constinit const HandlerDesc k_handlers[] = {
    {.name = ".leaving", KV_ON(LevelLeaving, on_leaving), .phase = Phase::Sim},
    {.name = ".loading", KV_ON(LevelLoading, on_loading), .phase = Phase::Sim},
    {.name = ".build", KV_ON(LevelBuild, on_build), .phase = Phase::Sim, .writes = ".thing"},
    {.name = ".entered", KV_ON(LevelEntered, on_entered), .phase = Phase::Sim},
};
constinit const SystemDesc k_systems[] = {{.name = ".count", .phase = Phase::Sim, .fn = sim_count}};
KV_MODULE_DEFINE(k_lt) = {.name = "lt", .kinds = k_kinds, .systems = k_systems, .handlers = k_handlers};

bool write(const char* dir, const char* name, const char* text) {
    char path[600];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fputs(text, f);
    return fclose(f) == 0;
}

struct Lines { u32 n = 0; char text[16][128]; };
void collect(void* user, const char* line) { Lines& l = *(Lines*)user; if (l.n < 16) snprintf(l.text[l.n++], 128, "%s", line); }

} // namespace

void test_level(kv::TestCtx& c) {
    using namespace kv;
    c.begin("level");
    const ModuleDesc* const list[] = {&platform_module, &asset_module, &sprite_module, &scene_module, &cli_module, &level_module, &k_lt};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    char dir[512];
    // The fixtures go in a scratch directory mounted over the defaults, not beside the
    // executable: an iOS app's bundle is signed and read-only.
    char scratch[400];
    KV_CHECK(c, io_make_temp_dir("korvid-level-test", scratch, sizeof scratch) && platform_io(e)->mount_dir(scratch));
    snprintf(dir, sizeof dir, "%s/level_test", scratch);
    io_make_dirs(dir);
    KV_CHECK(c, write(dir, "town.map", "town v1") && write(dir, "field.map", "field v1") && write(dir, "shared.bin", "shared"));
    // The scratch mount is the directory these went into.

    const LevelDesc levels[] = {{"town", "level_test/town.map", 1}, {"field", "level_test/field.map", 2}};
    LevelConfig cfg;
    cfg.levels = levels;
    KV_CHECK(c, level_catalog(e, cfg) && level_find(e, "field") == 1 && level_find(e, "nowhere") == KV_LEVELS);
    KV_CHECK(c, !level_request(e, "nowhere") && !level_reload(e));
    auto frames = [&](u32 n) { for (u32 i = 0; i < n; ++i) { platform_pump(e); engine_frame(e, 1.0 / 60.0); } };
    // Frames until the level is in and the sim runs, a millisecond apart, for up to two
    // seconds: its reads take real time, not a number of frames (io_uring punts a read from
    // tmpfs to a worker thread).
    auto until_active = [&]() {
        frames(1);
        for (u32 i = 0; i < 2000 && !(level_state(e) == LevelState::Active && !e.clock.paused); ++i) { frames(1); time_sleep(0.001); }
    };

    // The first level.
    LevelTransition quick;
    quick.fade_out = 0.1f; quick.fade_in = 0.1f;
    KV_CHECK(c, level_request(e, "town", quick));
    until_active();
    KV_CHECK(c, level_state(e) == LevelState::Active && level_current(e) == 0 && strcmp(level_name(e, 0), "town") == 0);
    KV_CHECK(c, g.leaving == 1 && g.loading == 1 && g.built == 1 && g.entered == 1 && g.last_level == 0 && g.entry[0] == 0);
    const BlobAsset* map = asset_blob(e, g.map);
    KV_CHECK(c, map && map->state == AssetState::Ready && strcmp((const char*)map->data, "town v1") == 0);
    KV_CHECK(c, asset_scope_progress(e, level_scope(e)).members == 2 && level_root(e) == g.root);
    KV_CHECK(c, g.sim_while_loading == 0 && !e.clock.paused);
    Handle town_map = g.map, town_child = g.child, shared = g.shared;
    KV_CHECK(c, engine_table_of(e, town_child) && engine_table_of(e, town_child)->is_alive(town_child.index()));

    // A switch: the shared blob is one asset across both levels; the town's map retires;
    // the town's child goes with its root. No sim tick runs while held.
    u64 sim_before = g.sim_ticks;
    LevelTransition door = quick;
    door.entry = "east gate";
    KV_CHECK(c, level_request(e, "field", door));
    until_active();
    KV_CHECK(c, level_current(e) == 1 && g.built == 2 && g.entered == 2 && strcmp(g.entry, "east gate") == 0);
    KV_CHECK(c, g.shared == shared && asset_refs(e, shared) == 1 && asset_blob(e, town_map) == nullptr);
    KV_CHECK(c, !engine_table(e, "lt.thing")->valid(town_child) && engine_table(e, "lt.thing")->live() == 1);
    KV_CHECK(c, g.sim_while_loading == 0 && g.sim_ticks > sim_before);

    // Reload: the files are read again (a new map handle, the new text), the entry says so.
    KV_CHECK(c, write(dir, "field.map", "field v2"));
    Handle field_map = g.map;
    KV_CHECK(c, level_reload(e, quick));
    until_active();
    KV_CHECK(c, g.built == 3 && strcmp(g.entry, "reload") == 0 && g.map != field_map);
    map = asset_blob(e, g.map);
    KV_CHECK(c, map && strcmp((const char*)map->data, "field v2") == 0 && g.shared != shared);

    // Two requests mid-transition: the latest waits for the first to finish.
    KV_CHECK(c, level_request(e, "town", quick));
    frames(2);
    KV_CHECK(c, level_request(e, "field", quick) && level_request(e, "town", quick));
    until_active();
    KV_CHECK(c, level_current(e) == 0 && g.built == 4);
    frames(3);
    until_active();
    KV_CHECK(c, level_current(e) == 0 && g.built == 5 && strcmp(g.entry, "reload") == 0); // town again: the queued one

    // The CLI.
    Lines out;
    KV_CHECK(c, cli_run(e, "level list", collect, &out) && out.n == 3 && strstr(out.text[0], "* town"));
    out.n = 0;
    KV_CHECK(c, cli_run(e, "level load field", collect, &out) && strstr(out.text[0], "loading 'field'"));
    until_active();
    KV_CHECK(c, level_current(e) == 1 && level_user(e, 1) == 2);
    KV_CHECK(c, !cli_run(e, "level load nowhere", collect, &out));

    // Per-level state: the town's stones remembered by name, across a visit to the field,
    // and through a save.
    const char* const kept[] = {"lt.thing"};
    KV_CHECK(c, level_persist(e, kept));
    KV_CHECK(c, level_request(e, "town", quick));
    until_active();
    Table* things = engine_table(e, "lt.thing");
    Handle a = engine_entity_find(e, "town#a"), b = engine_entity_find(e, "town#b");
    KV_CHECK(c, !a.is_null() && !b.is_null() && things->get<Local2>(a)->position.x == 10);
    things->get<Local2>(a)->position.x = 55;   // pushed
    things->destroy(b);                        // broken
    KV_CHECK(c, level_request(e, "field", quick));
    until_active();
    KV_CHECK(c, engine_entity_find(e, "town#a").is_null());
    KV_CHECK(c, level_request(e, "town", quick));
    until_active();
    a = engine_entity_find(e, "town#a");
    KV_CHECK(c, !a.is_null() && things->get<Local2>(a)->position.x == 55 && engine_entity_find(e, "town#b").is_null());
    KV_CHECK(c, things->get<Parent>(a)->parent == level_root(e)); // the root by name: the new one
    // A save: the records go in; forgotten, the town is its map again; loaded, it remembers.
    things->get<Local2>(a)->position.x = 70;
    SaveWriter w;
    w.init(mem_general());
    level_save(w, e);
    KV_CHECK(c, w.finish() && strstr((const char*)w.out.data, "town#a") && strstr((const char*)w.out.data, "\"gone\""));
    level_forget(e, "town");
    KV_CHECK(c, level_reload(e, quick));
    until_active();
    KV_CHECK(c, things->get<Local2>(engine_entity_find(e, "town#a"))->position.x == 10 && !engine_entity_find(e, "town#b").is_null());
    JsonDoc doc;
    KV_CHECK(c, save_parse(SaveFormat::Json, w.out.data, w.out.len, mem_scratch(), doc) && level_load(e, doc, doc.root()) == 2); // the town and the field
    KV_CHECK(c, level_reload(e, quick));
    until_active();
    KV_CHECK(c, things->get<Local2>(engine_entity_find(e, "town#a"))->position.x == 70 && engine_entity_find(e, "town#b").is_null());
    w.free();

    engine_shutdown(e);
    io_remove_tree(scratch);
}
