// Korvid asset_render: materials as files. A material file is JSON next to the textures under the
// module's assets/:
//
//   { "texture": "rocks/rock.png", "blend": "alpha", "tint": [1, 1, 1, 1],
//     "params": [0, 0, 0, 0, 0, 0, 0, 0], "filter": "nearest", "shader": "arcfire_nebula" }
//
// Every field is optional: no texture means the renderer's default, the rest default to
// an alpha-blended, untinted, zero-parameter, nearest-sampled material drawn by the
// engine's fragment shader. "shader" names a game's (MaterialDesc::shader). asset_load_material returns a
// handle at once; the renderer row it names exists from then on (showing the placeholder
// look) so sprites can use it immediately. The file read goes through the I/O queue, the
// parse happens in "asset_render.pump_materials" (phase PreSim, main thread, after "asset_render.pump"),
// the texture it names is loaded through asset_load_texture, and AssetReady arrives once
// that texture is resident (or at once if the file names none). Loading a path twice
// returns the same handle. Tools reach the records by slot and patch them; a patch
// reaches the renderer before the next frame.
#pragma once
#include "korvid/asset_render/texture.h"
#include "korvid/render/material.h"

namespace kv {


// A material asset's record, read through asset_material or asset_material_at. Owned by
// the module; the pointer is valid until the next load or shutdown.
struct MaterialAsset {
    char         path[KV_ASSET_PATH_MAX];    // as given to asset_load_material
    AssetState   state = AssetState::Pending; // Ready once the file is parsed and its texture resident
    AssetError   error = AssetError::None;    // Parse for a bad file; a texture's error is passed through
    Handle       material;      // the renderer's row handle: valid from load on; put it in a Sprite
    Handle       texture_asset; // the asset_render.texture the file named, once parsed; null for none or before
    Handle       io;            // the pending read's request handle while reading; null otherwise
    MaterialDesc desc;          // what the renderer row holds now (texture is the renderer's handle)
    char         texture_path[KV_ASSET_PATH_MAX]; // the file's "texture", or empty for the default
    u32          refs = 0;      // scopes and assets holding it; it holds its texture_asset
    u64          hash = 0;      // the file's bytes it was made from
    u32          generation = 0; // times made again from a changed file (korvid/asset/reload.h)
    bool         reread = false, again = false; // the module's
};

// Starts loading <exe dir>/assets/<path> as a material file. Returns the asset handle at
// once, or null under the same conditions as asset_load_texture. The same path returns
// the same handle. The scope takes a reference (the global scope without one); the
// material takes one on its texture, which therefore lives at least as long.
Handle               asset_load_material(Engine& e, const char* path, Handle scope);
Handle               asset_load_material(Engine& e, const char* path);
// The material for a texture asset, with no file of its own: named "<texture path>:<blend>"
// (":linear" added for that filter), so the renderer row has the same name in every run.
// The same texture, blend and filter is the same record. The scope takes a reference
// (null: the caller holds it with asset_ref); the material holds the texture. Ready when
// the texture is.
Handle               asset_material_for_texture(Engine& e, Handle texture_asset, BlendMode blend = BlendMode::Alpha,
                                                TextureFilter filter = TextureFilter::Nearest, Handle scope = Handle{});
// The record for a handle, or null if the handle is stale or not a material asset.
const MaterialAsset* asset_material(Engine& e, Handle asset);
// The handle a path was loaded as, or null if it never was.
Handle               asset_material_find(Engine& e, const char* path);

// For tools: records by slot, [0, asset_material_count); empty slots are null. Slots are
// stable for a record's life and reused after shutdown only.
u32                  asset_material_count(Engine& e);
const MaterialAsset* asset_material_at(Engine& e, u32 slot);
Handle               asset_material_handle_at(Engine& e, u32 slot);

// Edits: the record and the renderer row change together, from any main-thread code
// (the CLI's patch, a tool, a game's own tweak). `desc.texture` is ignored: the texture
// follows the record's texture path. Set that with asset_material_set_texture, which
// loads the path like any texture (empty for the default) and points the row at it;
// the row shows the placeholder until the texture is resident. False for a stale handle.
bool asset_material_patch(Engine& e, Handle asset, const MaterialDesc& desc);
bool asset_material_set_texture(Engine& e, Handle asset, const char* texture_path);

// The file format on its own, for tools and tests. material_parse fills the fields the
// text has and leaves the rest as `desc` came in; `texture_path` (cap bytes) receives the
// "texture" field. False with the reason and byte offset on a bad file: not an object, an
// unknown field, a wrong type, a bad blend name. material_format writes the record as a
// file's text into `out`; returns what snprintf would.
bool material_parse(const char* text, usize len, MaterialDesc& desc, char* texture_path, usize cap,
                    const char** error, u32* error_at);
int  material_format(const MaterialDesc& desc, const char* texture_path, char* out, usize cap);

} // namespace kv
