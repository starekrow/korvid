// Korvid asset: textures as assets. asset_load_texture returns a handle immediately; the
// texture it names already has a renderer slot showing the placeholder (the default
// checkerboard), so materials can be built from it at once and sprites drawn with it
// before the file has arrived. The file read is submitted to the platform I/O queue, the
// decode and upload happen in "asset.pump" (phase PreSim, main thread), and completion is
// an AssetReady or AssetFailed event at the next phase boundary. Paths are relative to
// <exe dir>/assets/ (kormake copies a module's assets/ there under the module's name).
// Loading a path twice returns the same handle; the record lives until module shutdown.
#pragma once
#include "korvid/core/handle.h"

namespace kv {

struct Engine;

// Where a load is. Pending from the call until the pump decides; then one of the others,
// for good.
enum class AssetState : u8 { Pending, Ready, Failed };

// Why a load failed. NotFound and Read come from the I/O queue (ENOENT, anything else);
// Decode is the PNG decoder; Upload the renderer; Parse a material file's text. A failed
// asset keeps its handle and its placeholder look, so nothing downstream has to care.
enum class AssetError : u8 { None, NotFound, Read, Decode, Upload, Parse, Count };

// A texture asset's record, read through asset_texture. Owned by the module; copy what
// you keep, the pointer is valid until the next load or shutdown.
struct TextureAsset {
    char       path[128];          // as given to asset_load_texture
    AssetState state   = AssetState::Pending;
    AssetError error   = AssetError::None;
    Handle     texture;            // the renderer's texture handle: valid from load on, the placeholder until Ready
    Handle     io;                 // the pending read's request handle while Pending; null otherwise
    u32        width = 0, height = 0; // the image's size once Ready; 0 before
};

// Events, declared by the asset module and emitted by the pumps. `asset` is the handle
// the load returned; its kind() tells a texture (asset.texture) from a material
// (asset.material). Handlers subscribe with KV_ON like any event; a system reads the
// batch with ctx.events->read<AssetReady>().
struct AssetReady  { Handle asset; };
struct AssetFailed { Handle asset; AssetError error; };

// Starts loading <exe dir>/assets/<path> as a PNG (8-bit samples, any color type, no
// interlace). Returns the asset handle at once, or null if the renderer, the platform or
// the module is not up, the path is too long (127 characters), or the tables are full.
// The same path returns the same handle; a second call never starts a second read.
Handle              asset_load_texture(Engine& e, const char* path);
// The record for a handle, or null if the handle is stale or not a texture asset.
const TextureAsset* asset_texture(Engine& e, Handle asset);
// "not found", "decode error", ...; "none" for AssetError::None.
const char*         asset_error_name(AssetError err);

} // namespace kv
