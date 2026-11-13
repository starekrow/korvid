// arcfire: the program around a run. Run plainly, the game opens on a title
// (Arcade, Stage select, High scores, Options, Quit); a flag that names a run (--demo, --autoplay, --stress,
// --bench, --stage, --arcade) goes straight into it. A run starts from a fresh sim
// (engine_sim_reset), so a second run in the process is the same run as the first in a new
// one, and is left for the title or the next run the same way: a switch is asked for
// (flow_request), happens behind a cover on a held tick (`flow_step`, from the boot
// system, which holds the writes), and the cover opens on what is there now. The pause
// menu (Resume, Options, Quit to title) holds the sim as the pause did. A run's end goes
// through the screens (screens.cc: results, name entry, the table) and then here again
// (run_finished). Everything here is
// presentation but the run it starts, whose plan (arcade or a stage, the difficulty) is
// hashed as the dials are.
//
// Attract mode: the title left alone for --attract seconds (20) plays the demo from a fresh
// sim, as --demo plays it; a key or a pad's button goes back to the title, and so does the
// demo's end.
//
// `--again`, a check: a run that ends starts once more from a fresh sim instead of quitting,
// so its hashes can be compared with the first's, tick for tick.
#include "arcfire.h"
#include "korvid/core/log.h"
#include "korvid/core/format.h"
#include "korvid/core/name.h"
#include "korvid/frameloop/frameloop.h"
#include "korvid/platform_hid/actions.h"
#include "korvid/platform_hid/input.h"
#include "korvid/ui/ui.h"
#include <string.h>

namespace kv::arcfire {

namespace {

constexpr f32 kCoverSeconds = 0.35f;
// The title holds the sim from the moment it opens (a screen's own hold lands a tick later,
// and a sim tick there would make the next run start at 1) until a run begins.
constexpr u32 kHoldTitle = (u32)HoldGame << 1;

Handle open_menu(Engine& e, Game& g, const char* screen, i32 layer, const char* focus = nullptr) {
    UiOpen o;
    o.layer = layer;
    o.hold = true;
    o.enter.fx = UiFx::Fade;
    Handle h = ui_open(e, g.ui, screen, o);
    if (focus) ui_focus(e, h, "items", focus);
    return h;
}

void close_menu(Engine& e, Handle& h) {
    if (!h.is_null()) ui_close(e, h);
    h = Handle{};
}

void close_pause(Engine& e, Game& g) {
    close_menu(e, g.confirm_screen);
    if (g.pause_screen.is_null()) return;
    close_menu(e, g.pause_screen);
    engine_pause(e, false);
}

void open_title(Engine& e, Game& g) {
    engine_hold(e, kHoldTitle, true);
    close_menu(e, g.attract_screen);
    g.attract = false;
    g.demo = false;
    g.idle_ticks = 0;
    char note[64];
    format(note, sizeof note, "ARCADE STARTS AT %s", position_name((u32)g.start));
    g.scores_from_title = false;
    for (char* c = note; *c; ++c) *c = (char)(*c >= 'a' && *c <= 'z' ? *c - 32 : *c);
    ui_model_set_str(e, "title_note", note);
    hud_shown(e, g, false);
    g.title_screen = open_menu(e, g, "title", 110, "arcade");
    g.mode = Mode::Title;
}

// What the old run left: every sim row, by the fresh sim; the cosmetic rows that ride on
// them or show them (a reused handle would give a shadow a new owner); the screens and the
// time scale it had up.
void clear_run(TickContext& ctx, Game& g) {
    Engine& e = *ctx.engine;
    engine_sim_reset(e);
    ctx.tick = e.clock.tick_count; // this tick's units after this one see the new sim tick
    const u32 cosmetic[] = {g_particle_kind, g_shadow_kind, g_boom_kind};
    for (u32 k : cosmetic) ctx.write_kind(k)->destroy_all();
    hud_cover(e, g, false);
    hud_stomp(e, g, false);
    hud_boss(e, g, nullptr, 0, 0);
    card_close(e, g);
    g.card_ticks = 0;
    if (!g.clear_label.is_null()) { ui_close(e, g.clear_label); g.clear_label = Handle{}; }
    g.clear_ticks = 0;
    engine_time_scale(e, 1.0f);
    g.world = g.ship = g.ring = g.mark = Handle{};
}

} // namespace

void flow_request(Engine& e, Game& g, Next next, bool cover) {
    if (g.next != Next::None) return; // one at a time
    g.next = next;
    if (cover) {
        UiCover c;
        c.seconds = kCoverSeconds;
        g.flow_cover = ui_cover(e, c);
        ui_cover_wait(e, g.flow_cover, true);
        g.next_ready = false;
    } else {
        engine_hold(e, HoldGame, true); // no cover: the switch on the next tick, held
        g.flow_cover = 0;
        g.next_ready = true;
    }
}

// The assets are in (boot): the planes, then the title or the flags' run.
void flow_ready(TickContext& ctx, Game& g) {
    Engine& e = *ctx.engine;
    stage_enter(ctx, g, g.front_door ? 0 : g.plan.stage, dials_start(e, g, g.demo ? Position::Demo : g.stress ? Position::Stress : g.start));
    if (g.front_door) open_title(e, g);
    else run_begin(ctx, g);
    music(ctx, g, false);
    engine_hold(e, HoldLoading, false);
}

// A run: a fresh sim (but for the first, which has one), the plan's dials, the world row,
// the ship and what it starts with.
void run_begin(TickContext& ctx, Game& g) {
    Engine& e = *ctx.engine;
    if (g.runs++) clear_run(ctx, g);
    engine_hold(e, kHoldTitle, false);
    g.arcade = g.plan.arcade;
    g.stage_select = g.plan.stage_select;
    g.start = g.plan.start;
    g.demo_core_bomb = false;
    g.pilot_v = vec2{0, 0};
    g.shake = 0.0f;
    for (Game::Ring& ring : g.rings) ring = Game::Ring{};
    Dials d = dials_start(e, g, g.demo ? Position::Demo : g.stress ? Position::Stress : g.start);
    dials_log(d);
    u32 first = g.plan.arcade ? 0 : g.plan.stage; // arcade starts at the first stage
    if (g.runs > 1 || first != g.stage_at) stage_enter(ctx, g, first, d); // the first run's was entered at boot
    Stage s{};
    s.index = first;
    stage_reset(s, g.stage);
    Run run{};
    run.eggs = kStartEggs;
    run.next_extend = kFirstExtend;
    run.bombs = kBaseBombs;
    Rng r = Rng::seeded(0xa2cf1e5eedull, 1); // the sim's stream: patterns and drift, the same every run
    g.world = ctx.write_kind(g_world_kind)->spawn(d, s, run, EggState{}, Chance{r.state, r.inc});
    g.ship = ship_spawn(e, g, d);
    for (u32 i = 0; i < d.options && i < kMaxOptions; ++i) option_add(ctx, g);
    if (g.bench[0]) bench_spawn(ctx, g, d);
    hud_shown(e, g, true);
    if (!g.attract) card_open(e, g, 1, d.position);
    else { // the attract's PRESS START, over the field's middle
        UiOpen o;
        o.layer = 96;
        o.input = false;
        g.attract_screen = ui_open(e, g.ui, "attract", o);
        g.blink = 0;
    }
    g.result = RunResult{};
    if (g.front_door) { // a recording of a run the title chose says how to start it
        char note[96];
        format(note, sizeof note, "%s %s --difficulty %s", g.arcade ? "--arcade" : "--stage", g.arcade ? "" : k_stage_names[g.stage_at], position_name((u32)g.start));
        frameloop_record_note(e, note);
    }
    g.mode = Mode::Run;
    KV_INFO("arcfire", "run %u: %s, %s at %s", g.runs, g.arcade ? "arcade" : g.stage_select ? "stage select" : "a run", g.stage.name,
            position_name(d.position));
}

// Phase PreSim (the boot system, which holds the writes), on a held tick: a switch asked
// for, once its cover has closed.
void flow_step(TickContext& ctx, Game& g) {
    Engine& e = *ctx.engine;
    if (g.next == Next::None || !g.next_ready || !ctx.paused) return;
    Next next = g.next;
    g.next = Next::None;
    close_pause(e, g);
    close_menu(e, g.title_screen);
    close_menu(e, g.select_screen);
    close_menu(e, g.difficulty_screen);
    close_menu(e, g.options_screen);
    close_menu(e, g.gameover_screen);
    if (next == Next::Run) run_begin(ctx, g);
    else if (next == Next::Stage) stage_change(ctx, g);
    else {
        clear_run(ctx, g);
        if (g.stage_at != 0) stage_enter(ctx, g, 0, dials_start(e, g, g.start)); // the title shows the first stage
        open_title(e, g);
        KV_INFO("arcfire", "the title");
    }
    music(ctx, g, false);
    if (g.flow_cover) ui_cover_wait(e, g.flow_cover, false);
    engine_hold(e, HoldGame, false); // run_over's, or the switch's own
    g.flow_cover = 0;
}

// The run is over (the demo at its end mark, the rest after the banner, or the game-over
// screen's End), g.result its numbers: from the title, and for the demo, the screens;
// otherwise the next thing at once.
void run_over(TickContext& ctx, Game& g) {
    // Held at once: a screen's own hold lands a tick later, and the run's last sim tick is
    // the one that ended it. The switch that follows lets go.
    engine_hold(*ctx.engine, HoldGame, true);
    if (g.attract) { flow_request(*ctx.engine, g, Next::Title, true); return; }
    if (g.front_door || g.demo) results_open(*ctx.engine, g, g.result);
    else run_finished(ctx, g);
}

// The screens after a run are done: once more (--again), the title, or a quit.
void run_finished(TickContext& ctx, Game& g) {
    Engine& e = *ctx.engine;
    if (g.again && !g.again_done) {
        g.again_done = true;
        KV_INFO("arcfire", "again: the run once more, from a fresh sim");
        flow_request(e, g, Next::Run, false);
    } else if (g.front_door) {
        flow_request(e, g, Next::Title, true);
    } else {
        engine_request_quit(e);
    }
}

// Phase PreSim, every tick (paused ones are when it matters): quit quits; pause opens the
// pause menu over a run, or closes it.
void pause(TickContext& ctx) {
    static const u32 k_actions_id = ctx.resource_id("input.actions");
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    const InputActions& a = *ctx.resource<const InputActions>(k_actions_id);
    if (action_pressed(a, g.act_quit)) engine_request_quit(e);
    // A person at the keys or a pad: the title stays (attract waits), or comes back.
    const Input& in = *ctx.resource<const Input>("input.state");
    bool touched = false;
    for (u64 b : in.sampled.bits) touched = touched || b;
    for (const GamepadState& p : in.sampled.pads) touched = touched || p.buttons;
    if (g.attract) {
        // On every tick, the demo's slow motion included: half a second on, half off.
        if (!g.attract_screen.is_null()) ui_set_visible(e, g.attract_screen, "press", (g.blink++ / 30) % 2 == 0);
        if (touched && g.next == Next::None) flow_request(e, g, Next::Title, true);
        return;
    }
    if (g.mode == Mode::Title && g.next == Next::None) {
        bool menus = !g.select_screen.is_null() || !g.difficulty_screen.is_null() || !g.options_screen.is_null() || !g.scores_screen.is_null();
        g.idle_ticks = touched || menus ? 0 : g.idle_ticks + 1;
        if (g.attract_after && g.idle_ticks >= g.attract_after) {
            KV_INFO("arcfire", "attract: the demo");
            g.attract = true;
            g.demo = true;
            g.plan = RunPlan{false, false, Position::Normal, 0};
            flow_request(e, g, Next::Run, true);
        }
        return;
    }
    if (!action_pressed(a, g.act_pause) || g.mode != Mode::Run || g.next != Next::None) return;
    if (!g.gameover_screen.is_null() || !g.results_screen.is_null() || !g.name_screen.is_null() || !g.scores_screen.is_null()) return;
    if (!g.options_screen.is_null()) { input_action_override(e, "cancel", true); return; } // as the options' own cancel
    if (!g.confirm_screen.is_null()) { close_menu(e, g.confirm_screen); return; }
    if (!g.pause_screen.is_null()) { close_pause(e, g); return; }
    UiOpen o;
    o.layer = 110;
    o.enter.fx = UiFx::Fade;
    g.pause_screen = ui_open(e, g.ui, "pause", o);
    ui_set_text(e, g.pause_screen, "quit", g.front_door ? "QUIT TO TITLE" : "QUIT");
    ui_focus(e, g.pause_screen, "items", "resume");
    engine_pause(e, true);
    KV_INFO("arcfire", "paused at tick %llu", (unsigned long long)e.clock.tick_count);
}

// What the menus say: an item chosen, a cancel.
void on_ui(TickContext& ctx, Span<const UiEvent> events) {
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    static const u64 k_select = name_intern("select").hash, k_cancel = name_intern("cancel").hash, k_back = name_intern("back").hash;
    for (const UiEvent& ev : events) {
        if (screens_ui(ctx, g, ev)) continue;
        bool chose = ev.event == k_select, back = ev.event == k_cancel || ev.event == k_back;
        const char* item = chose ? name_str(Name{ev.node}) : "";
        if (!item) item = "";
        if (ev.screen == g.title_screen && chose) {
            if (!strcmp(item, "arcade")) {
                g.plan = RunPlan{true, false, g.start, 0};
                flow_request(e, g, Next::Run, true);
            } else if (!strcmp(item, "select")) {
                g.select_screen = open_menu(e, g, "select", 112, "shoreline");
                ui_set_visible(e, g.title_screen, "column", false); // one menu at a time shows
            } else if (!strcmp(item, "scores")) {
                g.scores_from_title = true;
                scores_open(e, g, score_table_of(g.start), -1);
            } else if (!strcmp(item, "options")) {
                options_open(e, g, 112);
            } else if (!strcmp(item, "quit")) {
                engine_request_quit(e);
            }
        } else if (ev.screen == g.select_screen) {
            if (back) {
                close_menu(e, g.select_screen);
                ui_set_visible(e, g.title_screen, "column", true);
            } else if (chose) {
                u32 picked = stage_named(item);
                g.select_stage = picked < kStageCount ? picked : 0;
                char upper_name[32];
                format(upper_name, sizeof upper_name, "%s", g.stages[g.select_stage].name);
                ui_model_set_str(e, "select_stage", upper_name);
                g.difficulty_screen = open_menu(e, g, "difficulty", 114, position_name((u32)g.start));
                ui_set_visible(e, g.select_screen, "column", false);
            }
        } else if (ev.screen == g.difficulty_screen) {
            if (back) {
                close_menu(e, g.difficulty_screen);
                ui_set_visible(e, g.select_screen, "column", true);
            }
            else if (chose) {
                Position p = !strcmp(item, "easy") ? Position::Easy : !strcmp(item, "hard") ? Position::Hard : Position::Normal;
                g.plan = RunPlan{false, true, p, g.select_stage};
                flow_request(e, g, Next::Run, true);
            }
        } else if (ev.screen == g.pause_screen && g.confirm_screen.is_null()) {
            if (back || (chose && !strcmp(item, "resume"))) close_pause(e, g);
            else if (chose && !strcmp(item, "options")) options_open(e, g, 112);
            else if (chose && !strcmp(item, "quit")) {
                if (!g.front_door) engine_request_quit(e);
                else {
                    ui_model_set_str(e, "confirm", "QUIT TO THE TITLE? THE RUN ENDS.");
                    g.confirm_screen = open_menu(e, g, "confirm", 112, "no");
                }
            }
        } else if (ev.screen == g.confirm_screen) {
            if (chose && !strcmp(item, "yes")) flow_request(e, g, Next::Title, true);
            else if (back || chose) close_menu(e, g.confirm_screen);
        }
    }
}

// A switch's cover has closed: the switch may happen (flow_step, on the next held tick).
void on_covered(TickContext& ctx, Span<const UiCovered> covers) {
    Game& g = ctx.state_as<Game>();
    for (const UiCovered& c : covers)
        if (c.cover == g.flow_cover) g.next_ready = true;
}

} // namespace kv::arcfire
