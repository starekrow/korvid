// Korvid asset: the module record.
//
// What the module declares, in the engine's terms:
//
//   handle kinds  asset.texture, asset.material     the records; a handle names one
//   events        AssetReady, AssetFailed           carry the asset handle; the kind says which table
//   resources     asset.textures, asset.materials   the tables, written by the pumps
//   systems       asset.pump            phase PreSim, main thread: completed reads become textures
//                 asset.pump_materials  phase PreSim, main thread, after asset.pump: files become rows
//   commands      ls materials, dump material <path>, patch material <path> ...   for the CLI
//
// Both pumps run on the main thread because they call the renderer (texture uploads,
// material rows) and reap the platform's I/O queue, which is pumped once per frame
// before the tick. A load is a handle returned at once, a read in flight, and an event
// later; nothing completes inline. See the module's README.
#pragma once
#include "korvid/core/module.h"

namespace kv {

KV_MODULE_DECLARE(asset_module);

} // namespace kv
