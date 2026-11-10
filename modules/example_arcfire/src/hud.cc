// example_arcfire: what the player sees around the field. The camera fits the field to
// the window; the UI document (assets/ui/arcfire.ui.json) draws the side panels, the
// score, the eggs, the pause screen, the restart's cover and the stage-clear banner. The
// HUD is two calls a tick: model values from the world row and the counters.
#include "arcfire.h"
#include "korvid/asset/scope.h"
#include "korvid/core/log.h"
#include "korvid/platform_hid/actions.h"
#include "korvid/platform_hid/host_window.h"
#include "korvid/sprite/camera.h"
#include "korvid/ui/ui.h"
#include <math.h>
#include <stdio.h>

namespace kv::arcfire {

namespace {

constexpr u32 kClearTicks = 180;

// The HUD's labels are capitals.
void upper(char* s) {
    for (; *s; ++s) *s = (char)(*s >= 'a' && *s <= 'z' ? *s - 32 : *s);
}

} // namespace

void hud_open(Engine& e, Game& g) {
    g.ui = ui_load(e, "example_arcfire/ui/arcfire.ui.json", asset_scope_global(e));
    UiOpen o;
    o.layer = 90;
    o.input = false;
    g.hud = ui_open(e, g.ui, "hud", o);
    ui_model_set_int(e, "eggs_max", kMaxEggs);
    ui_model_set_int(e, "bombs_max", kMaxBombs);
    ui_model_set_int(e, "power_max", kMaxPower);
}

// The restart: a cut to black over the field, lifted after a moment (egg.cc counts it).
void hud_cover(Engine& e, Game& g, bool on) {
    if (on && g.cover.is_null()) {
        UiOpen o;
        o.layer = 120;
        o.input = false;
        g.cover = ui_open(e, g.ui, "cover", o);
    } else if (!on && !g.cover.is_null()) {
        ui_close(e, g.cover);
        g.cover = Handle{};
    }
}

// The bomb's flash: a white cover that closes in a moment and opens again, the sim not held.
void hud_flash(Engine& e) {
    UiCover c;
    c.fx = UiFx::Fade;
    c.seconds = 0.05f;
    c.tint = color{1.0f, 1.0f, 0.96f, 0.85f};
    c.hold = false;
    ui_cover(e, c);
}

void hud_boss(Engine& e, Game& g, const char* title, i32 hp, i32 total) {
    ui_set_visible(e, g.hud, "boss", title != nullptr);
    if (!title) return;
    ui_model_set_str(e, "boss_name", title);
    ui_model_set_int(e, "boss_hp", hp);
    ui_model_set_int(e, "boss_max", total > 0 ? total : 1);
}

// A boss's stomp: the field tinted red and a warning, while it lasts.
void hud_stomp(Engine& e, Game& g, bool on) {
    if (on && g.stomp_screen.is_null()) {
        UiOpen o;
        o.layer = 92;
        o.input = false;
        o.enter.fx = UiFx::Fade;
        g.stomp_screen = ui_open(e, g.ui, "stomp", o);
    } else if (!on && !g.stomp_screen.is_null()) {
        UiMotion out;
        out.fx = UiFx::Fade;
        ui_close_with(e, g.stomp_screen, out);
        g.stomp_screen = Handle{};
    }
}

void hud_stage_clear(Engine& e, Game& g, i64 bonus, const char* next) {
    char text[48];
    if (next) {
        snprintf(text, sizeof text, "NEXT ROUND: %s", next);
        upper(text);
    } else {
        snprintf(text, sizeof text, "THE END");
    }
    ui_model_set_int(e, "stage_bonus", bonus);
    ui_model_set_str(e, "next_round", text);
    if (g.clear_label.is_null()) {
        UiOpen o;
        o.layer = 95;
        o.input = false;
        o.enter.fx = UiFx::Fade;
        g.clear_label = ui_open(e, g.ui, "clear", o);
    }
    g.clear_ticks = kClearTicks;
}

// Phase PreSim, every tick (paused ones are when it matters): pause toggles the pause
// screen and the user hold; quit quits.
void pause(TickContext& ctx) {
    static const u32 k_actions_id = ctx.resource_id("input.actions");
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    const InputActions& a = *ctx.resource<const InputActions>(k_actions_id);
    if (action_pressed(a, g.act_quit)) engine_request_quit(e);
    if (!action_pressed(a, g.act_pause) || !g.stage_ready) return;
    if (g.pause_screen.is_null()) {
        UiOpen o;
        o.layer = 110;
        o.input = false;
        g.pause_screen = ui_open(e, g.ui, "pause", o);
        engine_pause(e, true);
        KV_INFO("arcfire", "paused at tick %llu", (unsigned long long)e.clock.tick_count);
    } else {
        ui_close(e, g.pause_screen);
        g.pause_screen = Handle{};
        engine_pause(e, false);
    }
}

// Phase PostSim, every tick: the field's height fills the window (or its width, in a
// window narrower than 3:4, so the field always shows whole); a shake, decaying. Only the
// camera knows the window, and the camera is not sim state.
void camera(TickContext& ctx) {
    static const u32 k_camera = ctx.resource_id("sprite.camera");
    static const u32 k_window = ctx.resource_id("platform_hid.window");
    Game& g = ctx.state_as<Game>();
    Camera2* cam = ctx.resource<Camera2>(k_camera);
    const HostWindow* w = ctx.resource<const HostWindow>(k_window);
    if (w && w->width() && w->height()) {
        f32 zx = (f32)w->width() / kFieldW, zy = (f32)w->height() / kFieldH;
        cam->zoom = zx < zy ? zx : zy;
    }
    if (g.shake > 0.1f && !ctx.paused) {
        f32 t = (f32)ctx.tick;
        cam->center = point2{sinf(t * 1.9f) * g.shake, cosf(t * 2.7f) * g.shake};
        g.shake *= 0.85f;
    } else if (!ctx.paused) {
        g.shake = 0.0f;
        cam->center = point2{0, 0};
    }
}

// Phase PostSim, every tick: the HUD's numbers, and the banner's time.
void hud(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    Span<const Run> run = ctx.read<Run>(ARC_WORLD(Run));
    Span<const Stage> stage_row = ctx.read<Stage>(ARC_WORLD(Stage));
    ui_set_visible(e, g.hud, "dev", KV_DEV != 0); // the live counters the stress runs read (the screen is built once its document is in)
    if (!run.len) return;
    const Run& r = run[0];
    ui_model_set_int(e, "score", r.score);
    ui_model_set_int(e, "eggs", r.eggs);
    ui_model_set_int(e, "bombs", r.bombs);
    ui_model_set_int(e, "restarts", r.restarts);
    ui_model_set_int(e, "grazes", r.grazes);
    ui_model_set_int(e, "chain", r.chain);
    ui_model_set_int(e, "chain_mult", chain_multiplier(r.chain));
    ui_model_set_int(e, "chain_left", r.chain_left);
    if (Span<const Dials> dials = ctx.read<Dials>(ARC_WORLD(Dials)); dials.len) {
        ui_model_set_int(e, "chain_window", dials[0].chain_window);
        char name[16];
        snprintf(name, sizeof name, "%s", position_name(dials[0].position));
        upper(name);
        ui_model_set_str(e, "difficulty", name);
    }
    if (const Ship* s = engine_table(e, g_ship_kind)->get<Ship>(g.ship)) {
        ui_model_set_str(e, "weapon", weapon_name(s->weapon));
        ui_model_set_int(e, "power", s->power);
    }
    ui_model_set_int(e, "options", engine_table(e, g_option_kind)->live());
    ui_model_set_str(e, "stage", g.stage.name);
    char text[48];
    snprintf(text, sizeof text, "%.0f / %.0f", (f64)stage_clock(stage_row[0]), (f64)g.stage.length);
    ui_model_set_str(e, "distance", text);
    ui_model_set_int(e, "sprites", g.stats.sprites);
    ui_model_set_int(e, "bullets", g.stats.bullets);
    ui_model_set_int(e, "runners", g.stats.runners);
    ui_model_set_int(e, "deltas", g.stats.deltas);
    ui_model_set_int(e, "pairs", g.stats.pairs_tested);
    // A boss's bar: its parts' health, open and armored, over their full.
    Span<const Boss> boss_rows = ctx.read<Boss>("example_arcfire.boss.Boss");
    Span<const Health> part_hp = ctx.read<Health>("example_arcfire.part.Health");
    Span<const Part> parts = ctx.read<Part>("example_arcfire.part.Part");
    const Table* bt = engine_table(e, g_boss_kind);
    const Table* pt = engine_table(e, g_part_kind);
    const BossDef* up = nullptr;
    Handle up_handle;
    for (u32 i = 0; i < bt->rows() && !up; ++i)
        if (bt->is_alive(i)) { up = &g.bosses[boss_rows[i].def]; up_handle = bt->handle_at(i); }
    i32 hp = 0;
    for (u32 i = 0; up && i < pt->rows(); ++i)
        if (pt->is_alive(i) && parts[i].boss == up_handle && part_hp[i].hp > 0) hp += part_hp[i].hp;
    hud_boss(e, g, up ? up->title : nullptr, hp, up ? up->total : 0);
    bool stomping = false;
    for (u32 i = 0; i < bt->rows(); ++i) stomping = stomping || (bt->is_alive(i) && boss_rows[i].state == (u32)BossState::Stomping);
    hud_stomp(e, g, stomping);
    if (g.clear_ticks && !ctx.paused && --g.clear_ticks == 0) {
        UiMotion out;
        out.fx = UiFx::Fade;
        ui_close_with(e, g.clear_label, out);
        g.clear_label = Handle{};
    }
}

} // namespace kv::arcfire
