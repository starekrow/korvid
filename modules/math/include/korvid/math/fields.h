// Korvid math: how the math types appear in component schemas. Include this where a
// component with vec2, point2, rect2 or color fields is declared.
#pragma once
#include "korvid/core/component.h"
#include "korvid/math/color.h"
#include "korvid/math/rect2.h"
#include "korvid/math/vec2.h"

namespace kv {

static_assert(sizeof(vec2) == 8 && sizeof(point2) == 8 && sizeof(rect2) == 16 && sizeof(color) == 16,
              "math types are packed f32 so a schema can describe them as F32 x N");
KV_FIELD_TRAITS(vec2,   F32, 2);
KV_FIELD_TRAITS(point2, F32, 2);
KV_FIELD_TRAITS(rect2,  F32, 4);
KV_FIELD_TRAITS(color,  F32, 4);

} // namespace kv
