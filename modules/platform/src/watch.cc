// Korvid platform: PlatformWatch, the one way an owner says what a waiting loop is woken by
// (module.h). Each kind of thing is turned here into what this OS can do about it: a
// handle for the watching thread (wake_watch.h), a call the thing makes itself, or a time
// to be asked again.
#include "korvid/platform/module.h"
#include "korvid/platform/fs_watch.h"
#include "korvid/platform/net.h"
#include "korvid/platform/process.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/time.h"
#include "wake_watch.h"

namespace kv {

namespace {
// What a watcher or a queue calls, on a thread of the OS's or a worker's: the loop's wait ends.
void wake_engine(void* engine) { platform_wake(*(Engine*)engine); }
} // namespace

void PlatformWatch::at(f64 when) {
    if (when > 0.0 && (due <= 0.0 || when < due)) due = when;
}

void PlatformWatch::soon() { at(time_now() + kPlatformSoon); }

void PlatformWatch::add(Handle h) {
    if (!os_handle_open(h)) return;
    if (os_handle_kind(h) == OsKind::File) {
        KV_ERROR("platform", "PlatformWatch: a file's handle wakes nothing (a file on a disk can always be read); it is not watched");
        return;
    }
    if (!platform_watches_kind(*engine, os_handle_kind(h)) || count == KV_COUNTOF(handles)) { soon(); return; }
    handles[count++] = h;
}

void PlatformWatch::add(const NetListener& l) { add(l.fd); }

void PlatformWatch::add(const NetSocket& s) {
    if (!s.is_open()) return;
    add(s.fd);
    if (s.out.len) soon(); // nothing readable says the kernel will take more: flush again shortly
}

void PlatformWatch::add(FsWatcher& w) {
    if (!w.is_open()) return;
    Handle h = w.wake_handle();
    if (os_handle_open(h)) add(h);
    else w.set_wake(wake_engine, engine); // where the OS calls instead; false where it cannot, and due() then says soon
    at(w.due());
}

void PlatformWatch::add(const Process& p) {
    if (!p.is_open()) return;
    add(p.out_fd);
    add(p.err_fd);
    if (p.exited) return; // running() has seen the end: nothing more will come
    // The exit, where the OS gives it as a handle (Windows' process handle, a pidfd, a
    // kqueue): a child that closed its pipes early, or never wrote, still wakes the loop.
    if (os_handle_open(p.process) && platform_watches_kind(*engine, OsKind::Process)) add(p.process);
    else if (!os_handle_open(p.out_fd) && !os_handle_open(p.err_fd)) soon(); // only running() can tell
}

void PlatformWatch::add(IoQueue& q) {
    q.set_wake(wake_engine, engine);
    // The workers call when a task finishes. A read queued or in flight says nothing till
    // the pump collects it, and a queue with no workers runs its stats at the pump too.
    if (q.queued.len || q.in_flight) soon();
}

namespace {

PlatformState* members_of(Engine& e) { return engine_module_state_as<PlatformState>(e, "platform"); }

// Gives the thread what `owner` is watched for now: what it stated and what stands, each
// handle once. A standing handle of a kind this OS cannot watch (a pipe on Windows) is not
// the thread's: platform_members_soon asks about it instead. False as platform_wake_set.
bool members_push(Engine& e, PlatformState& s, const void* owner) {
    Handle list[64];
    u32 n = 0;
    bool all = true;
    for (const PlatformState::WatchMember& m : s.members) {
        if (m.owner != owner || !os_handle_open(m.h)) continue;
        if (m.standing && !platform_watches_kind(e, os_handle_kind(m.h))) continue;
        bool have = false;
        for (u32 i = 0; i < n; ++i) if (list[i] == m.h) have = true;
        if (have) continue;
        if (n == KV_COUNTOF(list)) { all = false; continue; }
        list[n++] = m.h;
    }
    return platform_wake_set(e, owner, list, n) && all;
}

// Takes out `owner`'s members that `drop` says go; true if any did.
template <class F>
bool members_drop(PlatformState& s, const void* owner, F drop) {
    bool any = false;
    for (u32 i = 0; i < s.members.len;) {
        if (s.members.data[i].owner == owner && drop(s.members.data[i])) {
            s.members.data[i] = s.members.data[--s.members.len];
            any = true;
        } else {
            ++i;
        }
    }
    return any;
}

void member_add(Engine& e, const void* owner, Handle h) {
    PlatformState* s = members_of(e);
    if (!s || !os_handle_open(h)) return;
    for (const PlatformState::WatchMember& m : s->members) if (m.owner == owner && m.h == h && m.standing) return;
    s->members.push(PlatformState::WatchMember{owner, h, true});
    if (!members_push(e, *s, owner)) engine_want_wake_at(e, time_now() + kPlatformSoon);
}

void member_remove(Engine& e, const void* owner, Handle h) {
    PlatformState* s = members_of(e);
    if (!s) return;
    if (members_drop(*s, owner, [&](const PlatformState::WatchMember& m) { return m.standing && m.h == h; })) members_push(e, *s, owner);
}

} // namespace

void platform_watch(Engine& e, const void* owner, const PlatformWatch& w) {
    f64 due = w.due;
    PlatformState* s = members_of(e);
    bool taken = false;
    if (s) {
        // What it stated before goes; what stands stays.
        members_drop(*s, owner, [](const PlatformState::WatchMember& m) { return !m.standing; });
        for (u32 i = 0; i < w.count; ++i) s->members.push(PlatformState::WatchMember{owner, w.handles[i], false});
        taken = members_push(e, *s, owner);
    }
    // The thread could not take them all (64 in every owner's sets together, or no thread
    // at all): asked again soon, as a loop that polls would.
    if (!taken) {
        f64 soon = time_now() + kPlatformSoon;
        if (due <= 0.0 || soon < due) due = soon;
    }
    if (due > 0.0) engine_want_wake_at(e, due);
}

void platform_unwatch(Engine& e, const void* owner) {
    if (PlatformState* s = members_of(e)) members_drop(*s, owner, [](const PlatformState::WatchMember&) { return true; });
    platform_wake_set(e, owner, nullptr, 0);
}

void platform_watch_add(Engine& e, const void* owner, Handle h) { member_add(e, owner, h); }
void platform_watch_add(Engine& e, const void* owner, const NetListener& l) { member_add(e, owner, l.fd); }
void platform_watch_add(Engine& e, const void* owner, const NetSocket& sock) { member_add(e, owner, sock.fd); }
void platform_watch_add(Engine& e, const void* owner, const Process& p) {
    member_add(e, owner, p.out_fd);
    member_add(e, owner, p.err_fd);
    member_add(e, owner, p.process); // its exit, where the OS gives a handle for it
}

void platform_watch_remove(Engine& e, const void* owner, Handle h) { member_remove(e, owner, h); }
void platform_watch_remove(Engine& e, const void* owner, const NetListener& l) { member_remove(e, owner, l.fd); }
void platform_watch_remove(Engine& e, const void* owner, const NetSocket& sock) { member_remove(e, owner, sock.fd); }
void platform_watch_remove(Engine& e, const void* owner, const Process& p) {
    member_remove(e, owner, p.out_fd);
    member_remove(e, owner, p.err_fd);
    member_remove(e, owner, p.process);
}

bool platform_members_soon(Engine& e, PlatformState& s) {
    bool soon = false;
    for (u32 i = 0; i < s.members.len;) {
        const PlatformState::WatchMember m = s.members.data[i];
        if (!m.standing) { ++i; continue; }
        if (!os_handle_open(m.h)) {
            // Closed: it stands no more, and its owner's set goes to the thread again
            // without it, so its place there is free for another.
            s.members.data[i] = s.members.data[--s.members.len];
            members_push(e, s, m.owner);
            continue;
        }
        OsKind kind = os_handle_kind(m.h);
        if (!platform_watches_kind(e, kind)) soon = true;                               // a pipe on Windows
        else if (kind == OsKind::Socket && os_handle_flag(m.h, kOsQueued)) soon = true; // output still to send
        ++i;
    }
    return soon;
}

} // namespace kv
