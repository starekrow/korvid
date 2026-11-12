// gallery: a full-frame post shader. The world's image as it is in the left half;
// in the right half its channels rotated and darkened by the first parameter's w, with a
// line at the middle in the parameter's color. Uses the uv (+y down) and the size, so a
// backend that turns the pass over or misplaces it shows at once. No time.
#include "post_fragment.slangh"

[shader("fragment")]
float4 fs_main(PostVaryings i) : SV_Target {
    float3 c = post_world(i.uv).rgb;
    float4 p = gPost.count > 0 ? gPostParams[0] : float4(1.0, 1.0, 1.0, 0.75);
    float x = i.uv.x * gPost.size.x;
    float mid = gPost.size.x * 0.5;
    if (abs(x - mid) < 2.0) return float4(p.rgb, 1.0);
    if (x > mid) c = c.brg * p.w;
    // A mark in the top left corner, 16 pixels, so up and down cannot be confused.
    float2 px = i.uv * gPost.size;
    if (px.x < 16.0 && px.y < 16.0) return float4(1.0, 1.0, 0.0, 1.0);
    return float4(c, 1.0);
}
