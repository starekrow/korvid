// Korvid CLI: the process's standard input as a transport, one source per OS
// (posix/stdin_posix.cc, win32/stdin_win32.cc). Never blocks.
#pragma once
#include "korvid/core/types.h"

namespace kv {

// Whether stdin is an interactive terminal (then the CLI leaves it alone unless asked).
bool os_stdin_is_terminal();
// What has arrived, up to cap bytes: the count, 0 when nothing is waiting, -1 at the end
// of the stream (or on an error, after which stdin is done).
i32  os_stdin_read(void* buf, u32 cap);

} // namespace kv
