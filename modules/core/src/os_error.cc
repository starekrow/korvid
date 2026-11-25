#include "korvid/core/os_error.h"
#include "korvid/core/format.h"
#include <errno.h>

namespace kv {

const char* os_error_text(i32 err) {
    switch (err) {
    case 0:               return "no error";
    case EPERM:           return "operation not permitted";
    case ENOENT:          return "no such file or directory";
    case ESRCH:           return "no such process";
    case EINTR:           return "interrupted";
    case EIO:             return "input/output error";
    case ENXIO:           return "no such device or address";
    case E2BIG:           return "argument list too long";
    case ENOEXEC:         return "not an executable";
    case EBADF:           return "bad handle";
    case ECHILD:          return "no child process";
    case EAGAIN:          return "would block: try again";
    case ENOMEM:          return "out of memory";
    case EACCES:          return "permission denied";
    case EFAULT:          return "bad address";
    case EBUSY:           return "busy";
    case EEXIST:          return "already exists";
    case EXDEV:           return "not on the same file system";
    case ENODEV:          return "no such device";
    case ENOTDIR:         return "not a directory";
    case EISDIR:          return "is a directory";
    case EINVAL:          return "invalid argument";
    case ENFILE:          return "too many open files in the system";
    case EMFILE:          return "too many open files";
    case EFBIG:           return "file too large";
    case ENOSPC:          return "no space left on the device";
    case ESPIPE:          return "cannot seek";
    case EROFS:           return "read-only file system";
    case EMLINK:          return "too many links";
    case EPIPE:           return "broken pipe";
    case ERANGE:          return "out of range";
    case EDEADLK:         return "would deadlock";
    case ENAMETOOLONG:    return "name too long";
    case ENOTEMPTY:       return "directory not empty";
    case EILSEQ:          return "malformed data";
    case ELOOP:           return "too many levels of links";
    case ENOTSUP:         return "not supported";
    case ECANCELED:       return "cancelled";
    case EADDRINUSE:      return "address in use";
    case EADDRNOTAVAIL:   return "address not available";
    case ENETDOWN:        return "network is down";
    case ENETUNREACH:     return "network unreachable";
    case EHOSTUNREACH:    return "host unreachable";
    case ECONNREFUSED:    return "connection refused";
    case ECONNRESET:      return "connection reset";
    case ECONNABORTED:    return "connection aborted";
    case ENOTCONN:        return "not connected";
    case EISCONN:         return "already connected";
    case ETIMEDOUT:       return "timed out";
    case ENOBUFS:         return "no buffer space";
    case EINPROGRESS:     return "in progress";
    case EALREADY:        return "already in progress";
    case ENOTSOCK:        return "not a socket";
    default: break;
    }
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    if (err == EWOULDBLOCK) return "would block: try again"; // Windows' C library numbers it apart
#endif
#if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
    if (err == EOPNOTSUPP) return "not supported";
#endif
    static thread_local char unknown[32];
    format(unknown, sizeof unknown, "error %d", err);
    return unknown;
}

const char* os_error_name(i32 err) {
    switch (err) {
    case Err::None:         return "None";
    case Err::NotFound:     return "NotFound";
    case Err::Denied:       return "Denied";
    case Err::Exists:       return "Exists";
    case Err::NotDir:       return "NotDir";
    case Err::IsDir:        return "IsDir";
    case Err::NotEmpty:     return "NotEmpty";
    case Err::NameTooLong:  return "NameTooLong";
    case Err::CrossDevice:  return "CrossDevice";
    case Err::NoSpace:      return "NoSpace";
    case Err::TooManyOpen:  return "TooManyOpen";
    case Err::Busy:         return "Busy";
    case Err::Io:           return "Io";
    case Err::Malformed:    return "Malformed";
    case Err::Invalid:      return "Invalid";
    case Err::BadHandle:    return "BadHandle";
    case Err::NoMemory:     return "NoMemory";
    case Err::NotSupported: return "NotSupported";
    case Err::Cancelled:    return "Cancelled";
    case Err::WouldBlock:   return "WouldBlock";
    case Err::Interrupted:  return "Interrupted";
    case Err::TimedOut:     return "TimedOut";
    case Err::Pipe:         return "Pipe";
    case Err::AddrInUse:    return "AddrInUse";
    case Err::Refused:      return "Refused";
    case Err::Reset:        return "Reset";
    case Err::NotConnected: return "NotConnected";
    case Err::Unreachable:  return "Unreachable";
    default: break;
    }
    if (err == Err::Again) return "WouldBlock"; // one thing under two numbers on Windows
    return "Error";
}

} // namespace kv
