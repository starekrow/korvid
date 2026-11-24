// Korvid asset_live (README.md): the program's side of goldberg's connection. One socket, polled
// in the Input phase as the CLI's are, so an on-demand window hears a save as soon as it would
// hear a key. Lines down: `changed PATH`, `removed PATH`. One line up, once: `hello NAME CONFIG`.
#include "korvid/asset_live/module.h"
#include "korvid/asset/reload.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/phase.h"
#include "korvid/core/time.h"
#include "korvid/platform/io.h"
#include "korvid/platform/module.h"
#include "korvid/platform/net.h"
#include <stdlib.h>
#include <string.h>

namespace kv {

namespace {

constexpr f64 k_retry = 2.0;

struct LiveState {
    Engine*        e = nullptr;
    bool           off = false;
    u16            fixed_port = 0;          // KV_GOLDBERG=PORT
    char           tree[KV_PLATFORM_PATH_MAX] = {};
    char           name[64] = "program";
    NetSocket      sock;
    bool           said_hello = false;
    f64            next_try = 0.0;
    char           line[1024] = {};
    u32            line_len = 0;
    bool           told_gone = false;
    bool           watched = false;          // an engine_add_watched_waker: its socket stated to the platform
    AssetLiveStats stats;
};

// The port a watch on the program's tree wrote, or 0.
u16 tree_port(const LiveState& s) {
    if (s.fixed_port) return s.fixed_port;
    if (!s.tree[0]) return 0;
    char path[KV_PLATFORM_PATH_MAX + 32];
    format(path, sizeof path, "%s/.goldberg-port", s.tree);
    IoBytes b;
    if (!io_read_file(path, &b)) return 0;
    char text[16] = {};
    memcpy(text, b.data, b.size < sizeof text - 1 ? b.size : sizeof text - 1);
    b.free();
    long p = strtol(text, nullptr, 10);
    return p > 0 && p < 65536 ? (u16)p : 0;
}

void drop(LiveState& s) {
    if (s.sock.fd.is_open()) s.sock.close();
    s.sock.out.free(); // close() keeps the buffer for the next connection; this one is new
    s.sock = NetSocket{};
    s.said_hello = false;
    s.line_len = 0;
    if (s.stats.connected) KV_INFO("asset_live", "goldberg went away: assets reload no more until it is back");
    s.stats.connected = false;
}

void take_line(LiveState& s, char* line) {
    if (!strncmp(line, "changed ", 8)) {
        ++s.stats.changed;
        u32 n = asset_reload(*s.e, line + 8);
        s.stats.reloaded += n;
        if (n) { KV_INFO("asset_live", "goldberg: %s changed", line + 8); engine_want_frame(*s.e); }
    } else if (!strncmp(line, "removed ", 8)) {
        ++s.stats.removed;
        KV_INFO("asset_live", "goldberg: %s left the output tree; what was loaded from it stays", line + 8);
    }
}

// What has arrived read and acted on, the connection made or made again when due. True when
// a line came: the loop draws a frame for it (engine_add_waker), or the pump's tick does.
bool live_poll(LiveState& s) {
    if (s.off) return false;
    u32 heard = s.stats.changed + s.stats.removed;
    f64 now = time_now();
    if (!s.sock.is_open()) {
        if (s.sock.fd.is_open() || s.stats.connected) drop(s);
        if (now < s.next_try) return false;
        s.next_try = now + k_retry;
        u16 port = tree_port(s);
        if (!port) return false;
        if (!net_connect_local(port, &s.sock)) return false;
        s.stats.port = port;
        char hello[160];
        format(hello, sizeof hello, "hello %s %s\n", s.name, KV_DEBUG ? "debug" : "dev");
        s.sock.write_str(hello);
        s.said_hello = true;
    }
    s.sock.flush();
    char buf[2048];
    for (;;) {
        i32 n = s.sock.read(buf, sizeof buf);
        if (n < 0) { drop(s); return true; }
        if (n == 0) break;
        if (!s.stats.connected) {
            s.stats.connected = true;
            ++s.stats.connects;
            KV_INFO("asset_live", "goldberg on port %u: assets reload as it cooks them", s.stats.port);
        }
        for (i32 i = 0; i < n; ++i) {
            char ch = buf[i];
            if (ch == '\n') {
                s.line[s.line_len] = 0;
                if (s.line_len && s.line[s.line_len - 1] == '\r') s.line[s.line_len - 1] = 0;
                take_line(s, s.line);
                s.line_len = 0;
            } else if (s.line_len + 1 < sizeof s.line) {
                s.line[s.line_len++] = ch;
            }
        }
    }
    if (s.sock.closed) drop(s);
    return s.stats.changed + s.stats.removed != heard;
}

// What wakes an idle loop for it next (KR26): its connection, the next try while it has
// none, and soon while what it said is still queued (a connect under way says nothing a
// read would see).
void live_watch(LiveState& s) {
    if (!s.watched) return;
    PlatformWatch w(*s.e);
    w.add(s.sock);
    if (!s.sock.is_open()) w.at(s.next_try);
    platform_watch(*s.e, &s, w);
}

bool live_wake(Engine&, void* user) {
    LiveState& s = *(LiveState*)user;
    bool any = live_poll(s);
    live_watch(s);
    return any;
}

// The Input phase, every tick: the same, for a loop that does not pace on demand.
void live_pump(TickContext& ctx) { live_poll(ctx.state_as<LiveState>()); }

bool live_startup(Engine& e, ModuleInstance& self) {
    LiveState* s = arena_push<LiveState>(self.arena);
    if (!s) return false;
    s->e = &e;
    self.set_state(s);
#if !KV_DEV
    s->off = true; // a ship build never listens for goldberg
#else
    if (const char* env = getenv("KV_GOLDBERG")) {
        if (!strcmp(env, "off") || !strcmp(env, "0") || !strcmp(env, "no")) s->off = true;
        else if (long p = strtol(env, nullptr, 10); p > 0 && p < 65536) s->fixed_port = (u16)p;
    }
    if (!s->off && !s->fixed_port) {
        if (!platform_assets_dir(s->tree, sizeof s->tree)) s->tree[0] = 0;
    }
    // Its name: the output tree's (build/assets/<program>), else "program".
    if (s->tree[0]) {
        usize n = strlen(s->tree);
        while (n && (s->tree[n - 1] == '/' || s->tree[n - 1] == '\\')) s->tree[--n] = 0;
        const char* slash = strrchr(s->tree, '/');
        format(s->name, sizeof s->name, "%s", slash ? slash + 1 : s->tree);
    }
#endif
    if (!s->off) { s->watched = true; engine_add_watched_waker(e, live_wake, s); } // what wakes it: live_watch
    return true;
}

void live_shutdown(Engine& e, ModuleInstance& self) {
    LiveState* s = self.state_as<LiveState>();
    if (!s) return;
    engine_remove_waker(e, live_wake, s);
    if (s->watched) platform_unwatch(e, s);
    if (s->sock.fd.is_open()) s->sock.close();
    s->sock.out.free();
}

constinit const SystemDesc k_systems[] = {
    {.name = ".pump", .phase = Phase::Input, .fn = live_pump, .main_thread = true, .runs = RunWhen::Always},
};

} // namespace

KV_MODULE_DEFINE(asset_live_module) = {
    .name     = "asset_live",
    .systems  = k_systems,
    .startup  = live_startup,
    .shutdown = live_shutdown,
    .memory   = {.reserve = 16 * 1024},
};

AssetLiveStats asset_live_stats(Engine& e) {
    LiveState* s = engine_module_state_as<LiveState>(e, "asset_live");
    return s ? s->stats : AssetLiveStats{};
}

} // namespace kv
