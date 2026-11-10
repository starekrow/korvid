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
// UV offset; a game's own shaders own the rest (see renderer-vulkan's sprite_fragment.slangh).
constexpr u32 KV_MATERIAL_PARAMS = 8;

struct MaterialDesc {
    const char* name = nullptr;         // what saves and tools call it; null: derived from the content
                                        // ("material:<hash>"), the same for the same content in any run
    Handle    texture;                  // renderer texture handle; null: the default texture
    BlendMode blend = BlendMode::Alpha;
    color     tint  = color_white();
    f32       params[KV_MATERIAL_PARAMS] = {};
    TextureFilter filter = TextureFilter::Nearest;
    // A game's fragment shader: the name of a SPIR-V file in <exe>/shaders/ without its .spv
    // ("arcfire_nebula"), built by kormake's shader rule from the game module's
    // shaders/*.slang. Null: the engine's. One that will not load, or past the renderer's
    // five (shader, blend) pairs, draws as the engine's. The name is not copied: intern it.
    const char* shader = nullptr;
};

const char* blend_mode_name(BlendMode mode);          // "opaque", "alpha", "additive"
bool        blend_mode_parse(const char* s, BlendMode* out);

} // namespace kv
