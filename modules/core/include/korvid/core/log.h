// Korvid core: leveled log to stdout. The CLI replaces the sink later.
#pragma once
#include "korvid/core/types.h"

namespace kv {

enum class LogLevel : u8 { Trace, Debug, Info, Warn, Error, Off };

void     log_set_level(LogLevel level);
LogLevel log_level();
void     log_write(LogLevel level, const char* tag, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

} // namespace kv

#define KV_TRACE(tag, ...) ::kv::log_write(::kv::LogLevel::Trace, tag, __VA_ARGS__)
#define KV_DEBUG_LOG(tag, ...) ::kv::log_write(::kv::LogLevel::Debug, tag, __VA_ARGS__)
#define KV_INFO(tag, ...)  ::kv::log_write(::kv::LogLevel::Info,  tag, __VA_ARGS__)
#define KV_WARN(tag, ...)  ::kv::log_write(::kv::LogLevel::Warn,  tag, __VA_ARGS__)
#define KV_ERROR(tag, ...) ::kv::log_write(::kv::LogLevel::Error, tag, __VA_ARGS__)
