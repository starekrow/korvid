// Korvid asset: materials as files. A material file is JSON next to the textures under the
// module's assets/:
//
//   { "texture": "example_rocks/rock.png", "blend": "alpha", "tint": [1, 1, 1, 1],
//     "params": [0, 0, 0, 0, 0, 0, 0, 0] }
//
// Every field is optional: no texture means the renderer's default, the rest default to
// an alpha-blended, untinted, zero-parameter material. asset_load_material returns a
// handle at once; the renderer row it names exists from then on (showing the placeholder
// look) so sprites can use it immediately. The file read goes through the I/O queue, the
// parse happens in "asset.pump_materials" (phase PreSim, after "asset.pump"), the texture
// it names is loaded through asset_load_texture, and AssetReady arrives once that texture
// is resident (or at once if the file names none). Loading a path twice returns the same
// handle. Tools reach the records by slot and patch them; a patch reaches the renderer
// before the next frame.
#pragma once
#include "korvid/asset/texture.h"
#include "korvid/render/material.h"

namespace kv {

constexpr u32 KV_ASSET_PATH_MAX = 128;

struct MaterialAsset {
    char         path[KV_ASSET_PATH_MAX];
    AssetState   state = AssetState::Pending;
    AssetError   error = AssetError::None;
    Handle       material;      // the renderer's row handle: valid from load on
    Handle       texture_asset; // the asset.texture the file named, once parsed; null for none
    Handle       io;            // the pending read, while reading
    MaterialDesc desc;          // what the renderer row holds now
    char         texture_path[KV_ASSET_PATH_MAX];
};

Handle               asset_load_material(Engine& e, const char* path);
const MaterialAsset* asset_material(Engine& e, Handle asset); // null if stale
Handle               asset_material_find(Engine& e, const char* path); // null if never loaded

// For tools: records by slot, [0, asset_material_count); empty slots are null.
u32                  asset_material_count(Engine& e);
const MaterialAsset* asset_material_at(Engine& e, u32 slot);
Handle               asset_material_handle_at(Engine& e, u32 slot);

// Edits: the record and the renderer row change together. The texture is a path (empty
// for the default); it loads like any other and the row follows when it is resident.
bool asset_material_patch(Engine& e, Handle asset, const MaterialDesc& desc);
bool asset_material_set_texture(Engine& e, Handle asset, const char* texture_path);

// The file format, on its own for tools and tests: fills the fields the text has, leaves
// the rest as `desc` came in. False with the reason and byte offset on a bad file.
bool material_parse(const char* text, usize len, MaterialDesc& desc, char* texture_path, usize cap,
                    const char** error, u32* error_at);
// Writes the record as a file's text; what snprintf would return.
int  material_format(const MaterialDesc& desc, const char* texture_path, char* out, usize cap);

} // namespace kv
