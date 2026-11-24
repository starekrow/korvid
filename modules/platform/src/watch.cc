// Korvid platform: PlatformWatch, the one way an owner says what a waiting loop is woken by
// (module.h). Each kind of thing is turned here into what this OS can do about it: a
// handle for the watching thread (wake_watch.h), a call the thing makes itself, or a time
// to be asked again.
#include "korvid/platform/module.h"
#include "korvid/platform/fs_watch.h"
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

void PlatformWatch::add(KHandle h) {
    if (!h.is_open()) return;
    if (h.kind() == KHandleKind::File) {
        KV_ERROR("platform", "PlatformWatch: a file's handle wakes nothing (a file on a disk can always be read); it is not watched");
        return;
    }
    if (!platform_watches_kind(*engine, h.kind()) || count == KV_COUNTOF(handles)) { soon(); return; }
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
    KHandle h = w.wake_handle();
    if (h.is_open()) add(h);
    else w.set_wake(wake_engine, engine); // where the OS calls instead; false where it cannot, and due() then says soon
    at(w.due());
}

void PlatformWatch::add(const Process& p) {
    if (!p.is_open()) return;
    add(p.out_fd);
    add(p.err_fd);
}

void PlatformWatch::add(IoQueue& q) {
    q.set_wake(wake_engine, engine);
    // The workers call when a task finishes. A read queued or in flight says nothing till
    // the pump collects it, and a queue with no workers runs its stats at the pump too.
    if (q.queued.len || q.in_flight) soon();
}

void platform_watch(Engine& e, const void* owner, const PlatformWatch& w) {
    f64 due = w.due;
    // The thread could not take them all (64 in every owner's sets together, or no thread
    // at all): asked again soon, as a loop that polls would.
    if (!platform_wake_set(e, owner, w.handles, w.count)) {
        f64 soon = time_now() + kPlatformSoon;
        if (due <= 0.0 || soon < due) due = soon;
    }
    if (due > 0.0) engine_want_wake_at(e, due);
}

void platform_unwatch(Engine& e, const void* owner) { platform_wake_set(e, owner, nullptr, 0); }

} // namespace kv
