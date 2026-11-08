// example_rocks: an asteroids-like. The ship is steered from the keyboard (or by a script
// with --demo), bullets expire by scheduled event, rocks split when hit and spawn debris,
// waves arrive on a timer, the ship's death is a trigger on its Health column, and a
// restart follows two seconds later. Escape (or Start on a pad) pauses: the sim holds, a
// screen-space dimmer covers the field, Q quits. A hit shakes the camera. Shots, breaks
// and the ship's end make sounds, with a looping thruster whose volume follows the
// thrust. Everything the game knows is in kinds and one published Score; the only handles
// kept are the thruster voice and the assets.
#include "korvid/example_rocks/module.h"
#include "korvid/anim/module.h"
#include "korvid/asset/font.h"
#include "korvid/asset/material.h"
#include "korvid/asset/sound.h"
#include "korvid/audio/module.h"
#include "korvid/collision/module.h"
#include "korvid/core/arena.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/random.h"
#include "korvid/frameloop/frameloop.h"
#include "korvid/math/fields.h"
#include "korvid/platform_hid/events.h"
#include "korvid/platform_hid/host_window.h"
#include "korvid/platform_hid/input.h"
#include "korvid/platform/io.h"
#include "korvid/render/renderer.h"
#include "korvid/scene/module.h"
#include "korvid/sprite/camera.h"
#include "korvid/sprite/sprite.h"
#include "korvid/sprite/transform2.h"
#include "korvid/text/text.h"
#include <math.h>

namespace kv {

namespace {

// ---- components ------------------------------------------------------------------------
struct Velocity { vec2 v; f32 spin; };   // pixels per second, radians per second
struct Health   { i32 hp; };
struct RockSize { u8 size; };             // 0 big, 1 medium, 2 small
struct Ship     { f32 cooldown; u8 thrusting; };
struct Label    { u8 role; };             // what a text label shows: see Role
enum Role : u8 { ROLE_HUD, ROLE_PAUSED, ROLE_OVER };

constinit const FieldDesc k_velocity_fields[] = {KV_FIELDS(Velocity, v, spin)};
constinit const FieldDesc k_health_fields[]   = {KV_FIELDS(Health, hp)};

constinit const FieldDesc k_rocksize_fields[] = {KV_FIELDS(RockSize, size)};
constinit const FieldDesc k_ship_fields[]     = {KV_FIELDS(Ship, cooldown, thrusting)};
constinit const FieldDesc k_label_fields[]    = {KV_FIELDS(Label, role)};
constinit const ComponentDesc k_components[] = {
    KV_COMPONENT(Velocity, k_velocity_fields),
    KV_COMPONENT(Health, k_health_fields, CompPrev), // the death trigger watches it
    KV_COMPONENT(RockSize, k_rocksize_fields),
    KV_COMPONENT(Ship, k_ship_fields),
    KV_COMPONENT(Label, k_label_fields),
};

// ---- kinds -----------------------------------------------------------------------------
constinit u32 g_ship_kind = 0, g_bullet_kind = 0, g_rock_kind = 0, g_debris_kind = 0, g_overlay_kind = 0, g_flame_kind = 0, g_label_kind = 0;
constinit const KindDesc k_kinds[] = {
    {.name = ".ship",    .components = "Transform2, Sprite, Velocity, Health, Collider2, Ship",    .capacity = 1,    .id = &g_ship_kind},
    {.name = ".bullet",  .components = "Transform2, Sprite, Velocity, Collider2",  .capacity = 4096, .id = &g_bullet_kind},
    {.name = ".rock",    .components = "Transform2, Sprite, Velocity, Collider2, RockSize",    .capacity = 1024, .id = &g_rock_kind},
    {.name = ".debris",  .components = "Transform2, Sprite, Velocity, AnimPlayer",  .capacity = 8192, .id = &g_debris_kind},
    {.name = ".overlay", .components = "Transform2, Sprite", .capacity = 8,    .id = &g_overlay_kind},
    // The flame is cosmetic: its flipbook comes from a file and starts when the file
    // arrives, which is not a tick the sim may depend on.
    {.name = ".flame",   .components = "Transform2, Sprite, Local2, Parent, AnimPlayer",   .capacity = 4,    .id = &g_flame_kind, .cosmetic = true},
    // Labels are cosmetic too: the HUD shows the high score, which comes from a file.
    {.name = ".label",   .components = "Transform2, Text, Label",   .capacity = 8,    .id = &g_label_kind, .cosmetic = true},
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
    {.name = ".death", .field = ".ship.Health.hp", .edge = TriggerEdge::CrossedBelow, .value = 1.0, .id = &g_death},
};

// ---- the published score and the module's state --------------------------------------
struct Score { i32 total; u32 rocks_shot; u32 wave; u32 games; i32 high; };

enum Look : u8 { LOOK_SHIP, LOOK_ROCK, LOOK_BULLET, LOOK_DEBRIS, LOOK_WHITE, LOOK_FLAME, LOOK_COUNT };
enum Snd : u8 { SND_SHOT, SND_BREAK, SND_BOOM, SND_THRUST, SND_COUNT };

struct Game {
    Score  score;
    Handle looks[LOOK_COUNT];     // asset.material, one file per look
    Handle materials[LOOK_COUNT]; // render.material: the rows those files fill
    Handle font;                  // asset.font: DejaVu Sans at 28 px
    Handle flame_clip;            // anim.clip from a file: the flame's flipbook
    Handle fade_clip;             // anim.clip built here: one second of alpha to zero
    Handle sounds[SND_COUNT];     // asset.sound
    Handle thrust_voice;          // audio.voice: the looping thruster, alive with the ship
    Handle high_read;             // platform.io: the high score file on its way in
    char   high_path[512];        // <pref path>/highscore.txt
    u16    ship_hk, bullet_hk, rock_hk, debris_hk; // handle kinds, to tell what an event is about
    bool   started = false;
    bool   demo    = false;
    u32    next_wave = 0;        // the pending wave's ticket, cancelled at game over
    Rng    rng;
    f32    shake = 0.0f;         // camera shake amplitude in pixels, decaying
    u32    paused_ticks = 0;     // the demo's pause: how long it has held
    bool   demo_paused_once = false;
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

// A label in screen space; the HUD sits top-left, banners in the middle, over the dimmer.
void spawn_label(Table& labels, const Game& g, Role role, const char* str, point2 at, TextAlign align, f32 scale, i32 layer, color tint) {
    Text t;
    t.font  = g.font;
    t.scale = scale;
    t.tint  = tint;
    t.layer = layer;
    t.space = SpriteSpace::Screen;
    t.align = align;
    text_set(t, "%s", str);
    labels.spawn(transform2_at(at), t, Label{role});
}

// Every label of one role goes.
void destroy_labels(Table& labels, Role role) {
    Span<const Label> roles = labels.rows_of<Label>();
    for (u32 i = 0; i < labels.rows(); ++i)
        if (labels.is_alive(i) && roles[i].role == role) labels.destroy(labels.handle_at(i));
}

// A rock of `size` at `at`, drifting in a random direction. Direct: the caller holds the write.
void spawn_rock(Table& rocks, Game& g, point2 at, u8 size, vec2 drift) {
    Velocity v{drift, g.rng.range(-1.5f, 1.5f)};
    rocks.spawn(transform2_at(at, g.rng.range(0.0f, 2.0f * KV_PI)), sprite_of(g, LOOK_ROCK, kRockSprite[size], 1),
                v, collider_circle(kRockRadius[size], L_ROCK, L_SHIP | L_BULLET), RockSize{size});
}

// Debris fades through the one-second fade clip played at 1 / lifetime, so each piece
// lasts its own time; AnimFinished destroys it.
void spawn_debris(TickContext& ctx, Game& g, point2 at, u32 count, color tint) {
    for (u32 i = 0; i < count; ++i) {
        vec2 v = heading(g.rng.range(0.0f, 2.0f * KV_PI)) * g.rng.range(60.0f, 220.0f);
        f32 life = g.rng.range(0.3f, 0.8f);
        ctx.spawn_later(g_debris_kind, transform2_at(at), sprite_of(g, LOOK_DEBRIS, 6.0f, 0, tint), Velocity{v, 0.0f},
                        anim_player(g.fade_clip, 1.0f / life));
    }
}

void spawn_ship(TickContext& ctx, Game& g) {
    ctx.spawn_later(g_ship_kind, transform2_at(point2{0, 0}, KV_PI * 0.5f), sprite_of(g, LOOK_SHIP, 48.0f, 2),
                    Velocity{}, Health{3}, collider_circle(kShipRadius, L_SHIP, L_ROCK), Ship{0.0f, 0});
}

// ---- systems -----------------------------------------------------------------------------

// Phase PreSim, on every tick, paused or not: Escape or Start toggles the pause, Q quits;
// the demo pauses itself once, at tick 400, for 90 ticks. Pausing drops a dimmer over the
// field: one screen-space sprite the size of the window at a layer above everything;
// resuming removes it. Writes the clock through the engine API, which applies from the
// next tick, so this tick is still whatever kind it started as.
void pause(TickContext& ctx) {
    static const u32 k_window = ctx.resource_id("platform_hid.window");
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    bool toggle = false;
    if (g.demo) {
        if (!ctx.paused && ctx.tick == 400 && !g.demo_paused_once) { toggle = true; g.demo_paused_once = true; g.paused_ticks = 0; }
        if (ctx.paused && ++g.paused_ticks == 90) toggle = true;
    } else {
        for (const KeyDown& k : ctx.events->read<KeyDown>()) {
            if (k.key == Key::Escape && !k.repeat) toggle = true;
            if (k.key == Key::Q) engine_request_quit(e);
        }
        for (const PadButtonDown& b : ctx.events->read<PadButtonDown>())
            if (b.button == PadButton::Start) toggle = true;
    }
    if (!toggle) return;
    Table* overlay = ctx.write_kind(g_overlay_kind);
    Table* labels  = ctx.write_kind(g_label_kind);
    if (!e.clock.paused) {
        const HostWindow* w = ctx.resource<const HostWindow>(k_window);
        Sprite dim = sprite_of(g, LOOK_WHITE, 1.0f, 100, color_rgba(0.0f, 0.0f, 0.02f, 0.55f));
        dim.size  = vec2{(f32)w->width(), (f32)w->height()};
        dim.space = SpriteSpace::Screen;
        overlay->spawn(transform2_at(point2{0, 0}), dim);
        spawn_label(*labels, g, ROLE_PAUSED, "PAUSED", point2{0, 10}, TextAlign::Center, 2.0f, 101, color_white());
        spawn_label(*labels, g, ROLE_PAUSED, "Escape or Start resumes, Q quits", point2{0, -40}, TextAlign::Center, 0.75f, 101,
                    color_rgba(1.0f, 1.0f, 1.0f, 0.7f));
        engine_pause(e, true);
        KV_INFO("rocks", "paused: Escape or Start resumes, Q quits");
    } else {
        overlay->destroy_all();
        destroy_labels(*labels, ROLE_PAUSED);
        engine_pause(e, false);
    }
    // A click on the Ui bus that does not pause with the sim, so the resume click plays.
    SoundPlay click;
    click.sound  = g.sounds[SND_SHOT];
    click.volume = 0.3f;
    click.pitch  = e.clock.paused ? 0.5f : 0.7f; // the clock just flipped: low going in, higher coming out
    click.bus    = AudioBus::Ui;
    click.pause_with_sim = false;
    audio_play(ctx, click);
}

// Phase PostSim, every tick: the HUD label says what the score says. One write per tick;
// the text module relays out only when the string changes.
void hud(TickContext& ctx) {
    static const u32 k_score = ctx.resource_id(".score");
    const Score* score = ctx.resource<const Score>(k_score);
    for (KindRows k : ctx.view<Text, const Label>()) {
        Span<Text>        texts = k.col<Text>();
        Span<const Label> roles = k.col<const Label>();
        for (u32 i = 0; i < k.rows(); ++i)
            if (k.alive(i) && roles[i].role == ROLE_HUD) text_set(texts[i], "SCORE %d   WAVE %u   HIGH %d", score->total, score->wave, score->high);
    }
}

// Phase PreSim, main thread, every tick: the high score file, when it arrives. Its read was
// submitted at startup; the completion is ours by request handle. A missing file is a
// high score of zero.
void files(TickContext& ctx) {
    static const u32 k_score = ctx.resource_id(".score");
    Game& g = ctx.state_as<Game>();
    if (g.high_read.is_null()) return;
    IoQueue* io = platform_io(*ctx.engine);
    for (const IoCompletion& c : io->completions()) {
        if (c.request != g.high_read) continue;
        IoCompletion done = c;
        io->reap(c.request);
        g.high_read = Handle{};
        if (done.status == IoStatus::Done) {
            char text[32] = {};
            memcpy(text, done.data, done.size < sizeof text - 1 ? done.size : sizeof text - 1);
            Score* score = ctx.resource<Score>(k_score);
            score->high = atoi(text);
            KV_INFO("rocks", "high score %d from %s", score->high, g.high_path);
        } else if (done.status == IoStatus::Failed && done.error != 2) {
            KV_WARN("rocks", "high score file: errno %d", done.error);
        }
        if (done.data) free_in(mem_general(), done.data, done.size ? done.size : 1);
        break;
    }
}

// Phase PostSim: the camera shakes when the ship is hit and settles. The offset is a
// function of the tick, so it is the same every run; the camera is not sim state anyway.
void camera(TickContext& ctx) {
    static const u32 k_camera = ctx.resource_id("sprite.camera");
    Game& g = ctx.state_as<Game>();
    if (ctx.events->read<ShipHit>().len) g.shake = 12.0f;
    if (ctx.events->read<GameOver>().len) g.shake = 28.0f;
    Camera2* cam = ctx.resource<Camera2>(k_camera);
    if (g.shake > 0.05f) {
        f32 t = (f32)ctx.tick;
        cam->center = point2{sinf(t * 1.7f) * g.shake, cosf(t * 2.3f) * g.shake};
        g.shake *= 0.86f;
    } else {
        g.shake = 0.0f;
        cam->center = point2{0, 0};
    }
}

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
        // The first pad, beside the keys: left stick or d-pad turns, South or the right
        // trigger thrusts, West or the right shoulder fires.
        if (const GamepadState* pad = input->sampled.pad()) {
            f32 x = pad->axis(PadAxis::LeftX);
            left   |= x < -0.5f || pad->down(PadButton::DpadLeft);
            right  |= x >  0.5f || pad->down(PadButton::DpadRight);
            thrust |= pad->down(PadButton::South) || pad->axis(PadAxis::RightTrigger) > 0.5f || pad->down(PadButton::DpadUp);
            fire   |= pad->down(PadButton::West) || pad->down(PadButton::RShoulder);
        }
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
            // The flame is the ship's child: it follows by itself, the game only lights it.
            Table* flames = ctx.write_kind(g_flame_kind);
            Span<Sprite> fs = flames->rows_of<Sprite>();
            for (u32 f = 0; f < flames->rows(); ++f)
                if (flames->is_alive(f)) fs[f].tint.a = thrust ? 1.0f : 0.0f;
            // The thruster loop plays all along; its volume follows the thrust.
            if (!g.thrust_voice.is_null()) audio_set(ctx, g.thrust_voice, thrust ? 0.35f : 0.0f, 1.0f, 0.0f);
            s[i].cooldown -= dt;
            if (fire && s[i].cooldown <= 0.0f) {
                s[i].cooldown = kFireCooldown;
                Table* bullets = ctx.write_kind(g_bullet_kind);
                vec2 dir = heading(t[i].rotation);
                point2 at = t[i].position + dir * 26.0f;
                Handle b = bullets->spawn(transform2_at(at), sprite_of(g, LOOK_BULLET, 12.0f, 3, color_rgba(1.0f, 0.95f, 0.5f)),
                                          Velocity{v[i].v + dir * kBulletSpeed, 0.0f}, collider_circle(4.0f, L_BULLET, L_ROCK));
                if (!b.is_null()) ctx.schedule_in(Expire{b}, kBulletLife);
                SoundPlay shot;
                shot.sound  = g.sounds[SND_SHOT];
                shot.volume = 0.45f;
                shot.pitch  = g.rng.range(0.9f, 1.15f);
                shot.pan    = at.x / 900.0f;
                audio_play(ctx, shot);
            }
        }
    }
}

// Everything with a velocity moves. In parallel: the rows are independent, so chunks of
// them go to the pool and the result is the same whatever ran where.
void move(TickContext& ctx) {
    f32 dt = (f32)ctx.dt;
    for (KindRows k : ctx.view<Transform2, const Velocity>()) {
        Span<Transform2>     t = k.col<Transform2>();
        Span<const Velocity> v = k.col<const Velocity>();
        parallel_for(ctx, k.rows(), 512, [&](ChunkContext&, u32 begin, u32 end) {
            for (u32 i = begin; i < end; ++i) {
                if (!k.alive(i)) continue;
                t[i].position += v[i].v * dt;
                t[i].rotation += v[i].spin * dt;
            }
        });
    }
}

// Everything that left the window comes back on the other side. After move, by
// declaration. A wrap is a teleport: the previous copy is written too, so the renderer
// shows the sprite on the far side at once instead of interpolating it across the screen.
void wrap(TickContext& ctx) {
    static const u32 k_window = ctx.resource_id("platform_hid.window");
    const HostWindow* w = ctx.resource<const HostWindow>(k_window);
    f32 hw = (f32)w->width() * 0.5f + 32.0f, hh = (f32)w->height() * 0.5f + 32.0f;
    for (KindRows k : ctx.view<Transform2, const Velocity>()) {
        Span<Transform2> t    = k.col<Transform2>();
        Span<Transform2> prev = k.prev<Transform2>();
        for (u32 i = 0; i < k.rows(); ++i) {
            if (!k.alive(i)) continue;
            point2& p = t[i].position;
            point2 was = p;
            if (p.x > hw) p.x -= 2 * hw; else if (p.x < -hw) p.x += 2 * hw;
            if (p.y > hh) p.y -= 2 * hh; else if (p.y < -hh) p.y += 2 * hh;
            if (p.x != was.x || p.y != was.y) prev[i] = t[i]; // teleported: no interpolation to here
        }
    }
}


// Boot: the first tick starts the first game.
void game(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    if (!g.started) {
        g.started = true;
        g.demo    = frameloop_flag(*ctx.engine, "--demo");
        g.rng     = Rng::seeded(0x70c45, 7);
        KV_INFO("rocks", "%s. Arrows turn and thrust, space fires, Escape pauses, Q quits; on a pad the left stick turns, South thrusts, West fires, Start pauses.", g.demo ? "Demo: the ship flies itself" : "Your ship");
        ctx.emit(Restart{});
    }
}

// ---- handlers --------------------------------------------------------------------------

// The way out. A handler, not a Sim system: handlers run on paused ticks, so a quit asked
// for while the game is paused (the window closing, --run-seconds) still happens.
void on_exit(TickContext& ctx, Span<const ExitRequested> exits) {
    Game& g = ctx.state_as<Game>();
    for (const ExitRequested& e : exits) {
        KV_INFO("rocks", "exit (%s): score %d, %u rocks, wave %u, %u games", e.reason == ExitReason::Timer ? "timer" : "player",
                g.score.total, g.score.rocks_shot, g.score.wave, g.score.games);
        engine_request_quit(*ctx.engine);
    }
}

void on_asset_failed(TickContext&, Span<const AssetFailed> failures) {
    for (const AssetFailed& e : failures) {
        char text[160];
        kv_handle_describe(e.asset.bits, text, sizeof text);
        KV_WARN("rocks", "asset failed (%s): %s", asset_error_name(e.error), text);
    }
}

// An overlap is a hit when the kinds say so. Handle kinds were noted at startup.
void on_overlap(TickContext& ctx, Span<const OverlapBegin> overlaps) {
    Game& g = ctx.state_as<Game>();
    for (const OverlapBegin& o : overlaps) {
        u16 ka = o.a.kind(), kb = o.b.kind();
        if (ka == g.bullet_hk && kb == g.rock_hk) ctx.emit(RockHit{o.b, o.a});
        else if (ka == g.rock_hk && kb == g.bullet_hk) ctx.emit(RockHit{o.a, o.b});
        else if (ka == g.ship_hk && kb == g.rock_hk) ctx.emit(ShipHit{o.a, o.b});
        else if (ka == g.rock_hk && kb == g.ship_hk) ctx.emit(ShipHit{o.b, o.a});
    }
}

// The rock splits (direct: this handler holds the rock write), the bullet goes, debris
// flies by command, the score moves. A bullet that hit two rocks is destroyed once; the
// second RockHit still splits its rock, since the rock handle is live.
void on_rock_hit(TickContext& ctx, Span<const RockHit> hits) {
    static const u32 k_score = ctx.resource_id(".score");
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
        SoundPlay crack;
        crack.sound  = g.sounds[SND_BREAK];
        crack.volume = 0.7f - 0.15f * (f32)size;
        crack.pitch  = 1.0f + 0.35f * (f32)size; // smaller rocks, higher cracks
        crack.pan    = at.x / 900.0f;
        audio_play(ctx, crack);
        score->total += kRockPoints[size];
        score->rocks_shot += 1;
        ctx.emit(ScoreChanged{kRockPoints[size], score->total});
    }
}

// The ship takes a hit: the rock goes, health drops by a row write; the trigger notices.
void on_ship_hit(TickContext& ctx, Span<const ShipHit> hits) {
    static const u32 k_hp = ctx.column_id(".ship.Health");
    static const u32 k_pos = ctx.column_id(".ship.Transform2");
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

// The ship arrived (spawned by command at the restart): give it a flame, a child in its
// own kind that follows through Parent and Local2 and dies with the ship.
void on_spawned(TickContext& ctx, Span<const Spawned> spawns) {
    Game& g = ctx.state_as<Game>();
    for (const Spawned& s : spawns) {
        if (s.kind != g_ship_kind || s.entity.is_null()) continue;
        Table* flames = ctx.write_kind(g_flame_kind);
        Sprite flame = sprite_of(g, LOOK_FLAME, 32.0f, 1, color_rgba(1.0f, 1.0f, 1.0f, 0.0f));
        flame.uv = rect2{0.0f, 0.0f, 0.5f, 1.0f}; // frame 0 of the two-frame sheet, until the clip says otherwise
        flames->spawn(Transform2{}, flame, local2_at(point2{-30.0f, 0.0f}), Parent{s.entity}, anim_player(g.flame_clip));
        // The thruster: a silent loop until control turns it up.
        SoundPlay thrust;
        thrust.sound  = g.sounds[SND_THRUST];
        thrust.volume = 0.0f;
        thrust.loop   = true;
        g.thrust_voice = audio_play(ctx, thrust);
    }
}

// A fade ran out: the debris goes.
void on_anim_finished(TickContext& ctx, Span<const AnimFinished> done) {
    Game& g = ctx.state_as<Game>();
    for (const AnimFinished& f : done)
        if (f.entity.kind() == g.debris_hk) ctx.destroy(f.entity);
}

void on_score(TickContext&, Span<const ScoreChanged> changes) {
    for (const ScoreChanged& s : changes) KV_INFO("rocks", "+%d -> %d", s.points, s.total);
}

void on_expire(TickContext& ctx, Span<const Expire> expiries) {
    for (const Expire& e : expiries) ctx.destroy(e.bullet); // already gone if it hit: silently nothing
}

// A wave: rocks arrive from the edges, drifting inward. By command, so no write and no order.
void on_wave(TickContext& ctx, Span<const SpawnWave> waves) {
    static const u32 k_window = ctx.resource_id("platform_hid.window");
    static const u32 k_score  = ctx.resource_id(".score");
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
    static const u32 k_score = ctx.resource_id(".score");
    Game& g = ctx.state_as<Game>();
    const Score* score = ctx.resource<const Score>(k_score);
    Table* labels = ctx.write_kind(g_label_kind);
    for (const StateEdge& e : edges) {
        if (e.trigger != g_death) continue;
        ctx.destroy(e.entity);
        ctx.cancel(g.next_wave); // the field stays as it is until the restart
        if (!g.thrust_voice.is_null()) { audio_stop(ctx, g.thrust_voice); g.thrust_voice = Handle{}; }
        SoundPlay boom;
        boom.sound = g.sounds[SND_BOOM];
        audio_play(ctx, boom);
        // A new high score goes to the per-user directory, through the queue: the bytes
        // are copied at submission, nothing waits for the disk.
        if (score->total > score->high && g.high_path[0]) {
            Score* s = ctx.resource<Score>(k_score);
            s->high = score->total;
            char text[32];
            int n = snprintf(text, sizeof text, "%d\n", score->total);
            IoWriteRequest w;
            w.path = g.high_path; w.data = text; w.size = (usize)n;
            if (platform_io(*ctx.engine)->write_file(w).is_null()) KV_WARN("rocks", "could not queue the high score write");
            else KV_INFO("rocks", "new high score %d -> %s", score->total, g.high_path);
        }
        ctx.emit(GameOver{score->total});
        ctx.schedule_in(Restart{}, kRestartTicks);
        char line[48];
        snprintf(line, sizeof line, "GAME OVER   %d", score->total);
        spawn_label(*labels, g, ROLE_OVER, line, point2{0, 0}, TextAlign::Center, 1.5f, 50, color_rgba(1.0f, 0.5f, 0.4f));
        KV_INFO("rocks", "game over: %d points", score->total);
    }
}

// Clear the field, new ship, first wave in a second. Also how the game starts.
void on_restart(TickContext& ctx, Span<const Restart>) {
    static const u32 k_score  = ctx.resource_id(".score");
    static const u32 k_window = ctx.resource_id("platform_hid.window");
    Game& g = ctx.state_as<Game>();
    Score* score = ctx.resource<Score>(k_score);
    const HostWindow* w = ctx.resource<const HostWindow>(k_window);
    const u32 kinds[] = {g_rock_kind, g_bullet_kind, g_debris_kind, g_ship_kind};
    for (u32 kid : kinds) ctx.destroy_all(kid);
    score->games += 1;
    score->total = 0;
    score->rocks_shot = 0;
    score->wave = 0;
    // The labels are rewritten directly: the banner goes, the HUD comes back at the
    // top-right (the top-left is the frame loop's overlay).
    Table* labels = ctx.write_kind(g_label_kind);
    labels->destroy_all();
    spawn_label(*labels, g, ROLE_HUD, "", point2{(f32)w->width() * 0.5f - 16.0f, (f32)w->height() * 0.5f - 16.0f - 28.0f},
                TextAlign::Right, 1.0f, 50, color_rgba(1.0f, 1.0f, 1.0f, 0.85f));
    spawn_ship(ctx, g);
    g.next_wave = ctx.schedule_in(SpawnWave{4}, 60);
    KV_INFO("rocks", "game %u", score->games);
}

// ---- startup ---------------------------------------------------------------------------

bool rocks_startup(Engine& engine, ModuleInstance& self) {
    Game* g = arena_push<Game>(self.arena);
    if (!g) return false;
    self.set_state(g);
    if (!engine_publish(engine, self, ".score", &g->score)) return false;

    if (!renderer_get(engine)) return false;
    g->font = asset_load_font(engine, "text/DejaVuSans.ttf", 28.0f);
    if (g->font.is_null()) return false;
    // Two clips, two ways: the flame's flipbook is a file; the fade is built here, one
    // second of alpha from one to zero, played at 1 / lifetime by each piece of debris.
    g->flame_clip = anim_load(engine, "example_rocks/flame.anim.json");
    static const AnimKey k_fade_keys[] = {{.time = 0.0f, .v = {1.0f}}, {.time = 1.0f, .v = {0.0f}}};
    static const AnimTrackDesc k_fade_tracks[] = {{.channel = AnimChannel::Alpha, .interp = AnimInterp::Linear, .keys = k_fade_keys}};
    AnimClipDesc fade;
    fade.name = "example_rocks.fade"; fade.loop = AnimLoop::Once; fade.tracks = k_fade_tracks;
    g->fade_clip = anim_create(engine, fade);
    if (g->flame_clip.is_null() || g->fade_clip.is_null()) return false;
    const char* sounds[SND_COUNT] = {"example_rocks/shot.wav", "example_rocks/break.wav", "example_rocks/boom.wav", "example_rocks/thrust.wav"};
    for (u32 i = 0; i < SND_COUNT; ++i) {
        g->sounds[i] = asset_load_sound(engine, sounds[i]);
        if (g->sounds[i].is_null()) return false;
    }
    // Each look is a material file; the file names its texture. The handles are usable
    // now and show the placeholder until the files and textures arrive.
    const char* looks[LOOK_COUNT] = {"example_rocks/ship.material.json", "example_rocks/rock.material.json",
                                     "example_rocks/bullet.material.json", "example_rocks/debris.material.json",
                                     "example_rocks/white.material.json", "example_rocks/flame.material.json"};
    for (u32 i = 0; i < LOOK_COUNT; ++i) {
        g->looks[i] = asset_load_material(engine, looks[i]);
        const MaterialAsset* m = asset_material(engine, g->looks[i]);
        if (!m) return false;
        g->materials[i] = m->material;
    }
    // Handle kinds, so a handler can tell what an overlap is between without a lookup.
    // The high score: read from the per-user directory, written there at a game over. The
    // demo keeps its own file so a scripted run never changes a player's record.
    const char* pref = platform_pref_path(engine, "korvid", "rocks");
    if (pref[0]) {
        snprintf(g->high_path, sizeof g->high_path, "%shighscore%s.txt", pref, frameloop_flag(engine, "--demo") ? "-demo" : "");
        IoRequest req;
        req.path = g->high_path;
        g->high_read = platform_io(engine)->read_file(req);
    }
    g->ship_hk   = engine_table(engine, g_ship_kind)->kind;
    g->bullet_hk = engine_table(engine, g_bullet_kind)->kind;
    g->rock_hk   = engine_table(engine, g_rock_kind)->kind;
    g->debris_hk = engine_table(engine, g_debris_kind)->kind;
    return true;
}

// ---- tables ----------------------------------------------------------------------------

constinit const ResourceDesc k_resources[] = {{".score"}};


constinit const SystemDesc k_systems[] = {
    {.name = ".pause",   .phase = Phase::PreSim, .fn = pause, .reads = "platform_hid.window", .writes = ".overlay, .label, audio.commands", .runs = RunWhen::Always},
    {.name = ".files",   .phase = Phase::PreSim, .fn = files, .writes = ".score", .main_thread = true, .runs = RunWhen::Always},
    {.name = ".control", .phase = Phase::Sim, .fn = control, .reads = "input.state", .writes = ".ship.Transform2, .ship.Velocity, .ship.Ship, .bullet, .flame, audio.commands", .before = ".move"},
    // move and wrap touch no module state: they share levels with whatever else is free.
    {.name = ".move",    .phase = Phase::Sim, .fn = move,    .reads = "*.Velocity",    .writes = "*.Transform2", .state = StateUse::None},
    {.name = ".wrap",    .phase = Phase::Sim, .fn = wrap,    .reads = "*.Velocity, platform_hid.window",    .writes = "*.Transform2", .after = ".move", .state = StateUse::None},
    {.name = ".game",    .phase = Phase::Sim, .fn = game,    .reads = ".score"},
    {.name = ".camera",  .phase = Phase::PostSim, .fn = camera, .writes = "sprite.camera"},
    {.name = ".hud",     .phase = Phase::PostSim, .fn = hud, .reads = ".score, .label.Label", .writes = ".label.Text", .runs = RunWhen::Always},
};

constinit const HandlerDesc k_handlers[] = {
    {.name = ".on_exit",     KV_ON(ExitRequested, on_exit)},
    {.name = ".on_asset_failed", KV_ON(AssetFailed, on_asset_failed)},
    {.name = ".on_overlap",  KV_ON(OverlapBegin, on_overlap)},
    {.name = ".on_spawned",  KV_ON(Spawned, on_spawned), .writes = ".flame, audio.commands"},
    {.name = ".on_anim_finished", KV_ON(AnimFinished, on_anim_finished)},
    {.name = ".on_rock_hit", KV_ON(RockHit, on_rock_hit), .writes = ".rock, .score, audio.commands"},
    {.name = ".on_ship_hit", KV_ON(ShipHit, on_ship_hit), .reads = ".ship.Transform2", .writes = ".ship.Health"},
    {.name = ".on_score",    KV_ON(ScoreChanged, on_score)},
    {.name = ".on_expire",   KV_ON(Expire, on_expire)},
    {.name = ".on_wave",     KV_ON(SpawnWave, on_wave), .reads = "platform_hid.window", .writes = ".score", .after = ".on_rock_hit"},
    // The score's writers in a chain: rock_hit, wave, edge, restart.
    {.name = ".on_edge",     KV_ON(StateEdge, on_edge), .writes = ".label, audio.commands, .score", .after = ".on_wave"},
    {.name = ".on_restart",  KV_ON(Restart, on_restart), .reads = "platform_hid.window", .writes = ".score, .label", .after = ".on_edge"},
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
