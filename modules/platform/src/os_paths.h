// Korvid platform: the directories the OS gives a process, one source per OS.
#pragma once
#include "korvid/core/types.h"

namespace kv {

// The running executable's directory with a trailing separator. False if the OS will not say.
bool os_base_path(char* out, u32 cap);
// The per-user writable directory for (org, app), created if missing, with a trailing
// separator. False with a log line if there is no home to put it in.
bool os_pref_path(const char* org, const char* app, char* out, u32 cap);

} // namespace kv
