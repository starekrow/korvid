// Korvid core: what an OS error is called. Every blocking call in the engine that can fail
// says why with an errno value (the platform module translates Windows' own), in a last
// `i32* err` or a completion's `error`. The numbers are this OS's C library's: ENOENT is 2
// everywhere, but a refused connection is 111 on Linux, 61 on Apple's and 107 on Windows.
// So code names an error and never writes its number: Err's names are the common ones,
// each the local value, and compare with any `err` the engine hands back:
//
//     if (c.error == Err::NotFound) ...          switch (err) { case Err::Denied: ...
//
// A number is good inside one run of one program. What is saved, sent or logged for
// another machine is the name (os_error_name) or the words (os_error_text).
#pragma once
#include "korvid/core/types.h"
#include <errno.h>

namespace kv {

namespace Err {
enum : i32 {
    None         = 0,
    NotFound     = ENOENT,       // no such file, directory, entry or program
    Denied       = EACCES,       // no permission
    Exists       = EEXIST,       // already there
    NotDir       = ENOTDIR,      // something on the way is not a directory
    IsDir        = EISDIR,       // a directory where a file was wanted
    NotEmpty     = ENOTEMPTY,    // a directory with something in it
    NameTooLong  = ENAMETOOLONG,
    CrossDevice  = EXDEV,        // a rename across file systems
    NoSpace      = ENOSPC,       // the disk is full
    TooManyOpen  = EMFILE,
    Busy         = EBUSY,
    Io           = EIO,          // the device failed, or the data ended early
    Malformed    = EILSEQ,       // data that is not what it should be: a bad zip, a failed CRC
    Invalid      = EINVAL,       // an argument the call refuses
    BadHandle    = EBADF,        // a handle that is closed, stale or of the wrong kind
    NoMemory     = ENOMEM,
    NotSupported = ENOTSUP,      // not on this OS
    Cancelled    = ECANCELED,
    WouldBlock   = EWOULDBLOCK,  // asked without waiting, and it would have to wait (a lock taken)
    Again        = EAGAIN,       // the same number as WouldBlock on POSIX, its own on Windows
    Interrupted  = EINTR,
    TimedOut     = ETIMEDOUT,
    Pipe         = EPIPE,        // written to with nobody reading
    AddrInUse    = EADDRINUSE,   // the port is taken
    Refused      = ECONNREFUSED, // nobody is listening there
    Reset        = ECONNRESET,   // the peer went away without closing
    NotConnected = ENOTCONN,
    Unreachable  = EHOSTUNREACH,
};
} // namespace Err

// The words for an errno value: "no such file or directory", "address in use". The same
// text on every platform, lower case, no full stop; never null. A value it has no words
// for is "error N", in a buffer that is the calling thread's until its next such call. 0 is
// "no error".
const char* os_error_text(i32 err);
// Err's name for it, the same on every platform: "NotFound", "Refused"; "None" for 0. For
// what leaves the process (a save, a wire, a log read elsewhere). A value Err does not
// name is "Error".
const char* os_error_name(i32 err);

} // namespace kv
