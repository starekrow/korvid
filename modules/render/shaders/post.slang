// Korvid render: the full-frame pass's vertex shader. One triangle that covers the
// window, from the vertex index alone; the fragment shader is the game's (post_fragment.slangh).
#include "post_fragment.slangh"

[shader("vertex")]
PostVaryings vs_post(uint vid : SV_VertexID) {
    float2 uv = float2((vid << 1) & 2, vid & 2); // (0,0) (2,0) (0,2)
    PostVaryings o;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    o.uv  = uv;
    return o;
}
