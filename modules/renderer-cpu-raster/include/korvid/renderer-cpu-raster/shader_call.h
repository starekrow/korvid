// renderer-cpu-raster: a game shader's call on the CPU (korvid/renderer-cpu/shader.h has the
// registry and the order of preference). Plain data: the kernels fill it for each span of
// pixels, the shader's CPU form (Slang's C++ through shader_glue.inl, or the game's own)
// reads it.
#pragma once
#include "korvid/core/types.h"

namespace kv {

struct PushData;
struct GpuMaterial;

enum class CpuShaderKind : u8 { Sprite, Post };

// The post pass's push block as post_fragment.slangh has it.
struct CpuPostPush {
    f32 size[2];
    f32 time;
    u32 tick;
    u32 count;
    f32 alpha;
    u32 pad0, pad1;
};

// One span of pixels in a row: the shader's colors for `count` pixels into `out`, linear,
// straight alpha; an alpha of kCpuShaderDiscard is a pixel the shader discarded.
struct CpuShaderCall {
    CpuShaderKind kind = CpuShaderKind::Sprite;
    u32 count = 0;
    f32 x0 = 0.0f, y = 0.0f; // the first pixel's centre, window pixels
    // A sprite's varyings at the first pixel and their step a pixel (sprite_fragment.slangh).
    f32 uv0[2] = {}, duv[2] = {};
    f32 world0[2] = {}, dworld[2] = {};
    f32 local0[2] = {}, dlocal[2] = {};
    f32 tint[4] = {};
    u32 tex = 0, filter = 0, material = 0;
    const PushData*    push = nullptr;
    const f32*         layers = nullptr; // kLayerCount float4s
    u32                layer_count = 0;
    const GpuMaterial* materials = nullptr;
    u32                material_count = 0;
    // A post shader's: its push block and parameters (float4s).
    const CpuPostPush* post = nullptr;
    const f32*         params = nullptr;
    u32                param_count = 0;
    // What cpu_shader_sample reads: the renderer's textures, and the post pass's world image.
    const void* textures = nullptr;
    u32         texture_count = 0;
    const void* world = nullptr;
    f32 (*out)[4] = nullptr;
};

constexpr f32 kCpuShaderDiscard = -1.0e30f;
inline bool cpu_shader_discarded(const f32 c[4]) { return c[3] < -1.0e29f; }

using CpuShaderFn = void (*)(const CpuShaderCall& call);

} // namespace kv
