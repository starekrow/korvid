// Korvid platform: the thread that watches sockets for platform_wake_set (KR26), behind a
// seam each OS fills (posix/wake_watch_posix.cc; win32/wake_watch_win32.cc, which cannot
// yet). module.cc owns the state and calls these from the loop's thread.
#pragma once
#include "korvid/platform/module.h"

namespace kv {

// What the watching thread calls when a socket can be read: platform_wake, given the state.
void platform_wake_state(PlatformState& s);

bool       wake_watch_supported();
// Null when the OS cannot watch. The thread starts at the first socket given.
WakeWatch* wake_watch_create(PlatformState& s);
void       wake_watch_destroy(WakeWatch* w); // stops and joins the thread
bool       wake_watch_set(WakeWatch* w, const void* owner, const NetFd* fds, u32 n);
// Watches again what woke the thread since the last call.
void       wake_watch_rearm(WakeWatch* w);

} // namespace kv
