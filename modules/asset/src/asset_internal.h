// Korvid asset: the module's state, shared by texture.cc and material.cc.
#pragma once
#include "korvid/asset/material.h"
#include "korvid/asset/texture.h"
#include "korvid/core/map.h"
#include "korvid/core/name.h"
#include "korvid/core/pool.h"

namespace kv {

struct Engine;
struct IoQueue;
struct TickContext;
struct ModuleInstance;

struct AssetState_ {
    Pool<TextureAsset>  textures;
    Map<Name, Handle>   by_path;          // interned path -> texture asset
    Pool<MaterialAsset> materials;
    Map<Name, Handle>   material_by_path; // interned path -> material asset
    Allocator*          general = nullptr;
    u16                 kind = 0, material_kind = 0;
    u32                 loaded = 0, failed = 0;
    u32                 materials_loaded = 0, materials_failed = 0;
};

AssetState_* asset_state(Engine& e);

// Whether an I/O completion's user word names an asset of this kind.
inline bool asset_is_kind(const AssetState_&, u64 user, u16 kind) { return Handle{user}.kind() == kind && kind != 0; }

bool asset_materials_init(Engine& e, AssetState_& s, ModuleInstance& self);
void asset_materials_shutdown(Engine& e, AssetState_& s);
void asset_materials_pump(TickContext& ctx); // the "asset.pump_materials" system

} // namespace kv
