// renderer-cpu: the glue between one game shader's Slang C++ and the renderer (shader.h). The
// file kormake writes for each shader (slang's shader_cpu rule) includes the generated code and
// then this, inside a namespace of the shader's own, so no two shaders' code meets:
//
//   #include "korvid/renderer-cpu/shader_glue.h"   (outside: the system headers, the API)
//   #define cs_span cs_span_<module>_<stem>        (Slang's entry points are extern "C")
//   ...
//   namespace kv_cpu_<module>_<stem> {
//   #include "<stem>.cpu.cpp"
//   #include "korvid/renderer-cpu/shader_glue.inl"
//   }
//
// It defines kv_cpu_entry, a CpuShaderFn. Slang names a struct's fields with suffixes that
// depend on the shader (CpuSpan_0::tint_2 in one, tint_3 in another), so the span and the
// engine's tables go across by layout, checked by size; the globals (GlobalParams_0) are
// found by name, each only if the shader has it.

// The span kernels' blocks as shaders/cpu_sprite.slangh and cpu_post.slangh lay them out.
struct KvSpriteSpan {
    float uv0[2], duv[2], world0[2], dworld[2], local0[2], dlocal[2], tint[4];
    uint32_t tex, filter, material, count;
    float x0, y;
};
struct KvPostSpan {
    float x0, y;
    uint32_t count, pad;
};

// A texture as the shader's ITexture: its reads go to cpu_shader_sample with the call being run.
inline thread_local const ::kv::CpuShaderCall* kv_call = nullptr;

struct KvTexture : ITexture {
    uint32_t slot = 0;
    bool     world = false;
    static uint32_t sampler_of(SamplerState s) { return (uint32_t)(uintptr_t)s.state; }
    TextureDimensions GetDimensions(int) override {
        TextureDimensions d;
        d.reset();
        d.shape = SLANG_TEXTURE_2D;
        d.numberOfLevels = 1;
        ::kv::cpu_shader_size(*kv_call, slot, world, &d.width, &d.height);
        return d;
    }
    void Load(const int32_t* at, void* out, size_t bytes) override {
        float c[4];
        ::kv::cpu_shader_load(*kv_call, slot, world, at[0], at[1], c);
        memcpy(out, c, bytes < sizeof c ? bytes : sizeof c);
    }
    void Sample(SamplerState s, const float* at, void* out, size_t bytes) override {
        float c[4];
        ::kv::cpu_shader_sample(*kv_call, slot, world, sampler_of(s), at[0], at[1], c);
        memcpy(out, c, bytes < sizeof c ? bytes : sizeof c);
    }
    void SampleLevel(SamplerState s, const float* at, float, void* out, size_t bytes) override { Sample(s, at, out, bytes); }
};

// Every texture slot's object, made once a thread; the shader indexes them by slot.
constexpr uint32_t kKvTextureSlots = 1024; // the renderer's kMaxTextures
struct KvTextures {
    KvTexture                   textures[kKvTextureSlots];
    Texture2D<Vector<float, 4>> views[kKvTextureSlots];
    KvTexture                   world;
    KvTextures() {
        for (uint32_t i = 0; i < kKvTextureSlots; ++i) {
            textures[i].slot = i;
            views[i].texture = &textures[i];
        }
        world.world = true;
    }
};
inline KvTextures& kv_textures() {
    static thread_local KvTextures t;
    return t;
}

// Samplers are told apart by their state's value: 0 nearest and repeat, 1 linear and repeat,
// 2 linear and clamped (shader.h's cpu_shader_sample).
inline SamplerState kv_sampler(uintptr_t which) {
    SamplerState s;
    s.state = (ISamplerState*)which;
    return s;
}

// A template over the shader's globals, so that the branch for the other kind of shader, and
// each global the shader lacks, is never compiled.
template <class G>
inline void kv_run(const ::kv::CpuShaderCall& call) {
    G g{};
    kv_call = &call;
    KvTextures& tx = kv_textures();
    ComputeVaryingInput in{};
    in.endGroupID = uint3{1, 1, 1};
    if constexpr (requires(G& x) { x.gPost_0; }) {
        // A post shader: the world's image, its parameters, its push block.
        if (call.kind != ::kv::CpuShaderKind::Post) return;
        static_assert(sizeof(*g.gPost_0) == sizeof(::kv::CpuPostPush), "post_fragment.slangh's PostPush and shader.h's CpuPostPush differ");
        static_assert(sizeof(*g.gSpan_0) == sizeof(KvPostSpan), "cpu_post.slangh's span and the glue's differ");
        KvPostSpan span{call.x0, call.y, call.count, 0};
        g.gPost_0 = (decltype(g.gPost_0))call.post;
        g.gSpan_0 = (decltype(g.gSpan_0))&span;
        if constexpr (requires(G& x) { x.gWorld_0; }) g.gWorld_0.texture = &tx.world;
        if constexpr (requires(G& x) { x.gWorldSampler_0; }) g.gWorldSampler_0 = kv_sampler(2);
        if constexpr (requires(G& x) { x.gPostParams_0; }) {
            static const float none[4] = {};
            g.gPostParams_0.data = (decltype(g.gPostParams_0.data))(call.param_count ? call.params : none);
            g.gPostParams_0.count = call.param_count ? call.param_count : 1;
        }
        g.gOut_0.data = (decltype(g.gOut_0.data))call.out;
        g.gOut_0.count = call.count;
        cs_span(&in, nullptr, &g);
    } else {
        // A sprite shader: the push block, the layers, the materials and the textures.
        if (call.kind != ::kv::CpuShaderKind::Sprite) return;
        static_assert(sizeof(*g.gSpan_0) == sizeof(KvSpriteSpan), "cpu_sprite.slangh's span and the glue's differ");
        KvSpriteSpan span;
        memcpy(span.uv0, call.uv0, sizeof span.uv0);
        memcpy(span.duv, call.duv, sizeof span.duv);
        memcpy(span.world0, call.world0, sizeof span.world0);
        memcpy(span.dworld, call.dworld, sizeof span.dworld);
        memcpy(span.local0, call.local0, sizeof span.local0);
        memcpy(span.dlocal, call.dlocal, sizeof span.dlocal);
        memcpy(span.tint, call.tint, sizeof span.tint);
        span.tex = call.tex;
        span.filter = call.filter;
        span.material = call.material;
        span.count = call.count;
        span.x0 = call.x0;
        span.y = call.y;
        g.gSpan_0 = (decltype(g.gSpan_0))&span;
        if constexpr (requires(G& x) { x.gPush_0; }) {
            static_assert(sizeof(*g.gPush_0) == 112, "sprite_common.slangh's PushData and the engine's differ");
            g.gPush_0 = (decltype(g.gPush_0))call.push;
        }
        if constexpr (requires(G& x) { x.gLayers_0; }) {
            g.gLayers_0.data = (decltype(g.gLayers_0.data))call.layers;
            g.gLayers_0.count = call.layer_count;
        }
        if constexpr (requires(G& x) { x.gMaterials_0; }) {
            static_assert(sizeof(*g.gMaterials_0.data) == 64, "sprite_common.slangh's GpuMaterial and the engine's differ");
            g.gMaterials_0.data = (decltype(g.gMaterials_0.data))call.materials;
            g.gMaterials_0.count = call.material_count;
        }
        if constexpr (requires(G& x) { x.gSamplers_0; }) {
            g.gSamplers_0[0] = kv_sampler(0);
            g.gSamplers_0[1] = kv_sampler(1);
        }
        if constexpr (requires(G& x) { x.gTextures_0; }) {
            g.gTextures_0.data = tx.views;
            g.gTextures_0.count = call.texture_count < kKvTextureSlots ? call.texture_count : kKvTextureSlots;
        }
        g.gOut_0.data = (decltype(g.gOut_0.data))call.out;
        g.gOut_0.count = call.count;
        cs_span(&in, nullptr, &g);
    }
    kv_call = nullptr;
}

inline void kv_cpu_entry(const ::kv::CpuShaderCall& call) { kv_run<GlobalParams_0>(call); }
