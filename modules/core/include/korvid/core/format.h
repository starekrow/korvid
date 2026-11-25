// Korvid core: formatted text and the standard streams, without <stdio.h>. printf's
// language and the C library's work, behind one header: the C library is linked anyway,
// but its header brings FILE and its functions into every file that only wanted a line of
// text. Here it is included once, in src/format.cc (and by the log and the profiler's
// trace writer, which are core's own stream code).
//
// format() writes into a buffer the caller owns, usually a fixed array on the stack, and
// never past its end; what it returns says whether the whole text fit. print() and
// print_err() write to the process's output, for a tool's results or a game's one-off
// line; messages about what the game or engine is doing go through the log (log.h), which
// adds a level and a tag. The format strings are printf's, checked against the arguments
// by gcc and clang.
#pragma once
#include "korvid/core/types.h"
#include <stdarg.h>

namespace kv {

// ---- into a buffer ---------------------------------------------------------------------

// snprintf's contract: at most `cap` bytes written, the last a NUL (when cap > 0); returns
// the length the whole text would have had, or a negative number for a bad format. A
// result of `cap` or more means the text was cut (from modules/core/src/cli.cc):
//
//   char joined[2 * KV_CLI_PATH_MAX];
//   int n = format(joined, sizeof joined, "%s/%s", base, arg ? arg : "");
//   if (n < 0 || (usize)n >= sizeof joined) { ctx.fail("path too long"); return false; }
//
// A cut can fall inside a UTF-8 sequence; utf8_trim (utf8.h) shortens it to a whole one.
i32  format(char* out, usize cap, const char* fmt, ...) KV_PRINTF(3, 4);
// format() with the arguments as a va_list, for a function that takes `...` itself.
i32  vformat(char* out, usize cap, const char* fmt, va_list args);

// ---- the standard streams --------------------------------------------------------------

// To stdout, buffered as the C library buffers it, and pushed out soon after a line
// ends: text that ends in '\n' asks for a flush, which the engine makes at the end of the
// frame or tick it was printed in and before a loop waits (print_flush_lines). So a line
// printed is a line a reader sees, whether stdout is a terminal, a pipe or a file, and a
// program killed afterwards has not lost it; a thousand lines printed in one tick are
// still one flush. Text that does not end a line waits for one that does, or for
// print_flush. To stderr, unbuffered. Nothing is added: a line ends where the text puts '\n'.
void print(const char* fmt, ...) KV_PRINTF(1, 2);
void vprint(const char* fmt, va_list args);
void print_err(const char* fmt, ...) KV_PRINTF(1, 2);
void vprint_err(const char* fmt, va_list args);
// Pushes out what print has buffered for stdout, now. Nothing to do when a sink is set.
void print_flush();
// The same if a line has ended on stdout since the last flush, and nothing if not: one
// atomic read. What a loop calls once a pass; engine_frame and platform_wait do.
void print_flush_lines();

// Where print, print_err and the log go, for a process with no standard streams (an
// Android app, whose output is the system's log): when set, every write is handed to
// `sink` as the bytes it would have sent to stdout (err false) or stderr (err true), not
// NUL-terminated, on the calling thread; the sink cuts lines and takes its own lock. Null
// (the default) is the C library's streams. Set before any thread prints.
using OutputSink = void (*)(bool err, const char* text, usize len);
// Sets the sink; null goes back to the C library's streams.
void output_set_sink(OutputSink sink);
// One write to the sink or the stream, as print and the log make them: `len` bytes as they
// are, no format string, so it is the way to put raw bytes (a child's output, a file) on
// stdout. Buffered and flushed as print is (above): bytes that do not end in '\n' wait
// for print_flush. In a tool, stdout and stderr are binary on Windows too
// (os_stdio_binary): nothing is added to a '\n'.
void output_write(bool err, const char* text, usize len);

} // namespace kv
