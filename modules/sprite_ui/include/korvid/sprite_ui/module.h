// Korvid sprite_ui: the module record.
//
//   handle kinds sprite_ui.doc (an asset kind, in the asset module's scopes), sprite_ui.screen
//   kind         sprite_ui.sprite (Transform2, Sprite; cosmetic): what the screens draw
//   events       SpuiEvent{screen, node, event, value}
//   systems      sprite_ui.update  PreSim, main thread, Always: documents advance, reveals run,
//                                  input reaches the topmost screen, events go out
//                sprite_ui.draw    PostSim, main thread, Always: layout and the sprite rows
//   commands     spui reload
//   mounts       /spui      the open screens and their nodes
//
// See the module's README.
#pragma once
#include "korvid/core/module.h"
#include "korvid/sprite_ui/sprite_ui.h"

namespace kv {

KV_MODULE_DECLARE(sprite_ui_module);

} // namespace kv
