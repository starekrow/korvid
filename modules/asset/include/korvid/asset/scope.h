// Korvid asset: lifetimes. A scope is a named set of asset references; an asset lives
// while any scope holds it or another asset depends on it, and goes away when the last
// reference does.
//
//   Handle level = asset_scope_create(e, "town");
//   Handle look  = asset_load_material(e, "game/grass.material.json", level);
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
// keeps it. Retiring destroys the renderer's texture and material (past the frames in
// flight; sprites still naming a destroyed material fall back to the default look),
// cancels a read still in the queue, takes the path out of the name table so the next
// load reads the file again, and makes the handle stale. A sound's samples are freed only
// once the audio module says the mixer has let go of them.
#pragma once
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
