// Korvid asset_render: the asset kinds that are the renderer's. The asset module reads files
// and keeps records without drawing anything, so a tool can link it with no renderer; this
// module is the half that draws, for programs that do:
//
//   handle kinds  asset_render.texture, asset_render.material   registered with the asset
//                 module (AssetKindDesc): scopes hold them, asset_reload reaches them, the
//                 CLI's `scope load` takes .png and .material.json
//   resources     asset_render.textures, asset_render.materials   the tables
//   systems       asset_render.pump            phase PreSim, main thread, after asset.pump_scopes:
//                                              completed reads become textures (decode, upload)
//                 asset_render.pump_materials  after it: files become renderer rows
//   mounts        /materials (patchable), /textures
//   fonts         the asset module's glyph pages get a renderer texture and material each,
//                 and their changes are uploaded (AssetFontPages, korvid/asset/font.h)
//
// Both pumps run on the main thread because they call the renderer and reap the platform's
// I/O queue. The events are the asset module's (korvid/asset/asset.h).
#pragma once
#include "korvid/core/module.h"

namespace kv {

KV_MODULE_DECLARE(asset_render_module);

} // namespace kv
