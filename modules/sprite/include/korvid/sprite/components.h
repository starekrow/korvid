// Korvid sprite: the schemas of Transform2 and Sprite, declared once in the module's
// component table. Anyone comparing, hashing or printing rows reads them from here.
#pragma once
#include "korvid/core/component.h"

namespace kv {

const ComponentDesc& transform2_schema();
const ComponentDesc& sprite_schema();

} // namespace kv
