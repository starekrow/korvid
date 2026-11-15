// Korvid render: the UI primitive's shader (korvid/render/ui.h). One instanced draw per
// layer's list: the instance picks an item through the list's offsets, the vertex index a
// corner of its quad. Items are words in a byte buffer, laid out by render's src/ui.cc:
//
//   region: items, clips, clips' start, 0 | item offsets (bytes from the region) | clips | items
//   glyph:  head, slot, color, rect x y w h, uv0, uv1                         (9 words)
//   box:    head, slot, fill, fill2, border color, shadow color, rect x y w h,
//           radii tl tr br bl, border, angle, shadow x y, blur, spread, uv0, uv1  (22 words)
//
// head: bits 0-1 the kind (0 box, 1 glyph), 2-17 the clip, 18 a gradient, 19 textured.
// Colors are RGBA8 sRGB with straight alpha; uvs two unorm16s. Pixels have their origin at
// the window's top left, y down; the window's size is the push block's screen_cull.zw.
// The output is premultiplied: the pipeline blends one, one minus source alpha.
#if defined(KV_TARGET_METAL) || defined(KV_TARGET_D3D12)
// Metal and D3D12 do not draw the UI primitive yet: their backends' rb_ui_* are stubs and
// never load this shader. These entry points only keep the build whole there; the real
// ones below are for those backends to take up when they draw it (drop this branch then).
struct UiVaryings {
    float4 pos : SV_Position;
};
[shader("vertex")]
UiVaryings vs_ui(uint vid : SV_VertexID) {
    UiVaryings o;
    o.pos = float4(0.0, 0.0, 0.0, 1.0);
    return o;
}
[shader("fragment")]
float4 fs_ui(UiVaryings i) : SV_Target {
    return float4(0.0, 0.0, 0.0, 0.0);
}
#else
#include "sprite_fragment.slangh"

// The push block's spare words carry a UI draw's list (core/layout.h names them ui_base and
// ui_count): its region in gUi, in bytes, and its items. Named here, so the sprite shaders,
// which share the block, compile to the same bytes as before.
#define UI_BASE  gPush.pad_t0
#define UI_COUNT gPush.pad_t1

[[vk::binding(8, 0)]] ByteAddressBuffer gUi KV_REG(t8);

static const float2 kQuad[6] = {
    float2(0.0, 0.0), float2(1.0, 0.0), float2(1.0, 1.0),
    float2(0.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0),
};

static const uint kKindBox = 0u, kKindGlyph = 1u;
static const uint kBoxGradient = 1u << 18, kBoxTextured = 1u << 19;

struct UiVaryings {
    float4 pos : SV_Position;
    float2 px  : PX;   // the fragment's pixel, top-left origin
    float2 uv  : UV;   // a glyph's texture coordinate
    nointerpolation uint item : ITEM; // the item's byte offset in gUi
};

uint   ui_word(uint at)   { return gUi.Load(at); }
float  ui_float(uint at)  { return asfloat(gUi.Load(at)); }
float2 ui_float2(uint at) { return asfloat(gUi.Load2(at)); }
float4 ui_float4(uint at) { return asfloat(gUi.Load4(at)); }

float srgb_to_linear(float c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }

// RGBA8 sRGB, straight alpha, to linear premultiplied.
float4 ui_color(uint w) {
    float4 c = float4(float(w & 255u), float((w >> 8) & 255u), float((w >> 16) & 255u), float(w >> 24)) / 255.0;
    float3 rgb = float3(srgb_to_linear(c.r), srgb_to_linear(c.g), srgb_to_linear(c.b));
    return float4(rgb * c.a, c.a);
}

float2 ui_uv(uint w) { return float2(float(w & 0xFFFFu), float(w >> 16)) / 65535.0; }

// The signed distance from a rounded rectangle centered at the origin with half size b and
// a radius per corner (tl, tr, br, bl; y down), negative inside.
float sd_round_box(float2 p, float2 b, float4 r) {
    float rr = p.x > 0.0 ? (p.y > 0.0 ? r.z : r.y) : (p.y > 0.0 ? r.w : r.x);
    rr = min(rr, min(b.x, b.y));
    float2 q = abs(p) - b + rr;
    return min(max(q.x, q.y), 0.0) + length(max(q, float2(0.0, 0.0))) - rr;
}

// erf by Abramowitz and Stegun 7.1.26: a shadow's edge needs nothing better.
float erf_approx(float x) {
    float s = sign(x), a = abs(x);
    float t = 1.0 / (1.0 + 0.3275911 * a);
    float y = 1.0 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t * exp(-a * a);
    return s * y;
}

// A pixel position to clip space, through the push block's screen view (center origin, +y up).
float2 px_to_clip(float2 p) {
    float2 size = gPush.screen_cull.zw;
    float2 s = float2(p.x - size.x * 0.5, size.y * 0.5 - p.y);
    return apply_affine(gPush.screen, gPush.screen_trans, s);
}

[shader("vertex")]
UiVaryings vs_ui(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    uint region = UI_BASE;
    uint item = region + ui_word(region + 16u + 4u * iid);
    uint head = ui_word(item);
    float2 corner = kQuad[vid];
    UiVaryings o;
    o.item = item;
    if ((head & 3u) == kKindGlyph) {
        float4 r = ui_float4(item + 12u);
        float2 p = r.xy + corner * r.zw;
        o.px = p;
        o.uv = lerp(ui_uv(ui_word(item + 28u)), ui_uv(ui_word(item + 32u)), corner);
        o.pos = float4(px_to_clip(p), 0.0, 1.0);
        return o;
    }
    // A box's quad reaches as far as its shadow does, and a pixel past its edge for the
    // anti-aliasing.
    float4 r = ui_float4(item + 24u);
    float2 so = ui_float2(item + 64u);
    float blur = ui_float(item + 72u), spread = ui_float(item + 76u);
    float grow = 1.0;
    if ((ui_word(item + 20u) >> 24) != 0u) grow = max(grow, max(abs(so.x), abs(so.y)) + max(spread, 0.0) + blur * 1.5 + 1.0);
    float2 lo = r.xy - grow, hi = r.xy + r.zw + grow;
    float2 p = lerp(lo, hi, corner);
    o.px = p;
    o.uv = float2(0.0, 0.0);
    o.pos = float4(px_to_clip(p), 0.0, 1.0);
    return o;
}

[shader("fragment")]
float4 fs_ui(UiVaryings i) : SV_Target {
    uint region = UI_BASE;
    uint head = ui_word(i.item);
    uint clip = (head >> 2) & 0xFFFFu;
    float4 cr = ui_float4(region + ui_word(region + 8u) + 16u * clip);
    if (i.px.x < cr.x || i.px.y < cr.y || i.px.x >= cr.x + cr.z || i.px.y >= cr.y + cr.w) discard;

    if ((head & 3u) == kKindGlyph) {
        uint slot = ui_word(i.item + 4u);
        float4 c = ui_color(ui_word(i.item + 8u));
        float4 t = gTextures[NonUniformResourceIndex(slot)].Sample(gSamplers[1], i.uv);
        // The texel is straight alpha: its coverage scales the premultiplied color.
        return float4(c.rgb * t.rgb, c.a) * t.a;
    }

    float4 r = ui_float4(i.item + 24u);
    float4 radii = ui_float4(i.item + 40u);
    float border = ui_float(i.item + 56u);
    float2 half_size = r.zw * 0.5;
    float2 p = i.px - (r.xy + half_size);
    float d = sd_round_box(p, half_size, radii);
    float outer = saturate(0.5 - d);
    float inner = border > 0.0 ? saturate(0.5 - (d + border)) : outer;

    float4 fill = ui_color(ui_word(i.item + 8u));
    if ((head & kBoxGradient) != 0u) {
        float angle = ui_float(i.item + 60u);
        float2 dir = float2(cos(angle), sin(angle));
        float reach = abs(half_size.x * dir.x) + abs(half_size.y * dir.y);
        float t = reach > 0.0 ? saturate(dot(p, dir) / reach * 0.5 + 0.5) : 0.0;
        fill = lerp(fill, ui_color(ui_word(i.item + 12u)), t);
    }
    if ((head & kBoxTextured) != 0u) {
        uint slot = ui_word(i.item + 4u);
        float2 uv0 = ui_uv(ui_word(i.item + 80u)), uv1 = ui_uv(ui_word(i.item + 84u));
        float2 at = (i.px - r.xy) / max(r.zw, float2(1e-6, 1e-6));
        float4 t = gTextures[NonUniformResourceIndex(slot)].Sample(gSamplers[1], lerp(uv0, uv1, at));
        fill = float4(fill.rgb * t.rgb, fill.a) * t.a;
    }
    float4 edge = ui_color(ui_word(i.item + 16u));
    float4 color = fill * inner + edge * (outer - inner);

    float4 shade = ui_color(ui_word(i.item + 20u));
    if (shade.a > 0.0) {
        float2 so = ui_float2(i.item + 64u);
        float blur = ui_float(i.item + 72u), spread = ui_float(i.item + 76u);
        float ds = sd_round_box(p - so, half_size + spread, radii + spread);
        float sigma = max(blur * 0.5, 0.5);
        float a = 0.5 * (1.0 - erf_approx(ds / (sigma * 1.41421356)));
        color = color + shade * a * (1.0 - color.a);
    }
    return color;
}
#endif
