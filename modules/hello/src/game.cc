// hello: the game. Startup loads the ball texture and builds two materials; one system
// runs a three-state machine (INIT -> RUN -> EXIT) in phase Sim that bounces the ball
// over a tiled background. ExitRequested from the frame loop sends it through EXIT,
// which cleans up and asks the engine to quit. No window, renderer, loop or options
// here: those are the frameloop module's.
#include "korvid/hello/module.h"
#include "korvid/asset/texture.h"
#include "korvid/core/arena.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/frameloop/frameloop.h"
#include "korvid/platform/events.h"
#include "korvid/platform/module.h"
#include "korvid/render/renderer.h"
#include "korvid/sprite/module.h"
#include "korvid/sprite/world.h"

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

// The hello module's state: handles the game keeps, and the state machine.
struct Game {
    Handle    ball_asset;      // asset.texture
    Handle    ball_material;   // render.material, alpha, ball.png
    Handle    bg_material;     // render.material, opaque, the default checkerboard tinted dark

    GameState state          = ST_INIT;
    bool      exit_requested = false;
    Handle    background;
    Handle    ball;
    f32       ball_speed     = 420.0f; // world units (pixels) per second
    f32       ball_dir       = 1.0f;
    u32       bounces        = 0;
    u64       key_edges      = 0;
};

void set_state(Game& g, GameState next) {
    KV_INFO("game", "%s -> %s", state_name(g.state), state_name(next));
    g.state = next;
}

// Runs once per tick. Each state does its work and names its successor.
void game_update(Game& g, TickContext& ctx) {
    Engine& engine = *ctx.engine;
    SpriteWorld& world = *sprite_world(engine);
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
        g.background = world.create(transform2_at(point2{0.0f, 0.0f}), bg);

        Sprite s;
        s.material = g.ball_material; // placeholder until the PNG lands, then the ball
        s.size     = vec2{96.0f, 96.0f};
        s.layer    = 0;
        g.ball = world.create(transform2_at(point2{0.0f, 0.0f}), s);
        char text[96];
        kv_handle_describe(g.ball.bits, text, sizeof text);
        KV_INFO("game", "ball is %s", text);
        set_state(g, ST_RUN);
        break;
    }

    case ST_RUN: {
        if (g.exit_requested) {
            set_state(g, ST_EXIT);
            break;
        }
        Transform2* t = world.transform(g.ball);
        const Sprite* s = world.sprite(g.ball);
        if (!t || !s) {
            KV_ERROR("game", "ball handle went stale");
            set_state(g, ST_EXIT);
            break;
        }
        // World units are pixels with the origin at the window center; keep the whole
        // sprite on screen and reverse at the edges.
        f32 limit = (f32)window->width() * 0.5f - s->size.x * t->scale.x * 0.5f;
        t->position.x += g.ball_dir * g.ball_speed * (f32)ctx.dt;
        if (t->position.x >= limit && g.ball_dir > 0.0f) {
            t->position.x = limit;
            g.ball_dir    = -1.0f;
            ++g.bounces;
        } else if (t->position.x <= -limit && g.ball_dir < 0.0f) {
            t->position.x = -limit;
            g.ball_dir    = 1.0f;
            ++g.bounces;
        }
        break;
    }

    case ST_EXIT: {
        if (world.destroy(g.ball)) KV_INFO("game", "ball destroyed after %u bounces", g.bounces);
        world.destroy(g.background);
        g.ball = g.background = Handle{};
        KV_ASSERT(world.live() == 0);
        engine_request_quit(engine);
        break;
    }
    }
}

// Phase Sim: what happened, then the game.
void hello_sim(TickContext& ctx) {
    Game& g = *(Game*)ctx.state;
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
    self.state = g;

    Renderer* r = renderer_get(engine);
    if (!r) return false;

    // Assets are requested once and the handles kept. The texture slot exists now and
    // shows the placeholder; AssetReady arrives on the bus when the PNG is resident.
    g->ball_asset = asset_load_texture(engine, "hello/ball.png");
    const TextureAsset* ball = asset_texture(engine, g->ball_asset);
    if (!ball) return false;
    MaterialDesc ball_md;
    ball_md.texture = ball->texture;
    ball_md.blend   = BlendMode::Alpha;
    g->ball_material = renderer_material_create(r, ball_md);
    MaterialDesc bg_md;
    bg_md.texture = Handle{}; // the default checkerboard
    bg_md.blend   = BlendMode::Opaque;
    bg_md.tint    = color_rgba(0.22f, 0.22f, 0.26f);
    g->bg_material = renderer_material_create(r, bg_md);
    return !g->ball_material.is_null() && !g->bg_material.is_null();
}

void hello_shutdown(Engine&, ModuleInstance& self) {
    Game* g = (Game*)self.state;
    if (g) KV_INFO("hello", "%llu key edges seen", (unsigned long long)g->key_edges);
}

constinit const char* const k_sim_reads[]  = {"input.state", "asset.textures"};
constinit const char* const k_sim_writes[] = {"sprite.world"};
constinit const SystemDesc  k_systems[]    = {
    {"hello.sim", Phase::Sim, hello_sim, k_sim_reads, k_sim_writes},
};

} // namespace

KV_MODULE_DEFINE(hello_module) = {
    "hello", {}, {}, k_systems, hello_startup, hello_shutdown,
    {.reserve = 64 * 1024},
};

} // namespace kv
