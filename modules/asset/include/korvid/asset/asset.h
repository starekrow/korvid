// Korvid asset: what every kind of asset shares. A load returns a handle at once and the
// record behind it is Pending; the kind's pump makes it Ready or Failed, for good, and says so
// with an event at the next phase boundary. The kinds are the asset module's (blobs, shapes,
// fonts, sounds, streams), asset_render's (textures, materials: korvid/asset_render/) and any
// other module's that registers one (korvid/asset/scope.h, AssetKindDesc).
#pragma once
#include "korvid/core/handle.h"

namespace kv {

struct Engine;

// Where a load is. Pending from the call until the pump decides; then one of the others,
// for good.
enum class AssetState : u8 { Pending, Ready, Failed };

constexpr u32 KV_ASSET_PATH_MAX = 128; // bytes, NUL included, for asset paths

// Why a load failed. NotFound and Read come from the I/O queue (ENOENT, anything else);
// Decode is a file's decoder (PNG, TrueType, WAV, Ogg, kshape); Upload the renderer; Parse a
// text file's parser. A failed asset keeps its handle, so nothing downstream has to care.
enum class AssetError : u8 { None, NotFound, Read, Decode, Upload, Parse, Count };

// "not found", "decode error", ...; "none" for AssetError::None.
const char* asset_error_name(AssetError err);

// Events, declared by the asset module and emitted by the kinds' pumps, whichever module's.
// `asset` is the handle the load returned; its kind() says which table. Handlers subscribe
// with KV_ON like any event; a system reads the batch with ctx.events->read<AssetReady>().
struct AssetReady  { Handle asset; };
// `reload`: the file was read again (asset_reload) and could not be used; the asset keeps what
// it had and stays Ready.
struct AssetFailed { Handle asset; AssetError error; bool reload = false; };
// The asset was made again from its file, which changed (korvid/asset/reload.h): the same
// handle, new contents. Sent for a record that was Ready; one that failed and now loads sends
// AssetReady.
struct AssetReloaded { Handle asset; };

} // namespace kv
