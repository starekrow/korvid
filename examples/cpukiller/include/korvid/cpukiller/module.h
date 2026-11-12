// cpukiller: the engine-level stress scenario as a module (see stress.cc) and
// the pool and memory scenarios in main.cc.
#pragma once
#include "korvid/core/module.h"

namespace kv {

KV_MODULE_DECLARE(cpukiller_module);

// Bodies to simulate, set before engine_init.
void cpukiller_set_bodies(u32 n);
// The module list without a window: just this module. kv_module_list() is the one with
// the platform, the renderer and the frame loop.
Span<const ModuleDesc* const> cpukiller_core_modules();

} // namespace kv
