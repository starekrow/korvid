// Recording and replay: a run's input as a file, played back as a hashing test.
//
// `--record FILE` writes, for every sim tick since the last fresh sim (or the start), the
// values of the actions the game names in FrameLoopConfig::recorded_actions (its gameplay:
// not pause, not quit), as changes only, and the state hash every 60 ticks. Paused and held
// ticks are not sim time and record nothing, so menus, a pause and a disk of any speed
// leave the file as it is. The header carries the executable's name, the game's flags and
// options as given, and what the game adds (frameloop_record_note: the flags that start
// the run it chose from a menu), so a replay starts the same run.
//
// `--replay FILE` takes the header's game arguments as if given, sets the named actions
// from the file before each sim tick's sample (the game's pilots, asked
// frameloop_replaying, stand down), leaves the rest live so a viewer can pause and quit,
// compares the hash every 60 ticks, logs the first tick that differs, and at the file's
// end says whether every hash matched.
//
// The file (.kvr): "KVR1", a u32 header length, the header (lines: `exe NAME`, `arg TOKEN`,
// `action NAME`), then records, each a varint of ticks since the last and a byte: 1, an
// action (u8 index, u8 down, f32 x, f32 y); 2, a hash (u64); 3, the end. Little-endian.
// The frame loop reads and writes it with stdio, at startup and at shutdown: it is a
// developer's file, outside the run's I/O.
#include "frameloop_internal.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/platform_hid/actions.h"
#include <stdio.h>
#include <string.h>

namespace kv {

namespace {

constexpr u32 kHashEvery = 60;
constexpr u32 kMaxArgs = 64, kMaxRecorded = 16;
enum : u8 { kRecAction = 1, kRecHash = 2, kRecEnd = 3 };

struct ActionValue { bool down; vec2 axis; };

struct Replay {
    const char* record_path = nullptr;
    const char* replay_path = nullptr;
    char        exe[128] = {};
    // The game's arguments: the command line's, the note's (record); the file's (replay).
    char        args[kMaxArgs][96] = {};
    u32         arg_count = 0, cli_args = 0;
    // The actions, by the config's names; ids into the action map once it has them.
    u32         ids[kMaxRecorded] = {};
    u32         action_count = 0;
    bool        resolved = false;
    ActionValue last[kMaxRecorded] = {};
    // The records: being written, or read.
    u8*         data = nullptr;
    usize       len = 0, cap = 0, at = 0;
    u64         last_tick = 0, next_tick = 0;
    bool        recording = false, playing = false, ended = false, diverged = false;
    u64         hashes_checked = 0;
    // Recording: the hash of a hash tick, taken where a replay compares it (before the sample).
    u64         hash = 0, hash_tick = ~0ull;
};
Replay g_rep;

void put(const void* p, usize n) {
    if (g_rep.len + n > g_rep.cap) {
        usize cap = g_rep.cap ? g_rep.cap * 2 : 64 * 1024;
        while (cap < g_rep.len + n) cap *= 2;
        u8* grown = (u8*)alloc_in(mem_general(), cap, 8);
        if (g_rep.len) memcpy(grown, g_rep.data, g_rep.len);
        free_in(mem_general(), g_rep.data, g_rep.cap);
        g_rep.data = grown;
        g_rep.cap = cap;
    }
    memcpy(g_rep.data + g_rep.len, p, n);
    g_rep.len += n;
}

void put_record(u64 tick, u8 type) {
    u64 d = tick - g_rep.last_tick;
    g_rep.last_tick = tick;
    do {
        u8 b = (u8)(d & 0x7f);
        d >>= 7;
        if (d) b |= 0x80;
        put(&b, 1);
    } while (d);
    put(&type, 1);
}

bool get(void* p, usize n) {
    if (g_rep.at + n > g_rep.len) return false;
    memcpy(p, g_rep.data + g_rep.at, n);
    g_rep.at += n;
    return true;
}

// The next record's tick, read ahead; false at the end of the data.
bool peek_tick() {
    u64 d = 0;
    for (u32 shift = 0; shift < 64; shift += 7) {
        u8 b;
        if (!get(&b, 1)) return false;
        d |= (u64)(b & 0x7f) << shift;
        if (!(b & 0x80)) break;
    }
    g_rep.next_tick = g_rep.last_tick + d;
    return true;
}

bool resolve(Engine& e) {
    if (g_rep.resolved) return true;
    const InputActions* a = input_actions_get(e);
    Span<const char* const> names = frameloop_config(e).recorded_actions;
    if (!a || !names.len) return false;
    g_rep.action_count = names.len < kMaxRecorded ? names.len : kMaxRecorded;
    for (u32 i = 0; i < g_rep.action_count; ++i) {
        g_rep.ids[i] = a->find(names[i]);
        if (g_rep.ids[i] == KV_ACTION_NONE) KV_WARN("frameloop", "recorded action '%s' is not in the action map", names[i]);
    }
    g_rep.resolved = true;
    return true;
}

} // namespace

bool replay_arg(int& i, int argc, char** argv) {
    if (strcmp(argv[i], "--record") == 0 && i + 1 < argc) { g_rep.record_path = argv[++i]; return true; }
    if (strcmp(argv[i], "--replay") == 0 && i + 1 < argc) { g_rep.replay_path = argv[++i]; return true; }
    return false;
}

void replay_keep_arg(const char* arg) {
    if (g_rep.arg_count < kMaxArgs) snprintf(g_rep.args[g_rep.arg_count++], sizeof g_rep.args[0], "%s", arg);
    g_rep.cli_args = g_rep.arg_count;
}

bool replay_open(const char* exe, bool (*apply)(const char* arg, const char* value)) {
    snprintf(g_rep.exe, sizeof g_rep.exe, "%s", exe);
    if (g_rep.record_path && g_rep.replay_path) { KV_ERROR("frameloop", "--record and --replay: one at a time"); return false; }
    g_rep.recording = g_rep.record_path != nullptr;
    if (!g_rep.replay_path) return true;
    FILE* f = fopen(g_rep.replay_path, "rb");
    if (!f) { KV_ERROR("frameloop", "--replay %s: cannot open it", g_rep.replay_path); return false; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_rep.cap = size > 0 ? (usize)size : 1;
    g_rep.data = (u8*)alloc_in(mem_general(), g_rep.cap, 8);
    g_rep.len = fread(g_rep.data, 1, (usize)(size > 0 ? size : 0), f);
    fclose(f);
    char magic[4];
    u32 header = 0;
    if (!get(magic, 4) || memcmp(magic, "KVR1", 4) || !get(&header, 4) || g_rep.at + header > g_rep.len) {
        KV_ERROR("frameloop", "--replay %s: not a recording", g_rep.replay_path);
        return false;
    }
    // The header, a line at a time.
    const char* p = (const char*)g_rep.data + g_rep.at;
    const char* end = p + header;
    g_rep.at += header;
    char exe_in_file[128] = {};
    while (p < end) {
        const char* nl = (const char*)memchr(p, '\n', (usize)(end - p));
        usize n = nl ? (usize)(nl - p) : (usize)(end - p);
        char line[128];
        snprintf(line, sizeof line, "%.*s", (int)(n < sizeof line - 1 ? n : sizeof line - 1), p);
        if (!strncmp(line, "exe ", 4)) snprintf(exe_in_file, sizeof exe_in_file, "%s", line + 4);
        else if (!strncmp(line, "arg ", 4) && g_rep.arg_count < kMaxArgs) snprintf(g_rep.args[g_rep.arg_count++], sizeof g_rep.args[0], "%.95s", line + 4);
        p += n + 1;
    }
    if (strcmp(exe_in_file, exe)) {
        KV_ERROR("frameloop", "--replay %s: a recording of %s, not %s", g_rep.replay_path, exe_in_file, exe);
        return false;
    }
    for (u32 i = 0; i < g_rep.arg_count; ++i) {
        const char* value = i + 1 < g_rep.arg_count ? g_rep.args[i + 1] : nullptr;
        if (apply(g_rep.args[i], value)) ++i; // it took the value
    }
    g_rep.playing = true;
    g_rep.last_tick = 0;
    if (!peek_tick()) g_rep.ended = true;
    KV_INFO("frameloop", "replay %s: %u game arguments, %u bytes of input", g_rep.replay_path, g_rep.arg_count, (u32)(g_rep.len - g_rep.at));
    return true;
}

bool frameloop_replaying(Engine&) { return g_rep.playing && !g_rep.ended; }

void frameloop_record_note(Engine&, const char* args) {
    if (!g_rep.recording) return;
    g_rep.arg_count = g_rep.cli_args; // the note replaces the last one
    const char* p = args ? args : "";
    while (*p && g_rep.arg_count < kMaxArgs) {
        while (*p == ' ') ++p;
        const char* q = p;
        while (*q && *q != ' ') ++q;
        if (q > p) snprintf(g_rep.args[g_rep.arg_count++], sizeof g_rep.args[0], "%.*s", (int)(q - p), p);
        p = q;
    }
}

// Phase Input, sim ticks only, after the sample: what the named actions are this tick, as
// changes; the hash every 60 ticks.
void replay_record(TickContext& ctx) {
    if (!g_rep.recording || !resolve(*ctx.engine)) return;
    const InputActions& a = *ctx.resource<const InputActions>("input.actions");
    u64 t = ctx.tick;
    if (t % kHashEvery == 0) {
        // The state after the last tick, as a replay sees it: hashed before the sample (by
        // replay_play), not here, since the input phase's event handlers run in between and
        // can change it on this very tick (a boss part going down).
        u64 h = g_rep.hash_tick == t ? g_rep.hash : engine_hash_state(*ctx.engine);
        put_record(t, kRecHash);
        put(&h, 8);
    }
    for (u32 i = 0; i < g_rep.action_count; ++i) {
        if (g_rep.ids[i] == KV_ACTION_NONE) continue;
        const ActionState& s = a.actions[g_rep.ids[i]];
        ActionValue v{s.down, s.kind == ActionKind::Axis2 ? s.axis : vec2{0, 0}};
        ActionValue& was = g_rep.last[i];
        if (v.down == was.down && v.axis.x == was.axis.x && v.axis.y == was.axis.y && t) continue;
        was = v;
        put_record(t, kRecAction);
        u8 idx = (u8)i, down = v.down ? 1 : 0;
        put(&idx, 1);
        put(&down, 1);
        put(&v.axis.x, 4);
        put(&v.axis.y, 4);
    }
}

// Phase Input, sim ticks only, before the sample: the file's actions for this tick; the
// hashes compared where it has them. Recording, the hash of a hash tick is taken here, so
// the recording holds what a replay will compare.
void replay_play(TickContext& ctx) {
    if (g_rep.recording && ctx.tick % kHashEvery == 0) {
        g_rep.hash = engine_hash_state(*ctx.engine);
        g_rep.hash_tick = ctx.tick;
    }
    if (!g_rep.playing || g_rep.ended || !resolve(*ctx.engine)) return;
    Engine& e = *ctx.engine;
    u64 t = ctx.tick;
    while (!g_rep.ended && g_rep.next_tick <= t) {
        g_rep.last_tick = g_rep.next_tick;
        u8 type = 0;
        if (!get(&type, 1)) { g_rep.ended = true; break; }
        if (type == kRecAction) {
            u8 idx = 0, down = 0;
            vec2 axis{0, 0};
            if (!get(&idx, 1) || !get(&down, 1) || !get(&axis.x, 4) || !get(&axis.y, 4)) { g_rep.ended = true; break; }
            if (idx < g_rep.action_count) g_rep.last[idx] = ActionValue{down != 0, axis};
        } else if (type == kRecHash) {
            u64 want = 0;
            if (!get(&want, 8)) { g_rep.ended = true; break; }
            u64 have = engine_hash_state(e);
            ++g_rep.hashes_checked;
            if (have != want && !g_rep.diverged) {
                g_rep.diverged = true;
                KV_ERROR("frameloop", "replay: diverged at tick %llu: %016llx, the recording %016llx", (unsigned long long)t, (unsigned long long)have,
                         (unsigned long long)want);
            }
        } else {
            g_rep.ended = true;
            KV_INFO("frameloop", "replay: ended at tick %llu, %s (%llu hashes)", (unsigned long long)t, g_rep.diverged ? "DIVERGED" : "matched",
                    (unsigned long long)g_rep.hashes_checked);
            break;
        }
        if (!peek_tick()) { g_rep.ended = true; break; }
    }
    if (g_rep.ended) return;
    Span<const char* const> names = frameloop_config(e).recorded_actions;
    for (u32 i = 0; i < g_rep.action_count; ++i)
        if (g_rep.ids[i] != KV_ACTION_NONE) input_action_override_exact(e, names[i], g_rep.last[i].down, g_rep.last[i].axis);
}

// A fresh sim: the recording is of the run that begins now.
void replay_on_sim_reset(TickContext&, Span<const SimReset>) {
    if (!g_rep.recording) return;
    g_rep.len = 0;
    g_rep.last_tick = 0;
    for (ActionValue& v : g_rep.last) v = ActionValue{};
}

void replay_finish(Engine& e) {
    if (g_rep.recording) {
        put_record(e.clock.tick_count > g_rep.last_tick ? e.clock.tick_count : g_rep.last_tick, kRecEnd);
        char header[kMaxArgs * 104 + 2048];
        usize n = (usize)snprintf(header, sizeof header, "exe %s\n", g_rep.exe);
        for (u32 i = 0; i < g_rep.arg_count && n < sizeof header; ++i) n += (usize)snprintf(header + n, sizeof header - n, "arg %s\n", g_rep.args[i]);
        Span<const char* const> names = frameloop_config(e).recorded_actions;
        for (u32 i = 0; i < names.len && n < sizeof header; ++i) n += (usize)snprintf(header + n, sizeof header - n, "action %s\n", names[i]);
        if (n > sizeof header) n = sizeof header;
        FILE* f = fopen(g_rep.record_path, "wb");
        u32 hl = (u32)n;
        bool ok = f && fwrite("KVR1", 1, 4, f) == 4 && fwrite(&hl, 4, 1, f) == 1 && fwrite(header, 1, n, f) == n &&
                  (g_rep.len == 0 || fwrite(g_rep.data, 1, g_rep.len, f) == g_rep.len);
        if (f) fclose(f);
        if (ok) KV_INFO("frameloop", "record: %s, %u ticks, %u bytes", g_rep.record_path, (u32)e.clock.tick_count, (u32)(g_rep.len + n + 8));
        else KV_ERROR("frameloop", "record: could not write %s", g_rep.record_path);
    } else if (g_rep.playing && !g_rep.ended) {
        // A run that ends itself quits on its last tick, before the end record is read.
        bool at_end = g_rep.at >= g_rep.len || g_rep.data[g_rep.at] == kRecEnd;
        if (at_end)
            KV_INFO("frameloop", "replay: ended at tick %llu, %s (%llu hashes)", (unsigned long long)e.clock.tick_count, g_rep.diverged ? "DIVERGED" : "matched",
                    (unsigned long long)g_rep.hashes_checked);
        else
            KV_INFO("frameloop", "replay: stopped at tick %llu before the recording's end (%llu), %s so far (%llu hashes)", (unsigned long long)e.clock.tick_count,
                    (unsigned long long)g_rep.next_tick, g_rep.diverged ? "DIVERGED" : "matched", (unsigned long long)g_rep.hashes_checked);
    }
    free_in(mem_general(), g_rep.data, g_rep.cap);
    g_rep.data = nullptr;
    g_rep.len = g_rep.cap = 0;
}

} // namespace kv
