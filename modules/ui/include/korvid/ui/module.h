// Korvid ui: the module record.
//
//   handle kinds ui.doc (an asset kind, in the asset module's scopes), ui.screen
//   kind         ui.sprite (Transform2, Sprite; cosmetic): what the screens draw
//   events       UiEvent{screen, node, event, value}
//   systems      ui.update  PreSim, main thread, Always: documents advance, reveals run,
//                           input reaches the topmost screen, events go out
//                ui.draw    PostSim, main thread, Always: layout and the sprite rows
//   commands     ui reload
//   mounts       /ui        the open screens and their nodes
//
// See the module's README.
#pragma once
#include "korvid/core/module.h"
#include "korvid/ui/ui.h"

namespace kv {

KV_MODULE_DECLARE(ui_module);

} // namespace kv
