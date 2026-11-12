// example_gallery: a game's sprite shader. Rings across the quad from its local coordinates,
// in the material's tint and its params1 color, cut to a disc; the texture's alpha, through
// sprite_texel, keeps the bindless path in the picture. No time: every frame is the same.
#include "sprite_fragment.slangh"

[shader("fragment")]
float4 fs_main(SpriteVaryings i) : SV_Target {
    GpuMaterial m = sprite_material(i);
    float2 c = i.local * 2.0 - 1.0;
    float r = length(c);
    if (r > 1.0) discard;
    float band = step(0.5, frac(r * 4.0));
    float3 rgb = lerp(i.tint.rgb, m.params1.rgb, band);
    float a = sprite_texel(i, i.uv).a;
    return float4(rgb, max(a, 0.5));
}
