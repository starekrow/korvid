// Korvid platform: the thread that watches sockets for platform_wake_set (KR26), behind a
// seam each OS fills (posix/wake_watch_posix.cc; win32/wake_watch_win32.cc). module.cc owns the state and calls these from the loop's thread.
#pragma once
#include "korvid/platform/module.h"
#include "platform_state.h"
#include "os_handle_table.h"

namespace kv {

// What the watching thread calls when a socket can be read: platform_wake, given the state.
void platform_wake_state(PlatformState& s);

// Under PlatformWatch (watch.cc): the handles `owner` wants the loop woken by, replacing
// what it said before; none (n 0) forgets it. The platform watches every owner's from one
// thread of its own, blocked in poll (WSAPoll on Windows), started when the first handle
// is given, and calls platform_wake when one can be read. False where the sets pass 64
// handles or the thread cannot be had. A handle of a kind this OS cannot watch
// (platform_watches_kind) is an error, logged with its kind: left out, the rest watched,
// and the answer false. PlatformWatch::add asks first, so it never gives one.
bool platform_wake_set(Engine& e, const void* owner, const Handle* handles, u32 n);
// Whether a handle of this kind can be watched here: a socket everywhere; a pipe or a
// notify descriptor on POSIX only (Windows' watch is WSAPoll, which takes sockets).
bool platform_watches_kind(Engine& e, OsKind kind);

bool       wake_watch_supported();
// Whether a handle of this kind can be watched here: wake_watch_set is given only those.
bool       wake_watch_takes(OsKind k);
// Whether the handle has something to read now, asked of the OS without waiting: data, a
// connection to accept, or the end. False for a handle that is not open, and for a kind
// that is never read (a file, a process). Any kind that can be, whether or not the
// watching thread can take it: this is how a pipe on Windows is looked at.
bool       wake_watch_readable(Handle h);
// Null when the OS cannot watch. The thread starts at the first socket given.
WakeWatch* wake_watch_create(PlatformState& s);
void       wake_watch_destroy(WakeWatch* w); // stops and joins the thread
bool       wake_watch_set(WakeWatch* w, const void* owner, const Handle* fds, u32 n);
// Watches again what woke the thread since the last call.
void       wake_watch_rearm(WakeWatch* w);

} // namespace kv
