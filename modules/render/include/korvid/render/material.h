// Korvid render: materials are renderer-owned records; the outer engine holds handles
// the renderer issued and edits them through the render interface. A material names a
// texture (another renderer handle), a blend mode and a tint. The small parameter block
// comes later, when materials move into a GPU-side table.
#pragma once
#include "korvid/core/handle.h"
#include "korvid/math/color.h"

namespace kv {

enum class BlendMode : u8 { Opaque, Alpha, Additive, Count };

struct MaterialDesc {
    Handle    texture;                  // renderer texture handle; null: the default texture
    BlendMode blend = BlendMode::Alpha;
    color     tint  = color_white();
};

} // namespace kv
