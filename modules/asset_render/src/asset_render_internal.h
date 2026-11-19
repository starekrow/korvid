// Korvid asset_render: the module's state, shared by textures, materials and the mounts.
#pragma once
#include "korvid/asset_render/material.h"
#include "korvid/asset_render/texture.h"
#include "korvid/core/cli.h"
#include "korvid/core/map.h"
#include "korvid/core/name.h"
#include "korvid/core/pool.h"

namespace kv {

struct Engine;
struct TickContext;
struct ModuleInstance;
struct Allocator;

struct AssetRenderState {
    Pool<TextureAsset>  textures;
    Map<Name, Handle>   by_path;          // interned path -> texture asset
    Pool<MaterialAsset> materials;
    Map<Name, Handle>   material_by_path; // interned path -> material asset
    Allocator*          general = nullptr; // the reads' memory
    u16                 kind = 0, material_kind = 0;
    u32                 loaded = 0, failed = 0;
    u32                 materials_loaded = 0, materials_failed = 0;
};

AssetRenderState* asset_render_state(Engine& e);

// Completions of this kind whose record is gone (retired while the read was out): reaped and
// their data freed, so they do not sit on the queue's list.
void asset_render_reap_orphans(Engine& e, AssetRenderState& s, u16 kind);

// ---- textures (texture.cc) ----
bool   asset_textures_init(Engine& e, AssetRenderState& s, ModuleInstance& self);
void   asset_textures_shutdown(Engine& e, AssetRenderState& s);
void   asset_textures_pump(TickContext& ctx);  // the "asset_render.pump" system
Handle texture_load(Engine& e, AssetRenderState& s, const char* path); // no scope, refs untouched

// ---- materials (material.cc) ----
bool   asset_materials_init(Engine& e, AssetRenderState& s, ModuleInstance& self);
void   asset_materials_shutdown(Engine& e, AssetRenderState& s);
void   asset_materials_pump(TickContext& ctx); // the "asset_render.pump_materials" system

// ---- fonts' pages (fonts.cc): the asset module's AssetFontPages, drawn here ----
void   asset_render_fonts_init(Engine& e);
void   asset_render_fonts_shutdown(Engine& e);

// ---- the CLI (mounts.cc) ----
extern const CliMountDesc k_asset_render_mounts[2];

} // namespace kv
