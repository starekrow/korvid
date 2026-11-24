// Korvid platform: Windows does not watch sockets for platform_wake_set yet (slice 12,
// milestone 4: WSAPoll on a thread of its own). platform_watches says false, so a waker
// registers as one the loop polls for, as before.
#include "../wake_watch.h"

namespace kv {

bool       wake_watch_supported() { return false; }
WakeWatch* wake_watch_create(PlatformState&) { return nullptr; }
void       wake_watch_destroy(WakeWatch*) {}
bool       wake_watch_set(WakeWatch*, const void*, const NetFd*, u32) { return false; }
void       wake_watch_rearm(WakeWatch*) {}

} // namespace kv
