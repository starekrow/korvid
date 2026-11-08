// Korvid render: materials are renderer-owned rows; the outer engine holds handles the
// renderer issued and edits them through the render interface. A material names a texture
// (another renderer handle), a blend mode, a tint and a small parameter block. The
// renderer keeps the rows in a GPU table addressed by the handle's index; the sprite
// shader reads the row, so an edit reaches the next frame and no sprite has to change.
#pragma once
#include "korvid/core/handle.h"
#include "korvid/math/color.h"

namespace kv {

enum class BlendMode : u8 { Opaque, Alpha, Additive, Count };

// How the texture is sampled. Nearest keeps pixel art crisp at integer zooms; Linear
// smooths anything drawn at a scale of its own, text above all.
enum class TextureFilter : u8 { Nearest, Linear, Count };

// Two float4s the shader may interpret. The sprite shader uses params[0], params[1] as a
// UV offset; a game's own shaders own the rest.
constexpr u32 KV_MATERIAL_PARAMS = 8;

struct MaterialDesc {
    Handle    texture;                  // renderer texture handle; null: the default texture
    BlendMode blend = BlendMode::Alpha;
    color     tint  = color_white();
    f32       params[KV_MATERIAL_PARAMS] = {};
    TextureFilter filter = TextureFilter::Nearest;
};

const char* blend_mode_name(BlendMode mode);          // "opaque", "alpha", "additive"
bool        blend_mode_parse(const char* s, BlendMode* out);

} // namespace kv
