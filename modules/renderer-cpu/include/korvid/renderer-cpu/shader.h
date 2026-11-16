// Korvid renderer-cpu: game shaders on the CPU (docs/PRDs/CPU-Renderer-PRD.md, "Game shaders").
// A material that names a game shader draws, in this order of preference:
//
//   1. the game's own CPU form of it, registered with renderer_cpu_shader;
//   2. Slang's C++ of it: kormake's shader_cpu rule (modules/slang) compiles each game shader
//      with `slangc -target cpp` inside the engine's span kernel (shaders/cpu_*.slangh) and
//      registers it through a generated module of its own;
//   3. its blend's engine pipeline, the GPU's rule for a shader that will not load.
//
// The fast path skips the first two: every game shader (RendererDesc::cpu_shaders, frameloop's
// --cpu-shaders none) or one by name (renderer_cpu_shader_skip). A post shader takes the same
// steps; skipped, the frame draws without its post pass, as the GPU's does when it will not load.
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

// The game's own CPU form of the shader `name` (a shader file's stem): first choice. Null
// removes it.
void renderer_cpu_shader(const char* name, CpuShaderFn fn);
// Slang's form, registered by the generated module of each game shader: second choice.
void renderer_cpu_shader_generated(const char* name, CpuShaderFn fn);
// The fast path for one shader: its materials draw with their blend's engine pipeline (a post
// shader: no post pass). Takes effect for materials made after it.
void renderer_cpu_shader_skip(const char* name, bool skip = true);

// What a shader's texture read comes to (the generated code's sampler): texture `slot` of the
// call's textures (or, with `world`, the post pass's image) at uv, through the sampler `sampler`
// (0 nearest and repeat, 1 linear and repeat, 2 linear and clamped), linear RGBA out.
void cpu_shader_sample(const CpuShaderCall& call, u32 slot, bool world, u32 sampler, f32 u, f32 v, f32 out[4]);
// A texel by its integer coordinates (Load), and a texture's size.
void cpu_shader_load(const CpuShaderCall& call, u32 slot, bool world, i32 x, i32 y, f32 out[4]);
void cpu_shader_size(const CpuShaderCall& call, u32 slot, bool world, u32* width, u32* height);

} // namespace kv
