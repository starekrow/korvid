// example_arcfire: the nebula. A procedural field of gas over the deep plane: four octaves
// of value noise, warped once by two more, mapped from the sprite's tint (the dark of it)
// to the material's bright color where it is densest. The game scrolls the sprite's uv
// with the stage, so the field comes down the screen as the planes do; it also drifts a
// little with the render time. Material: tint the dark color; params0.z the seed, params0.w
// the density (0 sparse to 1 thick); params1.rgb the bright color, params1.w the scale.
#include "sprite_fragment.slangh"
#include "arcfire_noise.slangh"

[shader("fragment")]
float4 fs_main(SpriteVaryings i) : SV_Target {
    GpuMaterial m = sprite_material(i);
    float scale   = m.params1.w > 0.0 ? m.params1.w : 3.0;
    float2 p = i.uv * scale + m.params0.z + float2(gPush.time * 0.012, gPush.time * 0.004);
    float2 warp = float2(arc_fbm(p + 4.1), arc_fbm(p + float2(9.7, 2.3)));
    float n = arc_fbm(p + warp * 1.7);
    float density = saturate(m.params0.w);
    float glow = smoothstep(1.0 - density * 0.6, 1.0, n + 0.25);
    float3 dark = i.tint.rgb * (0.35 + 0.65 * n);
    float3 color = lerp(dark, m.params1.rgb, glow);
    return float4(color, i.tint.a);
}
