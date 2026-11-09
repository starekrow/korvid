// Korvid anim: the module's state, shared by the clips, the sheets and the players.
#pragma once
#include "korvid/anim/module.h"
#include "korvid/anim/sheet.h"
#include "korvid/core/map.h"
#include "korvid/core/pool.h"

namespace kv {

struct AnimState_ {
    Pool<AnimClip>    clips;
    Map<Name, Handle> by_path;     // a clip's name -> the first live clip with it
    Pool<AnimSheet>   sheets;
    Map<Name, Handle> sheet_by_path;
    Allocator*        general = nullptr;
    u16               kind = 0, sheet_kind = 0;
    u32               loaded = 0, failed = 0, created = 0, sheets_loaded = 0, sheets_failed = 0;
    AnimStats         stats;
};

AnimState_* anim_state(Engine& e);
u32         anim_channel_lanes(AnimChannel c);
// The registration of anim.sheet with the asset module, and its pump (anim.pump).
bool        anim_sheets_init(Engine& e, AnimState_& s, Allocator* general, MemTag tag);
void        anim_sheets_shutdown(Engine& e, AnimState_& s);
void        anim_sheets_pump(Engine& e, AnimState_& s);

} // namespace kv
