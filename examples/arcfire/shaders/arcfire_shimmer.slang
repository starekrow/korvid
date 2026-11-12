// arcfire: the shimmer, the full-frame pass over the world. Each of up to sixteen
// rings (gPostParams: a center in window pixels, +y down; the radius so far; the strength
// left) bends the world's image across its band: the far side of the crest pushed
// outward, the near side drawn in, as air over a blast front would, a few pixels at most,
// with a faint brightening on the crest. Off the rings the world is copied as it is.
#include "post_fragment.slangh"

[shader("fragment")]
float4 fs_main(PostVaryings i) : SV_Target {
    float2 px = i.uv * gPost.size;
    float2 bend = float2(0.0, 0.0);
    float crest = 0.0;
    for (uint k = 0; k < gPost.count; ++k) {
        float4 ring = gPostParams[k];
        float2 d = px - ring.xy;
        float dist = length(d);
        float width = max(6.0, ring.z * 0.18);
        float x = (dist - ring.z) / width;
        if (abs(x) > 3.0 || dist < 1e-3) continue;
        float w = exp(-x * x) * ring.w;
        bend += (d / dist) * (x * w * 14.0); // sampled from further out past the crest, further in before it
        crest += w;
    }
    float3 c = post_world((px - bend) / gPost.size).rgb;
    c += saturate(crest) * 0.07;
    return float4(c, 1.0);
}
