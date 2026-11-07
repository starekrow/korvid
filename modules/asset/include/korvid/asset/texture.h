// Korvid asset: textures. asset_load_texture returns a handle immediately; the texture
// it names already has a renderer slot (showing the placeholder), so materials can be
// built from it at once. The file read is submitted to the platform I/O queue, the
// decode and upload happen in the asset module's PreSim system, and completion is an
// AssetReady or AssetFailed event at the next phase boundary. Paths are relative to
// <exe dir>/assets/. Loading a path twice returns the same handle.
#pragma once
#include "korvid/core/handle.h"

namespace kv {

struct Engine;

enum class AssetState : u8 { Pending, Ready, Failed };
enum class AssetError : u8 { None, NotFound, Read, Decode, Upload, Count };

struct TextureAsset {
    char       path[128];
    AssetState state   = AssetState::Pending;
    AssetError error   = AssetError::None;
    Handle     texture;         // the renderer's handle: valid from load on, placeholder until Ready
    Handle     io;              // the pending read, while Pending
    u32        width = 0, height = 0;
};

// Events, declared by the asset module.
struct AssetReady  { Handle asset; };
struct AssetFailed { Handle asset; AssetError error; };

Handle              asset_load_texture(Engine& e, const char* path);
const TextureAsset* asset_texture(Engine& e, Handle asset); // null if stale
const char*         asset_error_name(AssetError err);

} // namespace kv
