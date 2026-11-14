// goldberg: the host. Everything the Lua application cannot do for itself: the pools its
// records live in, the sources that feed the event queue (the scan, the watchers, hash
// results), hashing on the job pool, and the pump that hands each tick's events to Lua
// in order. What an event means, and everything that follows from it, is Lua's
// (lua/main.lua and what it requires).
//
// One system, goldberg.pump, each tick:
//   1. a command the CLI left is handed to Lua (commands never touch the pools: the CLI
//      runs them in its own system, so they leave a request and the pump carries it out);
//   2. the sources are polled into the pending list: watcher events, finished hashes,
//      the scan's files;
//   3. pending events are folded (one per path) and moved into goldberg.event rows;
//   4. the rows go to Lua in sequence order, and are destroyed;
//   5. Lua is told whether the host has anything left (goldberg.tick), and decides
//      whether the session is stable.
#include "korvid/goldberg/module.h"
#include "korvid/core/array.h"
#include "korvid/core/cli.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/glob.h"
#include "korvid/core/job.h"
#include "korvid/core/log.h"
#include "korvid/core/sha256.h"
#include "korvid/core/sort.h"
#include "korvid/core/time.h"
#include "korvid/platform/fs_watch.h"
#include "korvid/platform/io.h"
#include "korvid/script/script.h"
#include "korvid/tool/tool.h"
#include <stdlib.h>
#include <string.h>

namespace kv {

constexpr u32 kGbPath = 200;

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

enum GbEv : u8 { EvSeen, EvAdded, EvChanged, EvRemoved, EvDirAdded, EvDirRemoved, EvRescan, EvScanDone, EvHashed, EvHashFailed, EvCookDone, EvCookFailed, EvCount };
constexpr const char* kEvNames[EvCount] = {"file-seen", "file-added", "file-changed", "file-removed", "dir-added", "dir-removed",
                                           "rescan", "scan-done", "hashed", "hash-failed", "cook-done", "cook-failed"};

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

struct HashReq {
    u32   root;
    char  path[kGbPath];
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
    u32  root;
    char source[kGbPath];
    char output[kGbPath + 64];
    bool ok;
    u64  size;
    i64  mtime;
    u8   hash[32];
    char error[200];
};

struct ScanItem { u32 root; u32 path; u64 size; i64 mtime; bool end; };

struct Text { char* data; u32 len; };

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
    Array<HashReq>  hashes;              // asked for, not yet run
    Array<CopyReq>  copies;              // likewise
    char            out_dir[600] = {};   // the output tree
    char            attic[700] = {};     // where what leaves it goes: <out>/.goldberg.attic/<stamp>
    u32             tmp_serial = 0;
    Array<ScanItem> scan;                // files a scan found, not yet events
    Array<char>     scan_text;
    u32             scan_at = 0;
    // The queue.
    Array<Pending>  pending;
    Array<char>     pending_text;
    Array<Text>     texts;               // texts of the events now in rows
    u32             seq = 0;
    Table*          events = nullptr;
};

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

// ---- hashing: one batch a tick, on the job pool ---------------------------------------------

void hash_job(u32, u32 begin, u32 end, void* user) {
    GoldbergState& s = *(GoldbergState*)user;
    for (u32 i = begin; i < end; ++i) {
        HashReq& r = s.hashes.data[i];
        char full[800];
        format(full, sizeof full, "%s/%s", s.roots.data[r.root].dir, r.path);
        IoBytes bytes;
        r.ok = io_read_now(full, &bytes);
        if (!r.ok) continue;
        sha256(bytes.data, bytes.size, r.hash);
        r.size = bytes.size;
        if (r.want_text) {
            r.text = (char*)mem_alloc(bytes.size + 1);
            memcpy(r.text, bytes.data, bytes.size);
            r.text[bytes.size] = 0;
            r.text_len = (u32)bytes.size;
        }
        bytes.free();
    }
}

void run_hashes(GoldbergState& s) {
    if (!s.hashes.len) return;
    // The modification time is read before the bytes: a write in between leaves a time
    // that is older than the content, and the next event for the file asks again.
    for (HashReq& r : s.hashes) {
        char full[800];
        format(full, sizeof full, "%s/%s", s.roots.data[r.root].dir, r.path);
        IoStat st;
        r.mtime = io_stat(full, &st) ? st.mtime_ns : 0;
    }
    jobs_run(s.hashes.len, 4, hash_job, &s, "goldberg.hash");
    for (HashReq& r : s.hashes) {
        if (r.ok) push_pending(s, EvHashed, r.root, r.path, r.size, r.mtime, r.hash, r.text, r.text_len);
        else push_pending(s, EvHashFailed, r.root, r.path);
    }
    s.hashes.len = 0;
}

// ---- copying: one batch a tick, on the job pool ----------------------------------------------

void parent_dir(const char* path, char* out, usize cap) {
    format(out, cap, "%s", path);
    char* slash = strrchr(out, '/');
    if (slash) *slash = 0; else out[0] = 0;
}

void copy_job(u32 chunk, u32 begin, u32 end, void* user) {
    GoldbergState& s = *(GoldbergState*)user;
    for (u32 i = begin; i < end; ++i) {
        CopyReq& r = s.copies.data[i];
        char from[800], to[900], tmp[960], dir[900];
        format(from, sizeof from, "%s/%s", s.roots.data[r.root].dir, r.source);
        format(to, sizeof to, "%s/%s", s.out_dir, r.output);
        format(tmp, sizeof tmp, "%s.tmp-%u-%u", to, s.tmp_serial, i);
        IoBytes bytes;
        i32 err = 0;
        if (!io_read_now(from, &bytes, nullptr, &err)) { format(r.error, sizeof r.error, "cannot read %s (errno %d)", from, err); continue; }
        sha256(bytes.data, bytes.size, r.hash);
        parent_dir(to, dir, sizeof dir);
        bool ok = (!dir[0] || io_make_dirs(dir)) && io_write_now(tmp, bytes.data, bytes.size, &err);
        bytes.free();
        if (!ok) { format(r.error, sizeof r.error, "cannot write %s (errno %d)", tmp, err); io_remove(tmp); continue; }
        // Whole, or not at all: a reader of the output tree never sees half a file.
        if (!io_rename(tmp, to)) { format(r.error, sizeof r.error, "cannot rename %s into place", tmp); io_remove(tmp); continue; }
        IoStat st;
        if (io_stat(to, &st)) { r.size = st.size; r.mtime = st.mtime_ns; }
        r.ok = true;
    }
    (void)chunk;
}

void run_copies(GoldbergState& s) {
    if (!s.copies.len) return;
    ++s.tmp_serial;
    jobs_run(s.copies.len, 2, copy_job, &s, "goldberg.copy");
    for (CopyReq& r : s.copies) {
        if (r.ok) push_pending(s, EvCookDone, r.root, r.output, r.size, r.mtime, r.hash);
        else {
            u32 n = (u32)strlen(r.error);
            char* text = (char*)mem_alloc(n + 1);
            memcpy(text, r.error, n + 1);
            push_pending(s, EvCookFailed, r.root, r.output, 0, 0, nullptr, text, n);
        }
    }
    s.copies.len = 0;
}

// ---- the scan ----------------------------------------------------------------------------------

struct ScanCtx { GoldbergState* s; u32 root; };
bool scan_one(void* user, const char* relative) {
    ScanCtx* c = (ScanCtx*)user;
    GoldbergState& s = *c->s;
    char full[800];
    format(full, sizeof full, "%s/%s", s.roots.data[c->root].dir, relative);
    ScanItem it{};
    it.root  = c->root;
    it.path  = s.scan_text.len;
    IoStat st;
    if (io_stat(full, &st)) { it.size = st.size; it.mtime = st.mtime_ns; }
    u32 n = (u32)strlen(relative);
    memcpy(s.scan_text.push_n(n + 1), relative, n + 1);
    s.scan.push(it);
    return true;
}

void scan_root(GoldbergState& s, u32 root) {
    ScanCtx c{&s, root};
    io_list_files(s.roots.data[root].dir, scan_one, &c); // hidden files left out, as the copy left them
    ScanItem end{};
    end.root = root;
    end.path = s.scan_text.len;
    end.end  = true;
    s.scan_text.push(0);
    s.scan.push(end);
}

// ---- the queue -----------------------------------------------------------------------------------

bool is_fs_kind(u8 k) { return k == EvSeen || k == EvAdded || k == EvChanged || k == EvRemoved; }

// One event per path: of several, what the last leaves true. Anything ending in a removal
// is a removal; anything else that includes an arrival is an arrival; otherwise a change.
void fold_pending(GoldbergState& s) {
    for (u32 i = s.pending.len; i-- > 0;) {
        Pending& later = s.pending.data[i];
        if (later.dead || !is_fs_kind(later.kind)) continue;
        const char* path = s.pending_text.data + later.path;
        for (u32 j = i; j-- > 0;) {
            Pending& earlier = s.pending.data[j];
            if (earlier.dead) continue;
            // A directory event or a rescan between them is a boundary: do not fold across it.
            if (earlier.kind == EvDirAdded || earlier.kind == EvDirRemoved || earlier.kind == EvRescan || earlier.kind == EvScanDone) break;
            if (!is_fs_kind(earlier.kind) || earlier.root != later.root || strcmp(s.pending_text.data + earlier.path, path) != 0) continue;
            if (later.kind != EvRemoved && (earlier.kind == EvAdded || earlier.kind == EvSeen)) later.kind = earlier.kind == EvSeen && later.kind == EvSeen ? EvSeen : EvAdded;
            earlier.dead = true;
        }
    }
}

void fill_rows(GoldbergState& s) {
    fold_pending(s);
    u32 taken = 0;
    for (; taken < s.pending.len; ++taken) {
        Pending& p = s.pending.data[taken];
        if (p.dead) { if (p.text) mem_free(p.text); continue; }
        Handle h = s.events->spawn();
        if (h.is_null()) break; // the pool is full: the rest wait a tick
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
        if (p.text) { s.texts.push(Text{p.text, p.text_len}); e->text = s.texts.len; }
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
    lua_pushstring(L, s.watcher.backend_name());
    return 1;
}

int host_hash(lua_State* L) {
    GoldbergState& s = *state_of(L);
    u32 at;
    check_root(L, 1, &at);
    const char* path = luaL_checkstring(L, 2);
    if (strlen(path) >= kGbPath) return luaL_error(L, "'%s': a path of %d bytes or more", path, (int)kGbPath);
    HashReq r{};
    r.root = at;
    format(r.path, sizeof r.path, "%s", path);
    r.want_text = lua_toboolean(L, 3);
    s.hashes.push(r);
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
    CopyReq r{};
    if (!s.out_dir[0]) return luaL_error(L, "copy: there is no output tree");
    if (strlen(source) >= sizeof r.source || strlen(output) >= sizeof r.output) return luaL_error(L, "copy: '%s': a path too long", output);
    r.root = at;
    format(r.source, sizeof r.source, "%s", source);
    format(r.output, sizeof r.output, "%s", output);
    s.copies.push(r);
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
int host_now(lua_State* L) { lua_pushnumber(L, time_now()); return 1; }
int host_cwd(lua_State* L) {
    char cwd[1024];
    if (!io_current_dir(cwd, sizeof cwd)) return luaL_error(L, "the current directory cannot be read");
    lua_pushstring(L, cwd);
    return 1;
}

// host.active(on): whether the pump should keep ticking a session.
int host_active(lua_State* L) { state_of(L)->active = lua_toboolean(L, 1); return 0; }

int host_finish(lua_State* L) {
    GoldbergState& s = *state_of(L);
    s.active = false;
    lua_getfield(L, LUA_REGISTRYINDEX, "goldberg.engine");
    Engine* e = (Engine*)lua_touserdata(L, -1);
    print_flush();
    tool_finish(*e, (int)luaL_optinteger(L, 1, 0));
    return 0;
}

constexpr luaL_Reg k_host[] = {
    {"add_root", host_add_root}, {"scan", host_scan}, {"watch", host_watch}, {"hash", host_hash},
    {"set_out", host_set_out}, {"copy", host_copy}, {"remove_output", host_remove_output}, {"write_atomic", host_write_atomic},
    {"glob", host_glob}, {"glob_path", host_glob_path}, {"print", host_print}, {"warn", host_warn},
    {"now", host_now}, {"cwd", host_cwd}, {"active", host_active}, {"finish", host_finish}, {nullptr, nullptr},
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
    if (ev.kind == EvSeen || ev.kind == EvHashed || ev.kind == EvCookDone) {
        lua_pushinteger(L, (lua_Integer)ev.size);
        lua_setfield(L, -2, "size");
        lua_pushinteger(L, ev.mtime);
        lua_setfield(L, -2, "mtime");
    }
    if (ev.kind == EvCookFailed && ev.text) {
        const Text& t = s.texts.data[ev.text - 1];
        lua_pushlstring(L, t.data, t.len);
        lua_setfield(L, -2, "text");
    }
    if (ev.kind == EvCookDone) {
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
            const Text& t = s.texts.data[ev.text - 1];
            lua_pushlstring(L, t.data, t.len);
            lua_setfield(L, -2, "text");
        }
    }
}

void pump(TickContext& ctx) {
    Engine& e = *ctx.engine;
    GoldbergState& s = ctx.state_as<GoldbergState>();
    if (s.failed) return;
    s.events = ctx_kind(ctx, "goldberg.event", true);
    ctx_kind(ctx, "goldberg.file", true);
    ctx_kind(ctx, "goldberg.berg", true);
    ctx_kind(ctx, "goldberg.step", true);

    // 1. A command.
    if (s.command_words) {
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

    // 2. The sources.
    if (s.watcher.is_open()) {
        for (const FsEvent& ev : s.watcher.poll()) {
            u32 root = ~0u;
            for (u32 i = 0; i < s.roots.len; ++i) if (s.roots.data[i].watch == ev.root) root = i;
            if (root == ~0u) continue;
            u8 kind = ev.kind == FsEventKind::Added ? EvAdded : ev.kind == FsEventKind::Changed ? EvChanged : ev.kind == FsEventKind::Removed ? EvRemoved
                    : ev.kind == FsEventKind::DirAdded ? EvDirAdded : ev.kind == FsEventKind::DirRemoved ? EvDirRemoved : EvRescan;
            if (strlen(ev.path) >= kGbPath) { print_err("goldberg: '%s/%s': a path too long to track\n", s.roots.data[root].dir, ev.path); continue; }
            push_pending(s, kind, root, ev.path);
        }
    }
    run_hashes(s);
    run_copies(s);
    // The scan's files, a pool's worth at a time.
    u32 room = s.events->capacity - s.events->live();
    for (; s.scan_at < s.scan.len && s.pending.len < room; ++s.scan_at) {
        const ScanItem& it = s.scan.data[s.scan_at];
        const char* path = s.scan_text.data + it.path;
        if (it.end) push_pending(s, EvScanDone, it.root, "");
        else if (strlen(path) >= kGbPath) print_err("goldberg: '%s/%s': a path too long to track\n", s.roots.data[it.root].dir, path);
        else push_pending(s, EvSeen, it.root, path, it.size, it.mtime);
    }
    if (s.scan_at == s.scan.len) { s.scan.len = 0; s.scan_text.len = 0; s.scan_at = 0; }

    // 3 and 4. Into rows, and to Lua in order.
    fill_rows(s);
    u32 delivered = 0;
    if (s.events->live()) {
        Array<Handle> order{};
        for (u32 i = 0; i < s.events->rows(); ++i) if (s.events->is_alive(i)) order.push(s.events->handle_at(i));
        SeqCmp cmp{s.events};
        sort(order.data, order.len, sizeof(Handle), seq_cmp, &cmp);
        for (Handle h : order) {
            if (!s.failed) {
                push_event(L, s, *s.events->get<GbEvent>(h));
                if (call_app(s, e, "event", 1, 0)) ++delivered;
            }
            s.events->destroy(h);
        }
        order.free();
        for (Text& t : s.texts) mem_free(t.data);
        s.texts.len = 0;
    }
    if (s.failed) return;

    // 5. Is anything left? The watchers are asked once more before the host says no.
    bool quiet = !s.pending.len && !s.hashes.len && !s.copies.len && !s.scan.len && !delivered;
    if (quiet && s.watcher.is_open()) {
        for (const FsEvent& ev : s.watcher.poll(true)) {
            u32 root = ~0u;
            for (u32 i = 0; i < s.roots.len; ++i) if (s.roots.data[i].watch == ev.root) root = i;
            if (root == ~0u || strlen(ev.path) >= kGbPath) continue;
            u8 kind = ev.kind == FsEventKind::Added ? EvAdded : ev.kind == FsEventKind::Changed ? EvChanged : ev.kind == FsEventKind::Removed ? EvRemoved
                    : ev.kind == FsEventKind::DirAdded ? EvDirAdded : ev.kind == FsEventKind::DirRemoved ? EvDirRemoved : EvRescan;
            push_pending(s, kind, root, ev.path);
            quiet = false;
        }
    }
    lua_pushboolean(L, quiet);
    call_app(s, e, "tick", 1, 0);
    if (s.active && quiet) time_sleep(0.002); // nothing to do this tick: do not spin
}

// Every verb is one command: it leaves its words for the pump.
void cmd_any(CliContext& ctx, Span<const char* const> args, const char* verb) {
    GoldbergState* s = state_of(*ctx.engine);
    if (!s) { ctx.fail("the goldberg module is not up"); return; }
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

bool goldberg_startup(Engine&, ModuleInstance& self) {
    GoldbergState* s = arena_push<GoldbergState>(self.arena);
    if (!s) return false;
    s->general = &self.general;
    find_lua_dir(*s);
    self.set_state(s);
    return true;
}

void goldberg_shutdown(Engine&, ModuleInstance& self) {
    GoldbergState* s = self.state_as<GoldbergState>();
    if (!s) return;
    if (s->L) lua_close(s->L);
    s->watcher.free();
    for (Pending& p : s->pending) if (p.text) mem_free(p.text);
    for (HashReq& r : s->hashes) if (r.text) mem_free(r.text);
    for (Text& t : s->texts) mem_free(t.data);
    s->command.free();
    s->roots.free();
    s->hashes.free();
    s->copies.free();
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
    {.name = "goldberg replay", .help = "goldberg replay LOG [--dump FILE]: feed a recorded event log to a fresh session, with no file system under it", .fn = cmd_replay},
};
constinit const SystemDesc k_systems[] = {
    {.name = ".pump", .phase = Phase::PreSim, .fn = pump, .writes = "goldberg.event, goldberg.file, goldberg.berg, goldberg.step", .main_thread = true, .runs = RunWhen::Always},
};

} // namespace

KV_MODULE_DEFINE(goldberg_module) = {
    .name       = "goldberg",
    .components = k_components,
    .kinds      = k_kinds,
    .systems    = k_systems,
    .commands   = k_commands,
    .startup    = goldberg_startup,
    .shutdown   = goldberg_shutdown,
    .memory     = {.reserve = 256 * 1024},
};

} // namespace kv
