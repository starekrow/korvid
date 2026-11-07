// example_testbed: the example game, written the way a game is meant to be written here. Balls are
// a kind (Transform2, Sprite, Motion, Bounces). One system moves every ball and emits
// Bounced at the edges; a handler counts bounces and re-aims the random ball. The blinker
// lives by scheduled events: BlinkOff destroys it and schedules BlinkOn, BlinkOn spawns
// a new one by command, and the Spawned event's ticket says which ball to schedule
// BlinkOff for. A trigger fires when a ball's bounce count passes two. The state machine
// (INIT -> RUN -> EXIT) spawns and destroys by command, so it holds no handles and no
// writes. No window, renderer, loop or options here: those are the frameloop module's.
#include "korvid/example_testbed/module.h"
#include "korvid/asset/texture.h"
#include "korvid/core/arena.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/random.h"
#include "korvid/frameloop/frameloop.h"
#include "korvid/math/fields.h"
#include "korvid/platform/events.h"
#include "korvid/platform/host_window.h"
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
constinit const char* const k_ball_components[] = {"Transform2", "Sprite", "Motion", "Bounces"};
constinit const KindDesc k_kinds[] = {
    {.name = "example_testbed.ball", .components = k_ball_components, .capacity = 64, .id = &g_ball_kind},
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
    {.name = "example_testbed.third_bounce", .field = "example_testbed.ball.Bounces.count", .edge = TriggerEdge::CrossedAbove, .value = 2.0, .id = &g_third_bounce},
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
    Handle    ball_asset;           // asset.texture
    Handle    materials[LOOK_COUNT]; // render.material
    Handle    bg_material;          // render.material: the default checkerboard, tinted dark
    GameState state          = ST_INIT;
    bool      exit_requested = false;
    u32       blink_ticket   = 0;   // the blinker's pending spawn, until Spawned answers
    Rng       rng;
    u64       key_edges      = 0;
};

constexpr u32 kBlinkOnTicks  = 45;
constexpr u32 kBlinkOffTicks = 30;

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
void example_testbed_move(TickContext& ctx) {
    static const u32 k_window = ctx.resource_id("platform.window"); // looked up once
    const HostWindow* window = ctx.resource<const HostWindow>(k_window);
    f32 half_w = (f32)window->width() * 0.5f, half_h = (f32)window->height() * 0.5f;
    f32 dt = (f32)ctx.dt;
    for (KindRows k : ctx.view<Transform2, Motion, const Sprite>()) {
        Span<Transform2>   t = k.col<Transform2>();
        Span<Motion>       m = k.col<Motion>();
        Span<const Sprite> s = k.col<const Sprite>();
        for (u32 i = 0; i < k.rows(); ++i) {
            if (!k.alive(i)) continue;
            if (bounce(t[i], m[i].velocity, s[i].size.x * 0.5f, half_w, half_h, dt)) ctx.events->emit(Bounced{k.handle_at(i)});
        }
    }
}

// Phase Sim: what happened this tick, then the state machine.
void example_testbed_sim(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    EventBus& bus = *ctx.events;

    for (const KeyDown& e : bus.read<KeyDown>()) {
        ++g.key_edges;
        if (!e.repeat) KV_INFO("example_testbed", "tick %llu: key down %s", (unsigned long long)ctx.tick, key_name(e.key));
    }
    for (const KeyUp& e : bus.read<KeyUp>()) {
        ++g.key_edges;
        KV_INFO("example_testbed", "tick %llu: key up   %s", (unsigned long long)ctx.tick, key_name(e.key));
    }
    for (const AssetReady& e : bus.read<AssetReady>()) {
        char text[160];
        kv_handle_describe(e.asset.bits, text, sizeof text);
        KV_INFO("example_testbed", "asset ready: %s", text);
    }
    for (const AssetFailed& e : bus.read<AssetFailed>()) {
        char text[160];
        kv_handle_describe(e.asset.bits, text, sizeof text);
        KV_WARN("example_testbed", "asset failed (%s): %s", asset_error_name(e.error), text);
    }
    for (const ExitRequested& e : bus.read<ExitRequested>()) {
        if (g.exit_requested) continue;
        KV_INFO("example_testbed", "exit requested: %s", e.reason == ExitReason::WindowClosed ? "window closed" :
                                               e.reason == ExitReason::Escape ? "escape" : "timer");
        g.exit_requested = true;
    }

    switch (g.state) {
    case ST_INIT: {
        const HostWindow* window = ctx.resource<const HostWindow>("platform.window");
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
        set_state(g, ST_RUN);
        break;
    }

    case ST_RUN:
        if (g.exit_requested) set_state(g, ST_EXIT);
        break;

    case ST_EXIT: {
        // Say goodbye to every ball, then take everything down by command and quit.
        static const u32 k_bounces = ctx.column_id("example_testbed.ball.Bounces");
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
    static const u32 k_bounces = ctx.column_id("example_testbed.ball.Bounces");
    static const u32 k_motion  = ctx.column_id("example_testbed.ball.Motion");
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
    const HostWindow* window = ctx.resource<const HostWindow>("platform.window");
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

bool example_testbed_startup(Engine& engine, ModuleInstance& self) {
    Game* g = arena_push<Game>(self.arena);
    if (!g) return false;
    self.set_state(g);

    Renderer* r = renderer_get(engine);
    if (!r) return false;

    // Assets are requested once and the handles kept. The texture slot exists now and
    // shows the placeholder; AssetReady arrives on the bus when the PNG is resident.
    g->ball_asset = asset_load_texture(engine, "example_testbed/ball.png");
    const TextureAsset* ball = asset_texture(engine, g->ball_asset);
    if (!ball) return false;
    // One material per look: the ball texture through alpha and additive blending with
    // different tints. The background is the opaque one.
    MaterialDesc md[LOOK_COUNT];
    for (MaterialDesc& m : md) m.texture = ball->texture;
    md[LOOK_WHITE].blend      = BlendMode::Alpha;
    md[LOOK_BLUE_ADD].blend   = BlendMode::Additive;
    md[LOOK_BLUE_ADD].tint    = color_rgba(0.3f, 0.8f, 1.0f);
    md[LOOK_GREEN].blend      = BlendMode::Alpha;
    md[LOOK_GREEN].tint       = color_rgba(0.5f, 1.0f, 0.5f);
    md[LOOK_MAGENTA].blend    = BlendMode::Alpha;
    md[LOOK_MAGENTA].tint     = color_rgba(1.0f, 0.4f, 1.0f);
    for (u32 i = 0; i < LOOK_COUNT; ++i) {
        g->materials[i] = renderer_material_create(r, md[i]);
        if (g->materials[i].is_null()) return false;
    }
    MaterialDesc bg_md;
    bg_md.texture = Handle{}; // the default checkerboard
    bg_md.blend   = BlendMode::Opaque;
    bg_md.tint    = color_rgba(0.22f, 0.22f, 0.26f);
    g->bg_material = renderer_material_create(r, bg_md);
    return !g->bg_material.is_null();
}

void example_testbed_shutdown(Engine&, ModuleInstance& self) {
    Game* g = self.state_as<Game>();
    if (g) KV_INFO("example_testbed", "%llu key edges seen", (unsigned long long)g->key_edges);
}

constinit const char* const k_move_reads[]    = {"example_testbed.ball.Sprite", "platform.window"};
constinit const char* const k_move_writes[]   = {"example_testbed.ball.Transform2", "example_testbed.ball.Motion"};
constinit const char* const k_sim_reads[]     = {"platform.window", "example_testbed.ball", "sprite.entity"};
constinit const char* const k_bounced_writes[] = {"example_testbed.ball.Bounces", "example_testbed.ball.Motion"};
constinit const char* const k_window_reads[]  = {"platform.window"};
constinit const SystemDesc k_systems[] = {
    {.name = "example_testbed.move", .phase = Phase::Sim, .fn = example_testbed_move, .reads = k_move_reads, .writes = k_move_writes},
    {.name = "example_testbed.sim",  .phase = Phase::Sim, .fn = example_testbed_sim,  .reads = k_sim_reads}, // after move: it reads what move writes
};
constinit const HandlerDesc k_handlers[] = {
    {.name = "example_testbed.on_bounced",   KV_ON(Bounced, on_bounced), .writes = k_bounced_writes},
    {.name = "example_testbed.on_spawned",   KV_ON(Spawned, on_spawned)},
    {.name = "example_testbed.on_blink_off", KV_ON(BlinkOff, on_blink_off)},
    {.name = "example_testbed.on_blink_on",  KV_ON(BlinkOn, on_blink_on), .reads = k_window_reads},
    {.name = "example_testbed.on_edge",      KV_ON(StateEdge, on_state_edge)},
};

} // namespace

KV_MODULE_DEFINE(example_testbed_module) = {
    .name       = "example_testbed",
    .events     = k_events,
    .components = k_components,
    .kinds      = k_kinds,
    .systems    = k_systems,
    .handlers   = k_handlers,
    .triggers   = k_triggers,
    .startup    = example_testbed_startup,
    .shutdown   = example_testbed_shutdown,
    .memory     = {.reserve = 64 * 1024},
};

} // namespace kv
