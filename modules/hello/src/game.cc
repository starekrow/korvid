// hello: the game. Startup loads the ball texture and builds the materials; one system
// runs a three-state machine (INIT -> RUN -> EXIT) in phase Sim that bounces four balls
// over a tiled background: one horizontally, one vertically, one in random directions,
// and one that blinks in and out of existence. Each has its own material, so all three
// blend modes are on screen. ExitRequested from the frame loop sends it through EXIT,
// which cleans up and asks the engine to quit. No window, renderer, loop or options
// here: those are the frameloop module's.
#include "korvid/hello/module.h"
#include "korvid/asset/texture.h"
#include "korvid/core/arena.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/random.h"
#include "korvid/frameloop/frameloop.h"
#include "korvid/platform/events.h"
#include "korvid/platform/module.h"
#include "korvid/render/renderer.h"
#include "korvid/sprite/sprite.h"
#include "korvid/sprite/transform2.h"

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

enum BallKind : u8 { BALL_HORIZONTAL, BALL_VERTICAL, BALL_RANDOM, BALL_BLINK, BALL_COUNT };

struct Ball {
    Handle entity;
    vec2   velocity;       // world units (pixels) per second
    f32    size;
    i32    layer;
    Handle material;
    u32    blink_ticks;    // BALL_BLINK: ticks until it toggles
    u32    bounces;
};

// The hello module's state: handles the game keeps, and the state machine.
struct Game {
    Handle    ball_asset;                 // asset.texture
    Handle    materials[BALL_COUNT];      // render.material, one per ball
    Handle    bg_material;                // render.material, opaque, the default checkerboard tinted dark

    GameState state          = ST_INIT;
    bool      exit_requested = false;
    Handle    background;
    Ball      balls[BALL_COUNT];
    Rng       rng;
    u64       key_edges      = 0;
};

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

vec2 random_direction(Rng& rng, f32 speed) {
    f32 a = rng.range(0.0f, 2.0f * KV_PI);
    return vec2{cosf(a) * speed, sinf(a) * speed};
}

// The game holds the write on "sprite.entity", so it spawns directly and has the handle at once.
void spawn_ball(Table& world, Ball& b, point2 at) {
    Sprite s;
    s.material = b.material;
    s.size     = vec2{b.size, b.size};
    s.layer    = b.layer;
    b.entity   = world.spawn(transform2_at(at), s);
}

void set_state(Game& g, GameState next) {
    KV_INFO("game", "%s -> %s", state_name(g.state), state_name(next));
    g.state = next;
}

// Runs once per tick. Each state does its work and names its successor.
void game_update(Game& g, TickContext& ctx) {
    Engine& engine = *ctx.engine;
    Table& world = *ctx.write_kind("sprite.entity"); // the engine's table for the sprite kind
    HostWindow* window = platform_window(engine);
    switch (g.state) {
    case ST_INIT: {
        // A background covering the window, tiling the checkerboard at 16-pixel cells.
        f32 w = (f32)window->width(), h = (f32)window->height();
        Sprite bg;
        bg.material = g.bg_material;
        bg.size     = vec2{w, h};
        bg.uv       = rect2{0.0f, 0.0f, w / 128.0f, h / 128.0f};
        bg.layer    = -1;
        g.background = world.spawn(transform2_at(point2{0.0f, 0.0f}), bg);

        g.rng = Rng::seeded(0x5eed, 1);
        Ball* b = g.balls;
        b[BALL_HORIZONTAL] = Ball{{}, vec2{420.0f, 0.0f}, 96.0f, 2, g.materials[BALL_HORIZONTAL], 0, 0};
        b[BALL_VERTICAL]   = Ball{{}, vec2{0.0f, 300.0f}, 72.0f, 1, g.materials[BALL_VERTICAL], 0, 0};
        b[BALL_RANDOM]     = Ball{{}, random_direction(g.rng, 360.0f), 56.0f, 3, g.materials[BALL_RANDOM], 0, 0};
        b[BALL_BLINK]      = Ball{{}, vec2{}, 120.0f, 0, g.materials[BALL_BLINK], 45, 0};
        spawn_ball(world, b[BALL_HORIZONTAL], point2{0.0f, 0.0f});
        spawn_ball(world, b[BALL_VERTICAL], point2{-200.0f, 0.0f});
        spawn_ball(world, b[BALL_RANDOM], point2{200.0f, 100.0f});
        spawn_ball(world, b[BALL_BLINK], point2{0.0f, -120.0f});
        for (u32 i = 0; i < BALL_COUNT; ++i) {
            char text[256];
            kv_handle_describe(b[i].entity.bits, text, sizeof text);
            KV_INFO("game", "ball %u is %s", i, text);
        }
        set_state(g, ST_RUN);
        break;
    }

    case ST_RUN: {
        if (g.exit_requested) {
            set_state(g, ST_EXIT);
            break;
        }
        // World units are pixels with the origin at the window center.
        f32 half_w = (f32)window->width() * 0.5f, half_h = (f32)window->height() * 0.5f;
        f32 dt = (f32)ctx.dt;
        for (u32 i = 0; i < BALL_COUNT; ++i) {
            Ball& b = g.balls[i];
            if (i == BALL_BLINK) {
                // Exists for a while, vanishes for a while: creates and removes, every time.
                // The removal is a command, so it takes effect at the end of this phase.
                if (--b.blink_ticks == 0) {
                    if (world.valid(b.entity)) { ctx.destroy(b.entity); b.entity = Handle{}; b.blink_ticks = 30; }
                    else { spawn_ball(world, b, point2{g.rng.range(-half_w * 0.5f, half_w * 0.5f), -120.0f}); b.blink_ticks = 45; }
                }
                continue;
            }
            Transform2* t = world.get<Transform2>(b.entity);
            if (!t) { KV_ERROR("game", "ball %u went stale", i); set_state(g, ST_EXIT); break; }
            if (bounce(*t, b.velocity, b.size * 0.5f, half_w, half_h, dt)) {
                ++b.bounces;
                if (i == BALL_RANDOM) b.velocity = random_direction(g.rng, g.rng.range(240.0f, 520.0f));
            }
        }
        break;
    }

    case ST_EXIT: {
        for (u32 i = 0; i < BALL_COUNT; ++i) {
            Ball& b = g.balls[i];
            if (world.destroy(b.entity)) KV_INFO("game", "ball %u destroyed after %u bounces", i, b.bounces);
            b.entity = Handle{};
        }
        world.destroy(g.background);
        g.background = Handle{};
        KV_ASSERT(world.live() == 0);
        engine_request_quit(engine);
        break;
    }
    }
}

// Phase Sim: what happened, then the game.
void hello_sim(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    EventBus& bus = *ctx.events;

    for (const KeyDown& e : bus.read<KeyDown>()) {
        ++g.key_edges;
        if (!e.repeat) KV_INFO("hello", "tick %llu: key down %s", (unsigned long long)ctx.tick, key_name(e.key));
    }
    for (const KeyUp& e : bus.read<KeyUp>()) {
        ++g.key_edges;
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
    for (const ExitRequested& e : bus.read<ExitRequested>()) {
        if (g.exit_requested) continue;
        KV_INFO("hello", "exit requested: %s", e.reason == ExitReason::WindowClosed ? "window closed" :
                                               e.reason == ExitReason::Escape ? "escape" : "timer");
        g.exit_requested = true;
    }

    game_update(g, ctx);
}

bool hello_startup(Engine& engine, ModuleInstance& self) {
    Game* g = arena_push<Game>(self.arena);
    if (!g) return false;
    self.set_state(g);

    Renderer* r = renderer_get(engine);
    if (!r) return false;

    // Assets are requested once and the handles kept. The texture slot exists now and
    // shows the placeholder; AssetReady arrives on the bus when the PNG is resident.
    g->ball_asset = asset_load_texture(engine, "hello/ball.png");
    const TextureAsset* ball = asset_texture(engine, g->ball_asset);
    if (!ball) return false;
    // One material per ball: the ball texture through alpha and additive blending with
    // different tints. The background is the opaque one.
    MaterialDesc md[BALL_COUNT];
    md[BALL_HORIZONTAL].texture = ball->texture;
    md[BALL_HORIZONTAL].blend   = BlendMode::Alpha;
    md[BALL_VERTICAL].texture   = ball->texture;
    md[BALL_VERTICAL].blend     = BlendMode::Additive;
    md[BALL_VERTICAL].tint      = color_rgba(0.3f, 0.8f, 1.0f);
    md[BALL_RANDOM].texture     = ball->texture;
    md[BALL_RANDOM].blend       = BlendMode::Alpha;
    md[BALL_RANDOM].tint        = color_rgba(0.5f, 1.0f, 0.5f);
    md[BALL_BLINK].texture      = ball->texture;
    md[BALL_BLINK].blend        = BlendMode::Alpha;
    md[BALL_BLINK].tint         = color_rgba(1.0f, 0.4f, 1.0f);
    for (u32 i = 0; i < BALL_COUNT; ++i) {
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

void hello_shutdown(Engine&, ModuleInstance& self) {
    Game* g = self.state_as<Game>();
    if (g) KV_INFO("hello", "%llu key edges seen", (unsigned long long)g->key_edges);
}

constinit const char* const k_sim_reads[]  = {"input.state", "asset.textures"};
constinit const char* const k_sim_writes[] = {"sprite.entity"}; // spawn and destroy, and every column
constinit const SystemDesc  k_systems[]    = {
    {.name = "hello.sim", .phase = Phase::Sim, .fn = hello_sim, .reads = k_sim_reads, .writes = k_sim_writes},
};

} // namespace

KV_MODULE_DEFINE(hello_module) = {
    .name     = "hello",
    .systems  = k_systems,
    .startup  = hello_startup,
    .shutdown = hello_shutdown,
    .memory   = {.reserve = 64 * 1024},
};

} // namespace kv
