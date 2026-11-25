// tool_run end to end: a command that finishes at once; one that starts work a system
// finishes ticks later; a command the CLI refuses; a bad option; --max-ticks.
#include "korvid/tool/tool.h"
#include "korvid/tool/module.h"
#include "korvid/cli/module.h"
#include "korvid/core/cli.h"
#include "korvid/core/engine.h"
#include "korvid/core/test.h"
#include "korvid/platform/module.h"
#include "korvid/core/atomic.h"
#include "korvid/core/time.h"
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
// tool_task: straight-line work on a thread of its own, the engine ticking beside it.
struct TaskSeen { u32 ticks_during = 0; bool running = false, saw_cancel = false; };
constinit TaskSeen g_task = {};
i32 task_sleeps(void*, const ToolTaskCtl&) {
    time_sleep(0.3); // blocking, as a write_now or a Process::wait would
    atomic_store(&g_task.running, false, MO_RELEASE);
    return 7;
}
i32 task_until_cancelled(void*, const ToolTaskCtl& ctl) {
    while (!ctl.cancelled()) time_sleep(0.002);
    g_task.saw_cancel = true;
    return 0;
}
void cmd_task(CliContext& ctx, Span<const char* const> args) {
    g_task = TaskSeen{};
    g_task.running = true;
    bool stop = args.len == 1 && args[0][0] == 's';
    if (!tool_task(*ctx.engine, stop ? task_until_cancelled : task_sleeps, nullptr, "test")) ctx.fail("no task");
}
void sys_task(TickContext&) {
    if (atomic_load(&g_task.running, MO_ACQUIRE)) ++g_task.ticks_during;
}
constinit const CliCommandDesc k_cmds[] = {
    {.name = "t now", .help = "t now <code>", .fn = cmd_now},
    {.name = "t count", .help = "t count <ticks>", .fn = cmd_count},
    {.name = "t forever", .help = "t forever", .fn = cmd_forever},
    {.name = "t task", .help = "t task [stop]", .fn = cmd_task},
};
constinit const SystemDesc k_systems[] = {{.name = ".count", .phase = Phase::Sim, .fn = sys_count}, {.name = ".task", .phase = Phase::Sim, .fn = sys_task}};
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

    // A task's return is the run's status; the engine ticked while it blocked, at the
    // heartbeat's pace and not flat out: nothing asked for more.
    const char* const task[] = {"t", "task"};
    f64 t0 = time_now();
    KV_CHECK(c, run(task) == 7);
    f64 took = time_now() - t0;
    KV_CHECK(c, took >= 0.29 && took < 2.0);
    KV_CHECK(c, g_task.ticks_during >= 1 && g_task.ticks_during <= 6);
    // The run ends first (--max-ticks): the task is told, and waited for.
    const char* const stopped[] = {"t", "--max-ticks", "20", "task", "stop"};
    KV_CHECK(c, run(stopped) == 2 && g_task.saw_cancel);
}
