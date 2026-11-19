// arcfire: the screens around the runs.
//
//   - Options (the title's, the pause menu's): the three volumes, arcade's starting
//     difficulty, the dev counters; up and down choose, left and right change; saved as the
//     screen closes.
//   - Game over (out of eggs, a player flying): the score, and a ten-second count; Continue
//     (the stage again, already behind it, as the restart left it) or End. The count running
//     out ends the run.
//   - Results (a run's end, from the title or the demo): the run's numbers revealed a line
//     at a time; then, for a score that makes its table, name entry; then the table, the new
//     row lit; then the next thing (run_finished: the title, once more, or a quit).
//   - High scores (the title's, and after name entry): one table per arcade starting
//     difficulty, left and right between them.
//   - The stage card: the stage's number and name as a round begins, slid in and out.
//
// The demo works the screens it reaches as a player would, through the action map and typed
// text ("DEMO"), from the `screens` system: its own script runs only on sim ticks, and every
// screen here holds the sim.
#include "arcfire.h"
#include "korvid/core/log.h"
#include "korvid/core/format.h"
#include "korvid/core/name.h"
#include "korvid/platform_hid/actions.h"
#include "korvid/platform_hid/module.h"
#include "korvid/sprite_ui/sprite_ui.h"
#include <string.h>
#include "korvid/math/scalar.h"
#include <time.h>

namespace kv::arcfire {

namespace {

constexpr u32 kGameOverTicks = 600; // the count: ten seconds
constexpr u32 kCardTicks = 150;     // the stage card's time up
constexpr u32 kDemoWait = 90;       // the demo reads a screen this long before it goes on
constexpr u64 kNameSettle = 20;     // ticks name entry takes no submit after it opens
constexpr Position k_table_positions[kScoreTables] = {Position::Easy, Position::Normal, Position::Hard};

Handle open_screen(Engine& e, Game& g, const char* screen, i32 layer, bool hold = true) {
    SpuiOpen o;
    o.layer = layer;
    o.hold = hold;
    o.enter.fx = SpuiFx::Fade;
    return spui_open(e, g.ui, screen, o);
}

void close_screen(Engine& e, Handle& h) {
    if (!h.is_null()) spui_close(e, h);
    h = Handle{};
}

void caps(char* s) { // the screens are capitals
    for (; *s; ++s) *s = (char)(*s >= 'a' && *s <= 'z' ? *s - 32 : *s);
}

const char* ordinal(u32 n) { return n == 1 ? "ST" : n == 2 ? "ND" : n == 3 ? "RD" : "TH"; }

// ---- options -------------------------------------------------------------------------------

void options_show(Engine& e, Game& g) {
    char line[48], pos[16];
    format(line, sizeof line, "MASTER VOLUME   < %2u >", g.options.master);
    spui_set_text(e, g.options_screen, "master", line);
    format(line, sizeof line, "MUSIC           < %2u >", g.options.music);
    spui_set_text(e, g.options_screen, "music", line);
    format(line, sizeof line, "EFFECTS         < %2u >", g.options.effects);
    spui_set_text(e, g.options_screen, "effects", line);
    format(pos, sizeof pos, "%s", position_name(g.options.start));
    caps(pos);
    format(line, sizeof line, "ARCADE STARTS  < %s >", pos);
    spui_set_text(e, g.options_screen, "start", line);
    format(line, sizeof line, "DEV COUNTERS   < %s >", g.options.dev ? "ON" : "OFF");
    spui_set_text(e, g.options_screen, "dev", line);
}

// Left or right on the focused item.
void options_change(Engine& e, Game& g, i32 by) {
    const char* item = spui_focused(e, g.options_screen, "items");
    if (!item) return;
    auto step = [by](u32& v) { v = by < 0 ? (v ? v - 1 : 0) : (v < 10 ? v + 1 : 10); };
    if (!strcmp(item, "master")) step(g.options.master);
    else if (!strcmp(item, "music")) step(g.options.music);
    else if (!strcmp(item, "effects")) step(g.options.effects);
    else if (!strcmp(item, "start")) {
        const Position order[] = {Position::Easy, Position::Normal, Position::Hard};
        u32 at = g.options.start == (u32)Position::Easy ? 0 : g.options.start == (u32)Position::Hard ? 2 : 1;
        at = by < 0 ? (at ? at - 1 : 0) : (at < 2 ? at + 1 : 2);
        g.options.start = (u32)order[at];
    } else if (!strcmp(item, "dev")) g.options.dev = !g.options.dev;
    else return;
    options_apply(e, g);
    options_show(e, g);
}

void options_close(Engine& e, Game& g) {
    close_screen(e, g.options_screen);
    if (!g.start_named) g.start = (Position)g.options.start;
    options_save(e, g);
    // What the screen under it shows.
    if (!g.title_screen.is_null()) {
        char note[64], pos[16];
        format(pos, sizeof pos, "%s", position_name((u32)g.start));
        caps(pos);
        format(note, sizeof note, "ARCADE STARTS AT %s", pos);
        spui_model_set_str(e, "title_note", note);
        spui_set_visible(e, g.title_screen, "column", true);
    }
    if (!g.pause_screen.is_null()) spui_set_visible(e, g.pause_screen, "column", true);
}

// ---- results and the tables ------------------------------------------------------------------

// Whether the run takes a row in its table (arcade's; the demo's own file has the one).
i32 result_row(const Game& g, u32* table) {
    *table = g.demo ? 1 : score_table_of((Position)g.result.start);
    if (!g.result.arcade && !g.demo) return -1;
    return score_rank(g.scores[*table], g.result.score);
}

void name_open(Engine& e, Game& g, u32 table, i32 row) {
    char line[64], pos[16];
    format(pos, sizeof pos, "%s", g.demo ? "THE DEMO" : position_name((u32)k_table_positions[table]));
    caps(pos);
    format(line, sizeof line, "%u%s ON %s", (u32)row + 1, ordinal((u32)row + 1), pos);
    spui_model_set_str(e, "name_rank", line);
    g.name_screen = open_screen(e, g, "name", 116);
    spui_set_text(e, g.name_screen, "entry", g.demo ? "" : g.last_name);
    g.name_opened = e.clock.tick_total;
}

void name_submit(Engine& e, Game& g) {
    char name[kNameMax + 1];
    format(name, sizeof name, "%s", spui_field_text(e, g.name_screen, "entry"));
    caps(name);
    close_screen(e, g.name_screen);
    u32 table = 1;
    i32 row = result_row(g, &table);
    if (row >= 0) {
        ScoreRow r{};
        format(r.name, sizeof r.name, "%s", name);
        r.score = g.result.score;
        format(r.stage, sizeof r.stage, "%s", g.result.stage);
        r.round = g.result.round;
        r.restarts = g.result.restarts;
        time_t now = time(nullptr);
        struct tm local;
#if defined(_WIN32)
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        strftime(r.date, sizeof r.date, "%Y-%m-%d", &local);
        score_insert(g.scores[table], (u32)row, r);
        if (!g.demo) format(g.last_name, sizeof g.last_name, "%s", name);
        scores_save(e, g);
        options_save(e, g); // the name, offered next time
        KV_INFO("arcfire", "high score: %s, %lld, row %d of table %u", name, (long long)r.score, row + 1, table);
    }
    scores_open(e, g, table, row);
}

void scores_show(Engine& e, Game& g) {
    u32 t = g.table_shown;
    char head[48], pos[16];
    format(pos, sizeof pos, "%s", g.demo ? "THE DEMO" : position_name((u32)k_table_positions[t]));
    caps(pos);
    format(head, sizeof head, g.demo ? "%s" : "<  %s  >", pos);
    spui_model_set_str(e, "scores_table", head);
    char text[2048];
    usize n = (usize)format(text, sizeof text, "{*c=dim}    NAME          SCORE  STAGE         RESTARTS{/c}\n");
    const ScoreTable& table = g.scores[t];
    for (u32 i = 0; i < kScoreRows && n < sizeof text; ++i) {
        bool lit = (i32)i == g.new_row && t == g.lit_table;
        if (i < table.count) {
            const ScoreRow& r = table.rows[i];
            char stage[24];
            format(stage, sizeof stage, "%.10s %u", r.stage, r.round);
            n += (usize)format(text + n, sizeof text - n, "%s%2u  %-8s %10lld  %-12s %3u%s\n", lit ? "{*c=gold}" : "", i + 1, r.name,
                                 (long long)r.score, stage, r.restarts, lit ? "{/c}" : "");
        } else {
            n += (usize)format(text + n, sizeof text - n, "{*c=dim}%2u  --------          -{/c}\n", i + 1);
        }
    }
    spui_set_text(e, g.scores_screen, "rows", text);
}

} // namespace

void options_open(Engine& e, Game& g, i32 layer) {
    g.options_screen = open_screen(e, g, "options", layer);
    spui_focus(e, g.options_screen, "items", "master");
    options_show(e, g);
    if (!g.title_screen.is_null()) spui_set_visible(e, g.title_screen, "column", false);
    if (!g.pause_screen.is_null()) spui_set_visible(e, g.pause_screen, "column", false);
}

void gameover_open(Engine& e, Game& g, i64 score) {
    char line[48];
    format(line, sizeof line, "SCORE %lld", (long long)score);
    spui_model_set_str(e, "gameover_score", line);
    spui_model_set_int(e, "gameover_count", kGameOverTicks / 60);
    g.gameover_score = score;
    g.gameover_ticks = kGameOverTicks;
    hud_cover(e, g, false); // the restart's cover counts sim ticks, which this screen holds
    card_close(e, g);
    g.card_ticks = 0;
    g.gameover_screen = open_screen(e, g, "gameover", 118);
    spui_focus(e, g.gameover_screen, "items", "continue");
    KV_INFO("arcfire", "game over: score %lld", (long long)score);
}

void results_open(Engine& e, Game& g, const RunResult& r) {
    g.result = r;
    card_close(e, g);
    g.card_ticks = 0;
    char pos[16];
    format(pos, sizeof pos, "%s", position_name(r.position));
    caps(pos);
    char text[1024];
    format(text, sizeof text,
             "SCORE          %12lld\n{*pause=0.3}"
             "STAGE          %12s\n{*pause=0.2}"
             "ROUND          %12u\n{*pause=0.2}"
             "DIFFICULTY     %12s\n{*pause=0.2}"
             "RESTARTS       %12u\n{*pause=0.2}"
             "EGGS LOST      %12u\n{*pause=0.2}"
             "BOMBS USED     %12u\n{*pause=0.2}"
             "LONGEST CHAIN  %12u",
             (long long)r.score, r.stage, r.round, pos, r.restarts, r.deaths, r.bombs_used, r.chain_best);
    g.results_screen = open_screen(e, g, "results", 116);
    spui_set_text(e, g.results_screen, "lines", text);
    g.screen_ticks = 0;
}

void scores_open(Engine& e, Game& g, u32 table, i32 lit) {
    g.table_shown = table;
    g.lit_table = table;
    g.new_row = lit;
    g.scores_screen = open_screen(e, g, "scores", 116);
    scores_show(e, g);
    if (!g.title_screen.is_null()) spui_set_visible(e, g.title_screen, "column", false);
    g.screen_ticks = 0;
}

void card_close(Engine& e, Game& g) {
    if (g.card_screen.is_null()) return;
    SpuiMotion out;
    out.fx = SpuiFx::Slide;
    out.from = SpuiPoint::Right;
    spui_close_with(e, g.card_screen, out);
    g.card_screen = Handle{};
}

void card_open(Engine& e, Game& g, u32 round, u32 position) {
    char line[48], pos[16];
    spui_model_set_str(e, "card_stage", g.stage.name);
    char number[16];
    format(number, sizeof number, "STAGE %u", g.stage_at + 1);
    spui_model_set_str(e, "card_number", number);
    format(pos, sizeof pos, "%s", position_name(position));
    caps(pos);
    if (round > 1) format(line, sizeof line, "ROUND %u  %s", round, pos);
    else format(line, sizeof line, "%s", pos);
    spui_model_set_str(e, "card_round", line);
    if (g.card_screen.is_null()) {
        SpuiOpen o;
        o.layer = 94;
        o.input = false;
        o.enter.fx = SpuiFx::Slide;
        o.enter.from = SpuiPoint::Left;
        g.card_screen = spui_open(e, g.ui, "card", o);
    }
    g.card_ticks = kCardTicks;
}

RunResult run_result(Engine& e, const Game& g, i64 score) {
    RunResult r{};
    const Table* world = engine_table(e, g_world_kind);
    const Run* run = world->get<Run>(g.world);
    const Stage* st = world->get<Stage>(g.world);
    const Dials* d = world->get<Dials>(g.world);
    r.score = score;
    format(r.stage, sizeof r.stage, "%s", g.stage.name);
    r.round = st ? st->loops + 1 : 1;
    r.position = d ? d->position : (u32)g.start;
    r.start = (u32)g.start;
    if (run) {
        r.restarts = run->restarts;
        r.deaths = run->deaths;
        r.bombs_used = run->bombs_used;
        r.chain_best = run->chain_best;
    }
    r.arcade = g.arcade;
    return r;
}

// What the screens say. True when the event was one of theirs.
bool screens_ui(TickContext& ctx, Game& g, const SpuiEvent& ev) {
    Engine& e = *ctx.engine;
    static const u64 k_select = name_intern("select").hash, k_cancel = name_intern("cancel").hash, k_confirm = name_intern("confirm").hash,
                     k_back = name_intern("back").hash, k_submit = name_intern("submit").hash;
    const char* item = ev.event == k_select ? name_str(Name{ev.node}) : nullptr;
    if (ev.screen.is_null()) return false;
    if (ev.screen == g.options_screen) {
        if (ev.event == k_cancel || ev.event == k_back || (item && !strcmp(item, "back"))) options_close(e, g);
        return true;
    }
    if (ev.screen == g.gameover_screen) {
        if (!item) return true;
        close_screen(e, g.gameover_screen);
        if (!strcmp(item, "continue")) KV_INFO("arcfire", "game over: continue");
        else {
            KV_INFO("arcfire", "game over: the run ends");
            g.result = run_result(e, g, g.gameover_score);
            results_log(g, g.result);
            run_over(ctx, g);
        }
        return true;
    }
    if (ev.screen == g.results_screen) {
        if (ev.event != k_confirm && ev.event != k_cancel) return true;
        if (spui_revealing(e, g.results_screen, "lines")) return true; // the confirm finished the reveal
        close_screen(e, g.results_screen);
        u32 table = 1;
        i32 row = result_row(g, &table);
        if (row >= 0) name_open(e, g, table, row);
        else if (g.result.arcade) scores_open(e, g, table, -1);
        else run_finished(ctx, g);
        return true;
    }
    if (ev.screen == g.name_screen) {
        // Not the Enter that confirmed the results, still landing as the field opens; and
        // not an empty name: the screen waits for one.
        const char* typed = spui_field_text(e, g.name_screen, "entry");
        if (ev.event == k_submit && e.clock.tick_total - g.name_opened >= kNameSettle && typed && typed[0]) name_submit(e, g);
        return true;
    }
    if (ev.screen == g.scores_screen) {
        if (ev.event == k_confirm || ev.event == k_cancel) {
            close_screen(e, g.scores_screen);
            if (g.scores_from_title) spui_set_visible(e, g.title_screen, "column", true);
            else run_finished(ctx, g);
            g.scores_from_title = false;
        }
        return true;
    }
    return false;
}

// Phase PreSim, main thread, every tick (paused ones are when it matters): left and right on
// the options and the tables; the game-over count; the stage card's time; the demo's hand on
// the screens it reaches.
void screens(TickContext& ctx) {
    static const u32 k_actions_id = ctx.resource_id("input.actions");
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    const InputActions& a = *ctx.resource<const InputActions>(k_actions_id);
    vec2 move = action_axis(a, g.act_move);
    bool sideways = action_repeat(a, g.act_move, 18, 6) && kv::fabs(move.x) > kv::fabs(move.y);
    if (sideways && !g.options_screen.is_null() && spui_top(e) == g.options_screen) options_change(e, g, move.x < 0.0f ? -1 : 1);
    if (sideways && !g.scores_screen.is_null() && spui_top(e) == g.scores_screen && !g.demo) {
        g.table_shown = (g.table_shown + (move.x < 0.0f ? kScoreTables - 1 : 1)) % kScoreTables;
        scores_show(e, g);
    }
    if (!g.gameover_screen.is_null() && g.gameover_ticks) {
        if (--g.gameover_ticks % 60 == 0) spui_model_set_int(e, "gameover_count", g.gameover_ticks / 60);
        if (g.gameover_ticks == 0) { // the count ran out: the run ends
            spui_focus(e, g.gameover_screen, "items", "end");
            input_action_override(e, "confirm", true);
        }
    }
    if (g.card_ticks && !ctx.paused && --g.card_ticks == 0) card_close(e, g);
    if (!g.demo) return;
    // The demo reads each screen a moment, then goes on: confirm, and its name.
    bool up = !g.results_screen.is_null() || !g.scores_screen.is_null() || !g.name_screen.is_null();
    if (!up) return;
    if (++g.screen_ticks == kDemoWait) {
        if (!g.name_screen.is_null()) input_text_override(e, "DEMO\n");
        else input_action_override(e, "confirm", true);
        g.screen_ticks = 0;
    }
}

} // namespace kv::arcfire
