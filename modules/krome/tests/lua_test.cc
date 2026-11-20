// Krome's Lua: screens as scripts, the model, events both ways, the trace, and the state hash
// with Krome running against the same run without it.
#include "korvid/krome/krome.h"
#include "../src/internal.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/ktable.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include "korvid/platform/io.h"
#include "korvid/platform/module.h"
#include "korvid/platform_hid/module.h"
#include "korvid/asset/module.h"
#include <string.h>

namespace {

using namespace kv;

// A sim to hash: one row that counts up.
struct Count { i32 n; };
constinit const FieldDesc k_count_fields[] = {KV_FIELDS(Count, n)};
constinit const ComponentDesc k_components[] = {KV_COMPONENT(Count, k_count_fields, 0)};
constinit const KTableDesc k_ktables[] = {{.name = ".thing", .components = "Count", .capacity = 8}};

struct Seen { u32 events; u64 save_node; f64 note_value; u32 notes, saves; };
constinit Seen g_seen = {};

void sys_count(TickContext& ctx) {
    for (Count& k : ctx.write<Count>("kt.thing.Count")) k.n += 1;
}
void sys_watch(TickContext& ctx) {
    for (const KromeEvent& ev : ctx.received<KromeEvent>()) {
        ++g_seen.events;
        if (ev.event == name_intern("note").hash) { ++g_seen.notes; g_seen.note_value = ev.value; if (ev.node) g_seen.save_node = ~0ull; }
        if (ev.event == name_intern("save").hash) { ++g_seen.saves; g_seen.save_node = ev.node; }
    }
}
bool kt_startup(Engine& e, ModuleInstance&) {
    engine_ktable(e, "kt.thing")->spawn(Count{0});
    return true;
}
constinit const SystemDesc k_systems[] = {
    {.name = ".count", .phase = Phase::Sim, .fn = sys_count, .writes = "kt.thing.Count"},
    {.name = ".watch", .phase = Phase::Sim, .fn = sys_watch, .reads = "KromeEvent"},
};
KV_MODULE_DEFINE(k_kt) = {.name = "kt", .components = k_components, .ktables = k_ktables, .systems = k_systems, .startup = kt_startup};
constinit const SystemDesc k_quiet_systems[] = {{.name = ".count", .phase = Phase::Sim, .fn = sys_count, .writes = "kt.thing.Count"}};
KV_MODULE_DEFINE(k_kt_quiet) = {.name = "kt", .components = k_components, .ktables = k_ktables, .systems = k_quiet_systems, .startup = kt_startup};

const char* k_form = R"lua(
return {
  model = { name = "Rowan", saved = 0 },
  on = {
    save = function(id, value)
      model.saved = model.saved + 1
      emit("note", 7)
    end,
  },
  root = col { pad = 12, gap = "8", bg = "#202430", radius = { 4, 8 },
    text "Name: {name}",
    row { id = "bar", w = "120..400", fill = true,
      text { "Saved {saved} times", color = "#ff0000" },
      hit { id = "save", event = "save", bg = "#334455", hover = { bg = "#445566" },
        text "Save",
      },
    },
    text "{count} and {flag}",
  },
}
)lua";

const f64 kDt = 1.0 / 60.0;

void frames(Engine& e, u32 n) {
    for (u32 i = 0; i < n; ++i) { platform_pump(e); engine_frame(e, kDt); engine_frame_cycle(e, kDt); }
}

u64 run_hash(bool with_krome, u32 ticks) {
    const ModuleDesc* const with[] = {&platform_module, &platform_hid_module, &asset_module, &krome_module, &k_kt};
    const ModuleDesc* const without[] = {&platform_module, &k_kt_quiet};
    Engine e;
    if (with_krome ? !engine_init(e, with) : !engine_init(e, without)) return 0;
    Name form = with_krome ? krome_open_source(e, "kt/hash", k_form) : Name{};
    for (u32 i = 0; i < ticks; ++i) {
        frames(e, 1);
        if (with_krome && i % 3 == 1) krome_press(e, form, "save");
        if (with_krome && i % 5 == 2) krome_set(e, form, "name", "Ada");
    }
    u64 h = engine_hash_state(e);
    engine_shutdown(e);
    return h;
}

} // namespace

void test_lua(kv::TestCtx& c) {
    using namespace kv;
    c.begin("krome lua");
    g_seen = Seen{};
    const ModuleDesc* const list[] = {&platform_module, &platform_hid_module, &asset_module, &krome_module, &k_kt};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    KromeModule* s = krome_state(e);
    KV_CHECK(c, s && s->L);

    // A screen from a source: the tree as the helpers made it, the model filled in.
    Name form = krome_open_source(e, "kt/form", k_form);
    KV_CHECK(c, !krome_ready(e, form));
    frames(e, 1);
    KV_CHECK(c, krome_ready(e, form));
    const KromeTree* t = krome_tree(e, form);
    KV_CHECK(c, t && t->nodes.len == 7);
    const KromeNode& root = t->nodes[t->root];
    KV_CHECK(c, root.dir == KromeDir::Col && root.pad[0] == 12 && root.gap == 8 && root.line == 10);
    KV_CHECK(c, t->looks[root.look].radius[1] == 8 && t->looks[root.look].radius[2] == 4);
    u32 bar = krome_find(*t, name_intern("bar")), save = krome_find(*t, name_intern("save"));
    KV_CHECK(c, bar != KROME_NONE && t->nodes[bar].dir == KromeDir::Row && t->nodes[bar].w.lo == 120 && t->nodes[bar].w.hi == 400 && (t->nodes[bar].flags & KROME_FILL));
    KV_CHECK(c, save != KROME_NONE && t->nodes[save].type == KromeType::Hit && t->nodes[save].event == name_intern("save") && t->nodes[save].line == 14);
    KV_CHECK(c, t->nodes[save].states[(u32)KromeState::Hover] != 0);
    u32 name_text = t->nodes[t->root].first;
    KV_CHECK(c, strcmp(t->text(name_text), "Name: Rowan") == 0);
    u32 saved_text = t->nodes[bar].first;
    KV_CHECK(c, strcmp(t->text(saved_text), "Saved 0 times") == 0);
    u32 last = t->nodes[t->root].last;
    KV_CHECK(c, strcmp(t->text(last), " and ") == 0);
    KV_CHECK(c, s->script_errors == 0);

    // Values from C++ arrive as events, and the texts that read them follow.
    krome_set(e, form, "name", "Ada");
    krome_set(e, form, "count", 3.0);
    krome_set(e, form, "flag", 2.5);
    frames(e, 1);
    t = krome_tree(e, form);
    KV_CHECK(c, strcmp(t->text(name_text), "Name: Ada") == 0 && strcmp(t->text(last), "3 and 2.5") == 0);
    krome_set_bool(e, form, "flag", true);
    frames(e, 1);
    KV_CHECK(c, strcmp(krome_tree(e, form)->text(last), "3 and true") == 0 && strcmp(krome_model_text(e, form, "count"), "3") == 0);

    // A press: the handler runs (the model, an emit), and KromeEvent reaches C++.
    KV_CHECK(c, krome_press(e, form, "save"));
    KV_CHECK(c, strcmp(krome_tree(e, form)->text(saved_text), "Saved 1 times") == 0);
    frames(e, 2);
    KV_CHECK(c, g_seen.events == 2 && g_seen.notes == 1);
    KV_CHECK(c, !krome_press(e, form, "bar") && !krome_press(e, form, "nothing") && !krome_press(e, Name{}, "save"));
    // The press's event names its node; the script's names none.
    KV_CHECK(c, g_seen.saves == 1 && g_seen.save_node == name_intern("save").hash && g_seen.note_value == 7.0);
#if KV_DEV
    // Traced: the dev ring holds Krome's events.
    bool traced = false;
    for (const TraceRecord& r : e.trace.ring) if (r.type == EventType<KromeEvent>::id && r.tick) traced = true;
    KV_CHECK(c, traced);
#endif

    // A script that returns a node, not a screen table; one that does not compile; one that
    // errors when it runs; one with a property that does not exist (it opens, with errors).
    Name bare = krome_open_source(e, "kt/bare", "return row { text 'a', text 'b' }");
    Name broken = krome_open_source(e, "kt/broken", "return col {");
    Name raising = krome_open_source(e, "kt/raising", "error('no')");
    Name odd = krome_open_source(e, "kt/odd", "return col { gapp = 3, text { 'x', bg = 'blue' } }");
    frames(e, 1);
    KV_CHECK(c, krome_ready(e, bare) && krome_tree(e, bare)->nodes.len == 3);
    KV_CHECK(c, !krome_ready(e, broken) && !krome_ready(e, raising));
    KV_CHECK(c, krome_ready(e, odd) && s->script_errors == 4); // broken, raising, and two in odd
    // Reopening a name replaces the screen.
    krome_open_source(e, "kt/bare", "return text 'only'");
    frames(e, 1);
    KV_CHECK(c, krome_tree(e, bare)->nodes.len == 1);
    krome_close(e, bare);
    KV_CHECK(c, krome_tree(e, bare) == nullptr);

    // A screen from an asset, read through the I/O queue.
    char scratch[400], dir[500], path[600];
    KV_CHECK(c, io_make_temp_dir("korvid-krome-test", scratch, sizeof scratch) && platform_io(e)->mount_dir(scratch));
    format(dir, sizeof dir, "%s/kt", scratch);
    io_make_dirs(dir);
    format(path, sizeof path, "%s/screen.lua", dir);
    const char* script = "return col { text 'from a file' }";
    KV_CHECK(c, io_write_now(path, script, strlen(script)));
    Name file = krome_open(e, "kt/screen.lua");
    for (u32 i = 0; i < 2000 && !krome_ready(e, file); ++i) { frames(e, 1); time_sleep(0.001); }
    KV_CHECK(c, krome_ready(e, file) && strcmp(krome_tree(e, file)->text(1), "from a file") == 0);
    Name missing = krome_open(e, "kt/missing.lua");
    for (u32 i = 0; i < 2000; ++i) {
        frames(e, 1);
        KromeScreen* sc = krome_screen(*s, missing);
        if (sc && !sc->loading) break;
        time_sleep(0.001);
    }
    KV_CHECK(c, !krome_ready(e, missing) && krome_screen(*s, missing)->failed);
    engine_shutdown(e);
    io_remove_tree(scratch);

    // Cosmetic: a run with Krome pressing and setting hashes as the same run without it.
    u64 a = run_hash(true, 40), b = run_hash(false, 40);
    KV_CHECK(c, a != 0 && a == b);
}
