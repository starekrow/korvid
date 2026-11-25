// Korvid platform: the warning a dev or debug build gives when code uses a socket, a
// listener or a process that is closed, over or never started: the loop that writes to a
// connection already gone, accepts on a listener whose open failed, reads a child that
// never ran. Each call quietly says false, -1 or nothing, which is right for code that
// looks; code that does not look would otherwise spin or end with nothing said. Once per
// object until it is opened again (`warned`, a field of each); nothing in a ship build.
// The words are here so every OS says the same.
#pragma once
#include "korvid/core/log.h"
#include "korvid/core/os_error.h"
#include "korvid/platform/net.h"
#include "korvid/platform/process.h"

namespace kv {

// `what`: the call, as the caller wrote it ("write", "accept", "read_out").
inline void misuse_socket(NetSocket& s, const char* what) {
#if KV_DEV
    if (s.warned) return;
    s.warned = true;
    if (!os_handle_open(s.fd))
        KV_WARN("net", "%s on a socket that is not open (never connected or adopted, or closed): it does nothing", what);
    else
        KV_WARN("net", "%s on a connection that is over (%s): it does nothing; is_open() says so", what,
                s.error ? os_error_text(s.error) : "the peer closed it");
#else
    (void)s; (void)what;
#endif
}

inline void misuse_listener(NetListener& l) {
#if KV_DEV
    if (l.warned) return;
    l.warned = true;
    if (l.error)
        KV_WARN("net", "accept on a listener that is not open: its open failed (%s), so nobody can connect", os_error_text(l.error));
    else
        KV_WARN("net", "accept on a listener that is not open (never opened, or closed): nobody can connect");
#else
    (void)l;
#endif
}

inline void misuse_process(Process& p, const char* what) {
#if KV_DEV
    if (p.warned) return;
    p.warned = true;
    if (p.error[0])
        KV_WARN("process", "%s on a process that is not running: its spawn failed (%s)", what, p.error);
    else
        KV_WARN("process", "%s on a process that is not running (never started, or closed)", what);
#else
    (void)p; (void)what;
#endif
}

} // namespace kv
