// arcfire: what the player sees around the field. The camera fits the field to
// the window; the UI document (assets/ui/arcfire.ui.json) draws the side panels, the
// score, the eggs, the restart's cover and the stage-clear banner; the menus are flow.cc's.
// The HUD is two calls a tick: model values from the world row and the counters.
#include "arcfire.h"
#include "korvid/asset/scope.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/platform_hid/actions.h"
#include "korvid/platform_hid/host_window.h"
#include "korvid/render/renderer.h"
#include "korvid/sprite/camera.h"
#include "korvid/ui/ui.h"
#include "korvid/math/scalar.h"

namespace kv::arcfire {

namespace {

constexpr u32 kClearTicks = 180;

// The HUD's labels are capitals.
void upper(char* s) {
    for (; *s; ++s) *s = (char)(*s >= 'a' && *s <= 'z' ? *s - 32 : *s);
}

} // namespace

void hud_open(Engine& e, Game& g) {
    g.ui = ui_load(e, "arcfire/ui/arcfire.ui.json", asset_scope_global(e));
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

// A run's numbers in the side panels, or (the title) the panels alone.
void hud_shown(Engine& e, Game& g, bool on) {
    ui_set_visible(e, g.hud, "left/info", on);
    ui_set_visible(e, g.hud, "right/info", on);
    g.hud_on = on;
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
        format(text, sizeof text, "%s", next);
        upper(text);
    } else {
        format(text, sizeof text, "THE END");
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
        cam->center = point2{kv::sin(t * 1.9f) * g.shake, kv::cos(t * 2.7f) * g.shake};
        g.shake *= 0.85f;
    } else if (!ctx.paused) {
        g.shake = 0.0f;
        cam->center = point2{0, 0};
    }
}

// Phase PostSim, every tick, after the camera: the shimmer. The dial sets the renderer's
// post pass (the world below Z_POST into a target, arcfire_shimmer over it); the rings age
// on unpaused ticks and go to the shader as window pixels: x, y (down), the radius so far
// and the strength left, fading as the ring spreads. Presentation only: nothing here is
// read back.
void shimmer(TickContext& ctx) {
    static const u32 k_camera = ctx.resource_id("sprite.camera");
    static const u32 k_window = ctx.resource_id("platform_hid.window");
    Game& g = ctx.state_as<Game>();
    Renderer* r = renderer_get(*ctx.engine);
    if (!r) return;
    Span<const Dials> dials = ctx.read<Dials>(ARC_WORLD(Dials));
    bool want = dials.len ? dials[0].shimmer != 0 : g.post_on; // between runs, as it was
    if (want != g.post_on) {
        renderer_post_layer(r, Z_POST, want ? "arcfire_shimmer" : nullptr); // a shader that will not load is logged once
        g.post_on = want;
    }
    f32 params[KV_POST_PARAMS * 4];
    u32 n = 0;
    const Camera2* cam = ctx.resource<const Camera2>(k_camera);
    const HostWindow* w = ctx.resource<const HostWindow>(k_window);
    for (Game::Ring& ring : g.rings) {
        if (!ring.life) continue;
        if (!ctx.paused && ++ring.age >= ring.life) { ring = Game::Ring{}; continue; }
        if (!g.post_on || !cam || !w || n == KV_POST_PARAMS) continue;
        f32 t = (f32)ring.age / (f32)ring.life;
        f32 spread = 1.0f - (1.0f - t) * (1.0f - t);
        f32* p = &params[4 * n++];
        p[0] = 0.5f * (f32)w->width() + (ring.at.x - cam->center.x) * cam->zoom;
        p[1] = 0.5f * (f32)w->height() - (ring.at.y - cam->center.y) * cam->zoom;
        p[2] = ring.reach * spread * cam->zoom;
        p[3] = ring.strength * (1.0f - t);
    }
    if (g.post_on) renderer_post_params(r, params, n);
}

// Phase PostSim, every tick: the HUD's numbers, and the banner's time.
void hud(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    Span<const Run> run = ctx.read<Run>(ARC_WORLD(Run));
    Span<const Stage> stage_row = ctx.read<Stage>(ARC_WORLD(Stage));
    ui_set_visible(e, g.hud, "dev", KV_DEV != 0 && g.hud_on); // the live counters the stress runs read (the screen is built once its document is in)
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
        format(name, sizeof name, "%s", position_name(dials[0].position));
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
    format(text, sizeof text, "%.0f / %.0f", (f64)stage_clock(stage_row[0]), (f64)g.stage.length);
    ui_model_set_str(e, "distance", text);
    ui_model_set_int(e, "sprites", g.stats.sprites);
    ui_model_set_int(e, "bullets", g.stats.bullets);
    ui_model_set_int(e, "runners", g.stats.runners);
    ui_model_set_int(e, "deltas", g.stats.deltas);
    ui_model_set_int(e, "pairs", g.stats.pairs_tested);
    // A boss's bar: its parts' health, open and armored, over their full.
    Span<const Boss> boss_rows = ctx.read<Boss>("arcfire.boss.Boss");
    Span<const Health> part_hp = ctx.read<Health>("arcfire.part.Health");
    Span<const Part> parts = ctx.read<Part>("arcfire.part.Part");
    const Table* bt = engine_table(e, g_boss_kind);
    const Table* pt = engine_table(e, g_part_kind);
    // Bosses that come together (Orbit's gunships) share it: every live one's parts, over
    // the full of all that came (it keeps the full of one gone down).
    const BossDef* up = nullptr;
    i32 hp = 0, total = 0;
    for (u32 i = 0; i < bt->rows(); ++i) {
        if (!bt->is_alive(i)) continue;
        const BossDef& def = g.bosses[boss_rows[i].def];
        if (!up) up = &def;
        total += def.total;
        Handle h = bt->handle_at(i);
        for (u32 k = 0; k < pt->rows(); ++k)
            if (pt->is_alive(k) && parts[k].boss == h && part_hp[k].hp > 0) hp += part_hp[k].hp;
    }
    g.bar_total = up ? (total > g.bar_total ? total : g.bar_total) : 0;
    hud_boss(e, g, up ? up->title : nullptr, hp, g.bar_total);
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
