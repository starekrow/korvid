// testbed: the example game, written the way a game is meant to be written here. Balls are
// a kind (Transform2, Sprite, Motion, Bounces). One system moves every ball and emits
// Bounced at the edges; a handler counts bounces and re-aims the random ball. The blinker
// lives by scheduled events: BlinkOff destroys it and schedules BlinkOn, BlinkOn spawns
// a new one by command, and the Spawned event's ticket says which ball to schedule
// BlinkOff for. A trigger fires when a ball's bounce count passes two. The state machine
// (INIT -> RUN -> EXIT) spawns and destroys by command, so it holds no handles and no
// writes. No window, renderer, loop or options here: those are the frameloop module's.
#include "korvid/testbed/module.h"
#include "korvid/asset/material.h"
#include "korvid/core/arena.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/random.h"
#include "korvid/frameloop/frameloop.h"
#include "korvid/math/fields.h"
#include "korvid/platform_hid/events.h"
#include "korvid/platform_hid/host_window.h"
#include "korvid/render/renderer.h"
#include "korvid/sprite/sprite.h"
#include "korvid/sprite/transform2.h"

namespace kv {

namespace {

// ---- what a ball is ------------------------------------------------------------------
struct Motion  { vec2 velocity; u8 random; }; // pixels per second; random: re-aim on every bounce
struct Bounces { u32 count; };

constinit const FieldDesc k_motion_fields[]  = {KV_FIELDS(Motion, velocity, random)};
constinit const FieldDesc k_bounces_fields[] = {KV_FIELDS(Bounces, count)};
constinit const ComponentDesc k_components[] = {
    KV_COMPONENT(Motion, k_motion_fields),
    KV_COMPONENT(Bounces, k_bounces_fields, CompPrev), // prev: the trigger below watches it
};

constinit u32 g_ball_kind = 0;
constinit const KindDesc k_kinds[] = {
    {.name = ".ball", .components = "Transform2, Sprite, Motion, Bounces", .capacity = 1 << 16, .id = &g_ball_kind}, // room for --swarm
};

// ---- what happens --------------------------------------------------------------------
struct Bounced  { Handle ball; };
struct BlinkOn  { u32 nothing; }; // a timer with no payload
struct BlinkOff { Handle ball; };

constinit const FieldDesc k_bounced_fields[]   = {KV_FIELDS(Bounced, ball)};
constinit const FieldDesc k_blink_on_fields[]  = {KV_FIELDS(BlinkOn, nothing)};
constinit const FieldDesc k_blink_off_fields[] = {KV_FIELDS(BlinkOff, ball)};
constinit const EventTypeDesc k_events[] = {
    KV_EVENT_EX(Bounced, k_bounced_fields, 0),
    KV_EVENT_EX(BlinkOn, k_blink_on_fields, 0),
    KV_EVENT_EX(BlinkOff, k_blink_off_fields, 0),
};

constinit u32 g_third_bounce = 0;
constinit const TriggerDesc k_triggers[] = {
    {.name = ".third_bounce", .field = ".ball.Bounces.count", .edge = TriggerEdge::CrossedAbove, .value = 2.0, .id = &g_third_bounce},
};

// ---- the module's state --------------------------------------------------------------
enum GameState { ST_INIT, ST_RUN, ST_EXIT };
enum Look : u8 { LOOK_WHITE, LOOK_BLUE_ADD, LOOK_GREEN, LOOK_MAGENTA, LOOK_COUNT };

const char* state_name(GameState s) {
    switch (s) {
    case ST_INIT: return "INIT";
    case ST_RUN:  return "RUN";
    case ST_EXIT: return "EXIT";
    }
    return "?";
}

struct Game {
    Handle    looks[LOOK_COUNT];     // asset.material, one file per look
    Handle    bg_look;               // asset.material: the default checkerboard, tinted dark
    Handle    materials[LOOK_COUNT]; // render.material: the rows those files fill
    Handle    bg_material;           // render.material
    GameState state          = ST_INIT;
    bool      exit_requested = false;
    u32       blink_ticket   = 0;   // the blinker's pending spawn, until Spawned answers
    Rng       rng;
    u64       key_edges      = 0;
};

constexpr u32 kBlinkOnTicks  = 45;
constexpr u32 kBlinkOffTicks = 30;
constexpr u32 kSwarmBalls    = 40000; // --swarm: a renderer stress across layers and blends
constexpr u32 kSwarmOffscreen = 2000; // plus sprites that sit outside the view, never moving

void set_state(Game& g, GameState next) {
    KV_INFO("game", "%s -> %s", state_name(g.state), state_name(next));
    g.state = next;
}

vec2 random_direction(Rng& rng, f32 speed) {
    f32 a = rng.range(0.0f, 2.0f * KV_PI);
    return vec2{cosf(a) * speed, sinf(a) * speed};
}

// Spawns by command: the ball exists after this phase, and Spawned carries the ticket.
u32 spawn_ball(TickContext& ctx, const Game& g, point2 at, vec2 velocity, bool random, f32 size, i32 layer, Look look) {
    Sprite s;
    s.material = g.materials[look];
    s.size     = vec2{size, size};
    s.layer    = layer;
    return ctx.spawn_later(g_ball_kind, transform2_at(at), s, Motion{velocity, (u8)random}, Bounces{0});
}

// Keeps the whole sprite inside the window; reverses the velocity on the edge it hit.
bool bounce(Transform2& t, vec2& v, f32 half_size, f32 half_w, f32 half_h, f32 dt) {
    bool hit = false;
    t.position += v * dt;
    f32 lx = half_w - half_size, ly = half_h - half_size;
    if (t.position.x > lx)       { t.position.x = lx;  v.x = -v.x; hit = true; }
    else if (t.position.x < -lx) { t.position.x = -lx; v.x = -v.x; hit = true; }
    if (t.position.y > ly)       { t.position.y = ly;  v.y = -v.y; hit = true; }
    else if (t.position.y < -ly) { t.position.y = -ly; v.y = -v.y; hit = true; }
    return hit;
}

// ---- systems -------------------------------------------------------------------------

// Phase Sim: every ball moves and bounces. World units are pixels, origin at the center.
void testbed_move(TickContext& ctx) {
    static const u32 k_window = ctx.resource_id("platform_hid.window"); // looked up once
    const HostWindow* window = ctx.resource<const HostWindow>(k_window);
    f32 half_w = (f32)window->width() * 0.5f, half_h = (f32)window->height() * 0.5f;
    f32 dt = (f32)ctx.dt;
    for (KindRows k : ctx.view<Transform2, Motion, const Sprite>()) {
        Span<Transform2>   t = k.col<Transform2>();
        Span<Motion>       m = k.col<Motion>();
        Span<const Sprite> s = k.col<const Sprite>();
        for (u32 i = 0; i < k.rows(); ++i) {
            if (!k.alive(i)) continue;
            if (bounce(t[i], m[i].velocity, s[i].size.x * 0.5f, half_w, half_h, dt)) ctx.emit(Bounced{k.handle_at(i)});
        }
    }
}

// Phase Sim: what happened this tick, then the state machine.
void testbed_sim(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();

    for (const KeyDown& e : ctx.received<KeyDown>()) {
        ++g.key_edges;
        if (!e.repeat) KV_INFO("testbed", "tick %llu: key down %s", (unsigned long long)ctx.tick, key_name(e.key));
    }
    for (const KeyUp& e : ctx.received<KeyUp>()) {
        ++g.key_edges;
        KV_INFO("testbed", "tick %llu: key up   %s", (unsigned long long)ctx.tick, key_name(e.key));
    }
    for (const AssetReady& e : ctx.received<AssetReady>()) {
        char text[160];
        kv_handle_describe(e.asset.bits, text, sizeof text);
        KV_INFO("testbed", "asset ready: %s", text);
    }
    for (const AssetFailed& e : ctx.received<AssetFailed>()) {
        char text[160];
        kv_handle_describe(e.asset.bits, text, sizeof text);
        KV_WARN("testbed", "asset failed (%s): %s", asset_error_name(e.error), text);
    }
    for (const ExitRequested& e : ctx.received<ExitRequested>()) {
        if (g.exit_requested) continue;
        KV_INFO("testbed", "exit requested: %s", e.reason == ExitReason::WindowClosed ? "window closed" :
                                               e.reason == ExitReason::Escape ? "escape" : "timer");
        g.exit_requested = true;
    }

    switch (g.state) {
    case ST_INIT: {
        const HostWindow* window = ctx.resource<const HostWindow>("platform_hid.window");
        f32 w = (f32)window->width(), h = (f32)window->height();
        // A background covering the window, tiling the checkerboard at 16-pixel cells.
        Sprite bg;
        bg.material = g.bg_material;
        bg.size     = vec2{w, h};
        bg.uv       = rect2{0.0f, 0.0f, w / 128.0f, h / 128.0f};
        bg.layer    = -1;
        ctx.spawn_later("sprite.entity", transform2_at(point2{0.0f, 0.0f}), bg);

        g.rng = Rng::seeded(0x5eed, 1);
        spawn_ball(ctx, g, point2{0.0f, 0.0f},      vec2{420.0f, 0.0f},                 false, 96.0f, 2, LOOK_WHITE);
        spawn_ball(ctx, g, point2{-200.0f, 0.0f},   vec2{0.0f, 300.0f},                 false, 72.0f, 1, LOOK_BLUE_ADD);
        spawn_ball(ctx, g, point2{200.0f, 100.0f},  random_direction(g.rng, 360.0f),    true,  56.0f, 3, LOOK_GREEN);
        g.blink_ticket = spawn_ball(ctx, g, point2{0.0f, -120.0f}, vec2{}, false, 120.0f, 0, LOOK_MAGENTA);
        if (frameloop_flag(*ctx.engine, "--swarm")) {
            // Many small balls on random layers (beyond the renderer's clamp on purpose), every
            // look, and a crowd of sprites parked far outside the view that the cull must drop.
            for (u32 i = 0; i < kSwarmBalls; ++i) {
                point2 at{g.rng.range(-w * 0.5f, w * 0.5f), g.rng.range(-h * 0.5f, h * 0.5f)};
                i32 layer = (i32)g.rng.range(-300.0f, 300.0f);
                spawn_ball(ctx, g, at, random_direction(g.rng, g.rng.range(40.0f, 200.0f)), false, 12.0f, layer, (Look)(i % LOOK_COUNT));
            }
            Sprite far_away;
            far_away.material = g.materials[LOOK_GREEN];
            far_away.size     = vec2{32.0f, 32.0f};
            for (u32 i = 0; i < kSwarmOffscreen; ++i)
                ctx.spawn_later("sprite.entity", transform2_at(point2{w + 1000.0f + (f32)i, 0.0f}), far_away);
            KV_INFO("testbed", "swarm: %u balls, %u off-screen sprites", kSwarmBalls, kSwarmOffscreen);
        }
        set_state(g, ST_RUN);
        break;
    }

    case ST_RUN:
        if (g.exit_requested) set_state(g, ST_EXIT);
        break;

    case ST_EXIT: {
        // Say goodbye to every ball, then take everything down by command and quit.
        static const u32 k_bounces = ctx.column_id(".ball.Bounces");
        const Table* balls = ctx.read_kind(g_ball_kind);
        Span<const Bounces> bounces = ctx.read<Bounces>(k_bounces);
        for (u32 i = 0; i < balls->rows(); ++i) {
            if (!balls->is_alive(i)) continue;
            KV_INFO("game", "ball %u destroyed after %u bounces", i, bounces[i].count);
            ctx.destroy(balls->handle_at(i));
        }
        const Table* sprites = ctx.read_kind("sprite.entity");
        for (u32 i = 0; i < sprites->rows(); ++i)
            if (sprites->is_alive(i)) ctx.destroy(sprites->handle_at(i));
        engine_request_quit(*ctx.engine);
        break;
    }
    }
}

// ---- handlers: at phase boundaries, on what the tick emitted ---------------------------

// A delivered handle is live, so its index is a valid row for the rest of the wave.
void on_bounced(TickContext& ctx, Span<const Bounced> hits) {
    static const u32 k_bounces = ctx.column_id(".ball.Bounces");
    static const u32 k_motion  = ctx.column_id(".ball.Motion");
    Game& g = ctx.state_as<Game>();
    Span<Bounces> bounces = ctx.write<Bounces>(k_bounces);
    Span<Motion>  motion  = ctx.write<Motion>(k_motion);
    for (const Bounced& hit : hits) {
        u32 i = hit.ball.index();
        ++bounces[i].count;
        if (motion[i].random) motion[i].velocity = random_direction(g.rng, g.rng.range(240.0f, 520.0f));
    }
}

// The blinker's spawn came through: now it has a handle to switch off later.
void on_spawned(TickContext& ctx, Span<const Spawned> spawns) {
    Game& g = ctx.state_as<Game>();
    for (const Spawned& s : spawns) {
        if (s.ticket != g.blink_ticket) continue;
        g.blink_ticket = 0;
        if (s.entity.is_null()) { KV_WARN("game", "the blinker did not spawn: kind full"); continue; }
        char text[256];
        kv_handle_describe(s.entity.bits, text, sizeof text);
        KV_INFO("game", "blinker is %s", text);
        ctx.schedule_in(BlinkOff{s.entity}, kBlinkOnTicks);
    }
}

void on_blink_off(TickContext& ctx, Span<const BlinkOff> offs) {
    for (const BlinkOff& b : offs) {
        ctx.destroy(b.ball);
        ctx.schedule_in(BlinkOn{}, kBlinkOffTicks);
    }
}

void on_blink_on(TickContext& ctx, Span<const BlinkOn> ons) {
    Game& g = ctx.state_as<Game>();
    if (g.state != ST_RUN) return;
    const HostWindow* window = ctx.resource<const HostWindow>("platform_hid.window");
    f32 quarter_w = (f32)window->width() * 0.25f;
    for (u32 i = 0; i < ons.len; ++i)
        g.blink_ticket = spawn_ball(ctx, g, point2{g.rng.range(-quarter_w, quarter_w), -120.0f}, vec2{}, false, 120.0f, 0, LOOK_MAGENTA);
}

void on_state_edge(TickContext&, Span<const StateEdge> edges) {
    for (const StateEdge& e : edges) {
        if (e.trigger != g_third_bounce) continue;
        char text[256];
        kv_handle_describe(e.entity.bits, text, sizeof text);
        KV_INFO("game", "third bounce: %s", text);
    }
}

// ---- startup, shutdown, the tables -----------------------------------------------------

bool testbed_startup(Engine& engine, ModuleInstance& self) {
    Game* g = arena_push<Game>(self.arena);
    if (!g) return false;
    self.set_state(g);

    if (!renderer_get(engine)) return false;

    // Assets are requested once and the handles kept. One material file per look (the
    // ball texture through alpha and additive blending with different tints; the
    // background is the default checkerboard, opaque and dark). The rows exist now and
    // show the placeholder; AssetReady arrives on the bus as each file and texture lands.
    const char* looks[LOOK_COUNT] = {"testbed/white.material.json", "testbed/blue_add.material.json",
                                     "testbed/green.material.json", "testbed/magenta.material.json"};
    for (u32 i = 0; i < LOOK_COUNT; ++i) {
        g->looks[i] = asset_load_material(engine, looks[i]);
        const MaterialAsset* m = asset_material(engine, g->looks[i]);
        if (!m) return false;
        g->materials[i] = m->material;
    }
    g->bg_look = asset_load_material(engine, "testbed/background.material.json");
    const MaterialAsset* bg = asset_material(engine, g->bg_look);
    if (!bg) return false;
    g->bg_material = bg->material;
    return true;
}

void testbed_shutdown(Engine&, ModuleInstance& self) {
    Game* g = self.state_as<Game>();
    if (g) KV_INFO("testbed", "%llu key edges seen", (unsigned long long)g->key_edges);
}

constinit const SystemDesc k_systems[] = {
    {.name = ".move", .phase = Phase::Sim, .fn = testbed_move, .reads = ".ball.Sprite, platform_hid.window", .writes = ".ball.Transform2, .ball.Motion", .emits = "Bounced"},
    {.name = ".sim",  .phase = Phase::Sim, .fn = testbed_sim,  .reads = "platform_hid.window, .ball, sprite.entity, KeyDown, KeyUp, AssetReady, AssetFailed, ExitRequested"}, // after move: it reads what move writes
};
constinit const HandlerDesc k_handlers[] = {
    {.name = ".on_bounced",   KV_ON(Bounced, on_bounced), .phase = Phase::PostSim, .writes = ".ball.Bounces, .ball.Motion"},
    {.name = ".on_spawned",   KV_ON(Spawned, on_spawned), .phase = Phase::PostSim},
    {.name = ".on_blink_off", KV_ON(BlinkOff, on_blink_off), .phase = Phase::Input},
    {.name = ".on_blink_on",  KV_ON(BlinkOn, on_blink_on), .phase = Phase::Input, .reads = "platform_hid.window"},
    {.name = ".on_edge",      KV_ON(StateEdge, on_state_edge), .phase = Phase::Input},
};

} // namespace

KV_MODULE_DEFINE(testbed_module) = {
    .name       = "testbed",
    .events     = k_events,
    .components = k_components,
    .kinds      = k_kinds,
    .systems    = k_systems,
    .handlers   = k_handlers,
    .triggers   = k_triggers,
    .startup    = testbed_startup,
    .shutdown   = testbed_shutdown,
    .memory     = {.reserve = 64 * 1024},
};

} // namespace kv
