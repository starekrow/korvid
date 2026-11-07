// Korvid asset: module record. Owns the texture asset table (kind "asset.texture",
// resource "asset.textures") and the "asset.pump" system (phase PreSim) that turns I/O
// completions into resident textures and AssetReady/AssetFailed events; likewise the
// material table (kind "asset.material") and "asset.pump_materials", which runs after.
// Declares the CLI commands over materials: ls, dump, patch.
#pragma once
#include "korvid/core/module.h"

namespace kv {

KV_MODULE_DECLARE(asset_module);

} // namespace kv
