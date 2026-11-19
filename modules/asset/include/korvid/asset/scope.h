// Korvid asset: lifetimes. A scope is a named set of asset references; an asset lives
// while any scope holds it or another asset depends on it, and goes away when the last
// reference does.
//
//   Handle level = asset_scope_create(e, "town");
//   Handle look  = asset_load_material(e, "game/grass.material.json", level); // asset_render's
//   ...
//   asset_scope_release(e, level); // the material, and its texture unless someone else holds it
//
// Every load takes a scope; the loads without one go into the global scope, which is never
// released, so code written before scopes keeps its assets for the run. Loading a path
// another scope already holds adds a reference and returns the same handle, so two scopes
// share what they both load. Dependencies hold their own references: a material holds its
// texture, so releasing a texture's scope never pulls it out from under a material.
//
// A count that reaches zero retires the record at the next asset pump, not at once: a
// load of the same path in between (the next level's scope taking what this one let go)
// keeps it. Retiring destroys what the record made (asset_render's renderer texture and
// material, past the frames in flight; sprites still naming a destroyed material fall back
// to the default look),
// cancels a read still in the queue, takes the path out of the name table so the next
// load reads the file again, and makes the handle stale. A sound's samples are freed only
// once the audio module says the mixer has let go of them.
#pragma once
#include "korvid/asset/asset.h"
#include "korvid/core/handle.h"

namespace kv {

struct Engine;

constexpr u32 KV_ASSET_SCOPE_NAME_MAX = 32; // bytes, NUL included
constexpr u32 KV_ASSET_SCOPES         = 256;

// Every member of the scope is Ready or Failed. Sent once when that first becomes true
// (by the first pump of the tick after the last member settled; at the first pump for a
// scope that is empty), and again after a later load into the scope has settled. `failed` counts the members that failed.
struct AssetScopeReady { Handle scope; u32 failed; };

struct AssetScopeProgress {
    u32 members = 0;
    u32 pending = 0, ready = 0, failed = 0;
    u64 bytes   = 0; // what the members occupy: decoded pixels, samples, atlases and tables
};

// A new scope (asset.scope). The name is for logs and the CLI; it need not be unique.
// Null if the module is not up or KV_ASSET_SCOPES are alive.
Handle             asset_scope_create(Engine& e, const char* name);
// The scope the scope-less loads use. Releasing it is refused.
Handle             asset_scope_global(Engine& e);
// Drops the scope's reference on each member and destroys the scope. False for a stale
// handle or the global scope.
bool               asset_scope_release(Engine& e, Handle scope);
// Adds a reference from the scope to an asset already loaded (by any scope). False for a
// stale scope or asset; true and nothing more if the scope already holds it.
bool               asset_scope_add(Engine& e, Handle scope, Handle asset);
// Where a scope's loads are. All zero for a stale handle.
AssetScopeProgress asset_scope_progress(Engine& e, Handle scope);
// The scope's name, or null if stale.
const char*        asset_scope_name(Engine& e, Handle scope);
// References on an asset (scopes and dependents); 0 for a stale handle.
u32                asset_refs(Engine& e, Handle asset);

// Asset kinds of other modules. A module that keeps records of its own (textures and
// materials in asset_render, tile maps, clips) registers their handle kind here, and from then
// on scopes hold them, counts reach them, readiness waits for them and retirement calls back,
// exactly as for the asset module's own kinds. The callbacks run on the main thread; retire
// runs at the asset pump ("asset.pump_scopes") once a record's count is zero, and must
// destroy the record (its handle then goes stale). A kind's own pump runs after that one.
struct AssetKindDesc {
    u16         kind = 0;                                    // the handle kind the records use
    u32*        (*refs)(void* user, Handle h)   = nullptr;   // the record's count, null if stale
    AssetState  (*state)(void* user, Handle h)  = nullptr;   // Pending, Ready or Failed
    u64         (*bytes)(void* user, Handle h)  = nullptr;   // optional: for progress
    const char* (*path)(void* user, Handle h)   = nullptr;   // optional: for logs and the CLI
    void        (*retire)(Engine& e, void* user, Handle h) = nullptr;
    // Optional: asset_reload and asset_reload_all reach the kind through this. Read again
    // every record loaded from `path` (every record, for null); how many were asked. The
    // helpers in korvid/asset/reload.h do it the way the asset module's kinds do.
    u32         (*reload)(Engine& e, void* user, const char* path) = nullptr;
    // Optional: the CLI's `scope load <scope> <path>` loads paths ending in `suffix`
    // (".png") through this, into `scope`.
    const char* suffix = nullptr;
    Handle      (*load)(Engine& e, void* user, const char* path, Handle scope) = nullptr;
    void*       user = nullptr;
};
constexpr u32 KV_ASSET_EXTERNAL_KINDS = 16;
bool asset_kind_register(Engine& e, const AssetKindDesc& desc);
void asset_kind_unregister(Engine& e, u16 kind);
// What a load does with its scope, for a kind's own load function: the scope takes a
// reference on the asset (once); a null scope takes none, and a record nobody holds then
// retires at the next pump. Returns the asset; null for a stale scope or a null asset.
Handle asset_hold(Engine& e, Handle scope, Handle asset);
// References for dependencies between assets: a record that needs another (a map its
// tilesets' textures) loads it with a null scope, which takes no reference, then holds one
// with asset_ref and drops it with asset_unref when it no longer needs it (at its
// retirement). An asset loaded with a null scope that nobody refs retires at the next pump.
void asset_ref(Engine& e, Handle asset);
void asset_unref(Engine& e, Handle asset);

// Counts for tools and tests: assets retired so far, retired sample buffers still waiting
// for the mixer's word and how many were ever handed to it, records still queued to
// retire at the next pump, live scopes.
struct AssetLifetimeStats { u32 retired, samples_out, samples_deferred, dying, scopes; };
AssetLifetimeStats asset_lifetime_stats(Engine& e);

// For the audio module: called when a sound's samples are about to be freed, with a ticket
// and the samples. Return true to take the ticket and call asset_retire_done(e, ticket)
// once nothing on the device thread can read them; false to have them freed at once.
// One hook; a second registration replaces the first, null removes it.
typedef bool (*AssetSamplesRetireFn)(void* user, u64 ticket, const f32* samples);
void asset_samples_retire_hook(Engine& e, AssetSamplesRetireFn fn, void* user);
void asset_retire_done(Engine& e, u64 ticket);

} // namespace kv
