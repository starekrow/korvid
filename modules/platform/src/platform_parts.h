// Korvid platform: the pieces module.cc puts in the module's record, defined in files of
// their own: the alert system (alert.cc) and the CLI's /dev mount (dev_mount.cc).
#pragma once
#include "korvid/core/cli.h"
#include "korvid/core/phase.h"
#include "platform_state.h"

namespace kv {

// Phase Input: every Alertable whose source has something to read gets an Alerted.
void platform_alerts(TickContext& ctx);
// At shutdown: the alerts' waker and watch let go of.
void platform_alerts_shutdown(Engine& e, PlatformState& s);

// `/dev`: what the OS has open for the process, as paths. /dev/handles/<kind>/<slot>.
void platform_dev_list(CliContext& ctx, const char* dir, CliEmitFn emit, void* user);
bool platform_dev_describe(CliContext& ctx, const char* rel);

} // namespace kv
