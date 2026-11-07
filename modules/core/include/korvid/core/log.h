// Korvid core: leveled log to stdout, one line per call, "[LEVEL] tag: message". The
// level is a process-wide threshold: lines below it are dropped before formatting. Info
// is the default; --quiet sets Warn. Thread-safe in the sense that lines do not
// interleave; no allocation, no buffering beyond stdout's. The CLI replaces the sink later.
#pragma once
#include "korvid/core/types.h"

namespace kv {

// Trace and Debug are for the engine's own chatter (the renderer's per-resize line, the
// validation layer's info), Info for what happened, Warn for what went wrong and was
// handled, Error for what went wrong and was not. Off silences everything.
enum class LogLevel : u8 { Trace, Debug, Info, Warn, Error, Off };

void     log_set_level(LogLevel level); // the threshold; lines below it are dropped
LogLevel log_level();
// One line: `tag` is the module or subsystem ("vulkan", "asset"), printf-style text after.
void     log_write(LogLevel level, const char* tag, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

} // namespace kv

#define KV_TRACE(tag, ...) ::kv::log_write(::kv::LogLevel::Trace, tag, __VA_ARGS__)
#define KV_DEBUG_LOG(tag, ...) ::kv::log_write(::kv::LogLevel::Debug, tag, __VA_ARGS__)
#define KV_INFO(tag, ...)  ::kv::log_write(::kv::LogLevel::Info,  tag, __VA_ARGS__)
#define KV_WARN(tag, ...)  ::kv::log_write(::kv::LogLevel::Warn,  tag, __VA_ARGS__)
#define KV_ERROR(tag, ...) ::kv::log_write(::kv::LogLevel::Error, tag, __VA_ARGS__)
