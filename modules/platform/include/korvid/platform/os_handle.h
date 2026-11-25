// Korvid platform: what the OS opened, as handles. A file, a socket, a pipe, a watch's
// descriptor or a child process is named by the same Handle as an entity or a texture
// (core/handle.h): the platform issues them, one handle kind for each sort of OS object
// ("os.file", "os.socket", "os.pipe", "os.notify", "os.process"), and keeps the OS's own
// value (a descriptor, a HANDLE, a SOCKET) in a table nothing outside the module reads.
//
// What a holder gets for that, with nothing to do for it:
// - The handle goes stale when what it names is closed. Every other copy stops resolving:
//   a read on a closed file fails with EBADF, and never reads whichever file the OS gave
//   the same descriptor to next.
// - Any handle can be asked what it is (os_handle_kind), and shown: kv_handle_describe
//   prints "os.file#3:1 C:/game/save.kbor, opened by save, 3900 read, 0 written" in a dev
//   or debug build, which keep what was opened, by which module, and the bytes each way.
// - A handle of the wrong kind is refused by whatever takes it, by name.
//
// The null Handle is none. docs/design/handles.md has the design.
#pragma once
#include "korvid/core/handle.h"

namespace kv {

enum class OsKind : u8 {
    None,
    File,    // a file on a disk: read at an offset, written, synced, locked
    Socket,  // a TCP socket, or one that listens
    Pipe,    // one end of a pipe: a child's output, the process's standard input
    Notify,  // a descriptor the OS makes readable when it has news (inotify's)
    Process, // a child process, or the group of what it started: waited on and ended, never read
};
const char* os_kind_name(OsKind k); // "file", "socket", ...; "none"

// What sort of OS object a handle names: None for the null handle, for one that is not
// the OS's (an entity's, an asset's), and for one whose object has been closed. One table
// lookup, from any thread.
OsKind os_handle_kind(Handle h);
// Whether the handle names something the OS still has open for this process.
inline bool os_handle_open(Handle h) { return os_handle_kind(h) != OsKind::None; }

// What a dev or debug build knows of an open handle. In a ship build only `kind` is filled.
struct OsHandleInfo {
    OsKind kind = OsKind::None;
    char   what[160] = {};   // what was opened: a path, "listener 127.0.0.1:4100", a command
    char   by[32] = {};      // the module whose code opened it; outside any, the program's main module (the executable's own); "" with no engine
    f64    opened_at = 0.0;  // time_now's clock
    u64    bytes_in = 0, bytes_out = 0; // read and written through it so far
};
// False for a handle that is not open.
bool os_handle_info(Handle h, OsHandleInfo* out);
// How many OS handles the process has open through the platform, and each of them in
// turn (in no order), for a report or a leak check. The callback must not open or close.
u32  os_handles_open();
void os_handles_each(void (*fn)(void* user, Handle h, const OsHandleInfo& info), void* user);

} // namespace kv
