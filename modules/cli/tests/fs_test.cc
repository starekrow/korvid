// The filesystem through cli_run: the root lists the mounts, a module is a directory under
// /ktables, rows and components and fields are nodes, a field patches through the schema,
// the clock patches through the engine, relative paths resolve against a working
// directory, and bad paths fail with their name.
#include "korvid/cli/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/test.h"
#include "korvid/platform/module.h"
#include <stdlib.h>
#include <string.h>

namespace {

using namespace kv;

struct Health { i32 hp; };
struct Pos { f32 x, y; f32 size[2]; };
struct Tag { char name[12]; };
constinit const FieldDesc k_health_fields[] = {KV_FIELDS(Health, hp)};
constinit const FieldDesc k_pos_fields[]    = {KV_FIELDS(Pos, x, y, size)};
constinit const FieldDesc k_tag_fields[]    = {KV_FIELDS(Tag, name)};
constinit const ComponentDesc k_components[] = {KV_COMPONENT(Health, k_health_fields, CompPrev), KV_COMPONENT(Pos, k_pos_fields), KV_COMPONENT(Tag, k_tag_fields)};
constinit const KTableDesc k_ktables[] = {
    {.name = ".ship", .components = "Health, Pos, Tag", .capacity = 8},
    {.name = ".rock", .components = "Pos", .capacity = 64},
};
constinit const ResourceDesc k_resources[] = {{".score"}};
void noop(TickContext&) {}
constinit const SystemDesc k_systems[] = {{.name = ".move", .phase = Phase::Sim, .fn = noop, .writes = "*.Pos"}};
constinit const TriggerDesc k_triggers[] = {{.name = ".death", .field = ".ship.Health.hp", .edge = TriggerEdge::CrossedBelow, .value = 1.0}};
KV_MODULE_DEFINE(k_g) = {.name = "game", .components = k_components, .ktables = k_ktables, .resources = k_resources, .systems = k_systems, .triggers = k_triggers};

// Collects a request's lines.
struct Out {
    char lines[32][256];
    u32  n = 0;
    void clear() { n = 0; }
    bool has(const char* needle) const { for (u32 i = 0; i < n; ++i) if (strstr(lines[i], needle)) return true; return false; }
};
void sink(void* user, const char* line) {
    Out& o = *(Out*)user;
    if (o.n < 32) { strncpy(o.lines[o.n], line, 255); o.lines[o.n][255] = 0; ++o.n; }
}

} // namespace

void test_cli_fs(kv::TestCtx& c) {
    using namespace kv;
    c.begin("cli_fs");
    const ModuleDesc* const list[] = {&platform_module, &cli_module, &k_g};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    KTable* ships = engine_ktable(e, "game.ship");
    KTable* rocks = engine_ktable(e, "game.rock");
    Handle ship = ships->spawn(Health{3}, Pos{1, 2, {5, 6}});
    for (u32 i = 0; i < 5; ++i) rocks->spawn(Pos{(f32)i, 0, {1, 1}});
    rocks->destroy(rocks->handle_at(2));
    Out o;
    auto run = [&](const char* line) { o.clear(); return cli_run(e, line, sink, &o); };

    // The root and a mount.
    KV_CHECK(c, run("ls /") && o.has("ktables/") && o.has("clock/") && o.has("systems/"));
    KV_CHECK(c, run("ls /ktables") && o.has("game/") && o.n == 1);
    KV_CHECK(c, run("ls /ktables/game") && o.has("ship/") && o.has("rock/") && o.has("1 live of 8"));
    KV_CHECK(c, run("ls /ktables/game/rock") && o.n == 4 && o.has("0/") && !o.has("2/")); // the dead slot is not listed

    // Rows, components, fields; patching a field goes through the schema.
    KV_CHECK(c, run("print /ktables/game/ship/0") && o.has("Health{hp=3}") && o.has("Pos{x=1, y=2, size=(5, 6)}"));
    KV_CHECK(c, run("ls /ktables/game/ship/0") && o.has("Health/") && o.has("Pos/"));
    KV_CHECK(c, run("ls /ktables/game/ship/0/Pos") && o.n == 3 && o.has("size") && o.has("5 6"));
    KV_CHECK(c, run("print /ktables/game/ship/0/Health/hp") && o.n == 1 && strcmp(o.lines[0], "3") == 0);
    KV_CHECK(c, run("patch /ktables/game/ship/0/Health/hp 1") && ships->get<Health>(ship)->hp == 1);
    KV_CHECK(c, run("patch /ktables/game/ship/0/Pos/x 7.5") && ships->get<Pos>(ship)->x == 7.5f);
    KV_CHECK(c, run("patch /ktables/game/ship/0/Pos/size 3 4") && ships->get<Pos>(ship)->size[0] == 3.0f && ships->get<Pos>(ship)->size[1] == 4.0f);
    KV_CHECK(c, !run("patch /ktables/game/ship/0/Pos/size 1") && o.has("takes 2 values"));
    KV_CHECK(c, !run("patch /ktables/game/ship/0/Pos/x 1 2") && o.has("takes 1 value"));
    // A string field: UTF-8 in and out, words joined, truncated at a character boundary.
    KV_CHECK(c, run("patch /ktables/game/ship/0/Tag/name \"Ga\xc3\xaf" "a\" one") && strcmp(ships->get<Tag>(ship)->name, "Ga\xc3\xaf" "a one") == 0);
    KV_CHECK(c, run("print /ktables/game/ship/0/Tag/name") && o.n == 1 && strcmp(o.lines[0], "\"Ga\xc3\xaf" "a one\"") == 0);
    KV_CHECK(c, run("ls /ktables/game/ship/0/Tag") && o.has("\"Ga\xc3\xaf" "a one\""));
    KV_CHECK(c, run("patch /ktables/game/ship/0/Tag/name \xe2\x80\x94\xe2\x80\x94\xe2\x80\x94\xe2\x80\x94") && strcmp(ships->get<Tag>(ship)->name, "\xe2\x80\x94\xe2\x80\x94\xe2\x80\x94") == 0);
    KV_CHECK(c, !run("print /ktables/game/ship/3") && o.has("no such path"));
    KV_CHECK(c, !run("ls /nowhere") && o.has("no such path: /nowhere"));

    // The engine's other mounts know this module.
    KV_CHECK(c, run("ls /systems/game") && o.has("move") && o.has("sim, level"));
    KV_CHECK(c, run("print /systems/game/move") && o.has("writes: game.rock.Pos, game.ship.Pos")); // ktables by name
    KV_CHECK(c, run("ls /triggers/game") && o.has("death") && o.has("game.ship.Health.hp crossed below 1"));
    KV_CHECK(c, run("print /resources/game/score") && o.has("declared, nothing published"));
    KV_CHECK(c, run("ls /events") && o.has("Spawned") && o.has("StateEdge"));
    KV_CHECK(c, run("print /modules/game") && o.has("2 ktables") && o.has("1 triggers"));

    // The clock: read, then patch; a bad rate is refused with a reason.
    KV_CHECK(c, run("print /clock/paused") && strcmp(o.lines[0], "0") == 0);
    KV_CHECK(c, run("patch /clock/paused 1") && e.clock.paused);
    KV_CHECK(c, run("patch /clock/rate 0.5") && e.clock.rate == 0.5);
    KV_CHECK(c, !run("patch /clock/rate 0") && o.has("positive"));
    KV_CHECK(c, !run("patch /clock/tick 5") && o.has("read-only"));
    KV_CHECK(c, run("patch /clock/paused 0") && !e.clock.paused);

    // Relative paths against a working directory: no session here, so cwd is the root and
    // cd is refused; the resolver still applies . and .. and folds slashes.
    KV_CHECK(c, !run("cd /ktables") && o.has("no session"));
    KV_CHECK(c, run("pwd") && strcmp(o.lines[0], "/") == 0);
    KV_CHECK(c, run("ls ktables//game/./ship/../rock") && o.n == 4);
    KV_CHECK(c, run("ls -n 2 /ktables/game/rock") && o.n == 3 && o.has("and 2 more"));
    KV_CHECK(c, run("print /ktables/game/ship") && o.has("1 live of 8") && o.has("Health (prev)"));
    KV_CHECK(c, run("help") && o.has("the filesystem"));

    // No sink: the lines are discarded, and the result is still the request's.
    KV_CHECK(c, cli_run(e, "print /ktables/game/ship", nullptr, nullptr));
    KV_CHECK(c, cli_run(e, "help", nullptr, nullptr));
    KV_CHECK(c, !cli_run(e, "ls /nowhere", nullptr, nullptr));
    KV_CHECK(c, !cli_run(e, "no such command", nullptr, nullptr));

    engine_shutdown(e);
}
