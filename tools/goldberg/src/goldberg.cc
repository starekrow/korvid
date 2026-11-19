// goldberg: the host. Everything the Lua application cannot do for itself: the pools its
// records live in, the sources that feed the event queue (the scan, the watchers, what
// its I/O finished), and the pump that hands each tick's events to Lua in order.
//
// Hashing, copying and converter runs are I/O: tasks on an IoQueue of goldberg's own, in
// three lanes (runs, hashes, copies), each with its own limit. They run on the queue's
// workers and a worker that finishes takes the next at once, so the converters stay busy
// however often the pump runs; the pump only turns what finished into events. What an event means, and everything that follows from it, is Lua's
// (lua/main.lua and what it requires).
//
// One system, goldberg.pump, each tick:
//   1. a command the CLI left is handed to Lua (commands never touch the pools: the CLI
//      runs them in its own system, so they leave a request and the pump carries it out);
//   2. the sources are polled into the pending list: watcher events, what the queue
//      finished (hashes, copies, runs), the scan's files;
//   3. pending events are folded (one per path) and moved into goldberg.event rows;
//   4. the rows go to Lua in sequence order, and are destroyed, as many as the session's
//      share of the tick allows (all of them, in a cook);
//   5. Lua is told whether the host has anything left (goldberg.tick), and decides
//      whether the session is stable.
#include "korvid/goldberg/module.h"
#include "korvid/core/array.h"
#include "korvid/core/cli.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/glob.h"
#include "korvid/core/thread.h"
#include "korvid/core/log.h"
#include "korvid/core/sha256.h"
#include "korvid/core/sort.h"
#include "korvid/core/name.h"
#include "korvid/core/profile.h"
#include "korvid/core/time.h"
#include "korvid/kim/kim.h"
#include "korvid/svg/svg.h"
#include "korvid/kimb/kimb.h"
#include "korvid/core/arena.h"
#include "korvid/platform/fs_watch.h"
#include "korvid/platform/io.h"
#include "korvid/platform/net.h"
#include "korvid/core/json.h"
#include "korvid/core/kbor.h"
#include "korvid/core/phase.h"
#include "korvid/frameloop/frameloop.h"
#include "korvid/krome/krome.h"
#include "korvid/platform_hid/events.h"
#include "korvid/platform_hid/module.h"
#include "korvid/platform_hid/host_window.h"
#include "korvid/render/renderer.h"
#include "korvid/cli/module.h"
#include "korvid/platform/process.h"
#include "korvid/script/script.h"
#include "korvid/tool/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace kv {

constexpr u32 kGbPath = 200;
constexpr f64 kTick = 1.0 / 60.0; // the tick a session's share is of: a frame at 60 Hz

int  goldberg_lock(const char* path, const char* who, char* holder, usize cap, i64* handle); // lock.cc
void goldberg_unlock(i64 handle);

// ---- the pools ---------------------------------------------------------------------------

// One thing that happened, waiting for Lua. The kind is a GbEv.
struct GbEvent {
    u32  seq;
    u8   kind;
    u32  root;
    char path[kGbPath];
    u64  size;
    i64  mtime;
    u8   hash[32];
    u32  text; // 1 + index into the state's texts, when the event carries a file's text
};
// A file under a root that goldberg knows of.
struct GbFile {
    char path[kGbPath];
    u32  root;
    u64  size;
    i64  mtime;
    u8   hash[32];
    bool hashed;
    bool is_berg;
    u32  scan;   // the scan that last saw it: what a later scan missed is gone
};
// A rule file, as last read.
struct GbBerg {
    char path[kGbPath];
    u32  root;
    u32  version; // +1 at every change that was read
    bool ok;      // it parsed
};

// One converter run, wanted or done: what it reads, what it writes, and the key of what
// it was last made from. The state is a GbStepState.
struct GbStep {
    char output[kGbPath + 64]; // relative to the output tree
    char source[kGbPath];
    u32  root;
    u8   key[32];
    u8   state;
};

namespace {

// file-made: a converter's output, moved into the work root and hashed on its way in: a
// file seen and hashed, in one event.
enum GbEv : u8 { EvSeen, EvAdded, EvChanged, EvRemoved, EvDirAdded, EvDirRemoved, EvRescan, EvScanDone, EvHashed, EvHashFailed, EvCookDone, EvCookFailed, EvStepExit, EvMade, EvCount };
constexpr const char* kEvNames[EvCount] = {"file-seen", "file-added", "file-changed", "file-removed", "dir-added", "dir-removed",
                                           "rescan", "scan-done", "hashed", "hash-failed", "cook-done", "cook-failed", "step-exit", "file-made"};

constinit const FieldDesc k_event_fields[] = {KV_FIELDS(GbEvent, seq, kind, root, path, size, mtime, hash, text)};
constinit const FieldDesc k_file_fields[]  = {KV_FIELDS(GbFile, path, root, size, mtime, hash, hashed, is_berg, scan)};
constinit const FieldDesc k_berg_fields[]  = {KV_FIELDS(GbBerg, path, root, version, ok)};
constinit const FieldDesc k_step_fields[]  = {KV_FIELDS(GbStep, output, source, root, key, state)};
constinit const ComponentDesc k_components[] = {
    KV_COMPONENT(GbEvent, k_event_fields),
    KV_COMPONENT(GbFile, k_file_fields),
    KV_COMPONENT(GbBerg, k_berg_fields),
    KV_COMPONENT(GbStep, k_step_fields),
};
// Tools have no simulation to hash: the pools are for inspection (ls /kinds/goldberg) and
// for memory that is laid out, not for determinism.
constinit const KindDesc k_kinds[] = {
    {.name = ".event", .components = "GbEvent", .capacity = 16384, .cosmetic = true},
    {.name = ".file",  .components = "GbFile",  .capacity = 262144, .cosmetic = true},
    {.name = ".berg",  .components = "GbBerg",  .capacity = 16384, .cosmetic = true},
    {.name = ".step",  .components = "GbStep",  .capacity = 262144, .cosmetic = true},
};

struct Pending {
    u8   kind;
    u32  root;
    u32  path;   // offset into pending_text
    u64  size;
    i64  mtime;
    u8   hash[32];
    char* text;  // a file's text (mem_alloc), or null
    u32  text_len;
    bool dead;   // folded into a later event for the same path
};

struct Root {
    char name[64];
    char dir[512];
    u32  watch;  // the watcher's id, or ~0u
    bool watched; // false for the output tree: scanned, never watched
};

// Every request handed to the queue starts with this: what it is, and where it sits in the
// list of the ones not yet finished (shutdown frees what never finished).
enum TaskKind : u8 { TaskHash, TaskCopy, TaskRun };
enum Lane : u8 { LaneRun, LaneHash, LaneCopy };
struct TaskHead { u8 kind; u32 live_at; };

struct HashReq {
    TaskHead head{TaskHash, 0};
    u32   root;
    char  path[kGbPath];
    char  full[800];
    bool  want_text;
    bool  ok;
    u64   size;
    i64   mtime;
    u8    hash[32];
    char* text;
    u32   text_len;
};

// One copy: a source under a root to a path under the output tree, written whole.
struct CopyReq {
    TaskHead head{TaskCopy, 0};
    u32  root;
    char source[kGbPath];
    char output[kGbPath + 64];
    char from[800], to[900], tmp[960];
    bool ok;
    u64  size;
    i64  mtime;
    u8   hash[32];
    char error[200];
};

struct ScanItem { u32 root; u32 path; u64 size; i64 mtime; bool end; };

// What a scanning step's directory held when its run began: a file still exactly so when
// the run ends was not written by it.
struct Before { u32 path; u64 size; i64 mtime; };

// One converter run: a child process, where it writes, and what happens when it ends.
//   declared outputs (scan false): it writes into run_dir, which nobody watches. At a
//     clean exit the files are moved over the step's directory in the work root, what
//     the last run left there and this one did not write is removed, and each is an
//     event. A failed run's directory is thrown away: it contributes nothing.
//   scanned outputs (scan true): it writes into the step's directory itself, which is
//     watched; its files are held until they have been quiet for `settle` and then
//     delivered while it still runs.
// Either way the last event is step-exit, for the source it ran on.
// What a run left in the work root, for its events: a file and what it was hashed as.
struct Made { u32 path; u64 size; i64 mtime; u8 hash[32]; };

struct Proc {
    TaskHead    head{TaskRun, 0};
    Process     p;
    bool        spawn_failed = false;
    i32         code = 0;
    u32         root = 0;               // the source's
    char        path[kGbPath] = {};
    u32         work_root = 0;
    char        dir[kGbPath] = {};      // the step's directory, relative to the work root
    char        run_dir[900] = {};      // where it writes: absolute
    bool        scan = false;
    f64         settle = 0;
    f64         exited_at = -1;
    char        builtin[16] = {};       // a built-in converter ("kim"), run in process; empty: a child process
    Array<char> roots_text;             // a built-in's view of the asset roots: name, dir, NUL after each
    Array<char> words;                  // argv then env, NUL separated
    u32         argc = 0, envc = 0;
    Array<char> err;                    // what it wrote to stderr (the tail is kept)
    Array<Before> before;
    Array<char>   before_text;
    char          step_dir[900] = {};   // the step's directory in the work root: absolute
    // A declared run's results, made by its task: paths relative to the work root.
    Array<Made>   made;
    Array<u32>    gone;                 // last run's files this one did not write: removed
    Array<char>   made_text;
};

// A directory whose watcher events wait until their file has been quiet a while.
struct Hold { u32 root; char prefix[kGbPath]; f64 settle; };
struct Held { u8 kind; u32 root; char path[kGbPath]; f64 due; };

struct GbText { char* data; u32 len; };

struct GoldbergState {
    Allocator*      general = nullptr;
    lua_State*      L = nullptr;
    bool            loaded = false;      // the Lua application is in
    bool            failed = false;      // it raised: the run ends
    char            lua_dir[600] = {};
    // A command the CLI left for the pump.
    Array<char>     command;             // words, NUL separated
    u32             command_words = 0;
    bool            active = false;      // a session is running: tick Lua
    // Sources.
    Array<Root>     roots;
    FsWatcher       watcher;
    IoQueue         io;                  // hashes, copies and runs, as tasks
    bool            io_ready = false;
    Array<TaskHead*> live;               // handed to the queue, not yet finished
    u32             runs = 0;            // of those, converter runs
    u32             max_procs = 8;       // converters running at once (startup: one a CPU, at least 8)
    // The session's share of a tick (1/60 s) for its events: 1 is all of them every tick
    // (a cook, or a watch asked to cook); less leaves the rest for the next tick, and a
    // headless session sleeps out the remainder.
    f64             share = 1.0;
    Array<Hold>     holds;
    Array<Held>     held;
    i64             lock = -1;           // the lock on the output tree's state, while held
    f64             watch_floor = 0;     // the least time a held file waits: over the polling watcher's interval
    bool            watch_sync = false;  // host.watch_sync: the watcher heard out before a run's exit is delivered
    char            out_dir[600] = {};   // the output tree
    char            attic[700] = {};     // where what leaves it goes: <out>/.goldberg.attic/<stamp>
    u32             tmp_serial = 0;
    Array<ScanItem> scan;                // files a scan found, not yet events
    Array<char>     scan_text;
    u32             scan_at = 0;
    // The queue.
    Array<Pending>  pending;
    Array<char>     pending_text;
    Array<GbText>   texts;               // texts of the events now in rows
    u32             seq = 0;
    Table*          events = nullptr;
    // Programs connected to a watch (docs/PRDs/Krome-Slice-6-PRD.md, "A program's connection to
    // goldberg"): told `changed PATH` after each output lands, `removed PATH` after one leaves.
    struct Program {
        NetSocket sock;
        char      name[64] = "?", config[16] = "";
        char      line[256] = {};
        u32       line_len = 0;
        bool      hello = false;
    };
    NetListener     listener;
    Array<Program*> programs;
    char            port_file[700] = {}; // <out>/.goldberg-port, while the listener is open
    u32             told = 0;            // lines sent to programs
    // The window (--window): the session in the Work cycle, a Krome screen over it.
    bool            window = false;
    bool            quiet = true;        // the last pump found nothing left to do
    f64             next_wake = 0.0;     // when the waker next says so anyway, while not quiet
    Name            screen;              // the window's Krome screen
    u64             ui_sets = 0;         // models sent to it
    // goldberg's own UI, watched while the window is up (a headless child watch on goldberg's
    // roots, unless the window's own session is that watch): saving window.kim reloads it.
    Process         ui_child;
    bool            ui_child_on = false;
    char            ui_tree[600] = {};   // goldberg's own output tree (<exe>.assets)
    bool            ui_checked = false;
};

// The watch of goldberg's own UI (below, with the window's systems).
void drain_ui_child(GoldbergState& s);
void stop_own_ui(GoldbergState& s);

// The command line of a windowed run, as one CLI request; set by goldberg_window_main.
char g_window_line[2048] = {};
bool g_window_mode = false;
// --demo: the pilot drives each page by pointer and keys and goldberg's Lua checks what each
// shows against the session; --shots DIR keeps a picture of each page.
bool        g_demo = false;
const char* g_shots = nullptr;
u32         g_demo_failures = 0;

// Every program that said hello hears the line.
void tell_programs(GoldbergState& s, const char* verb, const char* output) {
    if (!s.programs.len) return;
    char line[700];
    format(line, sizeof line, "%s %s\n", verb, output);
    for (GoldbergState::Program* p : s.programs)
        if (p->hello && p->sock.is_open()) { p->sock.write_str(line); ++s.told; }
}

// New connections taken, hellos read, what was told sent, the gone let go.
void serve_programs(GoldbergState& s) {
    if (!s.listener.is_open()) return;
    for (NetFd fd = s.listener.accept(); fd >= 0; fd = s.listener.accept()) {
        GoldbergState::Program* p = (GoldbergState::Program*)mem_alloc(sizeof(GoldbergState::Program));
        *p = GoldbergState::Program{};
        p->sock.adopt(fd);
        s.programs.alloc = s.general;
        s.programs.push(p);
    }
    for (u32 i = 0; i < s.programs.len;) {
        GoldbergState::Program& p = *s.programs[i];
        char buf[512];
        for (i32 n; (n = p.sock.read(buf, sizeof buf)) > 0;)
            for (i32 k = 0; k < n; ++k) {
                if (buf[k] != '\n') { if (p.line_len + 1 < sizeof p.line) p.line[p.line_len++] = buf[k]; continue; }
                p.line[p.line_len] = 0;
                if (p.line_len && p.line[p.line_len - 1] == '\r') p.line[p.line_len - 1] = 0;
                p.line_len = 0;
                char name[64] = {}, config[16] = {};
                if (sscanf(p.line, "hello %63s %15s", name, config) >= 1) {
                    format(p.name, sizeof p.name, "%s", name);
                    format(p.config, sizeof p.config, "%s", config);
                    if (!p.hello) { print("goldberg: %s (%s) connected: it reloads what lands\n", p.name, p.config[0] ? p.config : "?"); print_flush(); }
                    p.hello = true;
                    p.sock.write_str("welcome\n");
                }
            }
        p.sock.flush();
        if (!p.sock.is_open()) {
            if (p.hello) { print("goldberg: %s went away\n", p.name); print_flush(); }
            p.sock.close();
            p.sock.out.free();
            mem_free(&p);
            s.programs[i] = s.programs[s.programs.len - 1];
            --s.programs.len;
            continue;
        }
        ++i;
    }
}

void close_programs(GoldbergState& s) {
    for (GoldbergState::Program* p : s.programs) { p->sock.close(); p->sock.out.free(); mem_free(p); }
    s.programs.free();
    s.listener.close();
    if (s.port_file[0]) { io_remove(s.port_file); s.port_file[0] = 0; }
}

GoldbergState* state_of(Engine& e) { return engine_module_state_as<GoldbergState>(e, "goldberg"); }
GoldbergState* state_of(lua_State* L) { return (GoldbergState*)lua_touserdata(L, lua_upvalueindex(1)); }

void push_pending(GoldbergState& s, u8 kind, u32 root, const char* path, u64 size = 0, i64 mtime = 0, const u8* hash = nullptr, char* text = nullptr, u32 text_len = 0) {
    Pending p{};
    p.kind = kind;
    p.root = root;
    p.path = s.pending_text.len;
    u32 n = (u32)strlen(path);
    memcpy(s.pending_text.push_n(n + 1), path, n + 1);
    p.size = size;
    p.mtime = mtime;
    if (hash) memcpy(p.hash, hash, 32);
    p.text = text;
    p.text_len = text_len;
    s.pending.push(p);
}

// ---- what is handed to the queue ----------------------------------------------------------------

void live_add(GoldbergState& s, TaskHead* h) { h->live_at = s.live.len; s.live.push(h); }
void live_remove(GoldbergState& s, TaskHead* h) {
    TaskHead* last = s.live.data[s.live.len - 1];
    s.live.data[h->live_at] = last;
    last->live_at = h->live_at;
    --s.live.len;
}

bool submit(GoldbergState& s, TaskHead* h, IoTaskFn fn, u8 lane, const char* name) {
    IoTaskRequest r;
    r.fn = fn;
    r.arg = h;
    r.name = name;
    r.lane = lane;
    r.user = (u64)(uintptr_t)h;
    if (s.io.run_task(r).is_null()) return false;
    live_add(s, h);
    return true;
}

void parent_dir(const char* path, char* out, usize cap) {
    format(out, cap, "%s", path);
    char* slash = strrchr(out, '/');
    if (slash) *slash = 0; else out[0] = 0;
}

// ---- hashing: a task each, the file streamed ------------------------------------------------------

struct Sum { Sha256 h; u64 n; };
bool sum_piece(void* user, const u8* data, usize n) { Sum& m = *(Sum*)user; m.h.update(data, n); m.n += n; return true; }

i32 hash_task(void* arg, const IoTaskCtl&) {
    HashReq& r = *(HashReq*)arg;
    KV_ZONE("task.hash");
    // The modification time is read before the bytes: a write in between leaves a time
    // that is older than the content, and the next event for the file asks again.
    IoStat st;
    r.mtime = io_stat(r.full, &st) ? st.mtime_ns : 0;
    if (r.want_text) { // a rule file: small, and wanted whole
        IoBytes bytes;
        r.ok = io_read_now(r.full, &bytes);
        if (!r.ok) return 0;
        sha256(bytes.data, bytes.size, r.hash);
        r.size = bytes.size;
        r.text = (char*)mem_alloc(bytes.size + 1);
        memcpy(r.text, bytes.data, bytes.size);
        r.text[bytes.size] = 0;
        r.text_len = (u32)bytes.size;
        bytes.free();
        return 0;
    }
    Sum m;
    m.h.init();
    m.n = 0;
    r.ok = io_read_each(r.full, sum_piece, &m);
    if (r.ok) { m.h.finish(r.hash); r.size = m.n; }
    return 0;
}

void hash_done(GoldbergState& s, HashReq& r) {
    if (r.ok) push_pending(s, EvHashed, r.root, r.path, r.size, r.mtime, r.hash, r.text, r.text_len);
    else push_pending(s, EvHashFailed, r.root, r.path);
    r.text = nullptr; // the event has it now
}

// ---- copying: a task each, streamed, whole or not at all --------------------------------------------

i32 copy_task(void* arg, const IoTaskCtl&) {
    CopyReq& r = *(CopyReq*)arg;
    KV_ZONE("task.copy");
    char dir[900];
    i32 err = 0;
    parent_dir(r.to, dir, sizeof dir);
    if (dir[0] && !io_make_dirs(dir)) { format(r.error, sizeof r.error, "cannot make %s", dir); return 0; }
    if (!io_copy_now(r.from, r.tmp, r.hash, &err)) {
        format(r.error, sizeof r.error, "cannot copy %s to %s (errno %d)", r.from, r.tmp, err);
        io_remove(r.tmp);
        return 0;
    }
    // Whole, or not at all: a reader of the output tree never sees half a file.
    if (!io_rename(r.tmp, r.to)) { format(r.error, sizeof r.error, "cannot rename %s into place", r.tmp); io_remove(r.tmp); return 0; }
    IoStat st;
    if (io_stat(r.to, &st)) { r.size = st.size; r.mtime = st.mtime_ns; }
    r.ok = true;
    return 0;
}

void copy_done(GoldbergState& s, CopyReq& r) {
    if (r.ok) tell_programs(s, "changed", r.output); // in place, renamed whole
    if (r.ok) push_pending(s, EvCookDone, r.root, r.output, r.size, r.mtime, r.hash);
    else {
        u32 n = (u32)strlen(r.error);
        char* text = (char*)mem_alloc(n + 1);
        memcpy(text, r.error, n + 1);
        push_pending(s, EvCookFailed, r.root, r.output, 0, 0, nullptr, text, n);
    }
}

// ---- the scan ----------------------------------------------------------------------------------

struct ScanCtx { GoldbergState* s; u32 root; };
// A file the walk found, with its size and time from the listing: nothing asked of it.
bool scan_one(void* user, const char* relative, const IoStat& st) {
    ScanCtx* c = (ScanCtx*)user;
    GoldbergState& s = *c->s;
    ScanItem it{};
    it.root  = c->root;
    it.path  = s.scan_text.len;
    it.size  = st.size;
    it.mtime = st.mtime_ns;
    u32 n = (u32)strlen(relative);
    memcpy(s.scan_text.push_n(n + 1), relative, n + 1);
    s.scan.push(it);
    return true;
}

void scan_root(GoldbergState& s, u32 root) {
    KV_ZONE("goldberg.scan");
    ScanCtx c{&s, root};
    io_list_files_stat(s.roots.data[root].dir, scan_one, &c); // hidden files left out, as the copy left them
    ScanItem end{};
    end.root = root;
    end.path = s.scan_text.len;
    end.end  = true;
    s.scan_text.push(0);
    s.scan.push(end);
}

u8 fs_kind(FsEventKind k) {
    return k == FsEventKind::Added ? EvAdded : k == FsEventKind::Changed ? EvChanged : k == FsEventKind::Removed ? EvRemoved
         : k == FsEventKind::DirAdded ? EvDirAdded : k == FsEventKind::DirRemoved ? EvDirRemoved : EvRescan;
}

bool is_fs_event_held(GoldbergState& s, u8 kind, u32 root, const char* path) {
    if (kind != EvAdded && kind != EvChanged && kind != EvRemoved) return false;
    for (const Hold& hold : s.holds) {
        if (hold.root != root || strncmp(path, hold.prefix, strlen(hold.prefix)) != 0) continue;
        // A watcher that walks cannot tell quiet from not yet looked: with it, a file is
        // not called quiet before the next walk has had its say.
        f64 due = time_now() + (hold.settle > s.watch_floor ? hold.settle : s.watch_floor);
        for (Held& h : s.held) {
            if (h.root != root || strcmp(h.path, path) != 0) continue;
            if (kind == EvRemoved) h.kind = EvRemoved;
            else if (h.kind == EvRemoved) h.kind = EvAdded;
            h.due = due;
            return true;
        }
        Held h{};
        h.kind = kind;
        h.root = root;
        format(h.path, sizeof h.path, "%s", path);
        h.due = due;
        s.held.push(h);
        return true;
    }
    return false;
}

// ---- converter runs --------------------------------------------------------------------------------

struct ListCtx { Array<char>* text; Array<u32>* at; };
bool list_one(void* user, const char* relative) {
    ListCtx* c = (ListCtx*)user;
    c->at->push(c->text->len);
    u32 n = (u32)strlen(relative);
    memcpy(c->text->push_n(n + 1), relative, n + 1);
    return true;
}

void proc_free(Proc* pr) {
    pr->p.free();
    pr->words.free();
    pr->err.free();
    pr->before.free();
    pr->before_text.free();
    pr->made.free();
    pr->gone.free();
    pr->made_text.free();
    pr->roots_text.free();
    mem_free(pr);
}

void step_dir_of(GoldbergState& s, const Proc& pr, char* out, usize cap) {
    format(out, cap, "%s/%s", s.roots.data[pr.work_root].dir, pr.dir);
}

void push_exit(GoldbergState& s, const Proc& pr, i64 code, const char* text, u32 len) {
    char* copy = nullptr;
    if (len) {
        copy = (char*)mem_alloc(len + 1);
        memcpy(copy, text, len);
        copy[len] = 0;
    }
    push_pending(s, EvStepExit, pr.root, pr.path, 0, code, nullptr, copy, len);
}

// What a run needs from the main thread before its task goes: a scanning run's hold, and
// what its step's directory held when it began (a file still exactly so when the run ends
// was not written by it).
void proc_prepare(GoldbergState& s, Proc& pr) {
    step_dir_of(s, pr, pr.step_dir, sizeof pr.step_dir);
    if (!pr.scan) return;
    io_make_dirs(pr.step_dir);
    ListCtx c{&pr.before_text, nullptr};
    Array<u32> at{};
    c.at = &at;
    io_list_files(pr.step_dir, list_one, &c);
    for (u32 off : at) {
        char full[1200];
        format(full, sizeof full, "%s/%s", pr.step_dir, pr.before_text.data + off);
        IoStat st;
        if (io_stat(full, &st)) pr.before.push(Before{off, st.size, st.mtime_ns});
    }
    at.free();
    Hold h{};
    h.root = pr.work_root;
    format(h.prefix, sizeof h.prefix, "%s/", pr.dir);
    h.settle = pr.settle;
    s.holds.push(h);
}

void drop_hold(GoldbergState& s, const Proc& pr) {
    char prefix[kGbPath];
    format(prefix, sizeof prefix, "%s/", pr.dir);
    for (u32 i = 0; i < s.holds.len; ++i) {
        if (s.holds.data[i].root != pr.work_root || strcmp(s.holds.data[i].prefix, prefix) != 0) continue;
        s.holds.data[i] = s.holds.data[s.holds.len - 1];
        --s.holds.len;
        break;
    }
    // What was waiting to be quiet is final now: the writer is gone.
    u32 n = (u32)strlen(prefix), keep = 0;
    for (u32 i = 0; i < s.held.len; ++i) {
        Held& h = s.held.data[i];
        if (h.root == pr.work_root && strncmp(h.path, prefix, n) == 0) push_pending(s, h.kind, h.root, h.path);
        else s.held.data[keep++] = h;
    }
    s.held.len = keep;
}

// A run with declared outputs ended well: its files replace the step's.
// A declared run that ended well, on its worker: what it wrote is moved over the step's
// directory in the work root, hashed on the way, and what last run left there and this one
// did not write is removed. The events are the main thread's (run_done).
void commit_files(Proc& pr) {
    Array<char> text{}, old_text{};
    Array<u32> at{}, old_at{};
    ListCtx c{&text, &at}, oc{&old_text, &old_at};
    io_list_files(pr.run_dir, list_one, &c);
    io_list_files(pr.step_dir, list_one, &oc); // last run's, if there was one: not there is none
    auto record = [&](const char* rel) {
        u32 off = pr.made_text.len;
        char path[kGbPath * 2];
        format(path, sizeof path, "%s/%s", pr.dir, rel);
        u32 n = (u32)strlen(path);
        memcpy(pr.made_text.push_n(n + 1), path, n + 1);
        return off;
    };
    for (u32 off : old_at) {
        const char* rel = old_text.data + off;
        bool again = false;
        for (u32 n : at) if (strcmp(text.data + n, rel) == 0) { again = true; break; }
        if (again) continue;
        char full[1200];
        format(full, sizeof full, "%s/%s", pr.step_dir, rel);
        io_remove(full);
        if (strlen(pr.dir) + 1 + strlen(rel) < kGbPath) pr.gone.push(record(rel));
    }
    char made[1200] = {}; // the last directory made below, made once
    for (u32 off : at) {
        const char* rel = text.data + off;
        char from[1200], to[1200], dir[1200];
        format(from, sizeof from, "%s/%s", pr.run_dir, rel);
        format(to, sizeof to, "%s/%s", pr.step_dir, rel);
        if (strlen(pr.dir) + 1 + strlen(rel) >= kGbPath) { print_err("goldberg: '%s': a converter's output with a path too long to track\n", to); continue; }
        Made m{};
        Sum sum;
        sum.h.init();
        sum.n = 0;
        if (!io_read_each(from, sum_piece, &sum)) { print_err("goldberg: cannot read %s, which a converter wrote\n", from); continue; }
        sum.h.finish(m.hash);
        parent_dir(to, dir, sizeof dir);
        bool have = strcmp(dir, made) == 0 || io_make_dirs(dir);
        if (have) format(made, sizeof made, "%s", dir);
        if (!have || !io_rename(from, to)) { print_err("goldberg: cannot move %s into the work directory\n", from); continue; }
        IoStat st;
        if (!io_stat(to, &st)) continue;
        m.size = st.size;
        m.mtime = st.mtime_ns;
        m.path = record(rel);
        pr.made.push(m);
    }
    io_remove_tree(pr.run_dir);
    text.free(); at.free(); old_text.free(); old_at.free();
}

// A scanning run ended well: everything it left is seen once more (the watcher may have
// missed some, or there may be none), and what it did not touch is last run's and goes.
void finish_scan(GoldbergState& s, Proc& pr) {
    char step_dir[900];
    step_dir_of(s, pr, step_dir, sizeof step_dir);
    Array<char> text{};
    Array<u32> at{};
    ListCtx c{&text, &at};
    io_list_files(step_dir, list_one, &c);
    for (u32 off : at) {
        const char* rel = text.data + off;
        char full[1200], path[kGbPath * 2];
        format(full, sizeof full, "%s/%s", step_dir, rel);
        format(path, sizeof path, "%s/%s", pr.dir, rel);
        if (strlen(path) >= kGbPath) { print_err("goldberg: '%s': a converter's output with a path too long to track\n", full); continue; }
        IoStat st;
        if (!io_stat(full, &st)) continue;
        bool stale = false;
        for (const Before& b : pr.before) if (strcmp(pr.before_text.data + b.path, rel) == 0) { stale = b.size == st.size && b.mtime == st.mtime_ns; break; }
        if (stale) { io_remove(full); push_pending(s, EvRemoved, pr.work_root, path); }
        else push_pending(s, EvSeen, pr.work_root, path, st.size, st.mtime_ns);
    }
    text.free(); at.free();
}

void drain(Proc& pr) {
    char buf[4096];
    for (;;) { i32 r = pr.p.read_out(buf, sizeof buf); if (r <= 0) break; } // stdout is not kept
    for (;;) {
        i32 r = pr.p.read_err(buf, sizeof buf);
        if (r <= 0) break;
        memcpy(pr.err.push_n((u32)r), buf, (usize)r);
        if (pr.err.len > 8192) { memmove(pr.err.data, pr.err.data + pr.err.len - 4096, 4096); pr.err.len = 4096; }
    }
}

// ---- the kim converter's check -----------------------------------------------------------------

struct KimCheck {
    const Proc* pr = nullptr;
    Arena*      arena = nullptr;
    KimErrors*  errors = nullptr;
    KimbFile    files[KV_KIMB_MAX_SCOPE];
    char        names[KV_KIMB_MAX_SCOPE][160] = {};
    u32         count = 0;
    bool        outside = false; // a use whose root is not in this cook: nothing to check against
    // Every use asked for whose root is in this cook, read or not: the screen's step depends on
    // each (reported as `goldberg:use NAME` lines; graph.lua makes them edges).
    char        tried[KV_KIMB_MAX_SCOPE][160] = {};
    u32         tried_count = 0;
};

// A use ("krome/std") as the source under the root of its first part: that root's std.kim.
const KimbFile* kim_find_use(void* user, const char* use) {
    KimCheck& c = *(KimCheck*)user;
    for (u32 i = 0; i < c.count; ++i) if (!strcmp(c.names[i], use)) return &c.files[i];
    const char* slash = strchr(use, '/');
    if (!slash) return nullptr;
    usize nl = (usize)(slash - use);
    const char* dir = nullptr;
    for (const char* r = c.pr->roots_text.data; r && r < c.pr->roots_text.data + c.pr->roots_text.len;) {
        const char* d = r + strlen(r) + 1;
        if (strlen(r) == nl && !strncmp(r, use, nl)) dir = d;
        r = d + strlen(d) + 1;
    }
    if (!dir) { c.outside = true; return nullptr; }
    bool seen = false;
    for (u32 i = 0; i < c.tried_count; ++i) if (!strcmp(c.tried[i], use)) seen = true;
    if (!seen && c.tried_count < KV_KIMB_MAX_SCOPE) format(c.tried[c.tried_count++], sizeof c.tried[0], "%s", use);
    if (c.count == KV_KIMB_MAX_SCOPE) return nullptr;
    char path[1200], file[200];
    format(path, sizeof path, "%s/%s.kim", dir, slash + 1);
    format(file, sizeof file, "%s.kim", use);
    IoBytes b;
    if (!io_read_now(path, &b)) return nullptr;
    KimbFile& f = c.files[c.count];
    format(c.names[c.count], sizeof c.names[0], "%s", use);
    char* keep = (char*)arena_alloc(*c.arena, strlen(file) + 1, 1);
    memcpy(keep, file, strlen(file) + 1);
    JsonDoc doc;
    KimErrors quiet; // a prelude's own mistakes are its own step's to report
    bool ok = kim_parse((const char*)b.data, b.size, keep, &c.arena->a, doc, quiet) && kimb_from_doc(doc, f, quiet, keep);
    b.free();
    if (!ok) { c.outside = true; return nullptr; }
    ++c.count;
    return &f;
}

template <class Say>
bool check_kim(const Proc& pr, const char*, const char* in, const char* asset, Say& say) {
    IoBytes src;
    if (!io_read_now(in, &src)) return true;
    Arena arena;
    if (!arena_create(arena, src.size * 64 + (64u << 20), "goldberg.kim", MemTag{})) { src.free(); return true; }
    KimErrors errors;
    KimCheck c;
    c.pr = &pr;
    c.arena = &arena;
    c.errors = &errors;
    JsonDoc doc;
    KimbFile f;
    bool ok = true;
    if (kim_parse((const char*)src.data, src.size, asset, &arena.a, doc, errors) && kimb_from_doc(doc, f, errors, asset)) {
        KimbScope scope;
        KimErrors scope_errors;
        bool scoped = kimb_scope(scope, f, kim_find_use, &c, scope_errors);
        if (!c.outside) {
            if (!scoped)
                for (u32 i = 0; i < scope_errors.count; ++i) errors.add(scope_errors.list[i].file, scope_errors.list[i].line, 0, "%s", scope_errors.list[i].text);
            if (scoped) kimb_check(scope, errors);
        }
    }
    // The preludes it depends on, whether it checked or not: a prelude saved checks it again.
    for (u32 i = 0; i < c.tried_count; ++i) { say("goldberg:use "); say(c.tried[i]); say("\n"); }
    if (errors.total) {
        ok = false;
        for (u32 i = 0; i < errors.count; ++i) {
            char line[512];
            errors.format_one(i, line, sizeof line);
            say(line);
            say("\n");
        }
    }
    src.free();
    arena_destroy(arena);
    return ok;
}

// A built-in converter, on the run's worker in place of a child process: argv is the
// converter's name, the source's full path, and its asset path (what the result records).
// It writes into run_dir as a script would, and its errors go where a script's stderr
// goes. 0, or 1 when it failed.
//   kim  a .kim compiled to <name>.kimb (kim_compile): the source's name, ".kim" made ".kimb"
//   svg  an .svg read into a shape and written as <name>.kshape (svg_to_shape): ".svg" made
//        ".kshape"; every refusal with its line, and warnings printed without failing
i32 run_svg(Proc& pr, const Array<const char*>& argv);

i32 run_builtin(Proc& pr, const Array<const char*>& argv) {
    auto say = [&](const char* text) {
        u32 n = (u32)strlen(text);
        memcpy(pr.err.push_n(n), text, n);
    };
    if (!strcmp(pr.builtin, "svg") && argv.len >= 3) return run_svg(pr, argv);
    if (strcmp(pr.builtin, "kim") || argv.len < 3) { say("no such built-in converter\n"); return 1; }
    const char* in = argv[1];
    const char* asset = argv[2];
    IoBytes src;
    if (!io_read_now(in, &src)) { say("cannot read the source\n"); return 1; }
    Array<u8> out{};
    KimErrors errors;
    bool ok = kim_compile((const char*)src.data, src.size, asset, out, errors);
    src.free();
    if (!ok) {
        for (u32 i = 0; i < errors.count; ++i) {
            char line[512];
            errors.format_one(i, line, sizeof line);
            say(line);
            say("\n");
        }
        out.free();
        return 1;
    }
    // Checked against the preludes it uses, where their roots are in this cook: a mistake
    // the game would find at load fails the cook, with its line.
    if (!check_kim(pr, (const char*)nullptr, in, asset, say)) { out.free(); return 1; }
    const char* name = strrchr(in, '/');
    name = name ? name + 1 : in;
    char target[1100];
    usize nl = strlen(name);
    if (nl > 4 && !strcmp(name + nl - 4, ".kim")) format(target, sizeof target, "%s/%sb", pr.run_dir, name);
    else format(target, sizeof target, "%s/%s.kimb", pr.run_dir, name);
    bool wrote = io_write_now(target, out.data, out.len);
    out.free();
    if (!wrote) { say("cannot write the .kimb\n"); return 1; }
    return 0;
}

i32 run_svg(Proc& pr, const Array<const char*>& argv) {
    auto say = [&](const char* text) {
        u32 n = (u32)strlen(text);
        memcpy(pr.err.push_n(n), text, n);
    };
    const char* in = argv[1];
    const char* asset = argv[2];
    IoBytes src;
    if (!io_read_now(in, &src)) { say("cannot read the source\n"); return 1; }
    PathShape shape;
    shape.init(mem_general());
    SvgMessages msgs;
    bool ok = svg_to_shape((const char*)src.data, src.size, SvgOptions{}, shape, msgs);
    src.free();
    for (u32 i = 0; i < msgs.items.len; ++i) {
        char line[512];
        msgs.format_one(i, asset, line, sizeof line);
        say(line);
        say("\n");
    }
    msgs.free();
    Array<u8> out{};
    out.alloc = mem_general();
    if (ok) ok = patherizer_write_kshape(shape, out);
    shape.free();
    if (!ok) { out.free(); return 1; }
    const char* name = strrchr(in, '/');
    name = name ? name + 1 : in;
    char target[1100];
    usize nl = strlen(name);
    if (nl > 4 && !strcmp(name + nl - 4, ".svg")) format(target, sizeof target, "%s/%.*s.kshape", pr.run_dir, (int)(nl - 4), name);
    else format(target, sizeof target, "%s/%s.kshape", pr.run_dir, name);
    bool wrote = io_write_now(target, out.data, out.len);
    out.free();
    if (!wrote) { say("cannot write the .kshape\n"); return 1; }
    return 0;
}

// One converter run, on its worker: started, drained until it ends (killed if the queue
// asks it to stop), held open for `settle` when it declares its outputs (its real writer
// may not be the process that was started), and, if it ended well, its files committed.
i32 run_task(void* arg, const IoTaskCtl& ctl) {
    Proc& pr = *(Proc*)arg;
    if (!pr.scan) {
        KV_ZONE("run.fresh dir");
        io_remove_tree(pr.run_dir); // a fresh directory for every run
        io_make_dirs(pr.run_dir);
    }
    Array<const char*> argv{}, env{};
    const char* w = pr.words.data;
    for (u32 i = 0; i < pr.argc; ++i, w += strlen(w) + 1) argv.push(w);
    for (u32 i = 0; i < pr.envc; ++i, w += strlen(w) + 1) env.push(w);
    if (pr.builtin[0]) {
        pr.code = run_builtin(pr, argv);
        argv.free();
        env.free();
        if (pr.code == 0) commit_files(pr);
        else io_remove_tree(pr.run_dir);
        return 0;
    }
    ProcessDesc d;
    d.argv = Span<const char* const>(argv.data, argv.len);
    d.env  = Span<const char* const>(env.data, env.len);
    d.cwd  = pr.run_dir;
    bool ok;
    { KV_ZONE("run.spawn"); ok = pr.p.spawn(d); }
    argv.free();
    env.free();
    if (!ok) { pr.spawn_failed = true; return 0; }
    {
        KV_ZONE_WAIT("run.running");
        for (;;) {
            pr.p.wait(0.05);
            drain(pr);
            if (ctl.cancelled()) pr.p.kill();
            if (!pr.p.running()) { drain(pr); break; }
        }
    }
    if (!pr.scan && pr.settle > 0) {
        f64 end = time_now() + pr.settle;
        for (f64 now = time_now(); now < end && !ctl.cancelled(); now = time_now()) { pr.p.wait(end - now < 0.05 ? end - now : 0.05); drain(pr); }
    }
    pr.code = pr.p.code;
    if (!pr.scan) {
        KV_ZONE("run.commit");
        if (pr.code == 0) commit_files(pr);
        else io_remove_tree(pr.run_dir); // a failed run contributes nothing
    }
    return 0;
}

void take_watcher(GoldbergState& s, bool now);

// A run's task is over: its events, the last of them step-exit. With watch_sync (learn), the
// watcher is asked first for everything it has, so what the run wrote outside its own
// directory comes before its exit on every backend, not only where the OS reports at once.
void run_done(GoldbergState& s, Proc& pr, IoStatus status) {
    if (s.watch_sync && s.watcher.is_open()) {
        f64 until = time_now() + s.watcher.lateness();
        for (;;) {
            take_watcher(s, true);
            if (time_now() >= until) break;
            time_sleep(0.002);
        }
    }
    if (pr.scan) drop_hold(s, pr);
    if (pr.spawn_failed || status != IoStatus::Done) {
        const char* why = pr.spawn_failed ? pr.p.error : "stopped";
        push_exit(s, pr, -1, why, (u32)strlen(why));
        return;
    }
    if (pr.code == 0) {
        if (pr.scan) finish_scan(s, pr);
        else {
            for (u32 off : pr.gone) push_pending(s, EvRemoved, pr.work_root, pr.made_text.data + off);
            for (const Made& m : pr.made) push_pending(s, EvMade, pr.work_root, pr.made_text.data + m.path, m.size, m.mtime, m.hash);
        }
    }
    u32 tail = pr.err.len > 600 ? 600 : pr.err.len;
    push_exit(s, pr, pr.code, pr.err.data + pr.err.len - tail, tail);
}

// What the queue finished since the last tick, as events.
void drain_io(GoldbergState& s) {
    if (!s.io_ready) return;
    s.io.pump();
    Span<const IoCompletion> list = s.io.completions();
    if (!list.len) return;
    Array<IoCompletion> done{};
    for (const IoCompletion& c : list) done.push(c);
    for (const IoCompletion& c : done) {
        s.io.reap(c.request);
        TaskHead* h = (TaskHead*)(uintptr_t)c.user;
        live_remove(s, h);
        if (h->kind == TaskHash) { HashReq* r = (HashReq*)h; if (c.status != IoStatus::Done) r->ok = false; hash_done(s, *r); if (r->text) mem_free(r->text); mem_free(r); }
        else if (h->kind == TaskCopy) { CopyReq* r = (CopyReq*)h; if (c.status != IoStatus::Done && !r->error[0]) format(r->error, sizeof r->error, "stopped"); copy_done(s, *r); mem_free(r); }
        else { Proc* pr = (Proc*)h; run_done(s, *pr, c.status); --s.runs; proc_free(pr); }
    }
    done.free();
}

// A watcher's event: into the queue, or, under a scanning step's directory, held until
// the file has been quiet. True if it went into the queue.
bool take_fs_event(GoldbergState& s, u8 kind, u32 root, const char* path) {
    if (is_fs_event_held(s, kind, root, path)) return false;
    push_pending(s, kind, root, path);
    return true;
}

// Everything the watcher has, into the queue; with `now`, what the OS has not yet delivered
// too (FsWatcher::poll).
void take_watcher(GoldbergState& s, bool now) {
    for (const FsEvent& ev : s.watcher.poll(now)) {
        u32 root = ~0u;
        for (u32 i = 0; i < s.roots.len; ++i) if (s.roots.data[i].watch == ev.root) root = i;
        if (root == ~0u || strlen(ev.path) >= kGbPath) continue;
        take_fs_event(s, fs_kind(ev.kind), root, ev.path);
    }
}

void release_held(GoldbergState& s) {
    if (!s.held.len) return;
    f64 now = time_now();
    u32 keep = 0;
    for (u32 i = 0; i < s.held.len; ++i) {
        Held& h = s.held.data[i];
        if (now >= h.due) push_pending(s, h.kind, h.root, h.path);
        else s.held.data[keep++] = h;
    }
    s.held.len = keep;
}

// ---- the queue -----------------------------------------------------------------------------------

bool is_fs_kind(u8 k) { return k == EvSeen || k == EvAdded || k == EvChanged || k == EvRemoved; }

// One event per path: of several, what the last leaves true. Anything ending in a removal
// is a removal; anything else that includes an arrival is an arrival; otherwise a change.
// A directory event, a rescan or the end of a scan is a boundary: nothing folds across it.
//
// One pass, with a map from each path to the last of its events since the boundary, and
// what came before that last: whether any of it was an arrival (added or seen), and
// whether every arrival was a sighting. That is all the last event's kind depends on: a
// removal stays one; with no arrival before it, it is what it was; a sighting after only
// sightings is a sighting; anything else is an arrival. (The pass that compared each
// event with every one before it took 750 ms over a scan of 36,000 files.)
void fold_pending(GoldbergState& s) {
    struct Group { u32 last; u8 last_kind; bool arrival; bool all_seen; }; // last_kind: as it came
    Map<u64, Group> groups;
    for (u32 i = 0; i < s.pending.len; ++i) {
        Pending& p = s.pending.data[i];
        if (p.dead) continue;
        if (p.kind == EvDirAdded || p.kind == EvDirRemoved || p.kind == EvRescan || p.kind == EvScanDone) { groups.clear(); continue; }
        if (!is_fs_kind(p.kind)) continue;
        const char* path = s.pending_text.data + p.path;
        u64 key = hash_mix(hash_str(path) ^ ((u64)p.root * 0x9E3779B97F4A7C15ull));
        u8 came = p.kind;
        Group* g = groups.find(key);
        Pending* prev = g ? &s.pending.data[g->last] : nullptr;
        if (prev && prev->root == p.root && strcmp(s.pending_text.data + prev->path, path) == 0) {
            if (g->last_kind == EvAdded || g->last_kind == EvSeen) { g->arrival = true; g->all_seen = g->all_seen && g->last_kind == EvSeen; }
            prev->dead = true;
            g->last = i;
            g->last_kind = came;
            if (came != EvRemoved && g->arrival) p.kind = came == EvSeen && g->all_seen ? EvSeen : EvAdded;
            continue;
        }
        // A new path, or (a 64-bit hash shared by two paths) one that takes the slot: the
        // other then simply folds no further, which is never wrong, only less tidy.
        groups.set(key, Group{i, came, false, true});
    }
    groups.free();
}

void fill_rows(GoldbergState& s) {
    fold_pending(s);
    u32 taken = 0;
    for (; taken < s.pending.len; ++taken) {
        Pending& p = s.pending.data[taken];
        if (p.dead) { if (p.text) mem_free(p.text); continue; }
        // The pool is full (a session behind on its events): the rest wait in the list.
        if (s.events->live() >= s.events->capacity) break;
        Handle h = s.events->spawn();
        if (h.is_null()) break;
        GbEvent* e = s.events->get<GbEvent>(h);
        e->seq  = ++s.seq;
        e->kind = p.kind;
        e->root = p.root;
        const char* path = s.pending_text.data + p.path;
        usize n = strlen(path);
        if (n >= kGbPath) n = kGbPath - 1;
        memcpy(e->path, path, n);
        e->size  = p.size;
        e->mtime = p.mtime;
        memcpy(e->hash, p.hash, 32);
        if (p.text) { s.texts.push(GbText{p.text, p.text_len}); e->text = s.texts.len; }
    }
    if (taken == s.pending.len) { s.pending.len = 0; s.pending_text.len = 0; }
    else {
        // Keep what did not fit, in order. Offsets into the text stay valid.
        memmove(s.pending.data, s.pending.data + taken, sizeof(Pending) * (s.pending.len - taken));
        s.pending.len -= taken;
    }
}

struct SeqCmp { Table* events; };
int seq_cmp(const void* a, const void* b, void* ctx) {
    Table* t = ((SeqCmp*)ctx)->events;
    u32 x = t->get<GbEvent>(*(const Handle*)a)->seq, y = t->get<GbEvent>(*(const Handle*)b)->seq;
    return x < y ? -1 : x > y ? 1 : 0;
}

// ---- Lua -------------------------------------------------------------------------------------------

void lua_failed(GoldbergState& s, Engine& e, const char* what) {
    print_err("goldberg: %s: %s\n", what, lua_tostring(s.L, -1));
    lua_pop(s.L, 1);
    s.failed = true;
    s.active = false;
    tool_finish(e, 1);
}

// Calls goldberg.<name> with the `nargs` values on the stack. False if it raised.
bool call_app(GoldbergState& s, Engine& e, const char* name, int nargs, int nresults) {
    lua_State* L = s.L;
    int base = lua_gettop(L) - nargs;
    lua_pushcfunction(L, script_traceback);
    lua_insert(L, base + 1);
    lua_getglobal(L, "goldberg");
    lua_getfield(L, -1, name);
    lua_remove(L, -2);
    lua_insert(L, base + 2);
    if (lua_pcall(L, nargs, nresults, base + 1) != LUA_OK) {
        lua_remove(L, base + 1);
        lua_failed(s, e, name);
        return false;
    }
    lua_remove(L, base + 1);
    return true;
}

int host_add_root(lua_State* L) {
    GoldbergState& s = *state_of(L);
    const char* name = luaL_checkstring(L, 1);
    const char* dir  = luaL_checkstring(L, 2);
    Root r{};
    if (strlen(name) >= sizeof r.name || strlen(dir) >= sizeof r.dir) return luaL_error(L, "root '%s': the name or the path is too long", name);
    format(r.name, sizeof r.name, "%s", name);
    format(r.dir, sizeof r.dir, "%s", dir);
    usize n = strlen(r.dir);
    while (n > 1 && r.dir[n - 1] == '/') r.dir[--n] = 0;
    r.watch = ~0u;
    r.watched = lua_isnoneornil(L, 3) || lua_toboolean(L, 3);
    if (r.watched && s.watcher.is_open()) r.watch = s.watcher.add_root(r.dir); // a root added to a session already watching
    s.roots.push(r);
    lua_pushinteger(L, s.roots.len); // 1-based, as Lua counts
    return 1;
}

Root* check_root(lua_State* L, int index, u32* at) {
    GoldbergState& s = *state_of(L);
    lua_Integer i = luaL_checkinteger(L, index);
    if (i < 1 || i > (lua_Integer)s.roots.len) luaL_error(L, "no root %d", (int)i);
    *at = (u32)i - 1;
    return &s.roots.data[i - 1];
}

int host_scan(lua_State* L) {
    u32 at;
    check_root(L, 1, &at);
    scan_root(*state_of(L), at);
    return 0;
}

// host.watch_sync(on): whether a run's exit waits for the watcher to be heard out (above).
int host_watch_sync(lua_State* L) {
    state_of(L)->watch_sync = lua_toboolean(L, 1) != 0;
    return 0;
}

// host.watch(mode): "inotify", "poll", "native", "auto" or "none". Starts the watcher and
// puts every root under it. Returns the backend's name.
int host_watch(lua_State* L) {
    GoldbergState& s = *state_of(L);
    const char* mode = luaL_optstring(L, 1, "auto");
    s.watcher.free();
    for (Root& r : s.roots) r.watch = ~0u;
    if (strcmp(mode, "none") == 0) { lua_pushstring(L, "none"); return 1; }
    FsWatchBackend b = strcmp(mode, "poll") == 0 ? FsWatchBackend::Poll : strcmp(mode, "auto") == 0 ? FsWatchBackend::Auto : FsWatchBackend::Native;
    if (!s.watcher.init(b)) return luaL_error(L, "--watch %s: this platform has no such watcher", mode);
    if (b == FsWatchBackend::Native && strcmp(mode, "native") != 0 && strcmp(mode, s.watcher.backend_name()) != 0) {
        s.watcher.free();
        return luaL_error(L, "--watch %s: this platform has no such watcher", mode);
    }
    for (Root& r : s.roots) if (r.watched) r.watch = s.watcher.add_root(r.dir); // a root that is not there yet is simply not watched
    // A held file waits at least past what the watcher can still be about to say.
    s.watch_floor = strcmp(s.watcher.backend_name(), "poll") == 0 ? 0.3 : s.watcher.lateness() * 2.0;
    lua_pushstring(L, s.watcher.backend_name());
    return 1;
}

int host_hash(lua_State* L) {
    GoldbergState& s = *state_of(L);
    u32 at;
    check_root(L, 1, &at);
    const char* path = luaL_checkstring(L, 2);
    if (strlen(path) >= kGbPath) return luaL_error(L, "'%s': a path of %d bytes or more", path, (int)kGbPath);
    HashReq* r = (HashReq*)mem_alloc(sizeof(HashReq));
    *r = HashReq{};
    r->root = at;
    format(r->path, sizeof r->path, "%s", path);
    format(r->full, sizeof r->full, "%s/%s", s.roots.data[at].dir, path);
    r->want_text = lua_toboolean(L, 3);
    if (!submit(s, &r->head, hash_task, LaneHash, r->path)) { push_pending(s, EvHashFailed, at, path); mem_free(r); }
    return 0;
}

// host.set_out(dir, stamp): the output tree, and the name of this run's attic folder. host.copy(root, source, output) queues a copy into
// it; its result comes back as a cook-done or cook-failed event for `output`.
int host_set_out(lua_State* L) {
    GoldbergState& s = *state_of(L);
    const char* dir = luaL_checkstring(L, 1);
    if (strlen(dir) >= sizeof s.out_dir) return luaL_error(L, "the output tree's path is too long");
    format(s.out_dir, sizeof s.out_dir, "%s", dir);
    format(s.attic, sizeof s.attic, "%s/.goldberg.attic/%s", dir, luaL_optstring(L, 2, "0"));
    return 0;
}

int host_copy(lua_State* L) {
    GoldbergState& s = *state_of(L);
    u32 at;
    check_root(L, 1, &at);
    const char* source = luaL_checkstring(L, 2);
    const char* output = luaL_checkstring(L, 3);
    if (!s.out_dir[0]) return luaL_error(L, "copy: there is no output tree");
    if (strlen(source) >= sizeof CopyReq::source || strlen(output) >= sizeof CopyReq::output) return luaL_error(L, "copy: '%s': a path too long", output);
    CopyReq* r = (CopyReq*)mem_alloc(sizeof(CopyReq));
    *r = CopyReq{};
    r->root = at;
    format(r->source, sizeof r->source, "%s", source);
    format(r->output, sizeof r->output, "%s", output);
    format(r->from, sizeof r->from, "%s/%s", s.roots.data[at].dir, source);
    format(r->to, sizeof r->to, "%s/%s", s.out_dir, output);
    format(r->tmp, sizeof r->tmp, "%s.tmp-%u", r->to, ++s.tmp_serial);
    if (!submit(s, &r->head, copy_task, LaneCopy, r->output)) {
        format(r->error, sizeof r->error, "the queue would not take it");
        copy_done(s, *r);
        mem_free(r);
    }
    return 0;
}

// host.remove_output(output): takes a file out of the output tree. It is not deleted: it
// is moved to the attic, <out>/.goldberg.attic/<stamp>/<output>, where a person can get it
// back. (A file about to be written again is simply written over; this is for the ones
// nothing will replace.) The directories it leaves empty go, up to the tree. True if the
// file is no longer in the tree; false, and the file where it was, if it could not be moved.
int host_remove_output(lua_State* L) {
    GoldbergState& s = *state_of(L);
    const char* output = luaL_checkstring(L, 1);
    if (!s.out_dir[0] || !s.attic[0] || strstr(output, "..")) return luaL_error(L, "remove: '%s' is not in the output tree", output);
    char path[1300], to[1500], dir[1500];
    format(path, sizeof path, "%s/%s", s.out_dir, output);
    if (io_is_file(path)) {
        format(to, sizeof to, "%s/%s", s.attic, output);
        parent_dir(to, dir, sizeof dir);
        if (!io_make_dirs(dir) || !io_rename(path, to)) { lua_pushboolean(L, 0); return 1; }
        tell_programs(s, "removed", output);
    }
    usize floor = strlen(s.out_dir);
    for (;;) {
        char* slash = strrchr(path, '/');
        if (!slash || (usize)(slash - path) <= floor) break;
        *slash = 0;
        if (!io_remove(path)) break; // not empty, or not ours to remove
    }
    lua_pushboolean(L, 1);
    return 1;
}

// host.run{ root, path, argv, env, work_root, dir, write_dir, scan, settle }: queues one
// converter run for the source root:path. `dir` is the step's directory in the work root;
// `write_dir` is where the process writes and starts (the step's directory itself when
// it scans, a private one otherwise). Its end comes back as a step-exit event.
int host_run(lua_State* L) {
    GoldbergState& s = *state_of(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    Proc* pr = (Proc*)mem_alloc(sizeof(Proc));
    *pr = Proc{};
    auto fail = [&](const char* why) { proc_free(pr); return luaL_error(L, "run: %s", why); };
    lua_getfield(L, 1, "root");      lua_Integer root = lua_tointeger(L, -1);
    lua_getfield(L, 1, "work_root"); lua_Integer work = lua_tointeger(L, -1);
    lua_getfield(L, 1, "path");      const char* path = lua_tostring(L, -1);
    lua_getfield(L, 1, "dir");       const char* dir = lua_tostring(L, -1);
    lua_getfield(L, 1, "write_dir"); const char* write_dir = lua_tostring(L, -1);
    lua_getfield(L, 1, "scan");      pr->scan = lua_toboolean(L, -1);
    lua_getfield(L, 1, "settle");    pr->settle = lua_tonumber(L, -1);
    if (root < 1 || root > (lua_Integer)s.roots.len || work < 1 || work > (lua_Integer)s.roots.len) return fail("no such root");
    if (!path || !dir || !write_dir || strlen(path) >= kGbPath || strlen(dir) >= kGbPath - 2 || strlen(write_dir) >= sizeof pr->run_dir) return fail("a path is missing or too long");
    pr->root = (u32)root - 1;
    pr->work_root = (u32)work - 1;
    format(pr->path, sizeof pr->path, "%s", path);
    format(pr->dir, sizeof pr->dir, "%s", dir);
    format(pr->run_dir, sizeof pr->run_dir, "%s", write_dir);
    lua_pop(L, 7);
    for (int pass = 0; pass < 2; ++pass) {
        lua_getfield(L, 1, pass ? "env" : "argv");
        if (lua_istable(L, -1)) {
            lua_Integer n = (lua_Integer)lua_rawlen(L, -1);
            for (lua_Integer i = 1; i <= n; ++i) {
                lua_rawgeti(L, -1, i);
                usize len = 0;
                const char* w = lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &len) : nullptr;
                if (!w) return fail("argv and env are lists of strings");
                memcpy(pr->words.push_n((u32)len + 1), w, len + 1);
                if (pass) ++pr->envc; else ++pr->argc;
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }
    if (!pr->argc) return fail("no command");
    lua_getfield(L, 1, "builtin");
    if (lua_type(L, -1) == LUA_TSTRING) {
        format(pr->builtin, sizeof pr->builtin, "%s", lua_tostring(L, -1));
        // The roots, for what the source names in other files (a .kim's uses): a copy, since
        // the run is on a worker.
        for (const Root& r : s.roots) {
            if (!r.name[0]) continue;
            usize a = strlen(r.name) + 1, b = strlen(r.dir) + 1;
            memcpy(pr->roots_text.push_n((u32)a), r.name, a);
            memcpy(pr->roots_text.push_n((u32)b), r.dir, b);
        }
    }
    lua_pop(L, 1);
    proc_prepare(s, *pr);
    if (!submit(s, &pr->head, run_task, LaneRun, pr->path)) {
        pr->spawn_failed = true;
        format(pr->p.error, sizeof pr->p.error, "the queue would not take it");
        run_done(s, *pr, IoStatus::Failed);
        proc_free(pr);
        return 0;
    }
    ++s.runs;
    return 0;
}

// host.clear_work(work_root, dir): a step's directory and everything in it, gone.
// host.remove_work(work_root, path): one file of the work root.
// The work root is goldberg's scratch: what leaves it is deleted, not put in the attic.
int host_clear_work(lua_State* L) {
    u32 at;
    Root* r = check_root(L, 1, &at);
    const char* dir = luaL_checkstring(L, 2);
    if (!dir[0] || strstr(dir, "..") || dir[0] == '/') return luaL_error(L, "clear_work: '%s' is not a step's directory", dir);
    char full[1200];
    format(full, sizeof full, "%s/%s", r->dir, dir);
    io_remove_tree(full);
    return 0;
}
int host_remove_work(lua_State* L) {
    u32 at;
    Root* r = check_root(L, 1, &at);
    const char* path = luaL_checkstring(L, 2);
    if (!path[0] || strstr(path, "..") || path[0] == '/') return luaL_error(L, "remove_work: '%s' is not in the work root", path);
    char full[1200];
    format(full, sizeof full, "%s/%s", r->dir, path);
    io_remove(full);
    return 0;
}

// host.lock(path, who): takes the lock file at `path` for this process. true; or false and
// the holder's line ("watch 1234"); or nil when the file cannot be made.
int host_lock(lua_State* L) {
    GoldbergState& s = *state_of(L);
    char holder[128];
    if (s.lock >= 0) { lua_pushboolean(L, 1); return 1; }
    int r = goldberg_lock(luaL_checkstring(L, 1), luaL_checkstring(L, 2), holder, sizeof holder, &s.lock);
    if (r < 0) { lua_pushnil(L); return 1; }
    lua_pushboolean(L, r == 1);
    lua_pushstring(L, holder);
    return 2;
}
int host_sleep(lua_State* L) { time_sleep(luaL_checknumber(L, 1)); return 0; }

// host.stat(path): size and modification time, or nothing if it is not there.
int host_stat(lua_State* L) {
    IoStat st;
    if (!io_stat(luaL_checkstring(L, 1), &st)) return 0;
    lua_pushinteger(L, (lua_Integer)st.size);
    lua_pushinteger(L, st.mtime_ns);
    return 2;
}

// host.touch(root, path): a file-changed event for that file, as if a watcher had sent it.
int host_touch(lua_State* L) {
    GoldbergState& s = *state_of(L);
    u32 at;
    check_root(L, 1, &at);
    const char* path = luaL_checkstring(L, 2);
    if (strlen(path) >= kGbPath) return luaL_error(L, "'%s': a path too long", path);
    push_pending(s, EvChanged, at, path);
    return 0;
}

// host.write_atomic(path, text): the file written whole, its directory made if need be.
int host_write_atomic(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    usize n = 0;
    const char* text = luaL_checklstring(L, 2, &n);
    char tmp[1100], dir[1100];
    if (strlen(path) + 16 >= sizeof tmp) return luaL_error(L, "'%s': a path too long", path);
    parent_dir(path, dir, sizeof dir);
    format(tmp, sizeof tmp, "%s.tmp", path);
    bool ok = (!dir[0] || io_make_dirs(dir)) && io_write_now(tmp, text, n) && io_rename(tmp, path);
    if (!ok) io_remove(tmp);
    lua_pushboolean(L, ok);
    return 1;
}

int host_glob(lua_State* L) { lua_pushboolean(L, glob_match(luaL_checkstring(L, 1), luaL_checkstring(L, 2))); return 1; }
int host_glob_path(lua_State* L) { lua_pushboolean(L, glob_match_path(luaL_checkstring(L, 1), luaL_checkstring(L, 2))); return 1; }
int host_print(lua_State* L) { print("%s\n", luaL_checkstring(L, 1)); return 0; }
int host_warn(lua_State* L) { print_err("%s\n", luaL_checkstring(L, 1)); return 0; }
int host_flush(lua_State*) { print_flush(); return 0; }
int host_now(lua_State* L) { lua_pushnumber(L, time_now()); return 1; }
// host.zone(name), host.zone_end(): the engine's profiler from Lua (--prof-report, --profile), so
// what the Lua spends is seen beside what the C++ does. The profiler keys a zone by its
// name's address: the name is interned, one address for one name.
int host_zone(lua_State* L) { if (prof_enabled()) prof_begin(name_str(name_intern(luaL_checkstring(L, 1)))); return 0; }
int host_zone_end(lua_State*) { if (prof_enabled()) prof_end(); return 0; }
int host_cwd(lua_State* L) {
    char cwd[1024];
    if (!io_current_dir(cwd, sizeof cwd)) return luaL_error(L, "the current directory cannot be read");
    lua_pushstring(L, cwd);
    return 1;
}

// host.share(x): the session's share of a tick for its events, (0, 1]; 1 is all of them.
int host_share(lua_State* L) {
    f64 x = luaL_checknumber(L, 1);
    state_of(L)->share = x <= 0.05 ? 0.05 : x >= 1.0 ? 1.0 : x;
    return 0;
}

// host.active(on): whether the pump should keep ticking a session.
int host_active(lua_State* L) { state_of(L)->active = lua_toboolean(L, 1); return 0; }

// host.listen(port): a watch's listener for programs, on loopback; the port it got written to
// <out>/.goldberg-port, where a dev build's asset_live finds it. The port, or nil.
int host_listen(lua_State* L) {
    GoldbergState& s = *state_of(L);
    u16 want = (u16)luaL_optinteger(L, 1, 0);
    if (!s.out_dir[0]) return 0;
    if (!s.listener.is_open() && !s.listener.open(want, true)) return 0;
    format(s.port_file, sizeof s.port_file, "%s/.goldberg-port", s.out_dir);
    char text[16];
    int n = format(text, sizeof text, "%u\n", (u32)s.listener.port);
    if (!io_write_now(s.port_file, text, (usize)n)) s.port_file[0] = 0;
    lua_pushinteger(L, s.listener.port);
    return 1;
}

// host.programs(): { "name (config)", ... } for each program that said hello.
int host_programs(lua_State* L) {
    GoldbergState& s = *state_of(L);
    lua_newtable(L);
    int i = 0;
    for (GoldbergState::Program* p : s.programs) {
        if (!p->hello) continue;
        lua_pushfstring(L, "%s (%s)", p->name, p->config[0] ? p->config : "?");
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

int host_finish(lua_State* L) {
    GoldbergState& s = *state_of(L);
    s.active = false;
    close_programs(s);
    lua_getfield(L, LUA_REGISTRYINDEX, "goldberg.engine");
    Engine* e = (Engine*)lua_touserdata(L, -1);
    print_flush();
    tool_finish(*e, (int)luaL_optinteger(L, 1, 0));
    if (s.window) engine_request_quit(*e); // the window closes with the session
    return 0;
}

// host.ui_get(key): the window's model value at `key` as text (what a person typed or picked),
// or nil.
int host_ui_get(lua_State* L) {
    GoldbergState& s = *state_of(L);
    if (!s.window) return 0;
    lua_getfield(L, LUA_REGISTRYINDEX, "goldberg.engine");
    Engine& e = *(Engine*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    const char* v = krome_model_text(e, s.screen, luaL_checkstring(L, 1));
    if (!v) return 0;
    lua_pushstring(L, v);
    return 1;
}

// host.clipboard(text): the system's clipboard, from the window.
int host_clipboard(lua_State* L) {
    GoldbergState& s = *state_of(L);
    if (!s.window) return 0;
    lua_getfield(L, LUA_REGISTRYINDEX, "goldberg.engine");
    Engine& e = *(Engine*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    platform_clipboard_set(e, luaL_checkstring(L, 1));
    return 0;
}

// host.window(): whether there is one to keep up to date. host.ui_ready(): whether its screen is
// built, so that what is sent to it is kept (Krome drops a value for a screen still loading).
int host_window(lua_State* L) { lua_pushboolean(L, state_of(L)->window); return 1; }
int host_ui_ready(lua_State* L) {
    GoldbergState& s = *state_of(L);
    lua_getfield(L, LUA_REGISTRYINDEX, "goldberg.engine");
    Engine& e = *(Engine*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    lua_pushboolean(L, s.window && krome_ready(e, s.screen));
    return 1;
}

// host.ui_set(key, json): a model value of the window's screen, as JSON text: a table or a
// list is sent as KBOR (krome_set_doc), a string or a number as itself.
int host_ui_set(lua_State* L) {
    GoldbergState& s = *state_of(L);
    if (!s.window) return 0;
    const char* key = luaL_checkstring(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, "goldberg.engine");
    Engine& e = *(Engine*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (lua_type(L, 2) == LUA_TNUMBER) { krome_set(e, s.screen, key, (f64)lua_tonumber(L, 2)); ++s.ui_sets; return 0; }
    if (lua_type(L, 2) == LUA_TBOOLEAN) { krome_set_bool(e, s.screen, key, lua_toboolean(L, 2)); ++s.ui_sets; return 0; }
    size_t len = 0;
    const char* text = luaL_checklstring(L, 2, &len);
    if (!lua_toboolean(L, 3)) { krome_set(e, s.screen, key, text); ++s.ui_sets; return 0; } // a plain string
    MemScope scope;
    JsonDoc doc;
    if (!json_parse(text, len, mem_scratch(), doc)) return luaL_error(L, "ui_set %s: %s", key, doc.error);
    Array<u8> k{};
    kbor_write(doc, k);
    krome_set_doc(e, s.screen, key, k.data, k.len);
    k.free();
    ++s.ui_sets;
    return 0;
}

constexpr luaL_Reg k_host[] = {
    {"add_root", host_add_root}, {"scan", host_scan}, {"watch", host_watch}, {"watch_sync", host_watch_sync}, {"hash", host_hash},
    {"set_out", host_set_out}, {"copy", host_copy}, {"remove_output", host_remove_output}, {"write_atomic", host_write_atomic},
    {"flush", host_flush}, {"lock", host_lock}, {"sleep", host_sleep}, {"stat", host_stat}, {"touch", host_touch},
    {"run", host_run}, {"clear_work", host_clear_work}, {"remove_work", host_remove_work},
    {"glob", host_glob}, {"glob_path", host_glob_path}, {"print", host_print}, {"warn", host_warn},
    {"now", host_now}, {"zone", host_zone}, {"zone_end", host_zone_end}, {"cwd", host_cwd}, {"active", host_active}, {"share", host_share}, {"finish", host_finish},
    {"listen", host_listen}, {"programs", host_programs}, {"window", host_window}, {"ui_ready", host_ui_ready}, {"ui_set", host_ui_set}, {"ui_get", host_ui_get},
    {"clipboard", host_clipboard}, {nullptr, nullptr},
};

// The application's directory: tools/goldberg/lua beside this source, unless told
// otherwise. Read from the source tree at start, so an edit to the Lua needs no build.
void find_lua_dir(GoldbergState& s) {
    const char* over = getenv("GOLDBERG_LUA");
    if (over && over[0]) { format(s.lua_dir, sizeof s.lua_dir, "%s", over); return; }
    const char* file = __FILE__;
    const char* cut = strstr(file, "src/goldberg.cc");
    if (!cut) cut = strstr(file, "src\\goldberg.cc");
    usize n = cut ? (usize)(cut - file) : 0;
    format(s.lua_dir, sizeof s.lua_dir, "%.*slua", (int)n, file);
}

bool load_app(GoldbergState& s, Engine& e) {
    if (s.loaded) return true;
    lua_State* L = luaL_newstate();
    if (!L) { print_err("goldberg: no memory for Lua\n"); return false; }
    s.L = L;
    luaL_openlibs(L);
    script_open_kv(L);
    lua_setglobal(L, "kv");
    lua_pushlightuserdata(L, &e);
    lua_setfield(L, LUA_REGISTRYINDEX, "goldberg.engine");

    lua_newtable(L); // host
    lua_pushlightuserdata(L, &s);
    luaL_setfuncs(L, k_host, 1);
    lua_newtable(L);
    script_push_kind(L, engine_table(e, "goldberg.event"));
    lua_setfield(L, -2, "event");
    script_push_kind(L, engine_table(e, "goldberg.file"));
    lua_setfield(L, -2, "file");
    script_push_kind(L, engine_table(e, "goldberg.berg"));
    lua_setfield(L, -2, "berg");
    script_push_kind(L, engine_table(e, "goldberg.step"));
    lua_setfield(L, -2, "step");
    lua_setfield(L, -2, "kinds");
    lua_pushstring(L, s.lua_dir);
    lua_setfield(L, -2, "lua_dir");
    lua_pushboolean(L, fs_watch_has_native());
    lua_setfield(L, -2, "has_native_watch");
#if defined(_WIN32)
    lua_pushboolean(L, 1);
#else
    lua_pushboolean(L, 0);
#endif
    lua_setfield(L, -2, "windows");
    // The built-in converters and their versions: a step's key holds the version, so a new
    // compiler cooks everything again.
    lua_newtable(L);
    lua_pushfstring(L, "%d", (int)KV_KIM_VERSION);
    lua_setfield(L, -2, "kim");
    lua_pushfstring(L, "%d", (int)KV_SVG_VERSION);
    lua_setfield(L, -2, "svg");
    lua_setfield(L, -2, "builtins");
    lua_newtable(L);
    for (u32 i = 0; i < EvCount; ++i) { lua_pushstring(L, kEvNames[i]); lua_rawseti(L, -2, (lua_Integer)i); }
    lua_setfield(L, -2, "event_names");
    lua_setglobal(L, "host");

    // require finds the application's files, and nothing else by accident.
    lua_getglobal(L, "package");
    lua_pushfstring(L, "%s/?.lua", s.lua_dir);
    lua_setfield(L, -2, "path");
    lua_pushstring(L, "");
    lua_setfield(L, -2, "cpath");
    lua_pop(L, 1);

    char main_path[700];
    format(main_path, sizeof main_path, "%s/main.lua", s.lua_dir);
    IoBytes source;
    if (!io_read_now(main_path, &source)) {
        print_err("goldberg: cannot read '%s' (set GOLDBERG_LUA to the directory that holds main.lua)\n", main_path);
        return false;
    }
    char chunk[720];
    format(chunk, sizeof chunk, "@%s", main_path);
    lua_pushcfunction(L, script_traceback);
    int status = luaL_loadbuffer(L, (const char*)source.data, source.size, chunk);
    source.free();
    if (status == LUA_OK) status = lua_pcall(L, 0, 1, -2);
    if (status != LUA_OK) {
        print_err("goldberg: %s\n", lua_tostring(L, -1));
        return false;
    }
    lua_setglobal(L, "goldberg");
    lua_pop(L, 1);
    s.loaded = true;
    return true;
}

// ---- the pump ----------------------------------------------------------------------------------------

void push_event(lua_State* L, GoldbergState& s, const GbEvent& ev) {
    lua_createtable(L, 0, 8);
    lua_pushinteger(L, ev.seq);
    lua_setfield(L, -2, "seq");
    lua_pushstring(L, ev.kind < EvCount ? kEvNames[ev.kind] : "?");
    lua_setfield(L, -2, "kind");
    lua_pushinteger(L, (lua_Integer)ev.root + 1);
    lua_setfield(L, -2, "root");
    lua_pushstring(L, ev.path);
    lua_setfield(L, -2, "path");
    if (ev.kind == EvSeen || ev.kind == EvHashed || ev.kind == EvCookDone || ev.kind == EvMade) {
        lua_pushinteger(L, (lua_Integer)ev.size);
        lua_setfield(L, -2, "size");
        lua_pushinteger(L, ev.mtime);
        lua_setfield(L, -2, "mtime");
    }
    if (ev.kind == EvStepExit) {
        lua_pushinteger(L, ev.mtime);
        lua_setfield(L, -2, "code");
    }
    if ((ev.kind == EvCookFailed || ev.kind == EvStepExit) && ev.text) {
        const GbText& t = s.texts.data[ev.text - 1];
        lua_pushlstring(L, t.data, t.len);
        lua_setfield(L, -2, "text");
    }
    if (ev.kind == EvCookDone || ev.kind == EvMade) {
        char hex[65];
        sha256_hex(ev.hash, hex);
        lua_pushlstring(L, hex, 64);
        lua_setfield(L, -2, "hash");
    }
    if (ev.kind == EvHashed) {
        char hex[65];
        sha256_hex(ev.hash, hex);
        lua_pushlstring(L, hex, 64);
        lua_setfield(L, -2, "hash");
        if (ev.text) {
            const GbText& t = s.texts.data[ev.text - 1];
            lua_pushlstring(L, t.data, t.len);
            lua_setfield(L, -2, "text");
        }
    }
}

// One pass of the session. `work`: in the window's Work cycle, where the frame loop paces and
// nothing here sleeps; it says ctx.more() while it found something to do, and the waker
// (goldberg_wake) brings the cycle back when the sources have more.
void pump_body(TickContext& ctx, bool work) {
    Engine& e = *ctx.engine;
    GoldbergState& s = ctx.state_as<GoldbergState>();
    if (s.failed) return;
    f64 tick_start = time_now();
    s.events = ctx_kind(ctx, "goldberg.event", true);
    ctx_kind(ctx, "goldberg.file", true);
    ctx_kind(ctx, "goldberg.berg", true);
    ctx_kind(ctx, "goldberg.step", true);

    serve_programs(s);
    // 1. A command.
    if (s.command_words) {
        KV_ZONE("goldberg.command");
        if (!load_app(s, e)) { s.failed = true; tool_finish(e, 1); return; }
        lua_State* L = s.L;
        lua_createtable(L, (int)s.command_words, 0);
        const char* w = s.command.data;
        for (u32 i = 0; i < s.command_words; ++i, w += strlen(w) + 1) {
            lua_pushstring(L, w);
            lua_rawseti(L, -2, (lua_Integer)i + 1);
        }
        s.command.len = 0;
        s.command_words = 0;
        if (!call_app(s, e, "command", 1, 0)) return;
    }
    if (!s.active) return;
    lua_State* L = s.L;
    // The CLI's `quit`, in a session: the state is saved and the run ends.
    if (e.quit_requested) { call_app(s, e, "quit", 0, 0); return; }

    // 2. The sources.
    if (s.watcher.is_open()) {
        KV_ZONE("goldberg.watcher");
        for (const FsEvent& ev : s.watcher.poll()) {
            u32 root = ~0u;
            for (u32 i = 0; i < s.roots.len; ++i) if (s.roots.data[i].watch == ev.root) root = i;
            if (root == ~0u) continue;
            if (strlen(ev.path) >= kGbPath) { print_err("goldberg: '%s/%s': a path too long to track\n", s.roots.data[root].dir, ev.path); continue; }
            take_fs_event(s, fs_kind(ev.kind), root, ev.path);
        }
    }
    { KV_ZONE("goldberg.drain io"); drain_io(s); }
    release_held(s);
    // The scan's files, a pool's worth at a time.
    { KV_ZONE("goldberg.scan to events");
    u32 room = s.events->capacity - s.events->live();
    for (; s.scan_at < s.scan.len && s.pending.len < room; ++s.scan_at) {
        const ScanItem& it = s.scan.data[s.scan_at];
        const char* path = s.scan_text.data + it.path;
        if (it.end) push_pending(s, EvScanDone, it.root, "");
        else if (strlen(path) >= kGbPath) print_err("goldberg: '%s/%s': a path too long to track\n", s.roots.data[it.root].dir, path);
        else push_pending(s, EvSeen, it.root, path, it.size, it.mtime);
    }
    if (s.scan_at == s.scan.len) { s.scan.len = 0; s.scan_text.len = 0; s.scan_at = 0; }
    }

    // 3 and 4. Into rows, and to Lua in order, within the session's share of the tick:
    // what does not fit stays in its rows for the next. One always goes.
    { KV_ZONE("goldberg.fill rows"); fill_rows(s); }
    u32 delivered = 0;
    f64 budget = s.share >= 1.0 ? 0.0 : s.share * kTick;
    if (s.events->live()) {
        Array<Handle> order{};
        for (u32 i = 0; i < s.events->rows(); ++i) if (s.events->is_alive(i)) order.push(s.events->handle_at(i));
        SeqCmp cmp{s.events};
        sort(order.data, order.len, sizeof(Handle), seq_cmp, &cmp);
        KV_ZONE("goldberg.events to lua");
        for (Handle h : order) {
            if (!work && budget > 0 && delivered && time_now() - tick_start >= budget) break;
            if (work && delivered && ctx.time_left() <= 0.0) break;
            GbEvent* ev = s.events->get<GbEvent>(h);
            if (!s.failed) {
                push_event(L, s, *ev);
                if (call_app(s, e, "event", 1, 0)) ++delivered;
            }
            if (ev->text) { GbText& t = s.texts.data[ev->text - 1]; mem_free(t.data); t.data = nullptr; }
            s.events->destroy(h);
        }
        order.free();
        if (!s.events->live()) s.texts.len = 0; // every one delivered: the texts start over
    }
    if (s.failed) return;

    // 5. Is anything left? The watchers are asked once more before the host says no.
    bool idle = !s.pending.len && !s.io.pending() && !s.scan.len && !delivered && !s.events->live();
    if (idle && s.watcher.is_open()) {
        for (const FsEvent& ev : s.watcher.poll(true)) {
            u32 root = ~0u;
            for (u32 i = 0; i < s.roots.len; ++i) if (s.roots.data[i].watch == ev.root) root = i;
            if (root == ~0u || strlen(ev.path) >= kGbPath) continue;
            if (take_fs_event(s, fs_kind(ev.kind), root, ev.path)) idle = false;
        }
    }
    // A converter still running, or a file of its still settling, is something left.
    bool quiet = idle && !s.runs && !s.held.len;
    s.quiet = quiet;
    lua_pushboolean(L, quiet);
    { KV_ZONE("goldberg.lua tick"); call_app(s, e, "tick", 1, 0); }
    if (work) {
        if (!idle || s.events->live() || s.pending.len) ctx.more();
        return;
    }
    if (s.active && idle) time_sleep(0.002); // nothing to do this tick: do not spin
    else if (s.active && s.share < 1.0) {
        // Headless, nothing paces the ticks: a session with less than all of each sleeps
        // in proportion to what this one used, so that its share is what it takes of the
        // main thread, whatever the tick spent it on (events are budgeted; what Lua does
        // after them is not). The queue's workers go on meanwhile.
        f64 used = time_now() - tick_start;
        f64 rest = used * (1.0 - s.share) / s.share;
        time_sleep(rest < 4 * kTick ? rest : 4 * kTick);
    }
}

// What a running session answers at once, in the CLI's own turn: it reads the session and
// changes no pool.
bool session_verb(const char* v) {
    return strcmp(v, "status") == 0 || strcmp(v, "explain") == 0 || strcmp(v, "touch") == 0 || strcmp(v, "cook") == 0 ||
           strcmp(v, "berg") == 0 || strcmp(v, "berg-preview") == 0 || strcmp(v, "berg-set") == 0;
}

// Every other verb is one command: it leaves its words for the pump.
void cmd_any(CliContext& ctx, Span<const char* const> args, const char* verb) {
    GoldbergState* s = state_of(*ctx.engine);
    if (!s) { ctx.fail("the goldberg module is not up"); return; }
    if (s->active && s->loaded && !s->failed && session_verb(verb)) {
        lua_State* L = s->L;
        int top = lua_gettop(L);
        lua_pushcfunction(L, script_traceback);
        lua_getglobal(L, "goldberg");
        lua_getfield(L, -1, "ask");
        lua_remove(L, -2);
        lua_createtable(L, (int)args.len + 1, 0);
        lua_pushstring(L, verb);
        lua_rawseti(L, -2, 1);
        for (u32 i = 0; i < args.len; ++i) { lua_pushstring(L, args[i]); lua_rawseti(L, -2, (lua_Integer)i + 2); }
        // The answer: a list of lines, and a message if the request failed.
        if (lua_pcall(L, 1, 2, top + 1) != LUA_OK) { ctx.fail("%s", lua_tostring(L, -1)); lua_settop(L, top); return; }
        if (lua_istable(L, -2)) {
            lua_Integer n = (lua_Integer)lua_rawlen(L, -2);
            for (lua_Integer i = 1; i <= n; ++i) {
                lua_rawgeti(L, -2, i);
                if (lua_type(L, -1) == LUA_TSTRING) ctx.print("%s", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        }
        if (lua_type(L, -1) == LUA_TSTRING) ctx.fail("%s", lua_tostring(L, -1));
        lua_settop(L, top);
        return;
    }
    if (strcmp(verb, "touch") == 0) { ctx.fail("touch is for a running `goldberg watch`"); return; }
    if (s->command_words) { ctx.fail("goldberg is busy with another command"); return; }
    u32 n = (u32)strlen(verb);
    memcpy(s->command.push_n(n + 1), verb, n + 1);
    s->command_words = 1;
    for (const char* a : args) {
        n = (u32)strlen(a);
        memcpy(s->command.push_n(n + 1), a, n + 1);
        ++s->command_words;
    }
}
void cmd_cook(CliContext& ctx, Span<const char* const> args) { cmd_any(ctx, args, "cook"); }
void cmd_watch(CliContext& ctx, Span<const char* const> args) { cmd_any(ctx, args, "watch"); }
void cmd_explain(CliContext& ctx, Span<const char* const> args) { cmd_any(ctx, args, "explain"); }
void cmd_status(CliContext& ctx, Span<const char* const> args) { cmd_any(ctx, args, "status"); }
void cmd_clean(CliContext& ctx, Span<const char* const> args) { cmd_any(ctx, args, "clean"); }
void cmd_replay(CliContext& ctx, Span<const char* const> args) { cmd_any(ctx, args, "replay"); }
void cmd_touch(CliContext& ctx, Span<const char* const> args) { cmd_any(ctx, args, "touch"); }
void cmd_learn(CliContext& ctx, Span<const char* const> args) { cmd_any(ctx, args, "learn"); }
void cmd_berg(CliContext& ctx, Span<const char* const> args) { cmd_any(ctx, args, "berg"); }
void cmd_berg_preview(CliContext& ctx, Span<const char* const> args) { cmd_any(ctx, args, "berg-preview"); }
void cmd_berg_set(CliContext& ctx, Span<const char* const> args) { cmd_any(ctx, args, "berg-set"); }

// The window's waker (engine_add_waker), asked at each of the frame loop's polls while it is
// idle: the watcher's news taken, programs served, and true (with the Work cycle asked for)
// when anything came, a run finished, or, while the session is not quiet, every 50 ms (a file
// settling, a step held).
bool goldberg_wake(Engine& e, void* user) {
    GoldbergState& s = *(GoldbergState*)user;
    if (!s.active || s.failed) return false;
    bool any = false;
    if (s.watcher.is_open())
        for (const FsEvent& ev : s.watcher.poll()) {
            u32 root = ~0u;
            for (u32 i = 0; i < s.roots.len; ++i) if (s.roots.data[i].watch == ev.root) root = i;
            if (root == ~0u || strlen(ev.path) >= kGbPath) continue;
            if (take_fs_event(s, fs_kind(ev.kind), root, ev.path)) any = true;
        }
    serve_programs(s);
    drain_ui_child(s);
    if (s.pending.len || s.io.completions().len || (s.events && s.events->live())) any = true;
    f64 now = time_now();
    if (!s.quiet && now >= s.next_wake) { s.next_wake = now + 0.05; any = true; }
    if (any) e.work_pending = true;
    return any;
}

bool goldberg_startup(Engine& e, ModuleInstance& self) {
    GoldbergState* s = arena_push<GoldbergState>(self.arena);
    if (!s) return false;
    s->general = &self.general;
    // The queue: no reads of its own (goldberg's are tasks), workers for every lane at once.
    u32 cpus = os_cpu_count() ? os_cpu_count() : 1;
    // Converters at once: one a CPU, and no fewer than 8 however few there are. A small
    // file's run is mostly the OS starting a process, which is waiting more than working:
    // on Windows starts go on scaling to about two a CPU, and 8 on a 16-CPU machine left a
    // fifth of a small-file cook on the table.
    s->max_procs = cpus > 8 ? cpus : 8;
    if (s->io.init(&self.general, 1u << 18, IoBackend::Sync) && s->io.start_tasks(s->max_procs + cpus)) {
        s->io.set_lane(LaneRun, s->max_procs);
        s->io.set_lane(LaneHash, cpus);
        s->io.set_lane(LaneCopy, cpus > 4 ? cpus / 2 : 2);
        s->io_ready = true;
    } else {
        KV_ERROR("goldberg", "the I/O queue did not start");
        return false;
    }
    find_lua_dir(*s);
    self.set_state(s);
    if (g_window_mode) {
        // The window: its screen, then the session the command line asked for, in Work.
        s->window = true;
        s->screen = krome_open(e, "goldberg/ui/window.kimb", "window");
        engine_add_waker(e, goldberg_wake, s);
        e.work_pending = true; // the request runs in the first Work cycle, once the module is up
    }
    return true;
}

void goldberg_shutdown(Engine& e, ModuleInstance& self) {
    GoldbergState* s = self.state_as<GoldbergState>();
    if (!s) return;
    if (s->window) {
        engine_remove_waker(e, goldberg_wake, s);
        stop_own_ui(*s);
        // Ended some other way than by its close (the CLI's quit, a signal): the state saved.
        if (s->active && s->L && !s->failed) call_app(*s, e, "quit", 0, 0);
    }
    // The queue first: running converters are killed, the rest never start, and then
    // what was handed to it can go.
    if (s->io_ready) s->io.free();
    for (TaskHead* h : s->live) {
        if (h->kind == TaskHash) { HashReq* r = (HashReq*)h; if (r->text) mem_free(r->text); mem_free(r); }
        else if (h->kind == TaskCopy) mem_free(h);
        else proc_free((Proc*)h);
    }
    s->live.free();
    close_programs(*s);
    if (s->L) lua_close(s->L);
    s->watcher.free();
    goldberg_unlock(s->lock);
    s->holds.free();
    s->held.free();
    for (Pending& p : s->pending) if (p.text) mem_free(p.text);
    for (GbText& t : s->texts) mem_free(t.data);
    s->command.free();
    s->roots.free();
    s->scan.free();
    s->scan_text.free();
    s->pending.free();
    s->pending_text.free();
    s->texts.free();
}

constinit const CliCommandDesc k_commands[] = {
    {.name = "goldberg cook", .help = "goldberg cook [GAME] [CONFIG] [--root NAME=DIR]... [--log FILE]: cook until stable, then exit", .fn = cmd_cook},
    {.name = "goldberg watch", .help = "goldberg watch [GAME] [CONFIG]: cook, then keep watching", .fn = cmd_watch},
    {.name = "goldberg explain", .help = "goldberg explain PATH: what this file is, what happens to it, and why", .fn = cmd_explain},
    {.name = "goldberg status", .help = "goldberg status: dirty, running, failed", .fn = cmd_status},
    {.name = "goldberg clean", .help = "goldberg clean --out DIR [--state FILE]: remove what goldberg wrote there, and its state", .fn = cmd_clean},
    {.name = "goldberg learn", .help = "goldberg learn FILE [--dry-run] [--scan]: run FILE's converter once, see what it writes, and declare that in the .berg file that registers it", .fn = cmd_learn},
    {.name = "goldberg touch", .help = "goldberg touch PATH: in a running watch, look at this file again as if it had been saved", .fn = cmd_touch},
    // In a session (goldberg watch, on stdin or the CLI's port) the same verbs without the tool's name.
    {.name = "status", .help = "status: the running goldberg session: dirty, running, failed, warned", .fn = cmd_status},
    {.name = "explain", .help = "explain PATH: what this file is, what happens to it, and why", .fn = cmd_explain},
    {.name = "touch", .help = "touch PATH: look at this file again as if it had been saved", .fn = cmd_touch},
    {.name = "cook", .help = "cook: look at every file again, with the whole of each tick, until stable", .fn = cmd_cook},
    {.name = "berg", .help = "berg FILE: a rule file's values, each with its path (as berg-set takes it)", .fn = cmd_berg},
    {.name = "berg-preview", .help = "berg-preview FILE PATH... VALUE: what setting the value would change, file by file, before anything is saved", .fn = cmd_berg_preview},
    {.name = "berg-set", .help = "berg-set FILE PATH... VALUE: set a rule file's value (JSON) in place, its comments and order kept", .fn = cmd_berg_set},
    {.name = "goldberg replay", .help = "goldberg replay LOG [--dump FILE]: feed a recorded event log to a fresh session, with no file system under it", .fn = cmd_replay},
};
void pump(TickContext& ctx) { if (!ctx.state_as<GoldbergState>().window) pump_body(ctx, false); }

void sink_line(void*, const char* line) { print("%s\n", line); print_flush(); }

// The UI watch's output, passed on a line at a time (and its pipes kept from filling).
void drain_ui_child(GoldbergState& s) {
    if (!s.ui_child_on) return;
    char buf[2048];
    for (i32 n; (n = s.ui_child.read_out(buf, sizeof buf - 1)) > 0;) { buf[n] = 0; print("%s", buf); print_flush(); }
    for (i32 n; (n = s.ui_child.read_err(buf, sizeof buf - 1)) > 0;) { buf[n] = 0; print_err("%s", buf); }
}

// goldberg's own UI watched, so that a save of window.kim reloads the window (asset_live, which
// goldberg links, reads <its tree>/.goldberg-port). The window's session is that watch when its
// output tree is goldberg's own; else a headless child watch on goldberg's roots is started.
void watch_own_ui(GoldbergState& s) {
    char exe_dir[600], path[700];
    if (!platform_exe_dir(exe_dir, sizeof exe_dir)) return;
    format(path, sizeof path, "%sgoldberg.assets", exe_dir);
    IoBytes b;
    if (!io_read_now(path, &b)) return;
    usize n = 0;
    while (n < b.size && n + 1 < sizeof s.ui_tree && b.data[n] != '\n' && b.data[n] != '\r') { s.ui_tree[n] = (char)b.data[n]; ++n; }
    s.ui_tree[n] = 0;
    b.free();
    while (n && s.ui_tree[n - 1] == '/') s.ui_tree[--n] = 0;
    char out[600];
    format(out, sizeof out, "%s", s.out_dir);
    usize o = strlen(out);
    while (o && out[o - 1] == '/') out[--o] = 0;
    if (!strcmp(out, s.ui_tree)) return; // this session cooks goldberg's UI already
    static char roots[700], exe[700];
    format(roots, sizeof roots, "%s../goldberg/goldberg.roots", exe_dir);
    if (!io_is_file(roots)) { print_err("goldberg: no %s: the window will not reload its own screens\n", roots); return; }
    format(exe, sizeof exe, "%sgoldberg", exe_dir);
    static const char* argv[] = {exe, "watch", "--roots", roots, "--quiet"};
    ProcessDesc d;
    d.argv = Span<const char* const>(argv, KV_COUNTOF(argv));
    if (!s.ui_child.spawn(d)) { print_err("goldberg: could not start the watch of its own UI\n"); return; }
    s.ui_child_on = true;
}

void stop_own_ui(GoldbergState& s) {
    if (!s.ui_child_on) return;
    drain_ui_child(s);
    s.ui_child.kill();
    s.ui_child.free();
    s.ui_child_on = false;
    char port[700];
    format(port, sizeof port, "%s/.goldberg-port", s.ui_tree);
    io_remove(port); // the child could not take it away itself
}
void work(TickContext& ctx) {
    GoldbergState& s = ctx.state_as<GoldbergState>();
    if (!s.window) return;
    if (g_window_line[0]) {
        // The command line's request, as tool_main runs it: once, now that the module is up.
        char line[sizeof g_window_line];
        memcpy(line, g_window_line, sizeof line);
        g_window_line[0] = 0;
        if (!cli_run(*ctx.engine, line, sink_line, nullptr)) { s.failed = true; engine_request_quit(*ctx.engine); return; }
        ctx.more();
        return;
    }
    drain_ui_child(s);
    pump_body(ctx, true);
    if (s.active && !s.ui_checked) { s.ui_checked = true; watch_own_ui(s); } // the session is up: its tree is known
}

// The window's presses, to the Lua: goldberg.ui_event(event, node id, value).
void ui_events(TickContext& ctx) {
    GoldbergState& s = ctx.state_as<GoldbergState>();
    if (!s.window || !s.L || s.failed) return;
    for (const KromeEvent& ev : ctx.received<KromeEvent>()) {
        if (ev.screen != s.screen.hash) continue;
        lua_pushstring(s.L, name_str(Name{ev.event}));
        const char* node = ev.node ? name_str(Name{ev.node}) : nullptr;
        if (node) lua_pushstring(s.L, node);
        else lua_pushnil(s.L);
        lua_pushnumber(s.L, ev.value);
        call_app(s, *ctx.engine, "ui_event", 3, 0);
    }
}

// ---- the pilot (--demo) ---------------------------------------------------------------------------

enum class Act : u8 { Stable, Click, ClickLua, Type, TypeLua, Key, Check, Shot, Theme, Wait, Section, EndSection, Quit };
struct Step { Act act; const char* arg; };

// Each page in turn. A Check is goldberg's Lua comparing what the window was sent with what the
// session says (window.lua, M.demo); a ClickLua presses the node the Lua names.
constinit const Step k_pilot[] = {
    {Act::Stable, nullptr},
    {Act::Check, "status"},
    {Act::Shot, "status"},
    {Act::Click, "page_inspect"},
    {Act::Click, "files_filter"},
    {Act::TypeLua, "query"},
    {Act::Wait, nullptr},
    {Act::Check, "files"},
    {Act::ClickLua, "first_file"},
    {Act::Wait, nullptr},
    {Act::Check, "inspect"},
    {Act::Shot, "inspect"},
    {Act::Click, "page_reports"},
    {Act::Wait, nullptr},
    {Act::Check, "reports"},
    {Act::Shot, "reports"},
    // Rules and Diff, where the session has a rule file: a value edited, previewed by its
    // shortcut, the preview checked against berg-preview, accepted, cooked.
    {Act::Section, "has_bergs"},
    {Act::Click, "page_rules"},
    {Act::ClickLua, "first_berg"},
    {Act::Wait, nullptr},
    {Act::Check, "rules"},
    {Act::ClickLua, "rules_input"},
    {Act::Key, "ctrl+a"},
    {Act::TypeLua, "rules_value"},
    {Act::Wait, nullptr},
    {Act::Shot, "rules"},
    {Act::Key, "mod+s"},
    {Act::Wait, nullptr},
    {Act::Check, "diff"},
    {Act::Shot, "diff"},
    {Act::Click, "accept"},
    {Act::Wait, nullptr},
    {Act::Stable, nullptr},
    {Act::Wait, nullptr},
    {Act::Check, "saved"},
    {Act::EndSection, nullptr},
    {Act::Theme, "light"},
    {Act::Click, "page_status"},
    {Act::Wait, nullptr},
    {Act::Shot, "status_light"},
    {Act::Quit, nullptr},
};

struct Pilot { u32 step = 0, wait = 0, phase = 0, settled = 0; f64 since = 0; };
#if defined(__APPLE__)
constexpr Key KV_PLATFORM_MOD_KEY = Key::LGui;
#else
constexpr Key KV_PLATFORM_MOD_KEY = Key::LCtrl;
#endif
Pilot g_pilot;

bool aim(Engine& e, Name screen, const char* id, f32* x, f32* y) {
    rect2 r;
    if (!krome_find_rect(e, screen, id, &r)) return false;
    HostWindow* w = platform_window(e);
    f32 k = w && w->pixel_scale() > 0.0f ? w->pixel_scale() : 1.0f;
    *x = (r.x + r.w * 0.5f) * k;
    *y = (r.y + r.h * 0.5f) * k;
    return true;
}

// goldberg.demo(what) in the Lua: "" when it holds, else what did not.
const char* ask_demo(GoldbergState& s, Engine& e, const char* what) {
    static char answer[400];
    answer[0] = 0;
    lua_getglobal(s.L, "goldberg_demo");
    if (!lua_isfunction(s.L, -1)) { lua_pop(s.L, 1); return "no goldberg_demo in the Lua"; }
    lua_pushstring(s.L, what);
    if (lua_pcall(s.L, 1, 1, 0) != LUA_OK) { format(answer, sizeof answer, "%s", lua_tostring(s.L, -1)); lua_pop(s.L, 1); return answer; }
    format(answer, sizeof answer, "%s", lua_isstring(s.L, -1) ? lua_tostring(s.L, -1) : "");
    lua_pop(s.L, 1);
    (void)e;
    return answer;
}

void fail_demo(const char* step, const char* why) {
    ++g_demo_failures;
    print_err("goldberg: demo: %s: %s\n", step, why);
}

// Phase Frame, main thread, with --demo: a step every few frames, once the screen is built.
void pilot(TickContext& ctx) {
    Engine& e = *ctx.engine;
    GoldbergState& s = ctx.state_as<GoldbergState>();
    if (!g_demo || !s.window || !s.L || !krome_ready(e, s.screen)) return;
    engine_want_frame(e);
    Pilot& p = g_pilot;
    if (p.wait) { --p.wait; return; }
    if (p.step >= KV_COUNTOF(k_pilot)) return;
    const Step& st = k_pilot[p.step];
    f32 x = 0, y = 0;
    switch (st.act) {
    case Act::Stable: {
        if (p.since == 0) p.since = time_now();
        const char* state = krome_model_text(e, s.screen, "state");
        if (!(s.quiet && state && !strcmp(state, "stable"))) {
            if (time_now() - p.since > 60.0) { fail_demo("stable", "the session did not go stable in a minute"); p.since = 0; break; }
            return;
        }
        p.since = 0;
        break;
    }
    case Act::Click:
    case Act::ClickLua: {
        char id[400];
        if (st.act == Act::ClickLua) format(id, sizeof id, "%s", ask_demo(s, e, st.arg));
        else format(id, sizeof id, "%s", st.arg);
        if (!id[0] || !aim(e, s.screen, id, &x, &y)) { fail_demo(st.arg, "nothing to press on the screen"); break; }
        input_pointer_override(e, x, y, p.phase == 1 ? 1 : 0);
        if (++p.phase < 3) { p.wait = 1; return; }
        p.phase = 0;
        break;
    }
    case Act::Type: input_text_override(e, st.arg); break;
    case Act::TypeLua: input_text_override(e, ask_demo(s, e, st.arg)); break;
    case Act::Key: {
        // "ctrl+a", "mod+s" (mod: Command on a Mac, else Ctrl), "enter": held, pressed, let go.
        Key held = Key::Unknown, k = Key::Unknown;
        const char* name = st.arg;
        if (!strncmp(name, "ctrl+", 5)) { held = Key::LCtrl; name += 5; }
        else if (!strncmp(name, "mod+", 4)) { held = KV_PLATFORM_MOD_KEY; name += 4; }
        if (!strcmp(name, "a")) k = Key::A;
        else if (!strcmp(name, "s")) k = Key::S;
        else if (!strcmp(name, "enter")) k = Key::Enter;
        else if (!strcmp(name, "escape")) k = Key::Escape;
        if (p.phase == 0 && held != Key::Unknown) input_key_override(e, held, true);
        if (p.phase == 1) { input_key_override(e, k, true); input_key_override(e, k, false); }
        if (p.phase == 2 && held != Key::Unknown) input_key_override(e, held, false);
        if (++p.phase < 3) { p.wait = 1; return; }
        p.phase = 0;
        break;
    }
    case Act::Section:
        // Its steps only where the Lua says so; else on past its EndSection.
        if (ask_demo(s, e, st.arg)[0]) {
            while (p.step + 1 < KV_COUNTOF(k_pilot) && k_pilot[p.step + 1].act != Act::EndSection) ++p.step;
            print("goldberg: demo: %s: no, so that part is not driven\n", st.arg);
            print_flush();
        }
        break;
    case Act::EndSection: break;
    case Act::Wait: p.wait = 30; break;
    case Act::Check: {
        const char* why = ask_demo(s, e, st.arg);
        if (why[0]) fail_demo(st.arg, why);
        break;
    }
    case Act::Shot:
        if (g_shots) {
            if (!krome_settled(e)) { p.settled = 0; return; }
            if (p.settled++ < 3) return;
            p.settled = 0;
            char path[700];
            format(path, sizeof path, "%s/goldberg_%s.ppm", g_shots, st.arg);
            if (Renderer* r = renderer_get(e)) renderer_screenshot(r, path);
        }
        break;
    case Act::Theme: krome_theme(e, st.arg); break;
    case Act::Quit:
        if (g_demo_failures) print_err("goldberg: demo: %u checks failed\n", g_demo_failures);
        else { print("goldberg: window demo passed\n"); print_flush(); }
        if (s.active) call_app(s, e, "quit", 0, 0);
        engine_request_quit(e);
        break;
    }
    ++p.step;
    p.wait = 6;
}

// The window closed: the session saves its state and the run ends, as `quit` does.
void window_closed(TickContext& ctx, Span<const ExitRequested>) {
    GoldbergState& s = ctx.state_as<GoldbergState>();
    if (s.active && s.L) call_app(s, *ctx.engine, "quit", 0, 0);
    engine_request_quit(*ctx.engine);
}

constinit const SystemDesc k_systems[] = {
    {.name = ".pump", .phase = Phase::PreSim, .fn = pump, .writes = "goldberg.event, goldberg.file, goldberg.berg, goldberg.step", .main_thread = true, .runs = RunWhen::Always},
    {.name = ".work", .phase = Phase::Work, .fn = work, .writes = "goldberg.event, goldberg.file, goldberg.berg, goldberg.step", .main_thread = true},
    {.name = ".ui", .phase = Phase::Sim, .fn = ui_events, .reads = "KromeEvent", .main_thread = true},
    // The Frame cycle, before Krome reads input: one step a frame, so a key held across a press
    // is held in the frame that sees the press (a pass of the loop may run several ticks).
    {.name = ".pilot", .phase = Phase::Frame, .fn = pilot, .before = "krome.pump", .main_thread = true},
};
// Headless (a cook, a watch with no window): no Krome, so nothing reads its events.
constinit const SystemDesc k_headless_systems[] = {
    {.name = ".pump", .phase = Phase::PreSim, .fn = pump, .writes = "goldberg.event, goldberg.file, goldberg.berg, goldberg.step", .main_thread = true, .runs = RunWhen::Always},
};
constinit const HandlerDesc k_handlers[] = {
    {.name = ".exit", KV_ON(ExitRequested, window_closed), .phase = Phase::PreSim},
};

} // namespace

KV_MODULE_DEFINE(goldberg_module) = {
    .name       = "goldberg",
    .components = k_components,
    .kinds      = k_kinds,
    .systems    = k_systems,
    .handlers   = k_handlers,
    .commands   = k_commands,
    .startup    = goldberg_startup,
    .shutdown   = goldberg_shutdown,
    .memory     = {.reserve = 256 * 1024},
};

constinit const ModuleDesc goldberg_headless_module = {
    .name       = "goldberg",
    .components = k_components,
    .kinds      = k_kinds,
    .systems    = k_headless_systems,
    .commands   = k_commands,
    .startup    = goldberg_startup,
    .shutdown   = goldberg_shutdown,
    .memory     = {.reserve = 256 * 1024},
};

// ---- the window's program -----------------------------------------------------------------------

bool goldberg_wants_window(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) if (!strcmp(argv[i], "--window")) return true;
    return false;
}

namespace {
constinit const FrameLoopConfig k_window_config = {
    .title          = "goldberg",
    .width          = 1200,
    .height         = 760,
    .cpu_scale      = 1,       // a tool's window: sharp text on the CPU renderer
    .exit_on_escape = false,   // Escape closes Krome's overlays
    .pacing         = Pacing::OnDemand,
};
} // namespace

// The frame loop under goldberg: the command line (less --window, and goldberg's own options
// before the command, which become the frame loop's where it has them) is one CLI request,
// run when the goldberg module starts, as tool_main runs it.
int goldberg_window_main(int argc, char** argv) {
    const char* loop_argv[16] = {argv[0]};
    int loop_argc = 1;
    int first = 1;
    for (; first < argc && !strncmp(argv[first], "--", 2); ++first) {
        if (!strcmp(argv[first], "--window")) continue;
        if (!strcmp(argv[first], "--cli-port") && first + 1 < argc && loop_argc + 2 < 16) { loop_argv[loop_argc++] = argv[first]; loop_argv[loop_argc++] = argv[++first]; continue; }
        if ((!strcmp(argv[first], "--threads") || !strcmp(argv[first], "--max-ticks")) && first + 1 < argc) { ++first; continue; }
        if (!strcmp(argv[first], "--verbose")) continue; // the tool's, with nothing to do in a window
        if (!strcmp(argv[first], "--demo")) { g_demo = true; continue; }
        if (!strcmp(argv[first], "--shots") && first + 1 < argc) { g_shots = argv[++first]; continue; }
        // The rest are the frame loop's (--screenshot FILE, --size WxH, --renderer cpu, ...).
        static const char* const valued[] = {"--screenshot", "--screenshot-frame", "--screenshot-tick", "--renderer", "--size", "--run-seconds",
                                             "--pacing", "--profile", "--cpu-scale", "--cpu-shaders"};
        bool takes = false;
        for (const char* v : valued) if (!strcmp(argv[first], v)) takes = true;
        if (loop_argc + 2 < 16) {
            loop_argv[loop_argc++] = argv[first];
            if (takes && first + 1 < argc) loop_argv[loop_argc++] = argv[++first];
        }
    }
    if (loop_argc + 1 < 16) loop_argv[loop_argc++] = "--cli-stdin"; // `status`, `explain` and the rest typed at it
    usize at = (usize)format(g_window_line, sizeof g_window_line, "goldberg");
    for (int i = first; i < argc && at < sizeof g_window_line; ++i) {
        if (!strcmp(argv[i], "--window")) continue;
        bool quote = strchr(argv[i], ' ') || strchr(argv[i], '\t') || !argv[i][0];
        at += (usize)format(g_window_line + at, sizeof g_window_line - at, " %s%s%s", quote ? "\"" : "", argv[i], quote ? "\"" : "");
    }
    g_window_mode = true;
    if (g_shots) io_make_dirs(g_shots);
    int code = frameloop_main(loop_argc, (char**)loop_argv, k_window_config);
    return code ? code : g_demo_failures ? 1 : 0;
}

} // namespace kv
