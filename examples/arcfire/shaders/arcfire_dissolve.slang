// example_arcfire: the dissolve. The sprite's own texture, eaten away where noise falls
// below the sprite's tint alpha (its progress: 0 whole, 1 gone), with a bright burning edge
// along the cut. The tint's color still multiplies the texture; its alpha does not dim it.
// Material: params0.z the noise's scale across the sprite, params0.w the edge's width,
// params1.rgb the edge's color.
#include "sprite_fragment.slangh"
#include "arcfire_noise.slangh"

[shader("fragment")]
float4 fs_main(SpriteVaryings i) : SV_Target {
    GpuMaterial m  = sprite_material(i);
    float4 texel   = sprite_texel(i, i.uv);
    float progress = i.tint.a;
    float scale    = m.params0.z > 0.0 ? m.params0.z : 6.0;
    float width    = m.params0.w > 0.0 ? m.params0.w : 0.08;
    float n = arc_fbm(i.local * scale + 3.3);
    if (progress > 0.0 && n < progress) discard;
    float edge = progress > 0.0 ? 1.0 - smoothstep(progress, progress + width, n) : 0.0;
    float3 color = lerp(texel.rgb * i.tint.rgb, m.params1.rgb, edge);
    return float4(color, texel.a);
}
