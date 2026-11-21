// arcfire: what the game remembers between runs. Two files in the platform's
// per-user directory, written through the save module's writer and read as JSON:
// options.json (the volumes, arcade's starting difficulty, the dev counters) and
// scores.json (three tables of ten, one per arcade starting difficulty). The demo keeps
// scores-demo.json and never reads or writes the options, so a scripted run touches nothing
// a player has and plays the same anywhere. Both are asked for at startup and the title
// waits for them as it waits for the stage; a file that is missing or will not parse is
// the defaults, logged. Nothing here is sim state: the starting difficulty reaches the sim
// only through the run it starts, as the dials do.
#include "arcfire.h"
#include "korvid/audio/audio.h"
#include "korvid/core/format.h"
#include "korvid/core/json.h"
#include "korvid/core/log.h"
#include "korvid/platform/io.h"
#include "korvid/save/writer.h"
#include "korvid/sprite_ui/sprite_ui.h"
#include <string.h>

namespace kv::arcfire {

namespace {

u32 clamp_volume(u64 v) { return v > 10 ? 10 : (u32)v; }

void copy_str(char* out, usize cap, const char* s) { format(out, cap, "%s", s ? s : ""); }

void read_options(Game& g, const JsonDoc& doc) {
    const JsonValue& o = doc.root();
    Options& op = g.options;
    op.master = clamp_volume(json_u64(json_get(doc, o, "master"), op.master));
    op.music = clamp_volume(json_u64(json_get(doc, o, "music"), op.music));
    op.effects = clamp_volume(json_u64(json_get(doc, o, "effects"), op.effects));
    const char* start = json_string(json_get(doc, o, "start"), "normal");
    op.start = (u32)(!strcmp(start, "easy") ? Position::Easy : !strcmp(start, "hard") ? Position::Hard : Position::Normal);
    op.dev = json_bool(json_get(doc, o, "dev"), op.dev);
    copy_str(g.last_name, sizeof g.last_name, json_string(json_get(doc, o, "name"), g.last_name));
}

const char* const k_table_keys[kScoreTables] = {"easy", "normal", "hard"};

void read_scores(Game& g, const JsonDoc& doc) {
    for (u32 t = 0; t < kScoreTables; ++t) {
        ScoreTable& table = g.scores[t];
        table.count = 0;
        const JsonValue* rows = json_get(doc, doc.root(), k_table_keys[t]);
        for (const JsonValue* v = rows ? json_first(doc, *rows) : nullptr; v && table.count < kScoreRows; v = json_next(doc, *v)) {
            ScoreRow& r = table.rows[table.count++];
            copy_str(r.name, sizeof r.name, json_string(json_get(doc, *v, "name"), "?"));
            r.score = json_i64(json_get(doc, *v, "score"), 0);
            copy_str(r.stage, sizeof r.stage, json_string(json_get(doc, *v, "stage"), ""));
            r.round = (u32)json_u64(json_get(doc, *v, "round"), 1);
            r.restarts = (u32)json_u64(json_get(doc, *v, "restarts"), 0);
            copy_str(r.date, sizeof r.date, json_string(json_get(doc, *v, "date"), ""));
        }
    }
}

Handle read(Engine& e, const char* path) {
    if (!path[0]) return Handle{};
    IoRequest req;
    req.path = path;
    return platform_io(e)->read_file(req);
}

Handle write(Engine& e, const char* path, SaveWriter& w, const char* what) {
    Handle h;
    if (path[0] && w.finish()) {
        IoWriteRequest req;
        req.path = path;
        req.data = w.out.data;
        req.size = w.out.len;
        h = platform_io(e)->write_file(req);
    }
    if (h.is_null()) KV_WARN("arcfire", "%s: could not be written to %s", what, path[0] ? path : "(no per-user directory)");
    w.free();
    return h;
}

void records_load(Engine& e, Game& g) {
    const char* pref = platform_pref_path(e, "korvid", "arcfire");
    if (pref && pref[0]) {
        if (!g.demo) format(g.options_path, sizeof g.options_path, "%soptions.json", pref);
        format(g.scores_path, sizeof g.scores_path, "%sscores%s.json", pref, g.demo ? "-demo" : "");
    }
    g.options_read = read(e, g.options_path);
    g.scores_read = read(e, g.scores_path);
    g.records_in = g.options_read.is_null() && g.scores_read.is_null();
}

} // namespace

// Phase PreSim, main thread, every tick: the reads and the writes as they complete.
void records(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    Engine& e = *ctx.engine;
    if (g.options_read.is_null() && g.scores_read.is_null() && g.options_write.is_null() && g.scores_write.is_null()) return;
    IoQueue* io = platform_io(e);
    for (u32 i = 0; i < io->completions().len; ++i) {
        IoCompletion c = io->completions()[i];
        Handle* mine = c.request == g.options_read ? &g.options_read : c.request == g.scores_read ? &g.scores_read
                     : c.request == g.options_write ? &g.options_write : c.request == g.scores_write ? &g.scores_write : nullptr;
        if (!mine) continue;
        bool is_read = mine == &g.options_read || mine == &g.scores_read;
        const char* path = mine == &g.options_read || mine == &g.options_write ? g.options_path : g.scores_path;
        io->reap(c.request);
        --i;
        *mine = Handle{};
        if (!is_read) {
            if (c.status != IoStatus::Done) KV_WARN("arcfire", "records: errno %d writing %s", c.error, path);
            continue;
        }
        if (c.status == IoStatus::Done) {
            MemScope scope;
            JsonDoc doc;
            if (!json_parse((const char*)c.data, c.size, mem_scratch(), doc)) KV_WARN("arcfire", "records: %s: %s at byte %u; the defaults", path, doc.error, doc.error_at);
            else if (path == g.options_path) read_options(g, doc);
            else read_scores(g, doc);
            KV_INFO("arcfire", "records: %s read", path);
        } else if (c.error != 2) {
            KV_WARN("arcfire", "records: errno %d reading %s; the defaults", c.error, path);
        }
        if (c.data) free_in(mem_general(), c.data, c.size ? c.size : 1);
    }
    if (!g.records_in && g.options_read.is_null() && g.scores_read.is_null()) {
        g.records_in = true;
        if (!g.start_named) g.start = (Position)g.options.start;
        options_apply(e, g);
    }
}

void options_apply(Engine& e, const Game& g) {
    audio_bus_volume(e, AudioBus::Master, (f32)g.options.master / 10.0f);
    audio_bus_volume(e, AudioBus::Music, (f32)g.options.music / 10.0f);
    audio_bus_volume(e, AudioBus::Sfx, (f32)g.options.effects / 10.0f);
    audio_bus_volume(e, AudioBus::Ui, (f32)g.options.effects / 10.0f);
}

void options_save(Engine& e, Game& g) {
    if (!g.options_path[0] || !g.options_write.is_null()) return;
    SaveWriter w;
    w.init(mem_general());
    w.begin_object();
    w.key("game");    w.string_value("arcfire");
    w.key("master");  w.uint_value(g.options.master);
    w.key("music");   w.uint_value(g.options.music);
    w.key("effects"); w.uint_value(g.options.effects);
    w.key("start");   w.string_value(position_name(g.options.start));
    w.key("dev");     w.bool_value(g.options.dev);
    w.key("name");    w.string_value(g.last_name);
    w.end_object();
    g.options_write = write(e, g.options_path, w, "options");
}

void scores_save(Engine& e, Game& g) {
    if (!g.scores_path[0] || !g.scores_write.is_null()) return;
    SaveWriter w;
    w.init(mem_general());
    w.begin_object();
    w.key("game"); w.string_value("arcfire");
    for (u32 t = 0; t < kScoreTables; ++t) {
        w.key(k_table_keys[t]);
        w.begin_array();
        for (u32 i = 0; i < g.scores[t].count; ++i) {
            const ScoreRow& r = g.scores[t].rows[i];
            w.begin_object();
            w.key("name");     w.string_value(r.name);
            w.key("score");    w.int_value(r.score);
            w.key("stage");    w.string_value(r.stage);
            w.key("round");    w.uint_value(r.round);
            w.key("restarts"); w.uint_value(r.restarts);
            w.key("date");     w.string_value(r.date);
            w.end_object();
        }
        w.end_array();
    }
    w.end_object();
    g.scores_write = write(e, g.scores_path, w, "scores");
}

i32 score_rank(const ScoreTable& t, i64 score) {
    if (score <= 0) return -1;
    for (u32 i = 0; i < t.count; ++i)
        if (score > t.rows[i].score) return (i32)i; // a tie goes under the row that was there first
    return t.count < kScoreRows ? (i32)t.count : -1;
}

void score_insert(ScoreTable& t, u32 row, const ScoreRow& r) {
    if (row >= kScoreRows) return;
    u32 last = t.count < kScoreRows ? t.count : kScoreRows - 1;
    for (u32 i = last; i > row; --i) t.rows[i] = t.rows[i - 1];
    t.rows[row] = r;
    if (t.count < kScoreRows) ++t.count;
}

// ---- what this file declares (its part of the module: module.cc lists it) -----------------------
namespace records_part {
constinit const SystemDesc k_systems[] = {
    {.name = ".records", .phase = Phase::PreSim, .fn = records, .main_thread = true, .runs = RunWhen::Always},
};

// The options and the scores, asked for.
bool startup(Engine& e, ModuleInstance& self) {
    Game& g = *self.state_as<Game>();
    records_load(e, g);
    return true;
}
} // namespace records_part
KV_MODULE_PART(arcfire_records) = {.systems = records_part::k_systems, .startup = records_part::startup};

} // namespace kv::arcfire
