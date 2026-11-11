// Korvid render: the sprite shader. The draw is indirect, one per bucket; the
// instance index (base included: Slang's SV_InstanceID excludes it) selects a row through
// the sorted index list the compute sort wrote. The quad corners come from the vertex
// index; placement, UVs, tint and texture come from the row and its material. A game's
// fragment shader replaces fs_main for its material's sprites (sprite_fragment.slangh).
#include "sprite_fragment.slangh"

[[vk::binding(0, 0)]] StructuredBuffer<GpuSprite>   gSprites;
[[vk::binding(2, 0)]] StructuredBuffer<uint>        gSorted;

// Two triangles over the unit quad centered at the origin, +y up in world space.
static const float2 kCorners[6] = {
    float2(-0.5, -0.5), float2( 0.5, -0.5), float2( 0.5,  0.5),
    float2(-0.5, -0.5), float2( 0.5,  0.5), float2(-0.5,  0.5),
};

[shader("vertex")]
SpriteVaryings vs_main(uint vid : SV_VertexID, uint iid : SV_InstanceID, uint base : SV_StartInstanceLocation) {
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

    SpriteVaryings o;
    o.pos  = float4(clip, 0.0, 1.0);
    // Texture v runs top to bottom, world y runs bottom to top.
    o.uv   = float2(s.uv.x + (corner.x + 0.5) * s.uv.z,
                    s.uv.y + (0.5 - corner.y) * s.uv.w) + m.params0.xy;
    o.tint = s.tint * m.tint;
    o.world = world;
    o.local = corner + 0.5;
    o.tex  = m.texture;
    o.filter = min(m.filter, 1u);
    o.material = s.material;
    return o;
}

[shader("fragment")]
float4 fs_main(SpriteVaryings i) : SV_Target {
    return sprite_texel(i, i.uv) * i.tint;
}
