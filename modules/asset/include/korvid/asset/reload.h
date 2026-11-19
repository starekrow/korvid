// Korvid asset: reload (docs/PRDs/Krome-Slice-6-PRD.md, "Reload in the asset module"). A file
// already loaded is read again, and when its bytes changed the same handle is made of the new
// ones: a shape the new paths, a blob the new bytes, and in asset_render a texture's renderer
// texture the new pixels (at a new size, if it has one), a material's row the new look. Whatever holds the
// handle draws the new asset on the next frame with no code of its own; whoever parsed the old
// one hears AssetReloaded{asset} and parses again.
//
//   asset_reload(e, "mygame/ships.png");   // goldberg's connection, the CLI's `reload`, a test
//
// - Bytes that hash the same as what the record was made from change nothing and send nothing.
// - A file that no longer reads or decodes keeps what the record had: AssetFailed{asset, error,
//   reload = true}, and the record stays Ready.
// - A record whose first read is still out is read again once it lands. A record that failed
//   is loaded again, as if for the first time (AssetReady when it works).
// - Fonts, sounds and streams do not reload yet: asset_reload finds nothing by their paths.
// - Another module's kind reloads when it registers AssetKindDesc::reload (asset_render's do).
//
// Who calls it is not the module's business. The swap happens in the kind's pump (PreSim, main
// thread), where loads land, and the event goes out at the next phase boundary.
#pragma once
#include "korvid/asset/asset.h"

namespace kv {

struct Engine;
struct Allocator;

// Asks every record loaded from `path` (a texture, a material, a shape, by its source `.svg`
// too, and a blob) to read its file again. Returns how many were asked: 0 when nothing holds
// that path.
u32 asset_reload(Engine& e, const char* path);
// Every texture, material, shape and blob loaded, each read again and compared.
u32 asset_reload_all(Engine& e);

// ---- for kinds of other modules (AssetKindDesc::reload) -------------------------------------
//
// A record that reloads keeps these fields (TextureAsset's, BlobAsset's): its path, state, the
// read out (null if none), and the two flags. asset_reread submits the read with the record's
// handle as its user word; the kind's pump takes it as it takes a load, and when `reread` is
// set compares the bytes (asset_reload_unchanged), uses them (asset_reloaded) or keeps what it
// had (asset_reload_failed). Then asset_reread_after, for a read asked for while one was out.
struct AssetReread {
    const char* path;
    AssetState* state;
    Handle*     io;
    bool*       reread;
    bool*       again;
};
template <class R> AssetReread asset_reread_of(R& r) { return AssetReread{r.path, &r.state, &r.io, &r.reread, &r.again}; }
// Reads the record's file again into `alloc`. A read already out sets `again`; a Ready
// record's read is a reload (`reread`); any other is a load from the start (a Failed record
// goes back to Pending). False if nothing could be submitted.
bool asset_reread(Engine& e, Allocator* alloc, Handle h, const AssetReread& r);
// After the kind's pump took a read: the read asked for while it was out, if any.
void asset_reread_after(Engine& e, Allocator* alloc, Handle h, const AssetReread& r);
// A reload that could not be used: logged, AssetFailed{h, err, reload}, the record untouched.
void asset_reload_failed(Engine& e, Handle h, const char* path, AssetError err, const char* detail);
// A reload that changed the record: its generation, the log, AssetReloaded.
void asset_reloaded(Engine& e, Handle h, const char* path, u32& generation);
// Bytes that hash the same as the record's: logged at debug, nothing sent.
void asset_reload_unchanged(const char* path);

} // namespace kv
