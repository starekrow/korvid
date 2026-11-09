// example_arcfire: the field. The sim plays on a 540 x 720 rectangle of world units centered
// on the origin, +y up, whatever the window: the camera fits it (hud.cc) and the side
// panels cover the rest. Everything that clamps, culls or wraps reads these.
#pragma once
#include "korvid/math/vec2.h"

namespace kv::arcfire {

constexpr f32 kFieldW = 540.0f;
constexpr f32 kFieldH = 720.0f;
constexpr f32 kFieldLeft = -kFieldW * 0.5f, kFieldRight = kFieldW * 0.5f;
constexpr f32 kFieldBottom = -kFieldH * 0.5f, kFieldTop = kFieldH * 0.5f;

// How far past the edges a thing may be before it is gone: bullets and shots die past
// this margin, ground rows past the bottom, formations' paths start and end beyond it.
constexpr f32 kCullMargin = 48.0f;

inline bool in_field(point2 p, f32 margin) {
    return p.x > kFieldLeft - margin && p.x < kFieldRight + margin && p.y > kFieldBottom - margin && p.y < kFieldTop + margin;
}

} // namespace kv::arcfire
