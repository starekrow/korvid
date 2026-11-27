// hello: the game the introduction builds (docs/guides/introduction.md). One sprite, the
// blob, in a ktable of its own. The arrow keys steer it (the "move" action, read by the
// system steer); Space pulses it: steer emits Pulsed, a handler makes the blob large and
// orange and schedules Calm 20 ticks out, and Calm's handler puts it back. Every key that
// goes down is logged, as the raw events the window delivers.
#include "korvid/hello/module.h"
#include "korvid/asset_render/material.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/frameloop/frameloop.h"
#include "korvid/math/fields.h"
#include "korvid/platform_hid/actions.h"
#include "korvid/platform_hid/events.h"
#include "korvid/sprite/sprite.h"
#include "korvid/sprite/transform2.h"

namespace kv {

namespace {

// ---- what the blob is ----------------------------------------------------------------
constinit u32 g_blob_ktable = 0;
constinit const KTableDesc k_ktables[] = {
    {.name = ".blob", .components = "Transform2, Sprite", .capacity = 1, .id = &g_blob_ktable},
};

// ---- what happens to it --------------------------------------------------------------
struct Pulsed { Handle blob; };
struct Calm   { Handle blob; };

constinit const FieldDesc k_pulsed_fields[] = {KV_FIELDS(Pulsed, blob)};
constinit const FieldDesc k_calm_fields[]   = {KV_FIELDS(Calm, blob)};
constinit const EventTypeDesc k_events[] = {
    KV_EVENT_EX(Pulsed, k_pulsed_fields, 0),
    KV_EVENT_EX(Calm, k_calm_fields, 0),
};

// ---- what the player can do ----------------------------------------------------------
constinit const InputActionDesc k_actions[] = {
    {"move",  ActionKind::Axis2,  "keys Up Down Left Right, keys W S A D, stick left, dpad"},
    {"pulse", ActionKind::Button, "key Space, pad South"},
};

constexpr f32   kSpeed     = 360.0f;         // world units (pixels) a second
constexpr f32   kSize      = 128.0f;         // the blob at rest: the 64-pixel ball, twice
constexpr f32   kPulseSize = 192.0f;         // and pulsing
constexpr u64   kPulseTicks = 20;            // a third of a second at 60 Hz
constexpr f32   kHalfW = 480.0f, kHalfH = 270.0f; // the field: the window's size at the start
constexpr color kRest{0.35f, 0.75f, 1.0f, 1.0f};
constexpr color kPulse{1.0f, 0.55f, 0.15f, 1.0f};

// ---- systems -------------------------------------------------------------------------

// Phase Sim: the blob goes where "move" points, and stays in the field. Space emits Pulsed.
void steer(TickContext& ctx) {
    static const u32 k_actions_id = ctx.resource_id("input.actions");
    const InputActions& a = *ctx.resource<const InputActions>(k_actions_id);
    static const u32 k_move = a.find("move"), k_pulse = a.find("pulse");

    vec2 dir = action_axis(a, k_move);
    f32  dt  = (f32)ctx.dt;
    KTableRows k = ctx.rows(g_blob_ktable);
    Span<Transform2> t = k.col<Transform2>();
    for (u32 i = 0; i < k.rows(); ++i) {
        if (!k.alive(i)) continue;
        point2& p = t[i].position;
        p += dir * (kSpeed * dt);
        p.x = p.x < -kHalfW ? -kHalfW : p.x > kHalfW ? kHalfW : p.x;
        p.y = p.y < -kHalfH ? -kHalfH : p.y > kHalfH ? kHalfH : p.y;
        if (action_pressed(a, k_pulse)) ctx.emit(Pulsed{k.handle_at(i)});
    }
}

// ---- handlers ------------------------------------------------------------------------

// Phase Sim, after steer: large and orange now, back to rest in kPulseTicks.
void on_pulsed(TickContext& ctx, Span<const Pulsed> pulses) {
    static const u32 k_sprite = ctx.column_id(".blob.Sprite");
    Span<Sprite> s = ctx.write<Sprite>(k_sprite);
    for (const Pulsed& p : pulses) {
        Sprite& look = s[p.blob.index()];
        look.tint = kPulse;
        look.size = vec2{kPulseSize, kPulseSize};
        ctx.schedule_in(Calm{p.blob}, kPulseTicks);
        KV_INFO("hello", "tick %llu: pulse", (unsigned long long)ctx.tick);
    }
}

// Phase Input, at the start of the tick the timer is due.
void on_calm(TickContext& ctx, Span<const Calm> calms) {
    static const u32 k_sprite = ctx.column_id(".blob.Sprite");
    Span<Sprite> s = ctx.write<Sprite>(k_sprite);
    for (const Calm& c : calms) {
        Sprite& look = s[c.blob.index()];
        look.tint = kRest;
        look.size = vec2{kSize, kSize};
        KV_INFO("hello", "tick %llu: calm", (unsigned long long)ctx.tick);
    }
}

// Phase Input: every key that went down, as the window delivered it.
void on_key(TickContext& ctx, Span<const KeyDown> keys) {
    for (const KeyDown& k : keys)
        if (!k.repeat) KV_INFO("hello", "tick %llu: key %s", (unsigned long long)ctx.tick, key_name(k.key));
}

// Phase Sim: the window closed, Escape, or --run-seconds ran out. Nothing to save: quit.
void on_exit(TickContext& ctx, Span<const ExitRequested> exits) {
    KV_INFO("hello", "exit requested (%s)", exits[0].reason == ExitReason::Escape ? "Escape" :
                                            exits[0].reason == ExitReason::WindowClosed ? "window closed" : "timer");
    engine_request_quit(*ctx.engine);
}

// ---- startup and the tables ----------------------------------------------------------

bool hello_startup(Engine& e, ModuleInstance&) {
    if (!input_actions(e, k_actions)) return false;
    Handle ball = asset_load_material(e, "hello/ball.material.json");
    Sprite look;
    look.material = asset_material(e, ball)->material;
    look.size = vec2{kSize, kSize};
    look.tint = kRest;
    look.layer = 1;
    Handle blob = engine_ktable(e, g_blob_ktable)->spawn(transform2_at(point2{0.0f, 0.0f}), look);
    KV_INFO("hello", "the blob is up");
    return !blob.is_null();
}

constinit const SystemDesc k_systems[] = {
    {.name = ".steer", .phase = Phase::Sim, .fn = steer, .reads = "input.actions, .blob.@alive",
     .writes = ".blob.Transform2", .emits = "Pulsed"},
};
constinit const HandlerDesc k_handlers[] = {
    {.name = ".on_pulsed", KV_ON(Pulsed, on_pulsed), .phase = Phase::Sim, .writes = ".blob.Sprite"},
    {.name = ".on_calm", KV_ON(Calm, on_calm), .phase = Phase::Input, .writes = ".blob.Sprite"},
    {.name = ".on_key", KV_ON(KeyDown, on_key), .phase = Phase::Input},
    {.name = ".on_exit", KV_ON(ExitRequested, on_exit), .phase = Phase::Sim},
};

} // namespace

KV_MODULE_DEFINE(hello_module) = {
    .name     = "hello",
    .events   = k_events,
    .ktables  = k_ktables,
    .systems  = k_systems,
    .handlers = k_handlers,
    .startup  = hello_startup,
};

} // namespace kv
