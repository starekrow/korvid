// Korvid anim: sprite sheets as data. A .sheet.json names a texture, how it is cut and the
// clips in it; anim_sheet_load makes the texture's material and every clip, so a game
// that animates a character by its sheet writes no uv arithmetic.
//
//   { "texture": "mygame/hero.png", "cell": [16, 16],          the cell size in pixels
//     "fps": 8,                                                the default for every clip
//     "clips": {
//       "walk.down": { "column": 0, "rows": [0, 1, 2, 3], "loop": "loop" },
//       "idle.down": { "cells": [[0, 0]] },                     [column, row] pairs
//       "spin":      { "row": 5, "columns": [0, 1, 2, 3], "fps": 12, "loop": "pingpong" },
//       "attack":    { "cells": [[0, 4]], "duration": 0.3, "loop": "once", "events": [[0.1, "hit"]] } },
//     "sets": { "walk": { "down": "walk.down", "up": "walk.up" } } }
//
// Or "grid": [cols, rows] instead of "cell", when the cells divide the image evenly.
// Every frame lasts 1 / fps; a clip's duration is its frames' unless it gives one; loop
// defaults to "loop". Each clip is an anim.clip named "<sheet path>#<clip>", so an
// AnimPlayer on one saves by that name. A set is a name for a family of clips by variant
// (a walk by facing); anim_sheet_variant looks one up.
//
// The sheet is an asset of its own (anim.sheet, in the asset module's scopes): the file
// and the texture load into nothing and are held by the sheet, which turns Ready
// (AssetReady) when the texture is and its clips are built. It holds its clips, texture
// and material; when the last scope lets go of it they go with it.
#pragma once
#include "korvid/anim/clip.h"

namespace kv {

struct AnimSheetClip { Name name; Handle clip; };           // "walk.down"
struct AnimSheetSet  { Name set; Name variant; Handle clip; }; // walk / down

struct AnimSheet {
    char       path[KV_ASSET_PATH_MAX];
    AssetState state = AssetState::Pending;
    AssetError error = AssetError::None;
    u32        refs  = 0;
    Handle     blob;            // the file, held until the clips are built
    Handle     texture;         // asset_render.texture, held
    Handle     material_asset;  // asset_render.material for the texture (alpha, nearest), held
    Handle     material;        // its renderer material: what a Sprite of this sheet uses
    vec2       cell_size;       // pixels: what a Sprite's size is at scale 1, once Ready
    u32        cols = 0, rows = 0;
    Array<AnimSheetClip> clips; // in the file's order
    Array<AnimSheetSet>  sets;
    char       error_text[160] = {};
};

// Starts loading a sheet into a scope (null: the caller holds it with asset_ref); the same
// path is the same handle.
Handle           anim_sheet_load(Engine& e, const char* path, Handle scope);
const AnimSheet* anim_sheet(Engine& e, Handle sheet);
// A clip by its name in the sheet, or by set and variant; null before Ready or for none.
Handle           anim_sheet_clip(Engine& e, Handle sheet, const char* name);
Handle           anim_sheet_variant(Engine& e, Handle sheet, const char* set, const char* variant);

// The format on its own, for tools and tests: the clips of a sheet whose image is
// `image_w` x `image_h`, as descriptors whose key arrays allocate from `alloc`. Names are
// the clips' own ("walk.down"). Its temporaries go on the thread's scratch: call it inside
// a MemScope. False with the reason on a bad file.
struct AnimSheetParsed {
    char  texture[KV_ASSET_PATH_MAX];
    vec2  cell_size;
    u32   cols = 0, rows = 0;
    Array<AnimClipDesc>  clips;  // name points into the parse's allocator
    Array<AnimSheetSet>  sets;   // clip is the index into clips, as a handle's bits
};
bool anim_sheet_parse_header(const char* text, usize len, char* texture, usize cap, const char** error);
bool anim_sheet_parse(const char* text, usize len, u32 image_w, u32 image_h, Allocator* alloc, AnimSheetParsed& out, const char** error);

} // namespace kv
