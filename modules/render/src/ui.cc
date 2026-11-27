// The UI primitive's lists: the builder (korvid/render/ui.h) and the world's layers of them
// (korvid/render/world.h). The words are what shaders/ui.slang reads; change one and change
// the other.
#include "korvid/render/world.h"
#include "korvid/math/scalar.h"
#include <string.h>

namespace kv {

namespace {

constexpr u32 k_kind_box = 0, k_kind_glyph = 1;
constexpr u32 k_box_gradient = 1u << 18, k_box_textured = 1u << 19, k_box_nearest = 1u << 20;

u32 bits_of(f32 f) { u32 u; memcpy(&u, &f, sizeof u); return u; }

u32 channel(f32 c) {
    if (!(c > 0.0f)) return 0;
    if (c >= 1.0f) return 255;
    return (u32)(c * 255.0f + 0.5f);
}
u32 rgba8(color c) { return channel(c.r) | channel(c.g) << 8 | channel(c.b) << 16 | channel(c.a) << 24; }

u32 unorm16(f32 v) {
    if (!(v > 0.0f)) return 0;
    if (v >= 1.0f) return 65535;
    return (u32)(v * 65535.0f + 0.5f);
}
u32 uv_pair(f32 x, f32 y) { return unorm16(x) | unorm16(y) << 16; }

void push_rect(Array<u32>& w, rect2 r) {
    u32* p = w.push_n(4);
    p[0] = bits_of(r.x); p[1] = bits_of(r.y); p[2] = bits_of(r.w); p[3] = bits_of(r.h);
}

u32 clip_of(const UiList& l, u32 clip) { return clip < l.clips.len / 4 ? clip : 0; }

} // namespace

void UiList::init(Allocator* a) {
    words.alloc = offsets.alloc = clips.alloc = a;
    clear();
}

void UiList::free() {
    words.free();
    offsets.free();
    clips.free();
}

void UiList::clear() {
    words.clear();
    offsets.clear();
    clips.clear();
    f32* c = clips.push_n(4);
    c[0] = c[1] = -1e9f;
    c[2] = c[3] = 2e9f;
}

u32 UiList::clip(rect2 r) {
    u32 n = clips.len / 4;
    if (n > KV_UI_MAX_CLIPS) return 0;
    f32* c = clips.push_n(4);
    c[0] = r.x; c[1] = r.y; c[2] = r.w < 0 ? 0 : r.w; c[3] = r.h < 0 ? 0 : r.h;
    return n;
}

void UiList::box(const UiBox& b) {
    offsets.push(words.len);
    u32 flags = (b.gradient ? k_box_gradient : 0) | (b.textured ? k_box_textured : 0) | (b.nearest ? k_box_nearest : 0);
    words.push(k_kind_box | clip_of(*this, b.clip) << 2 | flags);
    words.push(b.texture);
    words.push(rgba8(b.fill));
    words.push(rgba8(b.gradient ? b.fill2 : b.fill));
    words.push(rgba8(b.border_color));
    words.push(rgba8(b.shadow_color));
    push_rect(words, b.rect);
    // Radii no larger than half the shorter side: the shader's distance assumes it.
    f32 most = (b.rect.w < b.rect.h ? b.rect.w : b.rect.h) * 0.5f;
    for (u32 i = 0; i < 4; ++i) words.push(bits_of(b.radius[i] < 0 ? 0 : b.radius[i] > most ? most : b.radius[i]));
    words.push(bits_of(b.border));
    words.push(bits_of(b.angle * (KV_PI / 180.0f)));
    words.push(bits_of(b.shadow_offset.x));
    words.push(bits_of(b.shadow_offset.y));
    words.push(bits_of(b.shadow_blur < 0 ? 0 : b.shadow_blur));
    words.push(bits_of(b.shadow_spread));
    // A box's uv is four floats: a region of a 4096-texel sheet needs better than unorm16's
    // sixteenth of a texel.
    words.push(bits_of(b.uv.x));
    words.push(bits_of(b.uv.y));
    words.push(bits_of(b.uv.x + b.uv.w));
    words.push(bits_of(b.uv.y + b.uv.h));
}

void UiList::glyph(const UiGlyph& g) {
    offsets.push(words.len);
    words.push(k_kind_glyph | clip_of(*this, g.clip) << 2);
    words.push(g.texture);
    words.push(rgba8(g.tint));
    push_rect(words, g.rect);
    words.push(uv_pair(g.uv.x, g.uv.y));
    words.push(uv_pair(g.uv.x + g.uv.w, g.uv.y + g.uv.h));
}

// ---- the world's layers ---------------------------------------------------------------------

namespace {

void repack(RenderWorld& w) {
    u64 at = 0;
    for (RwUiLayer& l : w.ui) {
        l.base = at;
        at += (l.bytes.len + 15) & ~15ull;
    }
    w.ui_bytes = at;
    ++w.ui_version;
}

i32 clamp_layer(i32 layer) { return layer < kLayerMin ? kLayerMin : layer > kLayerMax ? kLayerMax : layer; }

} // namespace

void rw_ui_set(RenderWorld& w, u32 surface, i32 layer, const UiList& list) {
    if (surface >= KV_RENDER_SURFACES) return;
    layer = clamp_layer(layer);
    u32 at = 0;
    while (at < w.ui.len && (w.ui[at].surface < surface || (w.ui[at].surface == surface && w.ui[at].layer < layer))) ++at;
    if (at == w.ui.len || w.ui[at].surface != surface || w.ui[at].layer != layer) {
        RwUiLayer fresh;
        fresh.surface = (u8)surface;
        fresh.layer = layer;
        fresh.bytes.alloc = w.ui.alloc;
        w.ui.push(fresh);
        for (u32 i = w.ui.len - 1; i > at; --i) w.ui[i] = w.ui[i - 1];
        w.ui[at] = fresh;
    }
    RwUiLayer& l = w.ui[at];
    u32 items = list.offsets.len, clips = list.clips.len / 4;
    u32 offsets_at = 16, clips_at = offsets_at + items * 4;
    clips_at = (clips_at + 15) & ~15u;
    u32 words_at = clips_at + clips * 16;
    u32 total = words_at + list.words.len * 4;
    l.bytes.clear();
    u8* p = l.bytes.push_n(total);
    memset(p, 0, words_at);
    u32 head[4] = {items, clips, clips_at, 0};
    memcpy(p, head, sizeof head);
    u32* off = (u32*)(p + offsets_at);
    for (u32 i = 0; i < items; ++i) off[i] = words_at + list.offsets.data[i] * 4;
    if (clips) memcpy(p + clips_at, list.clips.data, (usize)clips * 16);
    if (list.words.len) memcpy(p + words_at, list.words.data, (usize)list.words.len * 4);
    l.items = items;
    ++w.ui_sets;
    ++w.ui_surface_version[surface];
    repack(w);
}

void rw_ui_clear(RenderWorld& w, u32 surface, i32 layer) {
    if (surface >= KV_RENDER_SURFACES) return;
    layer = clamp_layer(layer);
    for (u32 i = 0; i < w.ui.len; ++i) {
        if (w.ui[i].surface != surface || w.ui[i].layer != layer) continue;
        w.ui[i].bytes.free();
        for (u32 j = i + 1; j < w.ui.len; ++j) w.ui[j - 1] = w.ui[j];
        --w.ui.len;
        ++w.ui_surface_version[surface];
        repack(w);
        return;
    }
}

void rw_ui_clear_surface(RenderWorld& w, u32 surface) {
    u32 k = 0;
    bool gone = false;
    for (u32 i = 0; i < w.ui.len; ++i) {
        if (w.ui[i].surface == surface) { w.ui[i].bytes.free(); gone = true; continue; }
        w.ui[k++] = w.ui[i];
    }
    w.ui.len = k;
    if (!gone) return;
    if (surface < KV_RENDER_SURFACES) ++w.ui_surface_version[surface];
    repack(w);
}

u32 rw_ui_first(const RenderWorld& w, u32 surface) {
    u32 i = 0;
    while (i < w.ui.len && w.ui[i].surface < surface) ++i;
    return i;
}

void rw_ui_stage(const RenderWorld& w, u8* dst) {
    for (const RwUiLayer& l : w.ui)
        if (l.bytes.len) memcpy(dst + l.base, l.bytes.data, l.bytes.len);
}

u32 rw_ui_bucket_end(i32 layer) { return rw_bucket_of(clamp_layer(layer), 0) + kSlotCount; }

} // namespace kv
