// Korvid frameloop: module record. Startup opens the window and the renderer from the
// config handed to frameloop_main; the "frameloop.exit" system (phase PreSim) turns
// window close, Escape and the run timer into ExitRequested events.
#pragma once
#include "korvid/core/module.h"

namespace kv {

KV_MODULE_DECLARE(frameloop_module);

} // namespace kv
