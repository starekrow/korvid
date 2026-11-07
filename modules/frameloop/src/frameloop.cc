#include "korvid/frameloop/frameloop.h"
#include "korvid/frameloop/module.h"
#include "korvid/cli/module.h"
#include "korvid/core/arena.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/name.h"
#include "korvid/core/profile.h"
#include "korvid/core/time.h"
#include "korvid/platform/events.h"
#include "korvid/platform/module.h"
#include "korvid/render/renderer.h"
#include "korvid/sprite/module.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace kv {

namespace {

// Set by frameloop_main before engine_init; read by startup. Plain static storage.
FrameLoopConfig g_config;
f64             g_hitch_ms     = 0.0;
bool            g_hash_ticks   = false;
bool            g_hash_systems = false;
bool            g_trace_events = false;
u64             g_flags_given  = 0; // bit i: config.flags[i] was on the command line
u32             g_threads      = 0; // 0: every core
const char*     g_trace_path   = nullptr; // --profile FILE
bool            g_prof_report  = false;   // --prof-report: the top zones with the fps line
const char*     g_shot_path    = nullptr; // --screenshot FILE: one frame as a PPM
u64             g_shot_frame   = 90;      // --screenshot-frame N: which one
bool            g_check_sort   = false;   // --check-sort: verify the GPU sort every 30th frame
CliConfig       g_cli;                    // --cli-port N, --cli-stdin

struct FrameLoopState {
    FrameLoopConfig config;
    HostWindow*     window   = nullptr; // platform's
    Renderer*       renderer = nullptr; // renderer-vulkan's
    u32             presented_w = 0, presented_h = 0;
    u32             deltas_last_frame = 0;
    bool            timer_fired = false; // the loop raises it; the system emits it once
    bool            timer_sent  = false;
};

// Wall clock in the exact format libwayland's WAYLAND_DEBUG uses (realtime microseconds
// truncated to 32 bits, printed as ms), so hitch lines line up with a protocol trace.
f64 wayland_debug_stamp() {
    struct timespec tp;
    clock_gettime(CLOCK_REALTIME, &tp);
    u32 us = (u32)((u64)tp.tv_sec * 1000000u + (u64)tp.tv_nsec / 1000u);
    return (f64)us / 1000.0;
}

// Index + 1 of a game flag, or 0.
u32 game_flag(const FrameLoopConfig& cfg, const char* arg) {
    for (u32 i = 0; i < cfg.flags.len && i < 64; ++i)
        if (strcmp(cfg.flags[i], arg) == 0) return i + 1;
    return 0;
}

bool parse_options(int argc, char** argv, FrameLoopConfig& cfg) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--run-seconds") == 0 && i + 1 < argc) cfg.run_seconds = atof(argv[++i]);
        else if (strcmp(argv[i], "--quiet") == 0) log_set_level(LogLevel::Warn);
        else if (strcmp(argv[i], "--no-validation") == 0) cfg.validation = false;
        else if (strcmp(argv[i], "--validation") == 0) cfg.validation = true;
        else if (strcmp(argv[i], "--no-vsync") == 0) cfg.vsync = false;
        else if (strcmp(argv[i], "--borderless") == 0) cfg.borderless = true;
        else if (strcmp(argv[i], "--hitch-ms") == 0 && i + 1 < argc) g_hitch_ms = atof(argv[++i]);
        else if (strcmp(argv[i], "--hash") == 0) g_hash_ticks = true;
        else if (strcmp(argv[i], "--hash-systems") == 0) g_hash_systems = true;
        else if (strcmp(argv[i], "--trace-events") == 0) g_trace_events = true;
        else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) g_threads = (u32)atoi(argv[++i]);
        else if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) g_trace_path = argv[++i];
        else if (strcmp(argv[i], "--prof-report") == 0) g_prof_report = true;
        else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) g_shot_path = argv[++i];
        else if (strcmp(argv[i], "--screenshot-frame") == 0 && i + 1 < argc) g_shot_frame = (u64)atoll(argv[++i]);
        else if (strcmp(argv[i], "--check-sort") == 0) g_check_sort = true;
        else if (strcmp(argv[i], "--cli-port") == 0 && i + 1 < argc) g_cli.port = (u16)atoi(argv[++i]);
        else if (strcmp(argv[i], "--cli-stdin") == 0) g_cli.stdin_ = true;
        else if (u32 f = game_flag(cfg, argv[i])) g_flags_given |= (u64)1 << (f - 1);
        else {
            KV_ERROR("frameloop", "usage: %s [--run-seconds N] [--quiet] [--validation|--no-validation] "
                                  "[--no-vsync] [--borderless] [--hitch-ms N] [--hash] [--hash-systems] [--trace-events] [--threads N] "
                                  "[--profile FILE] [--prof-report] [--screenshot FILE] [--screenshot-frame N] [--check-sort] [--cli-port N] [--cli-stdin]", argv[0]);
            return false;
        }
    }
    return true;
}

// Phase PreSim, with the tick's input sampled: the reasons to leave become one event each.
void frameloop_exit(TickContext& ctx) {
    FrameLoopState& s = ctx.state_as<FrameLoopState>();
    EventBus& bus = *ctx.events;
    const Input* input = ctx.resource<const Input>("input.state");
    if (s.config.exit_on_close && bus.read<CloseRequested>().len)
        bus.emit(ExitRequested{ExitReason::WindowClosed});
    if (s.config.exit_on_escape && input && input->sampled.down(Key::Escape))
        bus.emit(ExitRequested{ExitReason::Escape});
    if (s.timer_fired && !s.timer_sent) {
        s.timer_sent = true;
        bus.emit(ExitRequested{ExitReason::Timer});
    }
}

bool frameloop_startup(Engine& engine, ModuleInstance& self) {
    FrameLoopState* s = arena_push<FrameLoopState>(self.arena);
    if (!s) return false;
    self.set_state(s);
    s->config = g_config;

    WindowDesc desc;
    desc.title      = s->config.title;
    desc.width      = s->config.width;
    desc.height     = s->config.height;
    desc.surface    = WindowSurface::Vulkan;
    desc.vsync      = s->config.vsync;
    desc.borderless = s->config.borderless;
    s->window = platform_open_window(engine, desc);
    if (!s->window) return false;

    RendererDesc rd;
    rd.native_window = s->window->native_handle();
    rd.width         = s->window->width();
    rd.height        = s->window->height();
    rd.vsync         = s->window->vsync();
    rd.validation    = s->config.validation;
    s->renderer = renderer_create(engine, rd);
    return s->renderer != nullptr;
}

constinit const EventTypeDesc k_events[]  = {KV_EVENT(ExitRequested)};
constinit const char* const   k_exit_reads[] = {"input.state"};
constinit const SystemDesc    k_systems[] = {
    {.name = "frameloop.exit", .phase = Phase::PreSim, .fn = frameloop_exit, .reads = k_exit_reads},
};

// Hand the renderer what the frame's ticks changed, then draw at the clock's
// interpolation point.
void render_frame(Engine& engine, FrameLoopState& s) {
    // The size reaches the renderer at render rate, not tick rate: a frame presented at a
    // stale size during a live resize makes the compositor withhold the next frame.
    u32 w = s.window->width(), h = s.window->height();
    if (w != s.presented_w || h != s.presented_h) {
        renderer_resize(s.renderer, w, h);
        s.presented_w = w;
        s.presented_h = h;
    }

    Array<SpriteDelta>* deltas = sprite_deltas(engine);
    if (deltas) {
        renderer_apply(s.renderer, deltas->span());
        s.deltas_last_frame = deltas->len;
        deltas->clear();
    }

    RenderView view;
    view.alpha       = engine.clock.alpha();
    view.clear_color = s.config.clear_color;
    f32 vw = (f32)w, vh = (f32)h;
    view.view = rect2{-vw * 0.5f, -vh * 0.5f, vw, vh}; // one world unit per pixel, origin centered
    renderer_render(s.renderer, view);
}

void run(Engine& engine, FrameLoopState& s) {
    f64 start = time_now();
    f64 prev  = start;
    f64 next_report = start + (f64)s.config.report_every_seconds;
    u32 frames_in_window = 0;
    bool collecting = prof_enabled();

    while (!engine.quit_requested) {
        f64 now = time_now();
        f64 dt  = now - prev;
        prev = now;

        if (collecting) prof_frame(engine.frame_count);
        {
            KV_ZONE("pump");
            platform_pump(engine);
        }
        f64 t_pumped = time_now();
        engine_frame(engine, dt);
        f64 t_ticked = time_now();
        {
            KV_ZONE("render");
            if (g_shot_path && engine.frame_count == g_shot_frame) renderer_screenshot(s.renderer, g_shot_path);
            if (g_check_sort && engine.frame_count % 30 == 10) renderer_check_sort(s.renderer);
            render_frame(engine, s);
        }
        f64 t_rendered = time_now();
        ++frames_in_window;
        if (collecting) prof_collect(); // the frame's records: summary, trace, and the kept frame

        if (g_hitch_ms > 0.0 && (t_rendered - now) * 1e3 >= g_hitch_ms) {
            KV_WARN("frameloop", "[%11.3f] hitch ended: pump %.1f ms, ticks %.1f ms, render %.1f ms",
                    wayland_debug_stamp(), (t_pumped - now) * 1e3, (t_ticked - t_pumped) * 1e3,
                    (t_rendered - t_ticked) * 1e3);
            if (collecting) prof_log_frame(prof_ticks_from_ms(0.1)); // the whole frame's tree
        }
        if (s.config.report_every_seconds && now >= next_report) {
            const MemSample& mem = mem_last_sample();
            KV_INFO("frameloop", "%u fps, tick %llu, alpha %.2f, %u sprites, %u deltas/frame, %ux%u, %s, %llu KB live",
                    frames_in_window, (unsigned long long)engine.clock.tick_count, engine.clock.alpha(),
                    renderer_sprite_count(s.renderer), s.deltas_last_frame, s.window->width(), s.window->height(),
                    s.window->focused() ? "focused" : "unfocused",
                    (unsigned long long)((mem.live_bytes + mem.committed_bytes) / 1024));
            if (g_prof_report) prof_log_summary(10);
            frames_in_window = 0;
            next_report += (f64)s.config.report_every_seconds;
        }
        if (s.config.run_seconds >= 0.0 && now - start >= s.config.run_seconds && !s.timer_fired) {
            KV_INFO("frameloop", "run time reached");
            s.timer_fired = true;
        }
    }
}

} // namespace

KV_MODULE_DEFINE(frameloop_module) = {
    .name    = "frameloop",
    .events  = k_events,
    .systems = k_systems,
    .startup = frameloop_startup,
    .memory  = {.reserve = 64 * 1024},
};

const FrameLoopConfig& frameloop_config(Engine& e) {
    FrameLoopState* s = engine_module_state_as<FrameLoopState>(e, "frameloop");
    return s ? s->config : g_config;
}

void frameloop_set_config(const FrameLoopConfig& config) { g_config = config; }

void frameloop_step(Engine& e, f64 dt) {
    platform_pump(e);
    engine_frame(e, dt);
    if (FrameLoopState* s = engine_module_state_as<FrameLoopState>(e, "frameloop")) render_frame(e, *s);
}

bool frameloop_flag(Engine& e, const char* flag) {
    u32 f = game_flag(frameloop_config(e), flag);
    return f && (g_flags_given >> (f - 1)) & 1;
}

int frameloop_main(int argc, char** argv, const FrameLoopConfig& config) {
    g_config = config;
    if (!parse_options(argc, argv, g_config)) return 2;

    // The profiler records when something will read it: a trace, the report, or a hitch dump.
    if (g_trace_path || g_prof_report || g_hitch_ms > 0.0) {
        if (!KV_PROFILE) KV_WARN("frameloop", "the profiler exists in dev builds only");
        prof_enable(true);
        prof_thread_name("main");
        if (g_hitch_ms > 0.0) prof_keep_frame(true);
        if (g_trace_path) prof_trace_open(g_trace_path);
    }
    cli_configure(g_cli);
    Engine engine;
    engine.hash_ticks   = g_hash_ticks;
    engine.hash_systems = g_hash_systems;
    engine.trace.print  = g_trace_events;
    engine.threads      = g_threads;
    if (g_trace_events && !KV_DEV) KV_WARN("frameloop", "--trace-events: the event trace exists in dev builds only");
    if (!engine_init(engine, kv_module_list(), g_config.engine)) {
        names_shutdown();
        mem_shutdown();
        return 1;
    }
    FrameLoopState* s = engine_module_state_as<FrameLoopState>(engine, "frameloop");
    if (s) run(engine, *s);
    engine_shutdown(engine);
    if (prof_enabled()) { prof_collect(); if (g_prof_report) prof_log_summary(10); }
    prof_shutdown();
    names_shutdown();
    mem_shutdown();
    return 0;
}

} // namespace kv
