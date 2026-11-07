// hello: the example game. A module like any other (see hello/module.h): its startup
// asks for a Vulkan window and a renderer, its system runs a three-state gameplay machine
// (INIT -> RUN -> EXIT) in phase Sim that bounces one sprite across the window. main()
// parses options, runs the engine, and drives the frame loop. Esc or closing the window
// exits through the state machine; --run-seconds N exits by itself (smoke test). The
// game calls the render interface only, never a graphics API.
#include "korvid/hello/module.h"
#include "korvid/asset/texture.h"
#include "korvid/core/arena.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/time.h"
#include "korvid/platform/events.h"
#include "korvid/platform/host_window.h"
#include "korvid/platform/module.h"
#include "korvid/render/renderer.h"
#include "korvid/sprite/extract.h"
#include "korvid/sprite/module.h"
#include "korvid/sprite/world.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace kv {

namespace {

enum GameState { ST_INIT, ST_RUN, ST_EXIT };

const char* state_name(GameState s) {
    switch (s) {
    case ST_INIT: return "INIT";
    case ST_RUN:  return "RUN";
    case ST_EXIT: return "EXIT";
    }
    return "?";
}

struct Options {
    f64  run_seconds = -1.0;
    bool validation  = KV_DEBUG != 0;
    bool vsync       = true;
    bool borderless  = false;
    f64  hitch_ms    = 0.0; // > 0: log where any frame longer than this spent its time
};

// Set by main before engine_init; read by hello's startup. Plain static storage.
Options g_options;

// The hello module's state. The services are borrowed: their modules own and end them.
struct App {
    HostWindow*         window   = nullptr; // the platform module's
    Input*              input    = nullptr; // the platform module's
    SpriteWorld*        world    = nullptr; // the sprite module's
    Renderer*           renderer = nullptr; // the renderer-vulkan module's
    u32                 packets_last_frame = 0;
    u32                 presented_w = 0, presented_h = 0; // size the renderer was last told

    // assets and materials: renderer-issued handles the game keeps
    Handle    ball_asset;      // asset.texture
    Handle    ball_material;   // render.material, alpha, ball.png
    Handle    bg_material;     // render.material, opaque, the default checkerboard tinted dark

    // game
    GameState state          = ST_INIT;
    bool      exit_requested = false; // raised by input, the window or the timer; RUN acts on it
    Handle    background;
    Handle    ball;
    f32       ball_speed     = 420.0f; // world units (pixels) per second
    f32       ball_dir       = 1.0f;
    u32       bounces        = 0;
    u64       key_edges      = 0;
};

// Wall clock in the exact format libwayland's WAYLAND_DEBUG uses (realtime microseconds
// truncated to 32 bits, printed as ms), so hitch lines line up with a protocol trace.
f64 wayland_debug_stamp() {
    struct timespec tp;
    clock_gettime(CLOCK_REALTIME, &tp);
    u32 us = (u32)((u64)tp.tv_sec * 1000000u + (u64)tp.tv_nsec / 1000u);
    return (f64)us / 1000.0;
}

// ---- options ---------------------------------------------------------------

bool parse_options(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--run-seconds") == 0 && i + 1 < argc) opt.run_seconds = atof(argv[++i]);
        else if (strcmp(argv[i], "--quiet") == 0) log_set_level(LogLevel::Warn);
        else if (strcmp(argv[i], "--no-validation") == 0) opt.validation = false;
        else if (strcmp(argv[i], "--validation") == 0) opt.validation = true;
        else if (strcmp(argv[i], "--no-vsync") == 0) opt.vsync = false;
        else if (strcmp(argv[i], "--borderless") == 0) opt.borderless = true;
        else if (strcmp(argv[i], "--hitch-ms") == 0 && i + 1 < argc) opt.hitch_ms = atof(argv[++i]);
        else {
            KV_ERROR("hello", "usage: hello [--run-seconds N] [--quiet] [--validation|--no-validation] "
                              "[--no-vsync] [--borderless] [--hitch-ms N]");
            return false;
        }
    }
    return true;
}

// ---- gameplay: the state machine ---------------------------------------------

void set_state(App& app, GameState next) {
    KV_INFO("game", "%s -> %s", state_name(app.state), state_name(next));
    app.state = next;
}

// Runs once per tick from the sim phase. Each state does its work and names its successor.
void game_update(App& app, TickContext& ctx) {
    SpriteWorld& world = *app.world;
    switch (app.state) {
    case ST_INIT: {
        // A background covering the window, tiling the checkerboard at 16-pixel cells.
        f32 w = (f32)app.window->width(), h = (f32)app.window->height();
        Sprite bg;
        bg.material = app.bg_material;
        bg.size     = vec2{w, h};
        bg.uv       = rect2{0.0f, 0.0f, w / 128.0f, h / 128.0f};
        bg.layer    = -1;
        app.background = world.create(transform2_at(point2{0.0f, 0.0f}), bg);

        Sprite s;
        s.material = app.ball_material; // placeholder until the PNG lands, then the ball
        s.size     = vec2{96.0f, 96.0f};
        s.tint     = color_white();
        s.layer    = 0;
        app.ball = world.create(transform2_at(point2{0.0f, 0.0f}), s);
        char text[96];
        kv_handle_describe(app.ball.bits, text, sizeof text);
        KV_INFO("game", "ball is %s", text);
        set_state(app, ST_RUN);
        break;
    }

    case ST_RUN: {
        if (app.exit_requested) {
            set_state(app, ST_EXIT);
            break;
        }
        Transform2* t = world.transform(app.ball);
        const Sprite* s = world.sprite(app.ball);
        if (!t || !s) { // the ball vanished: nothing left to run
            KV_ERROR("game", "ball handle went stale");
            set_state(app, ST_EXIT);
            break;
        }
        // World units are pixels with the origin at the window center; keep the whole
        // sprite on screen and reverse at the edges.
        f32 limit = (f32)app.window->width() * 0.5f - s->size.x * t->scale.x * 0.5f;
        t->position.x += app.ball_dir * app.ball_speed * (f32)ctx.dt;
        if (t->position.x >= limit && app.ball_dir > 0.0f) {
            t->position.x = limit;
            app.ball_dir  = -1.0f;
            ++app.bounces;
        } else if (t->position.x <= -limit && app.ball_dir < 0.0f) {
            t->position.x = -limit;
            app.ball_dir  = 1.0f;
            ++app.bounces;
        }
        break;
    }

    case ST_EXIT: {
        if (world.destroy(app.ball)) KV_INFO("game", "ball destroyed after %u bounces", app.bounces);
        world.destroy(app.background);
        app.ball = app.background = Handle{};
        KV_ASSERT(world.live() == 0);
        engine_request_quit(*ctx.engine);
        break;
    }
    }
}

// ---- the module's system: platform events and sampled input first, then the game ----

void hello_sim(TickContext& ctx) {
    App& app = *(App*)ctx.state;
    EventBus& bus = *ctx.events;

    for (const WindowResized& e : bus.read<WindowResized>())
        KV_INFO("hello", "tick %llu: resized to %ux%u", (unsigned long long)ctx.tick, e.width, e.height);
    for (const FocusChanged& e : bus.read<FocusChanged>())
        KV_INFO("hello", "tick %llu: focus %s", (unsigned long long)ctx.tick, e.focused ? "gained" : "lost");
    for (const KeyDown& e : bus.read<KeyDown>()) {
        ++app.key_edges;
        if (!e.repeat) KV_INFO("hello", "tick %llu: key down %s", (unsigned long long)ctx.tick, key_name(e.key));
    }
    for (const KeyUp& e : bus.read<KeyUp>()) {
        ++app.key_edges;
        KV_INFO("hello", "tick %llu: key up   %s", (unsigned long long)ctx.tick, key_name(e.key));
    }
    for (const AssetReady& e : bus.read<AssetReady>()) {
        char text[160];
        kv_handle_describe(e.asset.bits, text, sizeof text);
        KV_INFO("hello", "asset ready: %s", text);
    }
    for (const AssetFailed& e : bus.read<AssetFailed>()) {
        char text[160];
        kv_handle_describe(e.asset.bits, text, sizeof text);
        KV_WARN("hello", "asset failed (%s): %s", asset_error_name(e.error), text);
    }
    if (bus.read<CloseRequested>().len && !app.exit_requested) {
        KV_INFO("hello", "close requested");
        app.exit_requested = true;
    }
    if (app.input->sampled.down(Key::Escape) && !app.exit_requested) {
        KV_INFO("hello", "escape held");
        app.exit_requested = true;
    }

    game_update(app, ctx);
}

// ---- module startup and shutdown ----------------------------------------------

bool hello_startup(Engine& engine, ModuleInstance& self) {
    App* app = arena_push<App>(self.arena);
    if (!app) return false;
    self.state = app;

    app->input = platform_input(engine);
    app->world = sprite_world(engine);
    if (!app->input || !app->world) {
        KV_ERROR("hello", "platform and sprite modules must be up"); // the list guarantees it
        return false;
    }

    WindowDesc desc;
    desc.title      = "korvid hello";
    desc.width      = 960;
    desc.height     = 540;
    desc.surface    = WindowSurface::Vulkan;
    desc.vsync      = g_options.vsync;
    desc.borderless = g_options.borderless;
    app->window = platform_open_window(engine, desc);
    if (!app->window) return false;

    RendererDesc rd;
    rd.native_window = app->window->native_handle();
    rd.width         = app->window->width();
    rd.height        = app->window->height();
    rd.vsync         = app->window->vsync();
    rd.validation    = g_options.validation;
    app->renderer = renderer_create(engine, rd);
    if (!app->renderer) return false;

    // Assets are requested once and the handles kept. The texture slot exists now and
    // shows the placeholder; AssetReady arrives on the bus when the PNG is resident.
    app->ball_asset = asset_load_texture(engine, "hello/ball.png");
    const TextureAsset* ball = asset_texture(engine, app->ball_asset);
    if (!ball) return false;
    MaterialDesc ball_md;
    ball_md.texture = ball->texture;
    ball_md.blend   = BlendMode::Alpha;
    app->ball_material = renderer_material_create(app->renderer, ball_md);
    MaterialDesc bg_md;
    bg_md.texture = Handle{}; // the default checkerboard
    bg_md.blend   = BlendMode::Opaque;
    bg_md.tint    = color_rgba(0.22f, 0.22f, 0.26f);
    app->bg_material = renderer_material_create(app->renderer, bg_md);
    return !app->ball_material.is_null() && !app->bg_material.is_null();
}

// Nothing of hello's own to free: its state lives in the module arena. The window and
// renderer end with their modules, later in the shutdown order because they come
// earlier in the module list.
void hello_shutdown(Engine&, ModuleInstance& self) {
    App* app = (App*)self.state;
    if (app) KV_INFO("hello", "%llu key edges seen", (unsigned long long)app->key_edges);
}

constinit const char* const k_sim_reads[]  = {"input.state", "asset.textures"};
constinit const char* const k_sim_writes[] = {"sprite.world"};
constinit const SystemDesc  k_systems[]    = {
    {"hello.sim", Phase::Sim, hello_sim, k_sim_reads, k_sim_writes},
};

// ---- frame loop ------------------------------------------------------------

// Extract after the frame's last tick, then draw at the clock's interpolation point.
void render_frame(Engine& engine, App& app) {
    // The size reaches the renderer at render rate, not tick rate: a frame presented at a
    // stale size during a live resize makes the compositor withhold the next frame, and
    // the window then only catches up when the drag ends. The pump that emitted
    // WindowResized also updated the window's size, so this is the same information.
    u32 w = app.window->width(), h = app.window->height();
    if (w != app.presented_w || h != app.presented_h) {
        renderer_resize(app.renderer, w, h);
        app.presented_w = w;
        app.presented_h = h;
    }

    // Packets live for this frame only: scratch, reset at the top of the next engine_frame.
    Array<RenderPacket> packets = array_in<RenderPacket>(mem_scratch());
    extract_sprites(*app.world, packets);
    app.packets_last_frame = packets.len;

    RenderFrame frame;
    frame.packets = packets.span();
    frame.alpha   = engine.clock.alpha();
    f32 vw = (f32)w, vh = (f32)h;
    frame.view    = rect2{-vw * 0.5f, -vh * 0.5f, vw, vh}; // one world unit per pixel, origin centered
    renderer_render(app.renderer, frame);
}

void run_frame_loop(Engine& engine, App& app, const Options& opt) {
    f64 start = time_now();
    f64 prev  = start;
    f64 next_report = start + 1.0;
    u32 frames_in_window = 0;

    while (!engine.quit_requested) {
        f64 now = time_now();
        f64 dt  = now - prev;
        prev = now;

        platform_pump(engine);
        f64 t_pumped = time_now();
        engine_frame(engine, dt);
        f64 t_ticked = time_now();
        render_frame(engine, app);
        f64 t_rendered = time_now();
        ++frames_in_window;

        if (opt.hitch_ms > 0.0 && (t_rendered - now) * 1e3 >= opt.hitch_ms) {
            KV_WARN("hello", "[%11.3f] hitch ended: pump %.1f ms, ticks %.1f ms, render %.1f ms",
                    wayland_debug_stamp(), (t_pumped - now) * 1e3, (t_ticked - t_pumped) * 1e3,
                    (t_rendered - t_ticked) * 1e3);
        }
        if (now >= next_report) {
            KV_INFO("hello", "%u fps, tick %llu, alpha %.2f, %u packets, state %s, %ux%u, %s",
                    frames_in_window, (unsigned long long)engine.clock.tick_count,
                    engine.clock.alpha(), app.packets_last_frame, state_name(app.state),
                    app.window->width(), app.window->height(),
                    app.window->focused() ? "focused" : "unfocused");
            frames_in_window = 0;
            next_report += 1.0;
        }
        if (opt.run_seconds >= 0.0 && now - start >= opt.run_seconds && !app.exit_requested) {
            KV_INFO("hello", "run time reached");
            app.exit_requested = true;
        }
    }
}

} // namespace

KV_MODULE_DEFINE(hello_module) = {
    "hello", {}, {}, k_systems, hello_startup, hello_shutdown,
    {.reserve = 64 * 1024},
};

} // namespace kv

int main(int argc, char** argv) {
    using namespace kv;
    if (!parse_options(argc, argv, g_options)) return 2;

    Engine engine;
    if (!engine_init(engine, kv_module_list())) {
        mem_shutdown();
        return 1;
    }
    App* app = (App*)engine_module_state(engine, "hello");
    run_frame_loop(engine, *app, g_options);
    engine_shutdown(engine);
    mem_shutdown();
    return 0;
}
