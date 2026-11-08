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
    bool no_stdin = false; // never read stdin, even when it is a pipe (a tool in a build)
    bool local_only = true;
    bool ascii = false;  // the transports are ASCII-only (a UART, a devkit TTY): non-ASCII is escaped
};

// Before engine_init: what the module opens at startup. The frame loop's --cli-port and
// --cli-stdin set it.
void cli_configure(const CliConfig& config);

// Runs one line as a request and hands each response line to `sink`. For tools and
// tests; the transports call it too. Returns false if the request failed.
bool cli_run(Engine& e, const char* line, void (*sink)(void* user, const char* line), void* user);

// The ASCII-only form of a UTF-8 line: each non-ASCII code point as \u{XXXX} (and a
// malformed byte as \u{FFFD}), a backslash as \\. And the reverse, which leaves any
// other backslash as it is. Both NUL-terminate, truncate to cap like snprintf, never cut
// an escape or a sequence, and return the length they wanted.
usize cli_ascii_escape(const char* utf8, char* out, usize cap);
usize cli_ascii_unescape(const char* ascii, char* out, usize cap);

struct CliStats { u32 sessions; u64 requests; u64 failed; u64 dropped; };
CliStats cli_stats(Engine& e);

} // namespace kv
