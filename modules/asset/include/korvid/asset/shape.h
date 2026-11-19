// Korvid asset: shapes (docs/PRDs/Krome-Slice-5-PRD.md, "The shape asset"). A .kshape file (the
// svg module's cook of an .svg) read into a patherizer shape: the same handle at once, the same
// read through the mounts, states, events, scopes and retirement as every other kind. A path
// that names the source (`icons/check.svg`) loads its cook (`icons/check.kshape`).
//
//   Handle h = asset_load_shape(e, "icons/check.svg");
//   if (const ShapeAsset* s = asset_shape(e, h); s && s->state == AssetState::Ready) patherizer_draw(s->shape, ...);
#pragma once
#include "korvid/asset/texture.h"
#include "korvid/patherizer/patherizer.h"

namespace kv {

struct ShapeAsset {
    char       path[KV_ASSET_PATH_MAX]; // the .kshape read
    AssetState state = AssetState::Pending;
    AssetError error = AssetError::None;
    Handle     io;
    PathShape  shape;          // once Ready
    bool       one_colour = false; // every paint currentColor: draw it once, white, and tint it
    u32        refs = 0;
    u64        hash = 0;           // the file's bytes it was made from
    u32        generation = 0;     // times made again from a changed file (korvid/asset/reload.h)
    bool       reread = false, again = false; // the module's
};

// The scope takes a reference; without one, the global scope does. The same path is the same
// handle; `.svg` is read as its `.kshape`.
Handle            asset_load_shape(Engine& e, const char* path, Handle scope);
Handle            asset_load_shape(Engine& e, const char* path);
const ShapeAsset* asset_shape(Engine& e, Handle asset);

} // namespace kv
