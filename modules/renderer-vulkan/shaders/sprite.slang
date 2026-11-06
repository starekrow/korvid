// Korvid renderer-vulkan: the sprite shader. One instanced draw per blend batch. The quad
// corners come from the vertex index; placement, UVs, tint and texture slot come from the
// instance buffer. Layouts mirror GpuInstance and PushData in renderer.cc exactly.

struct SpriteInstance {
    float4 model;   // affine2 a, b, c, d: columns of the 2x2 part
    float2 trans;   // affine2 tx, ty
    float2 pad0;
    float4 uv;      // x, y, w, h in texture space
    float4 tint;    // linear RGBA, multiplies the texel
    uint4  tex;     // x = bindless texture slot
};

struct PushData {
    float4 view;           // ortho affine2 a, b, c, d
    float2 view_trans;     // ortho tx, ty
    uint   first_instance; // this batch's start in the instance buffer
    uint   pad;
};

[[vk::binding(0, 0)]] StructuredBuffer<SpriteInstance> gInstances;
[[vk::binding(1, 0)]] SamplerState                     gSampler;
[[vk::binding(2, 0)]] Texture2D                        gTextures[]; // variable count: last binding
[[vk::push_constant]] ConstantBuffer<PushData>         gPush;

struct VSOut {
    float4 pos  : SV_Position;
    float2 uv   : UV;
    float4 tint : TINT;
    nointerpolation uint tex : TEX;
};

// Two triangles over the unit quad centered at the origin, +y up in world space.
static const float2 kCorners[6] = {
    float2(-0.5, -0.5), float2( 0.5, -0.5), float2( 0.5,  0.5),
    float2(-0.5, -0.5), float2( 0.5,  0.5), float2(-0.5,  0.5),
};

float2 apply(float4 m, float2 t, float2 p) {
    return float2(m.x * p.x + m.z * p.y + t.x,
                  m.y * p.x + m.w * p.y + t.y);
}

[shader("vertex")]
VSOut vs_main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    SpriteInstance inst = gInstances[gPush.first_instance + iid];
    float2 corner = kCorners[vid];
    float2 world  = apply(inst.model, inst.trans, corner);
    float2 clip   = apply(gPush.view, gPush.view_trans, world);

    VSOut o;
    o.pos  = float4(clip, 0.0, 1.0);
    // Texture v runs top to bottom, world y runs bottom to top.
    o.uv   = float2(inst.uv.x + (corner.x + 0.5) * inst.uv.z,
                    inst.uv.y + (0.5 - corner.y) * inst.uv.w);
    o.tint = inst.tint;
    o.tex  = inst.tex.x;
    return o;
}

[shader("fragment")]
float4 fs_main(VSOut i) : SV_Target {
    return gTextures[NonUniformResourceIndex(i.tex)].Sample(gSampler, i.uv) * i.tint;
}
