// Korvid asset_live: live reload from a running `goldberg watch` (README.md). Link it, put it in
// the module list after `asset`, and a dev build reloads what the watch cooks again.
//
//   KV_GOLDBERG=off        not this run (frameloop's --no-goldberg)
//   KV_GOLDBERG=PORT       this port on 127.0.0.1, not the one the output tree names (--goldberg PORT)
//
// Otherwise it reads the port from <the program's output tree>/.goldberg-port, which a watch
// writes, and tries again every two seconds while there is none or the watch has gone.
#pragma once
#include "korvid/core/module.h"
#include "korvid/core/types.h"

namespace kv {

struct Engine;

KV_MODULE_DECLARE(asset_live_module);

struct AssetLiveStats {
    bool connected = false;
    u16  port = 0;
    u32  connects = 0;   // times a connection was made
    u32  changed = 0;    // `changed` lines heard
    u32  reloaded = 0;   // records asked to read again for them
    u32  removed = 0;    // `removed` lines heard
};
AssetLiveStats asset_live_stats(Engine& e);

} // namespace kv
