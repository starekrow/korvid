// Korvid platform: the table behind OS handles, for the module's own sources. Only they
// make a handle, read the OS's value out of one, or give a slot back: that nothing else
// can is what lets a closed handle be stale everywhere at once.
#pragma once
#include "korvid/platform/os_handle.h"

namespace kv {

// A handle of `kind` for the OS's value, which the table now holds: the null handle when
// the value is negative (a failed open's -1, INVALID_HANDLE_VALUE, INVALID_SOCKET) or the
// table is full (65,536 open at once: logged). `what` says what it is, for a dev build's
// description (a path, a port); copied, cut at 159 bytes. Any thread.
Handle os_handle_make(OsKind kind, i64 os, const char* what = nullptr);
// The OS's value: -1 for a handle that is null, stale or not the OS's. With `want`, also
// -1 for a handle of another kind. Any thread, no lock.
i64    os_handle_value(Handle h);
i64    os_handle_value(Handle h, OsKind want);
// Gives the slot back and returns the OS's value for the caller to close: -1 if the handle
// was not open (closed already: closing twice closes once). Every copy of the handle is
// stale from here.
i64    os_handle_release(Handle h);
// Bytes moved through an open handle, for its description. Dev and debug; nothing in ship.
void   os_handle_count(Handle h, u64 in, u64 out);

// What a handle says of itself to whoever watches it, in every build. Cleared when the
// slot is made; a stale handle has none. Any thread.
enum OsHandleFlag : u32 {
    kOsQueued = 1u << 0, // a socket with output its owner queued and the kernel has yet to take
};
void   os_handle_set_flag(Handle h, u32 flag, bool on);
bool   os_handle_flag(Handle h, u32 flag);

} // namespace kv
