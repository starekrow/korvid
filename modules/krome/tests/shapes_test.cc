// Krome's shape cache (docs/PRDs/Krome-Slice-5-PRD.md, "Krome"), without a renderer: the pages
// are kept in memory all the same, so what is rasterized, when, and in which colour can be
// read back. A shape is rasterized once per size; a one-colour shape once for every colour (in
// white); a coloured one once per colour, with currentColor resolved to it; a large one has a
// page of its own.
#include "../src/internal.h"
#include "korvid/asset/module.h"
#include "korvid/asset/shape.h"
#include "korvid/cli/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/memory.h"
#include "korvid/core/test.h"
#include "korvid/krome/krome.h"
#include "korvid/platform/module.h"
#include "korvid/platform_hid/module.h"

namespace {

using namespace kv;

// A square over the whole view box, painted `paint`; with `half`, a second square over its
// right half in solid red.
void square(ShapeAsset& a, bool current, bool half) {
    a.shape.init(mem_general());
    a.shape.view[2] = a.shape.view[3] = 10;
    PathBuilder b(a.shape);
    b.move_to(0, 0); b.line_to(10, 0); b.line_to(10, 10); b.line_to(0, 10); b.close();
    b.fill(current ? a.shape.add_current() : a.shape.add_solid(0, 0, 255, 255));
    if (half) {
        b.move_to(5, 0); b.line_to(10, 0); b.line_to(10, 10); b.line_to(5, 10); b.close();
        b.fill(a.shape.add_solid(255, 0, 0, 255));
    }
    a.one_colour = a.shape.one_colour();
    a.state = AssetState::Ready;
}

// The RGBA of pixel (x, y) of the entry drawn last.
const u8* pixel(KromeModule& s, u32 x, u32 y) {
    const KromeShapeEntry& e = s.shape_entries.last();
    const KromeShapePage& p = s.shape_pages[e.page];
    return p.rgba + ((usize)(e.y + y) * p.w + e.x + x) * 4;
}

bool is(const u8* px, u8 r, u8 g, u8 b, u8 a) { return px[0] == r && px[1] == g && px[2] == b && px[3] == a; }

} // namespace

void test_shapes(kv::TestCtx& c) {
    using namespace kv;
    c.begin("krome shapes");
    const ModuleDesc* const list[] = {&platform_module, &platform_hid_module, &cli_module, &asset_module, &krome_module};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    KromeModule& s = *krome_state(e);
    ShapeAsset mono{}, coloured{}, big{};
    square(mono, true, false);
    square(coloured, true, true);
    square(big, true, false);
    Handle hm = Handle::make(9, 1, 1), hc = Handle::make(9, 1, 2), hb = Handle::make(9, 1, 3);
    KV_CHECK(c, mono.one_colour && !coloured.one_colour);
    UiBox box;

    // One colour: rasterized white once, whatever the text colour; a new size is a new picture.
    krome_shape_box(s, nullptr, mono, hm, 16, 16, color{1, 0, 0, 1}, box);
    KV_CHECK(c, s.shapes_drawn == 1 && s.shape_entries.len == 1 && s.shape_pages.len == 1);
    KV_CHECK(c, is(pixel(s, 8, 8), 255, 255, 255, 255));
    krome_shape_box(s, nullptr, mono, hm, 16, 16, color{0, 1, 0, 1}, box);
    krome_shape_box(s, nullptr, mono, hm, 16, 16, color{0, 0, 1, 0.5f}, box);
    KV_CHECK(c, s.shapes_drawn == 1 && s.shape_entries.len == 1);
    krome_shape_box(s, nullptr, mono, hm, 32, 32, color{1, 0, 0, 1}, box);
    KV_CHECK(c, s.shapes_drawn == 2 && s.shape_entries.len == 2 && s.shape_pages.len == 1);

    // Coloured: currentColor resolved, and the colour part of the key; its own colours kept.
    krome_shape_box(s, nullptr, coloured, hc, 20, 20, color{0, 1, 0, 1}, box);
    KV_CHECK(c, s.shapes_drawn == 3 && is(pixel(s, 2, 10), 0, 255, 0, 255) && is(pixel(s, 17, 10), 255, 0, 0, 255));
    krome_shape_box(s, nullptr, coloured, hc, 20, 20, color{0, 0, 1, 1}, box);
    KV_CHECK(c, s.shapes_drawn == 4 && is(pixel(s, 2, 10), 0, 0, 255, 255) && is(pixel(s, 17, 10), 255, 0, 0, 255));
    krome_shape_box(s, nullptr, coloured, hc, 20, 20, color{0, 1, 0, 1}, box);
    KV_CHECK(c, s.shapes_drawn == 4 && s.shape_entries.len == 4);

    // The entries pack along a shelf without overlapping.
    const KromeShapeEntry& a = s.shape_entries[0];
    const KromeShapeEntry& b = s.shape_entries[1];
    KV_CHECK(c, a.page == b.page && (a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y));

    // Past 512 pixels a shape has a page of its own, its size.
    krome_shape_box(s, nullptr, big, hb, 600, 300, color{1, 1, 1, 1}, box);
    KV_CHECK(c, s.shape_pages.len == 2 && s.shape_pages[1].own && s.shape_pages[1].w == 600 && s.shape_pages[1].h == 300);
    KV_CHECK(c, is(pixel(s, 300, 150), 255, 255, 255, 255) && is(pixel(s, 10, 150), 0, 0, 0, 0)); // xMidYMid: a square, centred
    // Without a renderer nothing is drawn, and nothing is a texture yet.
    KV_CHECK(c, !krome_shape_box(s, nullptr, mono, hm, 16, 16, color{1, 1, 1, 1}, box) && !box.textured);
    KV_CHECK(c, s.shape_pages[0].texture.is_null() && s.shape_pages[0].dirty);

    krome_shapes_free(s, nullptr);
    KV_CHECK(c, s.shape_pages.len == 0 && s.shape_entries.len == 0);
    mono.shape.free();
    coloured.shape.free();
    big.shape.free();
    engine_shutdown(e);
}
