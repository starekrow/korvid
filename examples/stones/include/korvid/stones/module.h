// stones: the example written the other way. Game objects are plain structs in
// plain arrays, one function does everything in order every tick, and the engine is used
// for the window, the keys, the sprites and a bit of reflection for the log. See game.cc.
#pragma once
#include "korvid/core/module.h"

namespace kv {

KV_MODULE_DECLARE(stones_module);

} // namespace kv
