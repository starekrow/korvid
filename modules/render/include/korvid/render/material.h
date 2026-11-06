// Korvid render: materials. STUB in this step: the struct exists so Sprite and the packet
// can carry a material handle, but no pool, no loader and no lookup exist yet. A null
// material handle renders with the renderer's built-in default texture.
#pragma once
#include "korvid/core/handle.h"
#include "korvid/math/color.h"

namespace kv {

enum class BlendMode : u8 { Opaque, Alpha, Additive, Count };

struct Material {
    Handle    texture;                  // asset handle; always null for now
    BlendMode blend = BlendMode::Alpha;
    color     tint  = color_white();
    // A small parameter block comes with the material pool.
};

} // namespace kv
