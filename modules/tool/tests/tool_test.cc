// tool_run end to end: a command that finishes at once; one that starts work a system
// finishes ticks later; a command the CLI refuses; a bad option; --max-ticks.
#include "korvid/tool/tool.h"
#include "korvid/tool/module.h"
#include "korvid/cli/module.h"
#include "korvid/core/cli.h"
#include "korvid/core/engine.h"
#include "korvid/core/test.h"
#include "korvid/platform/module.h"
#include <stdlib.h>

namespace {

using namespace kv;

struct Counter { u32 target = 0, ticks = 0; bool counting = false; };
constinit Counter g_counter = {};

void cmd_now(CliContext& ctx, Span<const char* const> args) {
    u32 code = 0;
    if (args.len != 1 || !cli_arg_u32(ctx, args[0], "code", &code)) return;
    ctx.print("finishing with %u", code);
    tool_finish(*ctx.engine, (int)code);
}
void cmd_count(CliContext& ctx, Span<const char* const> args) {
    if (args.len != 1 || !cli_arg_u32(ctx, args[0], "ticks", &g_counter.target)) { ctx.fail("t count <ticks>"); return; }
    g_counter.ticks = 0;
    g_counter.counting = true;
}
void cmd_forever(CliContext&, Span<const char* const>) {}
void sys_count(TickContext& ctx) {
    if (!g_counter.counting) return;
    if (++g_counter.ticks >= g_counter.target) { g_counter.counting = false; tool_finish(*ctx.engine, 0); }
}
constinit const CliCommandDesc k_cmds[] = {
    {.name = "t now", .help = "t now <code>", .fn = cmd_now},
    {.name = "t count", .help = "t count <ticks>", .fn = cmd_count},
    {.name = "t forever", .help = "t forever", .fn = cmd_forever},
};
constinit const SystemDesc k_systems[] = {{.name = ".count", .phase = Phase::Sim, .fn = sys_count}};
KV_MODULE_DEFINE(k_t) = {.name = "t", .systems = k_systems, .commands = k_cmds};

int run(Span<const char* const> words) {
    char* argv[8];
    for (u32 i = 0; i < words.len; ++i) argv[i] = const_cast<char*>(words[i]);
    const ModuleDesc* const list[] = {&platform_module, &cli_module, &tool_module, &k_t};
    ToolConfig cfg;
    cfg.name = "t";
    return tool_run((int)words.len, argv, list, cfg);
}

} // namespace

void test_tool(kv::TestCtx& c) {
    using namespace kv;
    c.begin("tool");
    const char* const now[] = {"t", "now", "5"};
    KV_CHECK(c, run(now) == 5);
    const char* const count[] = {"t", "--threads", "2", "count", "30"};
    KV_CHECK(c, run(count) == 0 && g_counter.ticks == 30);
    const char* const unknown[] = {"t", "frobnicate"};
    KV_CHECK(c, run(unknown) == 2);
    const char* const bad_arg[] = {"t", "now", "x"};
    KV_CHECK(c, run(bad_arg) == 2);
    const char* const bad_opt[] = {"t", "--nope", "now", "1"};
    KV_CHECK(c, run(bad_opt) == 2);
    const char* const none[] = {"t"};
    KV_CHECK(c, run(none) == 2);
    const char* const forever[] = {"t", "--max-ticks", "50", "forever"};
    KV_CHECK(c, run(forever) == 2);
    const char* const help[] = {"t", "help"};
    KV_CHECK(c, run(help) == 0); // the CLI's own: printed, nothing to wait for
}
