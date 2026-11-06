// hello: the example game as a module. Its startup opens the window and the renderer and
// owns the game state; its one system runs the gameplay state machine in phase Sim.
#pragma once
#include "korvid/core/module.h"

namespace kv {

KV_MODULE_DECLARE(hello_module);

} // namespace kv
