// Korvid renderer-vulkan: the sprite shader. The draw is indirect, one per bucket; the
// instance index (base included: Slang's SV_InstanceID excludes it) selects a row through
// the sorted index list the compute sort wrote. The quad corners come from the vertex
// index; placement, UVs, tint and texture come from the row and its material.
#include "sprite_common.slangh"

[[vk::binding(0, 0)]] StructuredBuffer<GpuSprite>   gSprites;
[[vk::binding(1, 0)]] StructuredBuffer<GpuMaterial> gMaterials;
[[vk::binding(2, 0)]] StructuredBuffer<uint>        gSorted;
[[vk::binding(0, 1)]] SamplerState                  gSampler;
[[vk::binding(1, 1)]] Texture2D                     gTextures[]; // variable count: last binding

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

[shader("vertex")]
VSOut vs_main(uint vid : SV_VertexID, uint iid : SV_InstanceID, uint base : SV_StartInstanceLocation) {
    uint        row = gSorted[base + iid];
    GpuSprite   s   = gSprites[row];
    GpuMaterial m   = gMaterials[s.material];

    float2 pos; float rot; float2 scale;
    row_placement(s, pos, rot, scale);
    scale *= s.size;
    float cs = cos(rot), sn = sin(rot);
    float4 model  = float4(cs * scale.x, sn * scale.x, -sn * scale.y, cs * scale.y);
    float2 corner = kCorners[vid];
    float2 world  = apply_affine(model, pos, corner);
    float2 clip   = row_to_clip(s, world);

    VSOut o;
    o.pos  = float4(clip, 0.0, 1.0);
    // Texture v runs top to bottom, world y runs bottom to top.
    o.uv   = float2(s.uv.x + (corner.x + 0.5) * s.uv.z,
                    s.uv.y + (0.5 - corner.y) * s.uv.w) + m.params0.xy;
    o.tint = s.tint * m.tint;
    o.tex  = m.texture;
    return o;
}

[shader("fragment")]
float4 fs_main(VSOut i) : SV_Target {
    return gTextures[NonUniformResourceIndex(i.tex)].Sample(gSampler, i.uv) * i.tint;
}
