// Korvid platform: the warning a dev or debug build gives when a blocking call is made from
// a system of a paced loop. A tick has a deadline and the blocking calls have none: a file
// read is a frame dropped on a cold disk, a wait is as many as it waits. The calls stay
// right for tools, startup, shutdown and tests, so it is the loop that says it is paced
// (io_warn_blocking_in_ticks: the frame loop's), and only a call made while one of its
// systems runs is told about, on whatever thread the system is on. Once for each system
// and call. Nothing in a ship build.
#pragma once
#include "korvid/core/types.h"

namespace kv {

#if KV_DEV
// At the top of a blocking call, for as long as it runs: `call` as the caller wrote it
// ("io_read_file"), `what` the path or the thing waited on (may be null). A blocking call
// made by another counts once, as the outer one.
struct BlockingCall {
    BlockingCall(const char* call, const char* what);
    ~BlockingCall();
};
#else
struct BlockingCall { BlockingCall(const char*, const char*) {} };
#endif

} // namespace kv
