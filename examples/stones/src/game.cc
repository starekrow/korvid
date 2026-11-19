// stones, written the way you'd write it if you didn't want to learn the engine.
//
// There's a player stone and a bunch of loose stones in a box. Arrows push the player,
// the player chases the mouse while the left button is down, space drops another stone
// in, stones bounce off the walls and each other. All of that
// is my own structs in my own arrays, updated in one function, top to bottom, every tick.
// The engine gives me a window, the keys, sprites to draw with, and a way to print my
// structs. That's all I asked it for.
#include "korvid/stones/module.h"
#include "korvid/asset_render/material.h"
#include "korvid/core/arena.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/random.h"
#include "korvid/frameloop/frameloop.h"
#include "korvid/math/fields.h"
#include "korvid/platform_hid/host_window.h"
#include "korvid/platform_hid/input.h"
#include "korvid/render/renderer.h"
#include "korvid/sprite/sprite.h"
#include "korvid/sprite/transform2.h"

namespace kv {

namespace {

// ---- my stuff ----------------------------------------------------------------------------

struct Stone {
    point2 pos;
    vec2   vel;
    f32    radius;
    f32    mass;
    u32    bumps;   // how many times it hit something
    Handle sprite;  // the engine's sprite for it; I move it to match
};

// I described the struct so the engine can print it. The engine logs it at startup as a
// component, which it is, sort of; nothing of mine lives in an engine table though, so
// it'll know the shape of a Stone and never see one.
constinit const FieldDesc k_stone_fields[] = {KV_FIELDS(Stone, pos, vel, radius, mass, bumps, sprite)};
constinit const ComponentDesc k_components[] = {KV_COMPONENT(Stone, k_stone_fields)};

struct Game {
    Array<Stone> stones;     // stones[0] is the player
    Handle       material;
    Handle       player_material;
    Handle       stone_look, player_look; // asset_render.material
    Rng          rng;
    u32          ticks   = 0;
    u32          log_in  = 60;
    f32          w = 0, h = 0; // the box, in pixels, origin at the center
};

constexpr f32 kPlayerRadius = 36.0f;
constexpr f32 kStoneRadius  = 22.0f;
constexpr f32 kPush         = 900.0f; // pixels per second^2
constexpr f32 kDrag         = 0.4f;

// Make a stone, and a sprite to show it with. Direct, because I hold the write on the
// sprite kind (I had to declare that, see the bottom of the file).
Stone& add_stone(Game& g, Table& sprites, point2 at, vec2 vel, f32 radius, Handle material, i32 layer) {
    Stone s{};
    s.pos = at; s.vel = vel; s.radius = radius; s.mass = radius * radius;
    Sprite sp;
    sp.material = material;
    sp.size     = vec2{radius * 2.0f, radius * 2.0f};
    sp.layer    = layer;
    s.sprite = sprites.spawn(transform2_at(at), sp);
    return g.stones.push(s);
}

// Two circles that overlap: push them apart and swap the velocity along the line
// between them, weighted by mass. Textbook, no engine involved.
void bounce(Stone& a, Stone& b) {
    vec2 d = b.pos - a.pos;
    f32 dist = length(d);
    f32 want = a.radius + b.radius;
    if (dist >= want || dist <= 0.0001f) return;
    vec2 n = d / dist;
    f32 overlap = want - dist;
    f32 total = a.mass + b.mass;
    a.pos -= n * (overlap * b.mass / total);
    b.pos += n * (overlap * a.mass / total);
    f32 rel = dot(b.vel - a.vel, n);
    if (rel > 0.0f) return; // already separating
    f32 j = -(1.0f + 0.9f) * rel / (1.0f / a.mass + 1.0f / b.mass);
    a.vel -= n * (j / a.mass);
    b.vel += n * (j / b.mass);
    ++a.bumps; ++b.bumps;
}

// ---- the whole game, every tick, in order -------------------------------------------------

void stones_tick(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    f32 dt = (f32)ctx.dt;
    const Input*      input  = ctx.resource<const Input>("input.state");
    const HostWindow* window = ctx.resource<const HostWindow>("platform_hid.window");
    Table& sprites = *ctx.write_kind("sprite.entity");
    g.w = (f32)window->width();
    g.h = (f32)window->height();

    // First tick: make everything.
    if (g.stones.len == 0) {
        g.rng = Rng::seeded(42);
        add_stone(g, sprites, point2{0, 0}, vec2{}, kPlayerRadius, g.player_material, 2);
        for (u32 i = 0; i < 12; ++i)
            add_stone(g, sprites, point2{g.rng.range(-g.w * 0.4f, g.w * 0.4f), g.rng.range(-g.h * 0.4f, g.h * 0.4f)},
                      vec2{g.rng.range(-150, 150), g.rng.range(-150, 150)}, kStoneRadius, g.material, 1);
        KV_INFO("stones", "arrows push, hold the left mouse button to chase the cursor, space drops a stone, esc quits");
    }
    ++g.ticks;

    // Keys. Just look at them; I don't need an event to tell me a key is down.
    Stone& player = g.stones[0];
    if (input->sampled.down(Key::Left))  player.vel.x -= kPush * dt;
    if (input->sampled.down(Key::Right)) player.vel.x += kPush * dt;
    if (input->sampled.down(Key::Up))    player.vel.y += kPush * dt;
    if (input->sampled.down(Key::Down))  player.vel.y -= kPush * dt;
    // The mouse: while the left button is down, push toward the cursor. The pointer comes
    // in window pixels from the top-left; the box is centered with y up, so convert.
    const MouseState& mouse = input->sampled.mouse;
    if (mouse.down(MouseButton::Left)) {
        f32 mx, my;
        mouse_screen(mouse.x, mouse.y, window->width(), window->height(), &mx, &my);
        vec2 to{mx - player.pos.x, my - player.pos.y};
        f32 dist = kv::sqrt(to.x * to.x + to.y * to.y);
        if (dist > kPlayerRadius * 0.5f) player.vel += to * (kPush * dt / dist);
    }
    if (input->sampled.down(Key::Space) && g.ticks % 10 == 0 && g.stones.len < 200) {
        f32 side = g.rng.chance(0.5f) ? 1.0f : -1.0f;
        add_stone(g, sprites, point2{side * (g.w * 0.5f - kStoneRadius), g.rng.range(-g.h * 0.4f, g.h * 0.4f)},
                  vec2{-side * 200.0f, g.rng.range(-80, 80)}, kStoneRadius, g.material, 1);
    }

    // Move everything, with a little drag, and keep it in the box.
    f32 hw = g.w * 0.5f, hh = g.h * 0.5f;
    for (Stone& s : g.stones) {
        s.vel *= 1.0f - kDrag * dt;
        s.pos += s.vel * dt;
        if (s.pos.x - s.radius < -hw) { s.pos.x = -hw + s.radius; s.vel.x = -s.vel.x * 0.9f; ++s.bumps; }
        if (s.pos.x + s.radius >  hw) { s.pos.x =  hw - s.radius; s.vel.x = -s.vel.x * 0.9f; ++s.bumps; }
        if (s.pos.y - s.radius < -hh) { s.pos.y = -hh + s.radius; s.vel.y = -s.vel.y * 0.9f; ++s.bumps; }
        if (s.pos.y + s.radius >  hh) { s.pos.y =  hh - s.radius; s.vel.y = -s.vel.y * 0.9f; ++s.bumps; }
    }

    // Everything against everything. It's a few hundred stones at most; fine.
    for (u32 i = 0; i < g.stones.len; ++i)
        for (u32 j = i + 1; j < g.stones.len; ++j)
            bounce(g.stones[i], g.stones[j]);

    // Tell the sprites where things ended up.
    for (const Stone& s : g.stones) {
        Transform2* t = sprites.get<Transform2>(s.sprite);
        if (t) t->position = s.pos;
    }

    // Once a second, say what the player is up to. This is what the reflection was for.
    if (--g.log_in == 0) {
        g.log_in = 60;
        char text[256];
        component_describe(*engine_component<Stone>(*ctx.engine), &player, text, sizeof text);
        KV_INFO("stones", "%u stones; %s", g.stones.len, text);
    }

    for (const ExitRequested& e : ctx.received<ExitRequested>()) {
        KV_UNUSED(e);
        engine_request_quit(*ctx.engine);
    }
}

// ---- the boring part: a texture, two materials, the module table -----------------------------

bool stones_startup(Engine& engine, ModuleInstance& self) {
    Game* g = arena_push<Game>(self.arena);
    if (!g) return false;
    self.set_state(g);
    g->stones.alloc = &self.general;

    Renderer* r = renderer_get(engine);
    if (!r) return false;
    KV_UNUSED(r);
    g->stone_look  = asset_load_material(engine, "stones/stone.material.json");
    g->player_look = asset_load_material(engine, "stones/player.material.json");
    const MaterialAsset* stone  = asset_material(engine, g->stone_look);
    const MaterialAsset* player = asset_material(engine, g->player_look);
    if (!stone || !player) return false;
    g->material        = stone->material;
    g->player_material = player->material;
    return true;
}

void stones_shutdown(Engine&, ModuleInstance& self) {
    Game* g = self.state_as<Game>();
    if (!g) return;
    u32 bumps = 0;
    for (const Stone& s : g->stones) bumps += s.bumps;
    KV_INFO("stones", "%u stones, %u bumps all told", g->stones.len, bumps);
    g->stones.free();
}

// The engine wanted to know what I touch. Sprites: all of them, since I make and move
// them. The keys, the window and the reasons to quit: I look at them.
constinit const SystemDesc k_systems[] = {
    {   .name = ".tick",
        .phase = Phase::Sim,
        .fn = stones_tick,
        .reads = "input.state, platform_hid.window, ExitRequested",
        .writes = "sprite.entity"
    },
};

} // namespace

KV_MODULE_DEFINE(stones_module) = {
    .name       = "stones",
    .components = k_components,
    .systems    = k_systems,
    .startup    = stones_startup,
    .shutdown   = stones_shutdown,
    .memory     = {.reserve = 64 * 1024},
};

} // namespace kv
