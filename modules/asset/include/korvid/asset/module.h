// Korvid asset: the module record.
//
// What the module declares, in the engine's terms:
//
//   handle kinds  asset.font, asset.sound, asset.blob, asset.shape, asset.stream, asset.scope
//   events        AssetReady, AssetFailed, AssetReloaded, AssetScopeReady   every kind's, whichever module's
//   resources     asset.fonts, asset.sounds, asset.blobs, asset.streams, asset.shapes   the tables
//   systems       asset.pump_scopes  phase PreSim, main thread, first: what nobody holds retires
//                 asset.pump_<kind>  phase PreSim, main thread, after it: completed reads become records
//                 asset.fonts_flush  phase Frame: changed glyph pages uploaded (AssetFontPages)
//   mounts        /fonts, /sounds, /scopes       the CLI's filesystem
//
// The module draws nothing and links no renderer: a tool can have assets. Textures and
// materials, which are the renderer's, are asset_render's (korvid/asset_render/), a kind
// registered here like any other module's (korvid/asset/scope.h, AssetKindDesc). A load is a
// handle returned at once, a read in flight, and an event later; nothing completes inline.
// See the module's README.
#pragma once
#include "korvid/core/module.h"

namespace kv {

KV_MODULE_DECLARE(asset_module);

} // namespace kv
