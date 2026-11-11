#include "korvid/frameloop/frameloop.h"
#include "korvid/frameloop/module.h"
#include "frameloop_internal.h"
#include "korvid/asset/font.h"
#include "korvid/cli/module.h"
#include "korvid/core/arena.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/name.h"
#include "korvid/core/profile.h"
#include "korvid/core/time.h"
#include "korvid/platform_hid/events.h"
#include "korvid/platform/module.h"
#include "korvid/platform_hid/module.h"
#include "korvid/render/renderer.h"
#include "korvid/sprite/camera.h"
#include "korvid/sprite/module.h"
#include "korvid/sprite/transform2.h"
#include "korvid/text/text.h"
#include <stdio.h>
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
struct GivenOption { u32 option; const char* value; }; // config.options[option], and what followed it
GivenOption     g_options[32];
u32             g_option_count = 0;
// --capacity kind=N, repeatable: over the game's config, which keeps its own as well.
char            g_capacity_names[16][64];
KindCapacity    g_capacities[32];
u32             g_capacity_count = 0;
u32             g_threads      = 0; // 0: every core
const char*     g_trace_path   = nullptr; // --profile FILE
bool            g_prof_report  = false;   // --prof-report: the top zones with the fps line
const char*     g_shot_path    = nullptr; // --screenshot FILE: one frame as a PPM
u64             g_shot_frame   = 90;      // --screenshot-frame N: which one
u64             g_shot_tick    = 0;       // --screenshot-tick N: the first frame at or past tick N, counting every tick but those a load held
bool            g_shot_taken   = false;
char            g_cli_shot[512] = {};     // the CLI's `screenshot FILE`: the next frame drawn
u64             g_shot_ticks   = 0;       // ticks run outside a level load's hold
bool            g_check_sort   = false;   // --check-sort: verify the GPU sort every 30th frame
CliConfig       g_cli;                    // --cli-port N, --cli-stdin, --cli-ascii
PlatformMounts  g_mounts;                 // --pack FILE (repeatable), --assets DIR
f64             g_idle_margin  = 0.0015;  // --idle-margin-ms N: slack kept back from idle work
bool            g_idle         = true;    // --no-idle
bool            g_overlay      = false;   // --overlay: the report on screen

constexpr u32 kOverlayLines = 8;
constexpr f32 kOverlayPx    = 16.0f;
constinit u32 g_overlay_kind = 0;

struct FrameLoopState {
    FrameLoopConfig config;
    HostWindow*     window   = nullptr; // platform's
    Renderer*       renderer = nullptr; // the bound backend's
    Engine*         engine   = nullptr; // for the frames run inside an OS modal loop
    f64             prev     = 0.0;     // when the last frame began, the loop's or a modal one's
    u64             modal_frames = 0;   // frames run inside an OS modal loop (a live resize)
    u32             presented_w = 0, presented_h = 0;
    u32             deltas_last_frame = 0;
    // The GPU's frames over the report window (renderer_gpu_stats, a frame late), when measured.
    u32             gpu_frames = 0;
    f64             gpu_sum = 0.0, gpu_worst = 0.0, gpu_sort = 0.0, gpu_draw = 0.0;
    f64             gpu_overdraw = 0.0;
    f64             wait_ema   = 0.0;    // the renderer's blocked time per frame, smoothed
    f64             idle_total = 0.0;    // seconds of idle work in the report window
    u64             idle_frames = 0;     // frames that ran idle work in the window
    bool            timer_fired = false; // the loop raises it; the system emits it once
    bool            timer_sent  = false;
    Handle          overlay_font;              // asset.font, when --overlay
    Handle          overlay_lines[kOverlayLines]; // frameloop.overlay labels, top to bottom
};

// Wall clock in the exact format libwayland's WAYLAND_DEBUG uses (realtime microseconds
// truncated to 32 bits, printed as ms), so hitch lines line up with a protocol trace.
f64 wayland_debug_stamp() {
    struct timespec tp;
    timespec_get(&tp, TIME_UTC); // C11: the realtime clock, on every platform
    u32 us = (u32)((u64)tp.tv_sec * 1000000u + (u64)tp.tv_nsec / 1000u);
    return (f64)us / 1000.0;
}

// Index + 1 of a game flag, or 0.
u32 game_flag(const FrameLoopConfig& cfg, const char* arg) {
    for (u32 i = 0; i < cfg.flags.len && i < 64; ++i)
        if (strcmp(cfg.flags[i], arg) == 0) return i + 1;
    return 0;
}

// Index + 1 of a game option (a flag with a value), or 0.
u32 game_option(const FrameLoopConfig& cfg, const char* arg) {
    for (u32 i = 0; i < cfg.options.len; ++i)
        if (strcmp(cfg.options[i], arg) == 0) return i + 1;
    return 0;
}

// A replay's game argument, as the command line's would be: a flag (false), or an option
// and its value (true: it took the value).
bool apply_game_arg(const char* arg, const char* value) {
    if (u32 f = game_flag(g_config, arg)) { g_flags_given |= (u64)1 << (f - 1); return false; }
    if (u32 o = game_option(g_config, arg); o && value) {
        if (g_option_count < KV_COUNTOF(g_options)) g_options[g_option_count++] = GivenOption{o - 1, value};
        return true;
    }
    KV_WARN("frameloop", "replay: the game takes no argument '%s'", arg);
    return false;
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
        else if (strcmp(argv[i], "--screenshot-tick") == 0 && i + 1 < argc) g_shot_tick = (u64)atoll(argv[++i]);
        else if (strcmp(argv[i], "--check-sort") == 0) g_check_sort = true;
        else if (strcmp(argv[i], "--cli-port") == 0 && i + 1 < argc) g_cli.port = (u16)atoi(argv[++i]);
        else if (strcmp(argv[i], "--cli-stdin") == 0) g_cli.stdin_ = true;
        else if (strcmp(argv[i], "--cli-ascii") == 0) g_cli.ascii = true;
        else if (strcmp(argv[i], "--pack") == 0 && i + 1 < argc) {
            if (g_mounts.pack_count >= KV_COUNTOF(g_mounts.packs)) { KV_ERROR("frameloop", "at most %u --pack", (u32)KV_COUNTOF(g_mounts.packs)); return false; }
            g_mounts.packs[g_mounts.pack_count++] = argv[++i];
        }
        else if (strcmp(argv[i], "--assets") == 0 && i + 1 < argc) g_mounts.assets_dir = argv[++i];
        else if (strcmp(argv[i], "--io") == 0 && i + 1 < argc) {
            const char* b = argv[++i];
            if (!strcmp(b, "sync")) g_mounts.io_backend = IoBackend::Sync;
            else if (!strcmp(b, "uring")) g_mounts.io_backend = IoBackend::Uring;
            else { KV_ERROR("frameloop", "--io sync or --io uring"); return false; }
        }
        else if (strcmp(argv[i], "--idle-margin-ms") == 0 && i + 1 < argc) g_idle_margin = atof(argv[++i]) * 1e-3;
        else if (strcmp(argv[i], "--no-idle") == 0) g_idle = false;
        else if (strcmp(argv[i], "--overlay") == 0) g_overlay = true;
        else if (strcmp(argv[i], "--spike-hunt") == 0) dev_spike_hunt(true);
        else if (strcmp(argv[i], "--sprite-rows") == 0 && i + 1 < argc) cfg.sprite_rows = (u32)strtoul(argv[++i], nullptr, 10);
        else if (strcmp(argv[i], "--capacity") == 0 && i + 1 < argc) {
            const char* arg = argv[++i];
            const char* eq = strchr(arg, '=');
            u32 n = g_capacity_count;
            if (!eq || eq == arg || (usize)(eq - arg) >= sizeof g_capacity_names[0] || n >= KV_COUNTOF(g_capacity_names)) {
                KV_ERROR("frameloop", "--capacity kind=N (a kind's full name, at most %u of them)", (u32)KV_COUNTOF(g_capacity_names));
                return false;
            }
            memcpy(g_capacity_names[n], arg, (usize)(eq - arg));
            g_capacity_names[n][eq - arg] = 0;
            g_capacities[g_capacity_count++] = KindCapacity{g_capacity_names[n], (u32)strtoul(eq + 1, nullptr, 10)};
        }
        else if (replay_arg(i, argc, argv)) {}
        else if (u32 f = game_flag(cfg, argv[i])) { g_flags_given |= (u64)1 << (f - 1); replay_keep_arg(argv[i]); }
        else if (u32 o = game_option(cfg, argv[i]); o && i + 1 < argc) {
            if (g_option_count >= KV_COUNTOF(g_options)) { KV_ERROR("frameloop", "at most %u game options", (u32)KV_COUNTOF(g_options)); return false; }
            replay_keep_arg(argv[i]);
            replay_keep_arg(argv[i + 1]);
            g_options[g_option_count++] = GivenOption{o - 1, argv[++i]};
        }
        else {
            KV_ERROR("frameloop", "usage: %s [--run-seconds N] [--quiet] [--validation|--no-validation] "
                                  "[--no-vsync] [--borderless] [--hitch-ms N] [--hash] [--hash-systems] [--trace-events] [--threads N] "
                                  "[--profile FILE] [--prof-report] [--screenshot FILE] [--screenshot-frame N] [--screenshot-tick N] [--check-sort] [--cli-port N] [--cli-stdin] [--cli-ascii] [--pack FILE]... [--assets DIR] [--io sync|uring] [--idle-margin-ms N] [--no-idle] [--overlay] [--capacity KIND=N]... [--sprite-rows N] [--spike-hunt] [--record FILE] [--replay FILE]", argv[0]);
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
    desc.surface    = renderer_surface() == RendererSurface::Metal ? WindowSurface::Metal : WindowSurface::Vulkan;
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
    rd.sprite_rows   = s->config.sprite_rows;
    s->renderer = renderer_create(engine, rd);
    if (!s->renderer) return false;
    if (s->config.sprite_rows) sprite_rows_hint(engine, s->config.sprite_rows); // the extract's tables, sized once too

    if (g_overlay) {
        s->overlay_font = asset_load_font(engine, "text/DejaVuSans.ttf", kOverlayPx);
        Table* labels = engine_table(engine, g_overlay_kind);
        for (u32 i = 0; i < kOverlayLines; ++i) {
            Text t;
            t.font   = s->overlay_font;
            t.space  = SpriteSpace::Screen;
            t.anchor = ScreenAnchor::TopLeft;
            t.layer  = 127;
            t.tint   = color_rgba(1.0f, 1.0f, 1.0f, 0.9f);
            // Down from the window's top-left corner, whatever its size: the anchor does that.
            s->overlay_lines[i] = labels->spawn(transform2_at(point2{12.0f, -12.0f - kOverlayPx * (1.0f + 1.2f * (f32)i)}), t);
        }
    }
    return true;
}

// The overlay's line i, given `text`. Called at every report.
void overlay_set(Engine& engine, FrameLoopState& s, u32 line, const char* text) {
    if (!g_overlay || line >= kOverlayLines) return;
    Table* labels = engine_table(engine, g_overlay_kind);
    Text* t = labels->get<Text>(s.overlay_lines[line]);
    if (!t) return;
    text_set(*t, "%s", text);
}

constinit const EventTypeDesc k_events[]  = {KV_EVENT(ExitRequested)};
constinit const KindDesc      k_kinds[] = {
    {.name = ".overlay", .components = "Transform2, Text", .capacity = kOverlayLines, .id = &g_overlay_kind, .cosmetic = true},
};
constinit const SystemDesc    k_systems[] = {
    {.name = ".exit", .phase = Phase::PreSim, .fn = frameloop_exit, .reads = "input.state", .runs = RunWhen::Always},
    // Recording and replay (replay.cc), sim ticks only: what the sample made of the named
    // actions; the file's actions before the sample.
    {.name = ".record", .phase = Phase::Input, .fn = replay_record, .reads = "input.actions", .after = "input.sample", .main_thread = true},
    {.name = ".replay", .phase = Phase::Input, .fn = replay_play, .before = "input.sample", .main_thread = true},
};
constinit const HandlerDesc   k_handlers[] = {
    {.name = ".on_sim_reset", KV_ON(SimReset, replay_on_sim_reset)},
};

// The frame's slack: what the renderer blocked for last frame (fences, acquire, present)
// is time this frame may spend on idle work before rendering, less a margin. Smoothed,
// and never more than the last measurement, so one long frame does not buy the next a
// budget it does not have. No vsync, no blocking, no slack.
f64 idle_slack(FrameLoopState& s) {
    f64 wait = renderer_last_wait(s.renderer);
    s.wait_ema = s.wait_ema == 0.0 ? wait : s.wait_ema * 0.8 + wait * 0.2;
    f64 budget = (wait < s.wait_ema ? wait : s.wait_ema) - g_idle_margin;
    return g_idle && budget > 0.0002 ? budget : 0.0;
}

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

    // The camera as the last tick left it, at render rate: the view is the window's size
    // over the zoom, centered on the camera, rotated by it. Zoom 1 is one world unit per pixel.
    RenderView view;
    view.alpha       = engine.clock.alpha();
    view.clear_color = s.config.clear_color;
    Camera2 cam;
    if (const Camera2* c = sprite_camera(engine)) cam = camera_resolve(engine, *c, w, h, view.alpha);
    view.view     = camera_view(cam, w, h);
    view.rotation = cam.rotation;
    renderer_render(s.renderer, view);
    RendererGpuStats gs = renderer_gpu_stats(s.renderer);
    if (gs.valid) {
        ++s.gpu_frames;
        s.gpu_sum += gs.frame_ms;
        s.gpu_sort += gs.sort_ms;
        s.gpu_draw += gs.draw_ms;
        s.gpu_overdraw += gs.overdraw;
        if (gs.frame_ms > s.gpu_worst) s.gpu_worst = gs.frame_ms;
    }
}

// A frame from inside an OS modal loop that holds platform_pump (platform_modal_frame: a
// live resize on macOS), with the window's queued events already on the bus: the ticks and
// the render, without the pump that is running further up the stack. Idle work, reports and
// the profiler's frame wait for the loop. The loop's clock is shared, so the frame after
// the resize does not see the resize as one long frame.
void modal_frame(void* user) {
    FrameLoopState& s = *(FrameLoopState*)user;
    Engine& engine = *s.engine;
    f64 now = time_now();
    f64 dt  = now - s.prev;
    s.prev = now;
    engine_frame(engine, dt);
    render_frame(engine, s);
    ++s.modal_frames;
}

void run(Engine& engine, FrameLoopState& s) {
    f64 start = time_now();
    s.engine = &engine;
    s.prev   = start;
    f64 next_report = start + (f64)s.config.report_every_seconds;
    u32 frames_in_window = 0;
    bool collecting = prof_enabled();
    platform_modal_frame(engine, modal_frame, &s);

    while (!engine.quit_requested) {
        f64 now = time_now();
        f64 dt  = now - s.prev;
        s.prev = now;

        if (collecting) prof_frame(engine.frame_count);
        {
            KV_ZONE("pump");
            platform_pump(engine);
        }
        f64 t_pumped = time_now();
        bool loading = (engine_holds(engine) & HoldLoading) != 0;
        u32 ran = engine_frame(engine, dt);
        if (!loading) g_shot_ticks += ran; // the ticks a script counts: not the ones a load held, which follow the disk
        f64 t_ticked = time_now();
        if (f64 slack = idle_slack(s)) {
            f64 used = engine_idle_run(engine, slack);
            if (used > 0.0) { s.idle_total += used; ++s.idle_frames; }
        }
        {
            KV_ZONE("render");
            // A tick is the same moment in every run, where a frame is not.
            bool shot = g_shot_tick ? !g_shot_taken && g_shot_ticks >= g_shot_tick : engine.frame_count == g_shot_frame;
            if (g_shot_path && shot) { renderer_screenshot(s.renderer, g_shot_path); g_shot_taken = true; }
            if (g_cli_shot[0]) { renderer_screenshot(s.renderer, g_cli_shot); g_cli_shot[0] = 0; }
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
            char scaled[32] = "";
            if (engine.clock.scale != 1.0) snprintf(scaled, sizeof scaled, " (time scale %.2f)", engine.clock.scale);
            KV_INFO("frameloop", "%u fps, tick %llu%s, alpha %.2f, %u sprites, %u deltas/frame, %ux%u, %s, %llu KB live, idle %.1f ms/frame on %llu",
                    frames_in_window, (unsigned long long)engine.clock.tick_count, scaled, engine.clock.alpha(),
                    renderer_sprite_count(s.renderer), s.deltas_last_frame, s.window->width(), s.window->height(),
                    s.window->focused() ? "focused" : "unfocused",
                    (unsigned long long)((mem.live_bytes + mem.committed_bytes) / 1024),
                    s.idle_frames ? s.idle_total / (f64)s.idle_frames * 1e3 : 0.0, (unsigned long long)s.idle_frames);
            if (s.gpu_frames) {
                f64 n = (f64)s.gpu_frames;
                KV_INFO("frameloop", "gpu %.3f ms/frame (worst %.3f): sort %.3f, draw %.3f; overdraw %.2f", s.gpu_sum / n, s.gpu_worst, s.gpu_sort / n,
                        s.gpu_draw / n, s.gpu_overdraw / n);
                s.gpu_frames = 0;
                s.gpu_sum = s.gpu_worst = s.gpu_sort = s.gpu_draw = s.gpu_overdraw = 0.0;
            }
            if (g_overlay) {
                char line[96];
                snprintf(line, sizeof line, "%u fps  tick %llu  %u sprites  %u deltas", frames_in_window,
                         (unsigned long long)engine.clock.tick_count, renderer_sprite_count(s.renderer), s.deltas_last_frame);
                overlay_set(engine, s, 0, line);
                snprintf(line, sizeof line, "%llu KB live  idle %.1f ms%s", (unsigned long long)((mem.live_bytes + mem.committed_bytes) / 1024),
                         s.idle_frames ? s.idle_total / (f64)s.idle_frames * 1e3 : 0.0, engine.clock.paused ? "  PAUSED" : "");
                overlay_set(engine, s, 1, line);
                u32 shown = 0;
                if (g_prof_report) {
                    ProfStat top[kOverlayLines - 2];
                    u32 frames = 1;
                    u32 n = prof_top(top, &frames);
                    for (; shown < n; ++shown) {
                        snprintf(line, sizeof line, "%-28.28s %6.3f ms", top[shown].name, prof_ms(top[shown].wall) / frames);
                        overlay_set(engine, s, 2 + shown, line);
                    }
                }
                for (u32 i = 2 + shown; i < kOverlayLines; ++i) overlay_set(engine, s, i, "");
            }
            s.idle_total = 0.0;
            s.idle_frames = 0;
            if (g_prof_report) prof_log_summary(10);
            frames_in_window = 0;
            next_report += (f64)s.config.report_every_seconds;
        }
        if (s.config.run_seconds >= 0.0 && now - start >= s.config.run_seconds && !s.timer_fired) {
            KV_INFO("frameloop", "run time reached");
            s.timer_fired = true;
        }
    }
    platform_modal_frame(engine, nullptr, nullptr);
    if (s.modal_frames) KV_INFO("frameloop", "%llu frames ran inside live resizes", (unsigned long long)s.modal_frames);
}

// `screenshot FILE`: the next frame drawn, as a PPM.
void cli_screenshot(CliContext& ctx, Span<const char* const> args) {
    if (args.len != 1 || !args[0][0]) { ctx.fail("screenshot FILE"); return; }
    if (strlen(args[0]) >= sizeof g_cli_shot) { ctx.fail("the path is too long"); return; }
    snprintf(g_cli_shot, sizeof g_cli_shot, "%s", args[0]);
    ctx.print("the next frame goes to %s", args[0]);
}

constinit const CliCommandDesc k_commands[] = {
    {.name = "screenshot", .help = "FILE: the next frame drawn, as a PPM", .fn = cli_screenshot},
};

} // namespace

KV_MODULE_DEFINE(frameloop_module) = {
    .name    = "frameloop",
    .events  = k_events,
    .kinds   = k_kinds,
    .systems = k_systems,
    .handlers = k_handlers,
    .commands = k_commands,
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

const char* frameloop_option(Engine& e, const char* option, u32 index) {
    u32 o = game_option(frameloop_config(e), option);
    if (!o) return nullptr;
    for (u32 i = 0; i < g_option_count; ++i)
        if (g_options[i].option == o - 1 && index-- == 0) return g_options[i].value;
    return nullptr;
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
    // The executable's name, for <exe>.kpak: argv[0] after its last separator ('\' too,
    // on Windows), less a ".exe" (Windows names executables that way; the pack does not).
    static char exe_name[128];
    const char* name = argv[0];
    for (const char* p = argv[0]; *p; ++p) if (*p == '/' || *p == '\\') name = p + 1;
    snprintf(exe_name, sizeof exe_name, "%s", name);
    usize len = strlen(exe_name);
    if (len > 4 && (exe_name[len - 4] == '.') && (exe_name[len - 3] | 0x20) == 'e' && (exe_name[len - 2] | 0x20) == 'x' &&
        (exe_name[len - 1] | 0x20) == 'e')
        exe_name[len - 4] = 0;
    g_mounts.exe_name = exe_name;
    if (!replay_open(exe_name, apply_game_arg)) return 2;
    platform_configure_mounts(g_mounts);
    Engine engine;
    engine.hash_ticks   = g_hash_ticks;
    engine.hash_systems = g_hash_systems;
    engine.trace.print  = g_trace_events;
    engine.threads      = g_threads;
    if (g_trace_events && !KV_DEV) KV_WARN("frameloop", "--trace-events: the event trace exists in dev builds only");
    // The game's capacities, then the command line's (a later entry for a kind wins).
    EngineConfig engine_config = g_config.engine;
    if (g_capacity_count) {
        for (const KindCapacity& kc : g_config.engine.capacities)
            if (g_capacity_count < KV_COUNTOF(g_capacities)) {
                memmove(g_capacities + 1, g_capacities, sizeof(KindCapacity) * g_capacity_count);
                g_capacities[0] = kc;
                ++g_capacity_count;
            }
        engine_config.capacities = Span<const KindCapacity>(g_capacities, g_capacity_count);
    }
#if KV_DEV
    if (dev_spike_hunting())
        KV_INFO("frameloop", "spike hunt: no memory fills or poison, no guard pages, no event emitters noted; checks, counters and the profiler as ever");
#endif
    if (!engine_init(engine, kv_module_list(), engine_config)) {
        names_shutdown();
        mem_shutdown();
        return 1;
    }
    FrameLoopState* s = engine_module_state_as<FrameLoopState>(engine, "frameloop");
    if (s) run(engine, *s);
    replay_finish(engine);
    engine_shutdown(engine);
    if (prof_enabled()) { prof_collect(); if (g_prof_report) prof_log_summary(10); }
    prof_shutdown();
    names_shutdown();
    mem_shutdown();
    return 0;
}

} // namespace kv
