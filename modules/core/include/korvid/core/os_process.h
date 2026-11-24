// Korvid core: the process's own environment and standard streams, where the OSes do not
// agree on how they are changed. Reading a variable is getenv on every one of them; the
// rest is here, so that no caller has an #if for it. Implemented per platform under
// src/<platform>/os_process.cc.
#pragma once
#include "korvid/core/types.h"

namespace kv {

// Sets a variable in the process's environment, for what reads it later in this process
// or in a child it starts; a null value takes it away. False if the OS refuses (a name
// with '=' in it, no memory). Not for use while another thread may be reading the
// environment: at startup, or in a test.
bool os_env_set(const char* name, const char* value);

// The process's standard output and error as the bytes written to them: on Windows the C
// runtime would turn each "\n" into "\r\n", so a tool's output would differ by platform.
// Nothing to do anywhere else.
void os_stdio_binary();

} // namespace kv
