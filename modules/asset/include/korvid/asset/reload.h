// Korvid asset: reload (docs/PRDs/Krome-Slice-6-PRD.md, "Reload in the asset module"). A file
// already loaded is read again, and when its bytes changed the same handle is made of the new
// ones: a texture's renderer texture takes the new pixels (at a new size, if it has one), a
// material's row the new look, a shape the new paths, a blob the new bytes. Whatever holds the
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
//
// Who calls it is not the module's business. The swap happens in the kind's pump (PreSim, main
// thread), where loads land, and the event goes out at the next phase boundary.
#pragma once
#include "korvid/core/handle.h"

namespace kv {

struct Engine;

// Asks every record loaded from `path` (a texture, a material, a shape, by its source `.svg`
// too, and a blob) to read its file again. Returns how many were asked: 0 when nothing holds
// that path.
u32 asset_reload(Engine& e, const char* path);
// Every texture, material, shape and blob loaded, each read again and compared.
u32 asset_reload_all(Engine& e);

} // namespace kv
