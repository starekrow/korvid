// Korvid kvscript: the script runner's module. In the kvscript executable it is the tool;
// linked into a game it adds the "kvscript run" command to the game's CLI.
#pragma once
#include "korvid/core/module.h"

namespace kv {

KV_MODULE_DECLARE(kvscript_module);

} // namespace kv
