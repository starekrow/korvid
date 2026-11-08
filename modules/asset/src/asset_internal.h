// Korvid asset: the module's state, shared by the record kinds and the scopes.
#pragma once
#include "korvid/asset/blob.h"
#include "korvid/asset/font.h"
#include "korvid/asset/material.h"
#include "korvid/asset/scope.h"
#include "korvid/asset/sound.h"
#include "korvid/asset/stream.h"
#include "korvid/asset/texture.h"
#include "korvid/core/array.h"
#include "korvid/core/cli.h"
#include "korvid/core/map.h"
#include "korvid/core/name.h"
#include "korvid/core/pool.h"

namespace kv {

struct Engine;
struct IoQueue;
struct TickContext;
struct ModuleInstance;

struct AssetScope_ {
    char          name[KV_ASSET_SCOPE_NAME_MAX];
    Array<Handle> members;        // each holds one reference
    bool          global = false;
    bool          ready_sent = false;
};

// Memory whose owner has gone but which another thread may still read: freed when the
// ticket comes back.
struct AssetGrave { u64 ticket; void* ptr; usize bytes; };

struct AssetState_ {
    Pool<TextureAsset>  textures;
    Map<Name, Handle>   by_path;          // interned path -> texture asset
    Pool<MaterialAsset> materials;
    Map<Name, Handle>   material_by_path; // interned path -> material asset
    Pool<FontAsset>     fonts;
    Map<Name, Handle>   font_by_key;      // interned "path@px" -> font asset
    Pool<SoundAsset>    sounds;
    Map<Name, Handle>   sound_by_path;
    Pool<StreamAsset>   streams;
    Map<Name, Handle>   stream_by_path;
    Pool<BlobAsset>     blobs;
    Map<Name, Handle>   blob_by_path;
    Pool<AssetScope_>   scopes;
    Handle              global_scope;
    Array<Handle>       dying;            // reached zero references; retired at the next pump if still there
    Array<AssetGrave>   graves;
    u64                 next_ticket = 1;
    AssetKindDesc       external[KV_ASSET_EXTERNAL_KINDS];
    u32                 external_count = 0;
    AssetSamplesRetireFn samples_hook = nullptr;
    void*               samples_hook_user = nullptr;
    Allocator*          general = nullptr;
    u16                 kind = 0, material_kind = 0, font_kind = 0, sound_kind = 0, blob_kind = 0, stream_kind = 0, scope_kind = 0;
    u32                 retired = 0, deferred = 0;
    u32                 loaded = 0, failed = 0;
    u32                 materials_loaded = 0, materials_failed = 0;
    u32                 fonts_loaded = 0, fonts_failed = 0;
    u32                 sounds_loaded = 0, sounds_failed = 0;
    u32                 blobs_loaded = 0, blobs_failed = 0;
};

AssetState_* asset_state(Engine& e);

// Whether an I/O completion's user word names an asset of this kind.
inline bool asset_is_kind(const AssetState_&, u64 user, u16 kind) { return Handle{user}.kind() == kind && kind != 0; }

// ---- references (scope.cc) ----
bool asset_scopes_init(Engine& e, AssetState_& s, ModuleInstance& self);
void asset_scopes_shutdown(Engine& e, AssetState_& s);
void asset_scopes_pump(TickContext& ctx);     // the "asset.pump_scopes" system: retire, then readiness
u32* asset_refs_of(AssetState_& s, Handle asset);
void asset_ref(AssetState_& s, Handle asset);
void asset_unref(AssetState_& s, Handle asset); // at zero: queued for the next pump
// The scope takes a reference on the asset (once); returns the asset, or null and an
// error for a stale scope (the asset is then left with whatever references it had).
Handle asset_hold(AssetState_& s, Handle scope, Handle asset);
// Frees now, or hands to the samples hook and frees when the ticket returns.
void asset_bury_samples(AssetState_& s, f32* samples, usize bytes);
// Completions of this kind whose record is gone (retired while the read was out): reaped
// and their data freed, so they do not sit on the queue's list.
void asset_reap_orphans(Engine& e, AssetState_& s, u16 kind);
// The module's CLI tables (mounts.cc, scope.cc); declared here so their definitions have
// external linkage whatever order the unity build puts the files in.
extern const CliMountDesc   k_asset_mounts[5];
extern const CliCommandDesc k_asset_commands[3];
// The /scopes mount.
void asset_scopes_list(CliContext& ctx, const char* dir, CliEmitFn emit, void* user);
bool asset_scopes_describe(CliContext& ctx, const char* rel);

// ---- the kinds: load without a scope (refs untouched), retire at zero ----
Handle texture_load(Engine& e, AssetState_& s, const char* path);
Handle material_load(Engine& e, AssetState_& s, const char* path);
Handle font_load(Engine& e, AssetState_& s, const char* path, const FontDesc& desc);
Handle sound_load(Engine& e, AssetState_& s, const char* path);
Handle blob_load(Engine& e, AssetState_& s, const char* path);
Handle stream_load(Engine& e, AssetState_& s, const char* path);
void   texture_retire(Engine& e, AssetState_& s, Handle h);
void   material_retire(Engine& e, AssetState_& s, Handle h);
void   font_retire(Engine& e, AssetState_& s, Handle h);
void   sound_retire(Engine& e, AssetState_& s, Handle h);
void   blob_retire(Engine& e, AssetState_& s, Handle h);
void   stream_retire(Engine& e, AssetState_& s, Handle h);

bool asset_materials_init(Engine& e, AssetState_& s, ModuleInstance& self);
void asset_materials_shutdown(Engine& e, AssetState_& s);
void asset_materials_pump(TickContext& ctx); // the "asset.pump_materials" system

bool asset_fonts_init(Engine& e, AssetState_& s, ModuleInstance& self);
void asset_fonts_shutdown(Engine& e, AssetState_& s);
void asset_fonts_pump(TickContext& ctx);     // the "asset.pump_fonts" system

bool asset_sounds_init(Engine& e, AssetState_& s, ModuleInstance& self);
void asset_sounds_shutdown(Engine& e, AssetState_& s);
void asset_sounds_pump(TickContext& ctx);    // the "asset.pump_sounds" system

bool asset_blobs_init(Engine& e, AssetState_& s, ModuleInstance& self);
void asset_blobs_shutdown(Engine& e, AssetState_& s);
void asset_blobs_pump(TickContext& ctx);     // the "asset.pump_blobs" system

bool asset_streams_init(Engine& e, AssetState_& s, ModuleInstance& self);
void asset_streams_shutdown(Engine& e, AssetState_& s);
void asset_streams_pump(TickContext& ctx);   // the "asset.pump_streams" system

} // namespace kv
