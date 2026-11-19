// Korvid core: the log: KV_INFO, KV_WARN and friends, one line per call.
//
// Everything the engine and a game tell the person running it goes through here. Each
// call takes a tag, the module or subsystem speaking, and printf-style text, which the
// compiler checks against its arguments (examples/rocks/src/game.cc):
//
//   KV_INFO("rocks", "wave %u: %u rocks", score->wave, wave.count);
//   KV_WARN("rocks", "high score file: errno %d", done.error);
//
// prints
//
//   [ INFO] rocks: wave 3: 7 rocks
//   [ WARN] rocks: high score file: errno 2
//
// Info, Debug and Trace lines go to stdout, Warn and Error to stderr (or, where a process
// has no standard streams, to the sink set by output_set_sink in format.h). Each line is
// written in one piece and flushed, so lines from two threads never interleave and a line
// is out before a crash that follows it. Nothing allocates; a message is cut at 1023
// bytes.
//
// One process-wide level decides what is printed: a line below it is dropped before its
// text is formatted, so a KV_DEBUG_LOG in a busy loop costs a comparison when it is off.
// Info is the default; the frame loop's --quiet sets Warn. The macros are in every build,
// ship included.
#pragma once
#include "korvid/core/types.h"

namespace kv {

// Trace and Debug are for the engine's own chatter (the renderer's per-resize line, the
// validation layer's info), Info for what happened, Warn for what went wrong and was
// handled, Error for what went wrong and was not. Off silences everything.
enum class LogLevel : u8 { Trace, Debug, Info, Warn, Error, Off };

void     log_set_level(LogLevel level); // the threshold; lines below it are dropped
LogLevel log_level();                   // the threshold now
// One line: `tag` is the module or subsystem ("vulkan", "asset"), printf-style text after.
// What the macros below call; any thread.
void     log_write(LogLevel level, const char* tag, const char* fmt, ...)
    KV_PRINTF(3, 4);

} // namespace kv

// One line at each level: KV_INFO(tag, fmt, ...). The Debug one is KV_DEBUG_LOG because
// KV_DEBUG is the debug build's switch.
#define KV_TRACE(tag, ...) ::kv::log_write(::kv::LogLevel::Trace, tag, __VA_ARGS__)
#define KV_DEBUG_LOG(tag, ...) ::kv::log_write(::kv::LogLevel::Debug, tag, __VA_ARGS__)
#define KV_INFO(tag, ...)  ::kv::log_write(::kv::LogLevel::Info,  tag, __VA_ARGS__)
#define KV_WARN(tag, ...)  ::kv::log_write(::kv::LogLevel::Warn,  tag, __VA_ARGS__)
#define KV_ERROR(tag, ...) ::kv::log_write(::kv::LogLevel::Error, tag, __VA_ARGS__)
