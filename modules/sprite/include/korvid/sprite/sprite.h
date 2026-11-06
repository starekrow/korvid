// Korvid sprite: the Sprite component. Material handle, UV rect, size, layer and tint.
#pragma once
#include "korvid/render/material.h"
#include "korvid/math/rect2.h"

namespace kv {

struct Sprite {
    Handle material;                 // null for now: materials are stubbed (render/material.h)
    rect2  uv   = rect2_unit();      // sub-rect of the texture
    vec2   size{64.0f, 64.0f};       // world units
    color  tint = color_white();
    i32    layer = 0;                // ascending draw order
};

} // namespace kv
