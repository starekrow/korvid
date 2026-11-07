// Korvid sprite: the Transform2 component. Flat, no hierarchy. The world keeps a
// previous-tick copy of every transform for render interpolation.
#pragma once
#include "korvid/render/sprite.h"

namespace kv {

// Position, rotation and scale; z ordering is the Sprite's layer.
typedef Placement2 Transform2;

inline Transform2 transform2_at(point2 position, f32 rotation = 0.0f, vec2 scale = vec2{1.0f, 1.0f}) {
    Transform2 t;
    t.position = position;
    t.rotation = rotation;
    t.scale    = scale;
    return t;
}

} // namespace kv
