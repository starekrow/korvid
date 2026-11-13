// Korvid core: formatted text and the standard streams, without <stdio.h>. printf's
// language and the C library's work, behind one header: the C library is linked anyway,
// but its header brings FILE and its functions into every file that only wanted a line of
// text. Here it is included once, in src/format.cc (and by the log and the profiler's
// trace writer, which are core's own stream code).
#pragma once
#include "korvid/core/types.h"
#include <stdarg.h>

namespace kv {

// snprintf's contract: at most `cap` bytes written, the last a NUL (when cap > 0); returns
// the length the whole text would have had, or a negative number for a bad format.
i32  format(char* out, usize cap, const char* fmt, ...) KV_PRINTF(3, 4);
i32  vformat(char* out, usize cap, const char* fmt, va_list args);

// To stdout, buffered as the C library buffers it; print_flush pushes it out. To stderr,
// unbuffered. Nothing is added: a line ends where the text puts '\n'.
void print(const char* fmt, ...) KV_PRINTF(1, 2);
void vprint(const char* fmt, va_list args);
void print_err(const char* fmt, ...) KV_PRINTF(1, 2);
void vprint_err(const char* fmt, va_list args);
void print_flush();

} // namespace kv
