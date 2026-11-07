// example_rocks: an asteroids-like. The ship is steered from the keyboard (or by a script
// with --demo), bullets expire by scheduled event, rocks split when hit and spawn debris,
// waves arrive on a timer, the ship's death is a trigger on its Health column, and a
// restart follows two seconds later. Everything the game knows is in kinds and one
// published Score; no handle is kept anywhere except on the events that carry them.
#include "korvid/example_rocks/module.h"
#include "korvid/asset/texture.h"
#include "korvid/collision/module.h"
#include "korvid/core/arena.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/random.h"
#include "korvid/frameloop/frameloop.h"
#include "korvid/math/fields.h"
#include "korvid/platform/events.h"
#include "korvid/platform/host_window.h"
#include "korvid/platform/input.h"
#include "korvid/render/renderer.h"
#include "korvid/sprite/sprite.h"
#include "korvid/sprite/transform2.h"
#include <math.h>

namespace kv {

namespace {

// ---- components ------------------------------------------------------------------------
struct Velocity { vec2 v; f32 spin; };   // pixels per second, radians per second
struct Health   { i32 hp; };
struct Lifetime { f32 left; f32 total; }; // seconds
struct RockSize { u8 size; };             // 0 big, 1 medium, 2 small
struct Ship     { f32 cooldown; u8 thrusting; };

constinit const FieldDesc k_velocity_fields[] = {KV_FIELDS(Velocity, v, spin)};
constinit const FieldDesc k_health_fields[]   = {KV_FIELDS(Health, hp)};
constinit const FieldDesc k_lifetime_fields[] = {KV_FIELDS(Lifetime, left, total)};
constinit const FieldDesc k_rocksize_fields[] = {KV_FIELDS(RockSize, size)};
constinit const FieldDesc k_ship_fields[]     = {KV_FIELDS(Ship, cooldown, thrusting)};
constinit const ComponentDesc k_components[] = {
    KV_COMPONENT(Velocity, k_velocity_fields),
    KV_COMPONENT(Health, k_health_fields, CompPrev), // the death trigger watches it
    KV_COMPONENT(Lifetime, k_lifetime_fields),
    KV_COMPONENT(RockSize, k_rocksize_fields),
    KV_COMPONENT(Ship, k_ship_fields),
};

// ---- kinds -----------------------------------------------------------------------------
constinit u32 g_ship_kind = 0, g_bullet_kind = 0, g_rock_kind = 0, g_debris_kind = 0;
constinit const char* const k_ship_components[]   = {"Transform2", "Sprite", "Velocity", "Health", "Collider2", "Ship"};
constinit const char* const k_bullet_components[] = {"Transform2", "Sprite", "Velocity", "Collider2"};
constinit const char* const k_rock_components[]   = {"Transform2", "Sprite", "Velocity", "Collider2", "RockSize"};
constinit const char* const k_debris_components[] = {"Transform2", "Sprite", "Velocity", "Lifetime"};
constinit const KindDesc k_kinds[] = {
    {.name = "example_rocks.ship",   .components = k_ship_components,   .capacity = 1,    .id = &g_ship_kind},
    {.name = "example_rocks.bullet", .components = k_bullet_components, .capacity = 4096, .id = &g_bullet_kind},
    {.name = "example_rocks.rock",   .components = k_rock_components,   .capacity = 1024, .id = &g_rock_kind},
    {.name = "example_rocks.debris", .components = k_debris_components, .capacity = 8192, .id = &g_debris_kind},
};

// ---- events ----------------------------------------------------------------------------
struct RockHit      { Handle rock, bullet; };
struct ShipHit      { Handle ship, rock; };
struct ScoreChanged { i32 points, total; }; // next tick: nobody acts on it within the tick
struct Expire       { Handle bullet; };
struct SpawnWave    { u32 count; };
struct GameOver     { i32 score; };
struct Restart      { u32 nothing; };

constinit const FieldDesc k_rock_hit_fields[] = {KV_FIELDS(RockHit, rock, bullet)};
constinit const FieldDesc k_ship_hit_fields[] = {KV_FIELDS(ShipHit, ship, rock)};
constinit const FieldDesc k_score_fields[]    = {KV_FIELDS(ScoreChanged, points, total)};
constinit const FieldDesc k_expire_fields[]   = {KV_FIELDS(Expire, bullet)};
constinit const FieldDesc k_wave_fields[]     = {KV_FIELDS(SpawnWave, count)};
constinit const FieldDesc k_over_fields[]     = {KV_FIELDS(GameOver, score)};
constinit const FieldDesc k_restart_fields[]  = {KV_FIELDS(Restart, nothing)};
constinit const EventTypeDesc k_events[] = {
    KV_EVENT_EX(RockHit, k_rock_hit_fields, 0),
    KV_EVENT_EX(ShipHit, k_ship_hit_fields, 0),
    KV_EVENT_EX(ScoreChanged, k_score_fields, EvNextTick),
    KV_EVENT_EX(Expire, k_expire_fields, 0),
    KV_EVENT_EX(SpawnWave, k_wave_fields, 0),
    KV_EVENT_EX(GameOver, k_over_fields, 0),
    KV_EVENT_EX(Restart, k_restart_fields, 0),
};

constinit u32 g_death = 0;
constinit const TriggerDesc k_triggers[] = {
    {.name = "example_rocks.death", .field = "example_rocks.ship.Health.hp", .edge = TriggerEdge::CrossedBelow, .value = 1.0, .id = &g_death},
};

// ---- the published score and the module's state --------------------------------------
struct Score { i32 total; u32 rocks_shot; u32 wave; u32 games; };

enum Look : u8 { LOOK_SHIP, LOOK_ROCK, LOOK_BULLET, LOOK_DEBRIS, LOOK_COUNT };

struct Game {
    Score  score;
    Handle textures[3];          // asset.texture: ship, rock, dot
    Handle materials[LOOK_COUNT]; // render.material
    u16    ship_hk, bullet_hk, rock_hk; // handle kinds, to tell what an overlap is between
    bool   started = false;
    bool   demo    = false;
    u32    next_wave = 0;        // the pending wave's ticket, cancelled at game over
    Rng    rng;
};

// Collider layers.
constexpr u32 L_SHIP = 1, L_BULLET = 2, L_ROCK = 4;

constexpr f32 kShipRadius     = 20.0f;
constexpr f32 kBulletSpeed    = 520.0f;
constexpr u32 kBulletLife     = 72;   // ticks
constexpr f32 kFireCooldown   = 0.15f; // seconds
constexpr u32 kWaveTicks      = 600;
constexpr u32 kRestartTicks   = 120;
constexpr f32 kRockRadius[3]  = {44.0f, 26.0f, 14.0f};
constexpr f32 kRockSprite[3]  = {96.0f, 56.0f, 30.0f};
constexpr i32 kRockPoints[3]  = {20, 50, 100};

// ---- helpers ---------------------------------------------------------------------------
Sprite sprite_of(const Game& g, Look look, f32 size, i32 layer, color tint = color_white()) {
    Sprite s;
    s.material = g.materials[look];
    s.size     = vec2{size, size};
    s.layer    = layer;
    s.tint     = tint;
    return s;
}

vec2 heading(f32 rotation) { return vec2{cosf(rotation), sinf(rotation)}; }

// A rock of `size` at `at`, drifting in a random direction. Direct: the caller holds the write.
void spawn_rock(Table& rocks, Game& g, point2 at, u8 size, vec2 drift) {
    Velocity v{drift, g.rng.range(-1.5f, 1.5f)};
    rocks.spawn(transform2_at(at, g.rng.range(0.0f, 2.0f * KV_PI)), sprite_of(g, LOOK_ROCK, kRockSprite[size], 1),
                v, collider_circle(kRockRadius[size], L_ROCK, L_SHIP | L_BULLET), RockSize{size});
}

void spawn_debris(TickContext& ctx, Game& g, point2 at, u32 count, color tint) {
    for (u32 i = 0; i < count; ++i) {
        vec2 v = heading(g.rng.range(0.0f, 2.0f * KV_PI)) * g.rng.range(60.0f, 220.0f);
        f32 life = g.rng.range(0.3f, 0.8f);
        ctx.spawn_later(g_debris_kind, transform2_at(at), sprite_of(g, LOOK_DEBRIS, 6.0f, 0, tint), Velocity{v, 0.0f}, Lifetime{life, life});
    }
}

void spawn_ship(TickContext& ctx, Game& g) {
    ctx.spawn_later(g_ship_kind, transform2_at(point2{0, 0}, KV_PI * 0.5f), sprite_of(g, LOOK_SHIP, 48.0f, 2),
                    Velocity{}, Health{3}, collider_circle(kShipRadius, L_SHIP, L_ROCK), Ship{0.0f, 0});
}

// ---- systems (phase Sim) ---------------------------------------------------------------

// The ship answers the keys, or the script: turn, thrust, fire. Fires by direct spawn,
// since it holds the bullet write, and schedules each bullet's expiry.
void control(TickContext& ctx) {
    static const u32 k_input  = ctx.resource_id("input.state");
    Game& g = ctx.state_as<Game>();
    const Input* input = ctx.resource<const Input>(k_input);
    f32 dt = (f32)ctx.dt;
    bool left, right, thrust, fire;
    if (g.demo) {
        u64 t = ctx.tick;
        left = (t / 90) % 2 == 0; right = !left;
        thrust = (t / 60) % 3 != 2;
        fire   = t % 8 == 0;
    } else {
        left = input->sampled.down(Key::Left);   right = input->sampled.down(Key::Right);
        thrust = input->sampled.down(Key::Up);   fire  = input->sampled.down(Key::Space);
    }
    for (KindRows k : ctx.view<Transform2, Velocity, Ship>()) {
        Span<Transform2> t = k.col<Transform2>();
        Span<Velocity>   v = k.col<Velocity>();
        Span<Ship>       s = k.col<Ship>();
        for (u32 i = 0; i < k.rows(); ++i) {
            if (!k.alive(i)) continue;
            if (left)  t[i].rotation += 3.5f * dt;
            if (right) t[i].rotation -= 3.5f * dt;
            if (thrust) v[i].v += heading(t[i].rotation) * (420.0f * dt);
            v[i].v *= 1.0f - 1.2f * dt; // drag
            s[i].thrusting = thrust;
            s[i].cooldown -= dt;
            if (fire && s[i].cooldown <= 0.0f) {
                s[i].cooldown = kFireCooldown;
                Table* bullets = ctx.write_kind(g_bullet_kind);
                vec2 dir = heading(t[i].rotation);
                point2 at = t[i].position + dir * 26.0f;
                Handle b = bullets->spawn(transform2_at(at), sprite_of(g, LOOK_BULLET, 12.0f, 3, color_rgba(1.0f, 0.95f, 0.5f)),
                                          Velocity{v[i].v + dir * kBulletSpeed, 0.0f}, collider_circle(4.0f, L_BULLET, L_ROCK));
                if (!b.is_null()) ctx.schedule_in(Expire{b}, kBulletLife);
            }
        }
    }
}

// Everything with a velocity moves.
void move(TickContext& ctx) {
    f32 dt = (f32)ctx.dt;
    for (KindRows k : ctx.view<Transform2, const Velocity>()) {
        Span<Transform2>     t = k.col<Transform2>();
        Span<const Velocity> v = k.col<const Velocity>();
        for (u32 i = 0; i < k.rows(); ++i) {
            if (!k.alive(i)) continue;
            t[i].position += v[i].v * dt;
            t[i].rotation += v[i].spin * dt;
        }
    }
}

// Everything that left the window comes back on the other side. After move, by declaration.
void wrap(TickContext& ctx) {
    static const u32 k_window = ctx.resource_id("platform.window");
    const HostWindow* w = ctx.resource<const HostWindow>(k_window);
    f32 hw = (f32)w->width() * 0.5f + 32.0f, hh = (f32)w->height() * 0.5f + 32.0f;
    for (KindRows k : ctx.view<Transform2, const Velocity>()) {
        Span<Transform2> t = k.col<Transform2>();
        for (u32 i = 0; i < k.rows(); ++i) {
            if (!k.alive(i)) continue;
            point2& p = t[i].position;
            if (p.x > hw) p.x -= 2 * hw; else if (p.x < -hw) p.x += 2 * hw;
            if (p.y > hh) p.y -= 2 * hh; else if (p.y < -hh) p.y += 2 * hh;
        }
    }
}

// Debris fades and goes.
void debris_fade(TickContext& ctx) {
    f32 dt = (f32)ctx.dt;
    for (KindRows k : ctx.view<Lifetime, Sprite>()) {
        Span<Lifetime> life = k.col<Lifetime>();
        Span<Sprite>   spr  = k.col<Sprite>();
        for (u32 i = 0; i < k.rows(); ++i) {
            if (!k.alive(i)) continue;
            life[i].left -= dt;
            if (life[i].left <= 0.0f) { ctx.destroy(k.handle_at(i)); continue; }
            spr[i].tint.a = life[i].left / life[i].total;
        }
    }
}

// Boot, logging and the way out.
void game(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    if (!g.started) {
        g.started = true;
        g.demo    = frameloop_flag(*ctx.engine, "--demo");
        g.rng     = Rng::seeded(0x70c45, 7);
        KV_INFO("rocks", "%s. Arrows turn and thrust, space fires, Esc quits.", g.demo ? "Demo: the ship flies itself" : "Your ship");
        ctx.events->emit(Restart{});
    }
    for (const AssetFailed& e : ctx.events->read<AssetFailed>()) {
        char text[160];
        kv_handle_describe(e.asset.bits, text, sizeof text);
        KV_WARN("rocks", "asset failed (%s): %s", asset_error_name(e.error), text);
    }
    for (const ExitRequested& e : ctx.events->read<ExitRequested>()) {
        KV_INFO("rocks", "exit (%s): score %d, %u rocks, wave %u, %u games", e.reason == ExitReason::Timer ? "timer" : "player",
                g.score.total, g.score.rocks_shot, g.score.wave, g.score.games);
        engine_request_quit(*ctx.engine);
    }
}

// ---- handlers --------------------------------------------------------------------------

// An overlap is a hit when the kinds say so. Handle kinds were noted at startup.
void on_overlap(TickContext& ctx, Span<const OverlapBegin> overlaps) {
    Game& g = ctx.state_as<Game>();
    for (const OverlapBegin& o : overlaps) {
        u16 ka = o.a.kind(), kb = o.b.kind();
        if (ka == g.bullet_hk && kb == g.rock_hk) ctx.events->emit(RockHit{o.b, o.a});
        else if (ka == g.rock_hk && kb == g.bullet_hk) ctx.events->emit(RockHit{o.a, o.b});
        else if (ka == g.ship_hk && kb == g.rock_hk) ctx.events->emit(ShipHit{o.a, o.b});
        else if (ka == g.rock_hk && kb == g.ship_hk) ctx.events->emit(ShipHit{o.b, o.a});
    }
}

// The rock splits (direct: this handler holds the rock write), the bullet goes, debris
// flies by command, the score moves. A bullet that hit two rocks is destroyed once; the
// second RockHit still splits its rock, since the rock handle is live.
void on_rock_hit(TickContext& ctx, Span<const RockHit> hits) {
    static const u32 k_score = ctx.resource_id("example_rocks.score");
    Game& g = ctx.state_as<Game>();
    Table* rocks = ctx.write_kind(g_rock_kind);
    Score* score = ctx.resource<Score>(k_score);
    for (const RockHit& h : hits) {
        const Transform2* t  = rocks->get<Transform2>(h.rock);
        const RockSize*   rs = rocks->get<RockSize>(h.rock);
        const Velocity*   rv = rocks->get<Velocity>(h.rock);
        if (!t) continue; // two bullets, one rock, one tick: the second finds it split already
        point2 at = t->position;
        u8 size = rs->size;
        vec2 drift = rv->v;
        rocks->destroy(h.rock);
        ctx.destroy(h.bullet);
        if (size < 2) {
            f32 a = g.rng.range(0.5f, 1.1f);
            vec2 d1{drift.x * cosf(a) - drift.y * sinf(a), drift.x * sinf(a) + drift.y * cosf(a)};
            vec2 d2{drift.x * cosf(-a) - drift.y * sinf(-a), drift.x * sinf(-a) + drift.y * cosf(-a)};
            spawn_rock(*rocks, g, at, (u8)(size + 1), d1 * 1.4f);
            spawn_rock(*rocks, g, at, (u8)(size + 1), d2 * 1.4f);
        }
        spawn_debris(ctx, g, at, 6, color_rgba(1.0f, 0.7f, 0.3f));
        score->total += kRockPoints[size];
        score->rocks_shot += 1;
        ctx.events->emit(ScoreChanged{kRockPoints[size], score->total});
    }
}

// The ship takes a hit: the rock goes, health drops by a row write; the trigger notices.
void on_ship_hit(TickContext& ctx, Span<const ShipHit> hits) {
    static const u32 k_hp = ctx.column_id("example_rocks.ship.Health");
    static const u32 k_pos = ctx.column_id("example_rocks.ship.Transform2");
    Game& g = ctx.state_as<Game>();
    Span<Health> hp = ctx.write<Health>(k_hp);
    Span<const Transform2> pos = ctx.read<Transform2>(k_pos);
    for (const ShipHit& h : hits) {
        u32 i = h.ship.index();
        hp[i].hp -= 1;
        ctx.destroy(h.rock);
        spawn_debris(ctx, g, pos[i].position, 10, color_rgba(0.6f, 0.8f, 1.0f));
        KV_INFO("rocks", "hit! %d left", hp[i].hp);
    }
}

void on_score(TickContext&, Span<const ScoreChanged> changes) {
    for (const ScoreChanged& s : changes) KV_INFO("rocks", "+%d -> %d", s.points, s.total);
}

void on_expire(TickContext& ctx, Span<const Expire> expiries) {
    for (const Expire& e : expiries) ctx.destroy(e.bullet); // already gone if it hit: silently nothing
}

// A wave: rocks arrive from the edges, drifting inward. By command, so no write and no order.
void on_wave(TickContext& ctx, Span<const SpawnWave> waves) {
    static const u32 k_window = ctx.resource_id("platform.window");
    static const u32 k_score  = ctx.resource_id("example_rocks.score");
    Game& g = ctx.state_as<Game>();
    const HostWindow* w = ctx.resource<const HostWindow>(k_window);
    Score* score = ctx.resource<Score>(k_score);
    f32 hw = (f32)w->width() * 0.5f, hh = (f32)w->height() * 0.5f;
    for (const SpawnWave& wave : waves) {
        score->wave += 1;
        for (u32 i = 0; i < wave.count; ++i) {
            // A point on the frame, aimed roughly at the middle.
            f32 u = g.rng.unit();
            point2 at = g.rng.chance(0.5f) ? point2{(u - 0.5f) * 2.0f * hw, g.rng.chance(0.5f) ? hh : -hh}
                                            : point2{g.rng.chance(0.5f) ? hw : -hw, (u - 0.5f) * 2.0f * hh};
            vec2 toward = normalize(point2{g.rng.range(-200.0f, 200.0f), g.rng.range(-120.0f, 120.0f)} - at) * g.rng.range(40.0f, 110.0f);
            ctx.spawn_later(g_rock_kind, transform2_at(at, g.rng.range(0.0f, 2.0f * KV_PI)), sprite_of(g, LOOK_ROCK, kRockSprite[0], 1),
                            Velocity{toward, g.rng.range(-1.0f, 1.0f)}, collider_circle(kRockRadius[0], L_ROCK, L_SHIP | L_BULLET), RockSize{0});
        }
        KV_INFO("rocks", "wave %u: %u rocks", score->wave, wave.count);
        g.next_wave = ctx.schedule_in(SpawnWave{wave.count + 1}, kWaveTicks);
    }
}

// The death trigger: the ship goes, the game is over, a restart is scheduled.
void on_edge(TickContext& ctx, Span<const StateEdge> edges) {
    static const u32 k_score = ctx.resource_id("example_rocks.score");
    Game& g = ctx.state_as<Game>();
    const Score* score = ctx.resource<const Score>(k_score);
    for (const StateEdge& e : edges) {
        if (e.trigger != g_death) continue;
        ctx.destroy(e.entity);
        ctx.cancel(g.next_wave); // the field stays as it is until the restart
        ctx.events->emit(GameOver{score->total});
        ctx.schedule_in(Restart{}, kRestartTicks);
        KV_INFO("rocks", "game over: %d points", score->total);
    }
}

// Clear the field, new ship, first wave in a second. Also how the game starts.
void on_restart(TickContext& ctx, Span<const Restart>) {
    static const u32 k_score = ctx.resource_id("example_rocks.score");
    Game& g = ctx.state_as<Game>();
    Score* score = ctx.resource<Score>(k_score);
    const u32 kinds[] = {g_rock_kind, g_bullet_kind, g_debris_kind, g_ship_kind};
    for (u32 kid : kinds) {
        const Table* t = ctx.read_kind(kid);
        for (u32 i = 0; i < t->rows(); ++i) if (t->is_alive(i)) ctx.destroy(t->handle_at(i));
    }
    score->games += 1;
    score->total = 0;
    score->rocks_shot = 0;
    score->wave = 0;
    spawn_ship(ctx, g);
    g.next_wave = ctx.schedule_in(SpawnWave{4}, 60);
    KV_INFO("rocks", "game %u", score->games);
}

// ---- startup ---------------------------------------------------------------------------

bool rocks_startup(Engine& engine, ModuleInstance& self) {
    Game* g = arena_push<Game>(self.arena);
    if (!g) return false;
    self.set_state(g);
    if (!engine_publish(engine, self, "example_rocks.score", &g->score)) return false;

    Renderer* r = renderer_get(engine);
    if (!r) return false;
    const char* paths[3] = {"example_rocks/ship.png", "example_rocks/rock.png", "example_rocks/dot.png"};
    Handle tex[3];
    for (u32 i = 0; i < 3; ++i) {
        g->textures[i] = asset_load_texture(engine, paths[i]);
        const TextureAsset* t = asset_texture(engine, g->textures[i]);
        if (!t) return false;
        tex[i] = t->texture;
    }
    MaterialDesc md[LOOK_COUNT];
    md[LOOK_SHIP].texture   = tex[0]; md[LOOK_SHIP].blend   = BlendMode::Alpha;
    md[LOOK_ROCK].texture   = tex[1]; md[LOOK_ROCK].blend   = BlendMode::Alpha;
    md[LOOK_BULLET].texture = tex[2]; md[LOOK_BULLET].blend = BlendMode::Additive;
    md[LOOK_DEBRIS].texture = tex[2]; md[LOOK_DEBRIS].blend = BlendMode::Additive;
    for (u32 i = 0; i < LOOK_COUNT; ++i) {
        g->materials[i] = renderer_material_create(r, md[i]);
        if (g->materials[i].is_null()) return false;
    }
    // Handle kinds, so a handler can tell what an overlap is between without a lookup.
    g->ship_hk   = engine_table(engine, g_ship_kind)->kind;
    g->bullet_hk = engine_table(engine, g_bullet_kind)->kind;
    g->rock_hk   = engine_table(engine, g_rock_kind)->kind;
    return true;
}

// ---- tables ----------------------------------------------------------------------------

constinit const ResourceDesc k_resources[] = {{"example_rocks.score"}};

constinit const char* const k_control_reads[]  = {"input.state"};
constinit const char* const k_control_writes[] = {"example_rocks.ship.Transform2", "example_rocks.ship.Velocity", "example_rocks.ship.Ship", "example_rocks.bullet"};
constinit const char* const k_before_move[]    = {"example_rocks.move"};
constinit const char* const k_move_reads[]     = {"*.Velocity"};
constinit const char* const k_move_writes[]    = {"*.Transform2"};
constinit const char* const k_wrap_reads[]     = {"*.Velocity", "platform.window"};
constinit const char* const k_after_move[]     = {"example_rocks.move"};
constinit const char* const k_fade_writes[]    = {"example_rocks.debris.Lifetime", "example_rocks.debris.Sprite"};
constinit const char* const k_game_reads[]     = {"example_rocks.score"};
constinit const SystemDesc k_systems[] = {
    {.name = "example_rocks.control", .phase = Phase::Sim, .fn = control, .reads = k_control_reads, .writes = k_control_writes, .before = k_before_move},
    {.name = "example_rocks.move",    .phase = Phase::Sim, .fn = move,    .reads = k_move_reads,    .writes = k_move_writes},
    {.name = "example_rocks.wrap",    .phase = Phase::Sim, .fn = wrap,    .reads = k_wrap_reads,    .writes = k_move_writes, .after = k_after_move},
    {.name = "example_rocks.fade",    .phase = Phase::Sim, .fn = debris_fade, .writes = k_fade_writes},
    {.name = "example_rocks.game",    .phase = Phase::Sim, .fn = game,    .reads = k_game_reads},
};

constinit const char* const k_rock_hit_writes[] = {"example_rocks.rock", "example_rocks.score"};
constinit const char* const k_ship_hit_reads[]  = {"example_rocks.ship.Transform2"};
constinit const char* const k_ship_hit_writes[] = {"example_rocks.ship.Health"};
constinit const char* const k_wave_reads[]      = {"platform.window"};
constinit const char* const k_wave_writes[]     = {"example_rocks.score"};
constinit const char* const k_score_reads[]     = {"example_rocks.score"};
constinit const char* const k_restart_reads[]   = {"example_rocks.rock", "example_rocks.bullet", "example_rocks.debris", "example_rocks.ship"};
constinit const char* const k_after_rock_hit[]  = {"example_rocks.on_rock_hit"};
constinit const char* const k_after_wave[]      = {"example_rocks.on_wave"};
constinit const HandlerDesc k_handlers[] = {
    {.name = "example_rocks.on_overlap",  KV_ON(OverlapBegin, on_overlap)},
    {.name = "example_rocks.on_rock_hit", KV_ON(RockHit, on_rock_hit), .writes = k_rock_hit_writes},
    {.name = "example_rocks.on_ship_hit", KV_ON(ShipHit, on_ship_hit), .reads = k_ship_hit_reads, .writes = k_ship_hit_writes},
    {.name = "example_rocks.on_score",    KV_ON(ScoreChanged, on_score)},
    {.name = "example_rocks.on_expire",   KV_ON(Expire, on_expire)},
    {.name = "example_rocks.on_wave",     KV_ON(SpawnWave, on_wave), .reads = k_wave_reads, .writes = k_wave_writes, .after = k_after_rock_hit},
    {.name = "example_rocks.on_edge",     KV_ON(StateEdge, on_edge), .reads = k_score_reads},
    {.name = "example_rocks.on_restart",  KV_ON(Restart, on_restart), .reads = k_restart_reads, .writes = k_wave_writes, .after = k_after_wave},
};

} // namespace

KV_MODULE_DEFINE(example_rocks_module) = {
    .name       = "example_rocks",
    .events     = k_events,
    .components = k_components,
    .kinds      = k_kinds,
    .resources  = k_resources,
    .systems    = k_systems,
    .handlers   = k_handlers,
    .triggers   = k_triggers,
    .startup    = rocks_startup,
    .memory     = {.reserve = 64 * 1024},
};

} // namespace kv
