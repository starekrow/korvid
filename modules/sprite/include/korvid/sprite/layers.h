// Korvid sprite: layer scroll. Every world sprite on a layer is drawn moved by that layer's
// offset, interpolated between ticks like a sprite's placement, so a plane of background
// tiles, or a map of a few thousand, scrolls for one value a tick and its sprites never
// change. Published by the sprite module as the resource "sprite.layers"; a system that
// scrolls declares the write and sets offsets through ctx.resource<SpriteLayers>(...).
//
// Presentation only: a sprite's Transform2 stays its world position, so collision and the
// game never see the offset, and the state hash does not include it. A layer that wraps (a
// repeating background moved back by a whole tile) sets with `snap`, and the renderer does
// not interpolate across the jump. Screen sprites ignore it.
#pragma once
#include "korvid/math/vec2.h"

namespace kv {

struct Engine;

constexpr i32 KV_SPRITE_LAYER_MIN = -128, KV_SPRITE_LAYER_MAX = 127;

struct SpriteLayers {
    vec2 offset[256]; // by layer - KV_SPRITE_LAYER_MIN
    u8   snap[256];   // set with the offset; the extract sends it and clears it
    vec2 tick_start[256]; // the offsets as the last tick began (sprite.layers_tick): what a frame
                          // holding several ticks interpolates the last one from

    void set(i32 layer, vec2 o, bool snap_to = false) {
        if (layer < KV_SPRITE_LAYER_MIN || layer > KV_SPRITE_LAYER_MAX) return;
        u32 i = (u32)(layer - KV_SPRITE_LAYER_MIN);
        offset[i] = o;
        if (snap_to) snap[i] = 1;
    }
    vec2 get(i32 layer) const {
        return layer < KV_SPRITE_LAYER_MIN || layer > KV_SPRITE_LAYER_MAX ? vec2{0, 0} : offset[layer - KV_SPRITE_LAYER_MIN];
    }
};

// The layers, or null if the sprite module is not started. For code outside the schedule.
SpriteLayers* sprite_layers(Engine& e);

} // namespace kv
