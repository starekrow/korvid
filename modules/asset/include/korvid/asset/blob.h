// Korvid asset: blobs. A file's bytes in memory, as an asset: the same handle at once, the
// same read through the mounts, the same states, events, scopes and retirement as every
// other kind, and no opinion about what the bytes mean. A module that reads a format of
// its own (a Tiled map, an animation clip, a level description) loads the file as a blob
// and parses it when it is Ready: the asset module reads bytes, the module that
// understands them owns the parse.
//
//   Handle h = asset_load_blob(e, "mygame/town.tmj", scope);
//   ...
//   if (const BlobAsset* b = asset_blob(e, h); b && b->state == AssetState::Ready) parse(b->data, b->size);
//
// The bytes are followed by a NUL that `size` does not count, so a text format can be
// parsed in place. They live as long as the record.
#pragma once
#include "korvid/asset/asset.h"

namespace kv {

struct BlobAsset {
    char       path[KV_ASSET_PATH_MAX];
    AssetState state = AssetState::Pending;
    AssetError error = AssetError::None;
    Handle     io;
    u8*        data = nullptr; // size bytes and a NUL, once Ready
    usize      size = 0;
    u32        refs = 0;       // scopes and assets holding it
    u64        hash = 0;       // hash_bytes of data
    u32        generation = 0; // times made again from a changed file (korvid/asset/reload.h):
                               // `data` is then new, and the old bytes are freed
    bool       reread = false, again = false; // the module's
};

// The scope takes a reference; without one, the global scope does. The same path is the
// same handle.
Handle           asset_load_blob(Engine& e, const char* path, Handle scope);
Handle           asset_load_blob(Engine& e, const char* path);
const BlobAsset* asset_blob(Engine& e, Handle asset);

} // namespace kv
