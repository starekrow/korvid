// Korvid sprite: the Sprite component. Material handle, UV rect, size, layer and tint.
#pragma once
#include "korvid/render/material.h"
#include "korvid/math/rect2.h"

namespace kv {

struct Sprite {
    Handle material;                 // a renderer material handle; null: the default material
    rect2  uv   = rect2_unit();      // texture rect; outside [0, 1] repeats
    vec2   size{64.0f, 64.0f};       // world units
    color  tint = color_white();
    i32    layer = 0;                // ascending draw order
};

} // namespace kv
