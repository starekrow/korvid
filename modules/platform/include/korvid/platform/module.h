// Korvid platform: module record, state and services. The operating system as a tool or a
// game sees it with no display and no devices: the file read queue and its mounts, the
// executable's and the user's directories, and sockets (net.h). No SDL and no external
// library; the window, input and gamepads are platform_hid's, the audio device
// platform_audio's, so a tool links this and nothing heavy.
// The module has one system, "platform.io_idle" in the Work cycle (docs/design/cycles.md),
// which serves the I/O queue's idle reads and whatever the frame's pumps held back under
// their byte budget, in the time the cycle has left.
#pragma once
#include "korvid/core/module.h"
#include "korvid/platform/io.h"
#include "korvid/platform/paths.h"
#include "korvid/core/os_thread.h"
#include "korvid/platform/net.h"

namespace kv {

struct Engine;

KV_MODULE_DECLARE(platform_module);

constexpr u32 KV_PLATFORM_PATH_MAX  = 512;
constexpr u32 KV_PLATFORM_PUMP_MAX  = 8;

// Called from platform_pump before the I/O queue's own pump: a device module (the window)
// turns its OS events into bus events there.
typedef void (*PlatformPumpFn)(Engine& e, void* user);

// A device module's wait (the window's), for platform_wait. `wait` blocks till its OS has
// something for the next pump, `wake` is called, or `seconds` pass, and takes nothing off
// any queue: 1 when it ended early, 0 when the time was up, -1 when it cannot wait now
// (no window open), and the platform waits on its own. `wake` ends a wait under way or
// the next to begin, from any thread.
struct PlatformWaiter {
    int  (*wait)(void* user, f64 seconds) = nullptr;
    void (*wake)(void* user) = nullptr;
    void* user = nullptr;
};

// What ended each platform_wait, counted from the start.
struct PlatformWaits {
    u64 waits = 0;   // every platform_wait given time to wait
    u64 window = 0;  // the window had something
    u64 woken = 0;   // platform_wake
    u64 timed = 0;   // the time was up
};

// Where read_asset looks, set before engine_init (the frame loop's --pack and --assets).
// At startup the platform mounts, in this order, so later ones win: <exe dir>/assets/ if
// it exists; <exe dir>/<exe name>.kpak if it exists; each of `packs`; `assets_dir`.
struct PlatformMounts {
    const char* exe_name = nullptr;     // for the default pack's name; null: none looked for
    const char* packs[8] = {};
    u32         pack_count = 0;
    const char* assets_dir = nullptr;
    bool        none = false;           // mount nothing, not even <exe dir>/assets/ (a tool)
    IoBackend   io_backend = IoBackend::Auto; // the frame loop's --io sync|ring
};
void platform_configure_mounts(const PlatformMounts& mounts);

// Once per frame: the pump hooks run (the window's events become bus events and raw
// input), then the I/O queue reaps its completions.
void platform_pump(Engine& e);
// Adds or removes a pump hook. A module adds one at startup and removes it at shutdown.
bool platform_pump_add(Engine& e, PlatformPumpFn fn, void* user);
void platform_pump_remove(Engine& e, PlatformPumpFn fn, void* user);

// Blocks the calling thread till the window has something for the pump, platform_wake is
// called, or `seconds` pass, whichever is first (KR26). It takes nothing off any queue:
// the pump after it reads what came. With no window it waits on a condition. A frame
// loop with nothing to do waits here between passes. No time (0 or less) is no wait: it
// returns Time at once and leaves a wake for the next.
enum class PlatformWoken : u8 { Time, Window, Wake };
PlatformWoken platform_wait(Engine& e, f64 seconds);
// Ends the wait under way, or the next to begin. Any thread; not a signal handler.
void platform_wake(Engine& e);
// How long a loop with nothing to do may sleep in platform_wait, `most` at the longest:
// the one answer the frame loop and a tool's loop both wait by. No later than the earliest
// time a waker asked for (engine_want_wake_at, PlatformWatch::at), and no longer than
// kPlatformSoon while something can only be found by asking: a waker the platform does
// not watch (engine_add_waker), or a read the queue's pump has yet to hand over or collect
// (the queue's task workers wake the loop themselves). 0 or less: do not wait.
f64 platform_idle_time(Engine& e, f64 most);
// The window's wait: platform_hid sets it at startup and clears it (null) at shutdown.
void platform_set_waiter(Engine& e, const PlatformWaiter* w);
// What ended the waits so far: for a loop's last line, and a measurement.
PlatformWaits platform_waits(Engine& e);

// ---- What a waiting loop is woken by ----------------------------------------------------
// One way to say it, whatever the thing is and whatever the OS can do about it (KR26). Two
// ways to hand it over. A standing watch (platform_watch_add, below): each thing added
// once, watched until it closes, no waker. Or a stated one: an owner (a module with a
// waker: engine_add_watched_waker, the owner its `user`) fills a PlatformWatch with what it
// is waiting on each time its waker is asked, and hands it over:
//
//     PlatformWatch w(e);
//     w.add(listener);  w.add(sock);  w.add(watcher);  w.add(child);  w.add(queue);
//     w.at(next_retry);                      // and a time, if it has one
//     platform_watch(e, &me, w);             // replaces what `me` stated before
//
// and the loop sleeps until one of them has something. How each is watched is the
// platform's business, not the owner's:
//   a socket, a listener   the platform's watching thread (poll; WSAPoll on Windows)
//   an FsWatcher           inotify's descriptor on that thread, or the OS's own call
//                          (ReadDirectoryChangesW, FSEvents); a polled root, by its next walk
//   a Process's pipes      that thread where a pipe can be polled (POSIX); elsewhere asked
//                          again in kPlatformSoon seconds
//   a Process's exit       that thread, as a pidfd (Linux, Android) or a kqueue (Apple); a
//                          wait of the thread pool's on Windows. Heard once.
//   an IoQueue             its task workers wake the loop as each finishes; reads in
//                          flight are asked for again soon (the pump collects them)
//   a time                 engine_want_wake_at
// What cannot be watched on an OS is asked again soon instead: the owner's code is the
// same, and only how promptly it hears differs. The platform watches every owner's
// handles from one thread, started when the first is given, at most 64 in all; past that,
// soon. A handle that woke the loop is watched again at the next platform_wait (after
// the pass that should have read it), so one nobody drains does not spin the thread.
struct NetListener;
struct NetSocket;
struct FsWatcher;
struct Process;
// How long "soon" is: one pass of a loop that polls, 250 a second. The wait keeps to it on
// every OS (os_event_wait).
constexpr f64 kPlatformSoon = 0.004;

struct PlatformWatch {
    explicit PlatformWatch(Engine& e) : engine(&e) {}
    void add(const NetListener& l);  // a connection waiting to be accepted
    void add(const NetSocket& s);    // bytes to read; and soon, while what was written is still queued
    void add(FsWatcher& w);          // news for its poll(): sets the watcher's wake, and takes its due()
    // Output to read on either pipe, a pipe's end, and the child's exit, where the OS gives a
    // handle for it (every desktop OS; Linux from 5.3). Where it gives none, soon once both
    // pipes have ended, since only running() can tell then. Nothing once running() has
    // seen the end.
    void add(const Process& p);
    void add(IoQueue& q);            // a completion to take: sets the queue's wake
    // A handle of the owner's own. Watched where this OS can watch its kind; asked again
    // soon where it cannot (a pipe on Windows). A file on a disk is always readable and
    // wakes nothing anywhere: an error, logged.
    void add(Handle h);
    void at(f64 when);               // no later than `when` (time_now's clock)
    void soon();                     // no later than kPlatformSoon from now

    Engine* engine;
    Handle handles[64];
    u32     count = 0;
    f64     due   = 0.0;             // the earliest time asked for; 0: none
};
// States what `owner` is waiting on, replacing what it said before (its standing members,
// below, stay). From the owner's waker, on the loop's thread.
void platform_watch(Engine& e, const void* owner, const PlatformWatch& w);
// Forgets all of it, stated and standing: at the owner's shutdown, before what it watched
// is closed.
void platform_unwatch(Engine& e, const void* owner);

// ---- A standing watch -------------------------------------------------------------------
// What `owner` waits on until it closes or the owner takes it back: added once, with no
// waker and nothing stated again each pass. A listener, a socket, a child (its pipes and
// its exit) or a handle of the owner's own. What it is waiting for wakes a waiting loop as
// a stated watch's would, and the tick that follows is the owner's systems' to read it in:
// a tool that waits, or a game paced on demand, needs only these and a system.
//
//     s.listener.open(port);   platform_watch_add(e, &s, s.listener);
//     ...accepted:             s.sock.adopt(fd);  platform_watch_add(e, &s, s.sock);
//     ...started:              s.child.spawn(d);  platform_watch_add(e, &s, s.child);
//     ...at shutdown:          platform_unwatch(e, &s);  then close them
//
// Closing a thing ends its membership: its handle goes stale and is let go before the next
// wait, so a closed socket's place is free for another. What the platform cannot watch on
// an OS (a pipe on Windows) and a socket with output still queued are asked about again
// in kPlatformSoon, as a stated watch's are. Members share the 64 handles of every owner's
// sets. A stated watch (platform_watch) is for an owner whose set is worked out afresh
// each pass, or that wants a time (PlatformWatch::at); an owner may have both. Loop's thread.
void platform_watch_add(Engine& e, const void* owner, const NetListener& l);
void platform_watch_add(Engine& e, const void* owner, const NetSocket& s);
void platform_watch_add(Engine& e, const void* owner, const Process& p);
void platform_watch_add(Engine& e, const void* owner, Handle h);
// Takes one back before it closes; nothing if it is not a member.
void platform_watch_remove(Engine& e, const void* owner, const NetListener& l);
void platform_watch_remove(Engine& e, const void* owner, const NetSocket& s);
void platform_watch_remove(Engine& e, const void* owner, const Process& p);
void platform_watch_remove(Engine& e, const void* owner, Handle h);

} // namespace kv
