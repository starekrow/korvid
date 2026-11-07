// Korvid cli: the command line, as a module. Transports are stdin (when it is not a
// terminal, or when asked) and a TCP listener on loopback; each connection is a session
// with its own line buffer. A line is a command: the longest registered name that
// prefixes its words, the rest as arguments. Responses are lines; a request written as
// "#id cmd ..." gets every response line prefixed "#id " and ends with "#id ok" or
// "#id err <message>", a bare request ends with "ok" or "err: <message>". The commands
// come from every module's ModuleDesc::commands; the module adds "help" and "quit".
//
// The system "cli.pump" (phase Input, main thread) reads what arrived, runs the commands
// and flushes the answers. Nothing blocks: a session whose output cannot drain gets a
// "!dropped" line when it can again.
#pragma once
#include "korvid/core/module.h"

namespace kv {

struct Engine;

KV_MODULE_DECLARE(cli_module);

struct CliConfig {
    u16  port  = 0;     // 0: no listener
    bool stdin_ = false; // read commands from stdin even when it is a terminal
    bool local_only = true;
};

// Before engine_init: what the module opens at startup. The frame loop's --cli-port and
// --cli-stdin set it.
void cli_configure(const CliConfig& config);

// Runs one line as a request and hands each response line to `sink`. For tools and
// tests; the transports call it too. Returns false if the request failed.
bool cli_run(Engine& e, const char* line, void (*sink)(void* user, const char* line), void* user);

struct CliStats { u32 sessions; u64 requests; u64 failed; u64 dropped; };
CliStats cli_stats(Engine& e);

} // namespace kv
