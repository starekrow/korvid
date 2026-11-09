// Korvid anim: module record.
//
//   handle kinds anim.clip, anim.sheet (both asset kinds in the asset module's scopes)
//   components   AnimPlayer
//   events       AnimEvent{entity, clip, name}, AnimFinished{entity, clip}
//   resources    anim.clips
//   systems      anim.pump   PreSim, main thread, Always: clip files that arrived are parsed,
//                            sheets whose texture arrived get their clips
//                anim.play   PostSim, Always, after text.layout and before scene.compose when
//                            present: reads *.AnimPlayer; writes *.AnimPlayer, *.Sprite,
//                            *.Local2, *.Transform2
//
// See the module's README.
#pragma once
#include "korvid/anim/clip.h"
#include "korvid/anim/player.h"
#include "korvid/core/module.h"

namespace kv {

KV_MODULE_DECLARE(anim_module);

struct AnimStats {
    u32 players  = 0; // players seen last tick
    u32 advanced = 0; // players that moved
    u32 events   = 0; // event keys fired
    u32 finished = 0; // Once clips that ended
};
const AnimStats* anim_stats(Engine& e);

} // namespace kv
