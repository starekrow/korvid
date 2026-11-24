// Korvid platform: KHandle, anything the OS opened for the process, with what it is. A
// descriptor on POSIX, a HANDLE or a SOCKET on Windows, in one 64-bit word with its kind in
// the top byte, so a file's handle cannot be given where a socket's is wanted without the
// taker knowing: what can wait on a socket cannot always wait on a pipe (Windows), and one
// that is asked to says so, by name, instead of misbehaving.
//
// None is -1 and every open handle is 0 or more, whatever its kind. The OS's own value has
// 56 bits: a descriptor is an int, and Windows' handles carry 32 significant bits on every
// build. Only the platform module's own sources take one apart (os()); everything else
// holds it, asks is_open() and kind(), compares it, and hands it back.
#pragma once
#include "korvid/core/types.h"

namespace kv {

enum class KHandleKind : u8 {
    None,
    File,    // a file on a disk: read at an offset, written, synced
    Socket,  // a TCP socket, or one that listens
    Pipe,    // one end of a pipe: a child's output, the process's standard input
    Notify,  // a descriptor the OS makes readable when it has news (inotify's)
    Process, // a child process, or the group of what it started: waited on and ended, never read
};
const char* khandle_kind_name(KHandleKind k);

struct KHandle {
    i64 bits = -1;

    bool        is_open() const { return bits >= 0; }
    KHandleKind kind() const { return bits < 0 ? KHandleKind::None : (KHandleKind)(bits >> 56); }
    // The OS's own value (a descriptor, a HANDLE, a SOCKET), or -1 for none.
    i64         os() const { return bits < 0 ? -1 : (bits & kOsMask); }
    bool operator==(KHandle o) const { return bits == o.bits; }
    bool operator!=(KHandle o) const { return bits != o.bits; }

    static constexpr i64 kOsMask = ((i64)1 << 56) - 1;
};

// A handle of `kind` around the OS's value. None when the value is negative (a failed open's
// -1, INVALID_HANDLE_VALUE, INVALID_SOCKET) or the kind is None.
inline KHandle khandle_make(KHandleKind kind, i64 os) {
    KHandle h;
    if (os < 0 || os > KHandle::kOsMask || kind == KHandleKind::None) return h;
    h.bits = ((i64)kind << 56) | os;
    return h;
}

} // namespace kv
