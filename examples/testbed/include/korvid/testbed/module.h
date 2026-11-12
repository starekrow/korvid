// testbed: the example game as a module. Its startup loads assets and builds materials;
// its one system runs the gameplay state machine in phase Sim. The loop, window and
// renderer are the frameloop module's.
#pragma once
#include "korvid/core/module.h"

namespace kv {

KV_MODULE_DECLARE(testbed_module);

} // namespace kv
