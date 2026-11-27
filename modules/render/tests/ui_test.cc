// The UI primitive's lists without a GPU: the words ui.slang reads, and the world's layers.
#include "korvid/render/world.h"
#include "korvid/core/test.h"
#include <string.h>

namespace {

using namespace kv;

u32 word(const RwUiLayer& l, u32 byte) { u32 w; memcpy(&w, l.bytes.data + byte, 4); return w; }
f32 fword(const RwUiLayer& l, u32 byte) { f32 w; memcpy(&w, l.bytes.data + byte, 4); return w; }

} // namespace

void test_ui_lists(kv::TestCtx& c) {
    using namespace kv;
    c.begin("render ui lists");
    UiList list;
    list.init(mem_general());
    KV_CHECK(c, list.count() == 0 && list.clips.len == 4); // clip 0, everything
    u32 clip = list.clip(rect2{10, 20, 300, 200});
    KV_CHECK(c, clip == 1);

    UiBox b;
    b.rect = rect2{10, 20, 100, 30};
    b.radius[0] = b.radius[1] = b.radius[2] = b.radius[3] = 40; // held to half the short side
    b.fill = color{1, 0, 0, 1};
    b.gradient = true;
    b.fill2 = color{0, 0, 1, 0.5f};
    b.border = 2;
    b.border_color = color{0, 1, 0, 1};
    b.shadow_color = color{0, 0, 0, 0.25f};
    b.shadow_blur = 6;
    b.clip = clip;
    b.nearest = true;
    b.uv = rect2{0.25f, 0.5f, 0.125f, 0.25f};
    list.box(b);
    UiGlyph g;
    g.rect = rect2{12, 22, 8, 10};
    g.uv = rect2{0.5f, 0.25f, 0.125f, 0.125f};
    g.texture = 7;
    g.tint = color{1, 1, 1, 1};
    g.clip = 99; // no such clip: none
    list.glyph(g);
    KV_CHECK(c, list.count() == 2 && list.words.len == 24 + 9 && list.offsets[1] == 24);

    RenderWorld w;
    KV_CHECK(c, rw_init(w, mem_general()));
    u64 v0 = w.ui_version;
    rw_ui_set(w, 0, 5, list);
    KV_CHECK(c, w.ui.len == 1 && w.ui_version == v0 + 1 && w.ui_bytes % 16 == 0 && w.ui[0].items == 2);
    const RwUiLayer& l = w.ui[0];
    // The header, the offsets, the clips, the words.
    u32 clips_at = word(l, 8);
    KV_CHECK(c, word(l, 0) == 2 && word(l, 4) == 2 && clips_at % 16 == 0 && clips_at >= 16 + 8);
    u32 box = word(l, 16), glyph = word(l, 20);
    KV_CHECK(c, box == clips_at + 32 && glyph == box + 96);
    KV_CHECK(c, fword(l, clips_at + 16) == 10.0f && fword(l, clips_at + 28) == 200.0f);
    u32 head = word(l, box);
    KV_CHECK(c, (head & 3) == 0 && ((head >> 2) & 0xFFFF) == 1 && (head & (1u << 18)) && !(head & (1u << 19)));
    KV_CHECK(c, word(l, box + 8) == 0xFF0000FFu && word(l, box + 12) == 0x80FF0000u && word(l, box + 16) == 0xFF00FF00u);
    KV_CHECK(c, fword(l, box + 24) == 10.0f && fword(l, box + 36) == 30.0f && fword(l, box + 40) == 15.0f && fword(l, box + 56) == 2.0f);
    KV_CHECK(c, fword(l, box + 72) == 6.0f && word(l, box + 20) >> 24 == 64);
    // Nearest sampling, and the uv rect as floats, its corners.
    KV_CHECK(c, (head & (1u << 20)) && fword(l, box + 80) == 0.25f && fword(l, box + 84) == 0.5f && fword(l, box + 88) == 0.375f && fword(l, box + 92) == 0.75f);
    KV_CHECK(c, word(l, glyph) == 1u && word(l, glyph + 4) == 7 && fword(l, glyph + 12) == 12.0f);
    KV_CHECK(c, word(l, glyph + 28) == (32768u | 16384u << 16) && word(l, glyph + 32) == (40959u | 24576u << 16));

    // Layers stay in order with their bases packed; clearing repacks.
    UiList small;
    small.init(mem_general());
    small.glyph(g);
    rw_ui_set(w, 0, -300, small); // clamped to -128
    rw_ui_set(w, 0, 50, small);
    KV_CHECK(c, w.ui.len == 3 && w.ui[0].layer == -128 && w.ui[1].layer == 5 && w.ui[2].layer == 50);
    KV_CHECK(c, w.ui[0].base == 0 && w.ui[1].base == (w.ui[0].bytes.len + 15) / 16 * 16 && w.ui[2].base == w.ui[1].base + (w.ui[1].bytes.len + 15) / 16 * 16);
    u64 before = w.ui_version;
    rw_ui_set(w, 0, 5, small); // replaced in place
    KV_CHECK(c, w.ui.len == 3 && w.ui[1].items == 1 && w.ui_version == before + 1);
    rw_ui_clear(w, 0, -128);
    KV_CHECK(c, w.ui.len == 2 && w.ui[0].layer == 5 && w.ui[0].base == 0);
    rw_ui_clear(w, 0, 77); // nothing there
    KV_CHECK(c, w.ui.len == 2);

    // Another surface's lists come after the main one's, in layer order, and move its
    // version alone (slice 13, KR40).
    u64 main_v = w.ui_surface_version[0], two_v = w.ui_surface_version[2];
    rw_ui_set(w, 2, 9, small);
    rw_ui_set(w, 2, -4, small);
    rw_ui_set(w, 1, 100, small);
    KV_CHECK(c, w.ui.len == 5 && w.ui[2].surface == 1 && w.ui[3].surface == 2 && w.ui[3].layer == -4 && w.ui[4].layer == 9);
    KV_CHECK(c, w.ui_surface_version[0] == main_v && w.ui_surface_version[2] == two_v + 2);
    KV_CHECK(c, rw_ui_first(w, 0) == 0 && rw_ui_first(w, 1) == 2 && rw_ui_first(w, 2) == 3 && rw_ui_first(w, 3) == 5);
    rw_ui_clear(w, 2, 5); // the main surface has a 5, surface 2 has none
    KV_CHECK(c, w.ui.len == 5 && w.ui_surface_version[2] == two_v + 2);
    rw_ui_clear_surface(w, 2);
    KV_CHECK(c, w.ui.len == 3 && w.ui[2].surface == 1 && w.ui_surface_version[2] == two_v + 3 && w.ui_surface_version[0] == main_v);
    rw_ui_clear_surface(w, 1);
    KV_CHECK(c, w.ui.len == 2 && w.ui[1].base + w.ui[1].bytes.len <= w.ui_bytes);

    // Staging copies each region to its base.
    Array<u8> gpu;
    gpu.alloc = mem_general();
    gpu.resize_fill((u32)w.ui_bytes, 0xEE);
    rw_ui_stage(w, gpu.data);
    KV_CHECK(c, memcmp(gpu.data + w.ui[1].base, w.ui[1].bytes.data, w.ui[1].bytes.len) == 0);

    // A list is drawn when the bucket walk passes its layer's sprites.
    KV_CHECK(c, rw_ui_bucket_end(5) == rw_bucket_of(6, 0) && rw_ui_bucket_end(127) == kBucketCount && rw_ui_bucket_end(-500) == rw_bucket_of(-127, 0));
    gpu.free();
    small.free();
    list.free();
    rw_free(w);
}
