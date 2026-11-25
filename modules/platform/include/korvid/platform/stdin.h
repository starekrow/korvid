// Korvid platform: the process's standard input, read without blocking whatever it is (a
// pipe, a file, a terminal): the CLI's transport when a program is driven by another. One
// source per OS (posix/stdin_posix.cc, win32/stdin_win32.cc).
#pragma once
#include "korvid/core/types.h"
#include "korvid/platform/os_handle.h"

namespace kv {

// Whether stdin is an interactive terminal (then the CLI leaves it alone unless asked).
bool io_stdin_is_terminal();
// What has arrived, up to cap bytes: the count, 0 when nothing is waiting, -1 at the end
// of the stream (or on an error, after which stdin is done).
i32  io_stdin_read(void* buf, u32 cap);
// What a waiting loop can be woken by when input arrives (PlatformWatch::add), or none
// where this OS cannot watch it (Windows: a console's or a pipe's handle is no socket).
Handle io_stdin_handle();

} // namespace kv
