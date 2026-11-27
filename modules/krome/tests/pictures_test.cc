// Pictures (docs/PRDs/Krome-Slice-7-PRD.md): what Krome draws of an image besides the whole
// of it. Regions of a sheet, a look's per state, measured as the region; `pixel`, the flag
// that inherits; a node's state looks written in .kim among its children.
#include "korvid/krome/krome.h"
#include "../src/internal.h"
#include "korvid/asset/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/test.h"
#include "korvid/kim/kim.h"
#include "korvid/platform/module.h"
#include "korvid/platform_hid/module.h"
#include <string.h>

namespace {

using namespace kv;

template <u32 N> struct Nums { f32 v[N]; };

vec2 fake_text(void*, const char*, u32 len, Name, f32 size) { return vec2{(f32)len * size * 0.5f, size + 2.0f}; }
vec2 fake_image(void*, Name) { return vec2{200, 40}; } // the sheet: ten 20 x 20 cells across, two down
const KromeMeasure k_measure{nullptr, fake_text, fake_image};

void frames(Engine& e, u32 n) {
    for (u32 i = 0; i < n; ++i) { platform_pump(e); engine_frame(e, 1.0 / 60.0); engine_frame_cycle(e, 1.0 / 60.0); }
}

bool region_is(const KromeLook& l, f32 x, f32 y, f32 w, f32 h) {
    if ((l.set & KROME_LOOK_REGION) && l.region[0] == x && l.region[1] == y && l.region[2] == w && l.region[3] == h) return true;
    KV_ERROR("test", "region (%g, %g, %g, %g)%s, wanted (%g, %g, %g, %g)", l.region[0], l.region[1], l.region[2], l.region[3],
             (l.set & KROME_LOOK_REGION) ? "" : " unset", x, y, w, h);
    return false;
}

void test_regions(TestCtx& c) {
    c.begin("krome pictures: regions");
    KromeTree t;
    t.init(mem_general());
    u32 root = t.add(KromeType::Box);
    t.set(root, "dir", KromeValue::str("row"));
    u32 whole = t.add(KromeType::Image, root);
    t.set(whole, "image", KromeValue::str("t/sheet.png"));
    u32 cut = t.add(KromeType::Image, root);
    t.set(cut, "image", KromeValue::str("t/sheet.png"));
    KV_CHECK(c, t.set(cut, "region", KromeValue::nums(Nums<4>{{40, 0, 20, 20}}.v, 4)) == nullptr);
    KV_CHECK(c, t.set(cut, "region", KromeValue::nums(Nums<4>{{60, 0, 20, 20}}.v, 4), KromeState::Hover) == nullptr);
    // A region is four numbers, a corner at 0 or more and a size above 0.
    KV_CHECK(c, t.set(cut, "region", KromeValue::nums(Nums<3>{{1, 2, 3}}.v, 3)) != nullptr);
    KV_CHECK(c, t.set(cut, "region", KromeValue::nums(Nums<4>{{-1, 0, 20, 20}}.v, 4)) != nullptr);
    KV_CHECK(c, t.set(cut, "region", KromeValue::nums(Nums<4>{{0, 0, 0, 20}}.v, 4)) != nullptr);
    KV_CHECK(c, t.set(cut, "region", KromeValue::num(4)) != nullptr);

    // A node with a region measures as the region, the file's size whatever it is.
    krome_layout(t, root, rect2{0, 0, 640, 480}, k_measure, false);
    KV_CHECK(c, t.nodes[whole].rect.w == 200 && t.nodes[whole].rect.h == 40);
    KV_CHECK(c, t.nodes[cut].rect.x == 200 && t.nodes[cut].rect.w == 20 && t.nodes[cut].rect.h == 20);

    // A state's look moves it; the base keeps its own.
    KromeLook base = t.resolve_look(cut, KromeState::Base, nullptr);
    KromeLook hover = t.resolve_look(cut, KromeState::Hover, nullptr);
    KV_CHECK(c, region_is(base, 40, 0, 20, 20) && region_is(hover, 60, 0, 20, 20));

    // The part of the file drawn: the region, the whole without one, refused past its edge.
    rect2 src;
    KV_CHECK(c, krome_image_source(base, 200, 40, &src) && src.x == 40 && src.y == 0 && src.w == 20 && src.h == 20);
    KromeLook none = t.resolve_look(whole, KromeState::Base, nullptr);
    KV_CHECK(c, krome_image_source(none, 200, 40, &src) && src.x == 0 && src.w == 200 && src.h == 40);
    KV_CHECK(c, krome_image_source(base, 200, 40, &src) && !krome_image_source(base, 59, 40, &src) && !krome_image_source(base, 200, 19, &src));

    // `pixel` inherits: set at the root, every image under it is sampled nearest.
    KV_CHECK(c, !t.resolve_look(cut, KromeState::Base, nullptr).pixel);
    KV_CHECK(c, t.flag(root, "pixel") == nullptr);
    KromeLook rl = t.resolve_look(root, KromeState::Base, nullptr);
    KV_CHECK(c, rl.pixel && t.resolve_look(cut, KromeState::Base, &rl).pixel);
    KromeLook plain = t.resolve_look(whole, KromeState::Base, nullptr);
    KV_CHECK(c, !t.resolve_look(whole, KromeState::Base, &plain).pixel);
    t.free();
}

// The PRD's sheet in .kim: an image's string is its path, its states' looks are blocks among
// its children, and `pixel` at the root.
void test_regions_kim(TestCtx& c) {
    c.begin("krome pictures: regions in .kim");
    const char* text =
        "screen main { box pixel dir=row {\n"
        "  image \"t/sheet.png\" id=btn region=40,0,20,20 {\n"
        "    hover   { region=60,0,20,20 }\n"
        "    pressed { region=80,0,20,20 }\n"
        "  }\n"
        "} }\n";
    Array<u8> k{};
    KimErrors errs;
    KV_CHECK(c, kim_compile(text, strlen(text), "t/pic.kim", k, errs) && errs.count == 0);
    {
        // The check: a look's layout, a state that is not, a region of three numbers.
        const char* bad =
            "screen main { box {\n"
            "  image \"t/sheet.png\" region=1,2,3 {\n"
            "    hover { w=5 }\n"
            "  }\n"
            "} }\n";
        JsonDoc doc;
        KimErrors e2;
        KimbFile f;
        KimbScope scope;
        KV_CHECK(c, kim_parse(bad, strlen(bad), "t/bad.kim", mem_scratch(), doc, e2) && kimb_from_doc(doc, f, e2, "t/bad.kim"));
        KV_CHECK(c, kimb_scope(scope, f, [](void*, const char*) -> const KimbFile* { return nullptr; }, nullptr, e2) && !kimb_check(scope, e2));
        bool layout = false;
        char line[300];
        for (u32 i = 0; i < e2.count; ++i) {
            e2.format_one(i, line, sizeof line);
            if (strstr(line, "t/bad.kim:2: hover.w is layout")) layout = true;
        }
        KV_CHECK(c, layout);
    }

    const ModuleDesc* const list[] = {&platform_module, &platform_hid_module, &asset_module, &krome_module};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    KromeModule& s = *krome_state(e);
    krome_viewport(e, 400, 300, 1.0f);
    krome_provide(e, "t/pic.kimb", k.data, k.len);
    Name scn = krome_open(e, "t/pic.kimb", "main");
    frames(e, 3);
    KV_CHECK(c, krome_ready(e, scn));
    KromeScreen& sc = *krome_screen(s, scn);
    u32 btn = krome_find(sc.tree, name_intern("btn"));
    KV_CHECK(c, btn != KROME_NONE);
    if (btn != KROME_NONE) {
        const KromeTree& t = sc.tree;
        KromeLook rl = t.resolve_look(t.root, KromeState::Base, nullptr);
        KromeLook b = t.resolve_look(btn, KromeState::Base, &rl);
        KV_CHECK(c, b.image == name_intern("t/sheet.png") && region_is(b, 40, 0, 20, 20) && b.pixel);
        KV_CHECK(c, region_is(t.resolve_look(btn, KromeState::Hover, &rl), 60, 0, 20, 20));
        KV_CHECK(c, region_is(t.resolve_look(btn, KromeState::Pressed, &rl), 80, 0, 20, 20));
        // Laid out at the region's size before the file is in, and at any scale.
        KV_CHECK(c, t.nodes[btn].rect.w == 20 && t.nodes[btn].rect.h == 20);
        krome_viewport(e, 600, 450, 1.5f);
        frames(e, 1);
        KV_CHECK(c, t.nodes[btn].rect.w == 20 && krome_pixel_rect(t.nodes[btn].rect, 1.5f).w == 30);
    }
    k.free();
    engine_shutdown(e);
}

bool rect_is(rect2 r, f32 x, f32 y, f32 w, f32 h) {
    if (r.x == x && r.y == y && r.w == w && r.h == h) return true;
    KV_ERROR("test", "rect (%g, %g, %g, %g), wanted (%g, %g, %g, %g)", r.x, r.y, r.w, r.h, x, y, w, h);
    return false;
}

// Fit (KR31): how an image fills a rect that is not its size, as the pieces paint draws.
// The sheet is 84 x 40: a 24 x 24 frame at (60, 0) with 8-pixel borders, a 20 x 20 button
// at (0, 0).
void test_fit(TestCtx& c) {
    c.begin("krome pictures: fit");
    KromeTree t;
    t.init(mem_general());
    u32 img = t.add(KromeType::Image);
    KV_CHECK(c, t.set(img, "fit", KromeValue::str("tiles")) != nullptr);
    KV_CHECK(c, t.set(img, "slice", KromeValue::nums(Nums<2>{{8, 8}}.v, 2)) != nullptr);
    KV_CHECK(c, t.set(img, "slice", KromeValue::num(-1)) != nullptr);
    KV_CHECK(c, krome_fit_of(t.resolve_look(img, KromeState::Base, nullptr)) == KromeFit::Stretch);
    KV_CHECK(c, t.set(img, "slice", KromeValue::num(8)) == nullptr);
    KromeLook frame = t.resolve_look(img, KromeState::Base, nullptr);
    KV_CHECK(c, krome_fit_of(frame) == KromeFit::Slice); // slice= alone slices
    const rect2 src{60, 0, 24, 24};
    KromeImagePieces p;

    // The frame at 200 x 100 and at 640 x 480: the corners 8 x 8, the edges and middle stretched.
    krome_image_pieces(frame, src, 84, 40, rect2{10, 20, 200, 100}, 1.0f, KromeAlign::Start, KromeAlign::Start, p);
    KV_CHECK(c, p.count == 9 && !p.clip && p.tile.x == 0.0f);
    KV_CHECK(c, rect_is(p.rect[0], 10, 20, 8, 8) && rect_is(p.rect[2], 202, 20, 8, 8) && rect_is(p.rect[6], 10, 112, 8, 8) && rect_is(p.rect[8], 202, 112, 8, 8));
    KV_CHECK(c, rect_is(p.rect[4], 18, 28, 184, 84) && rect_is(p.rect[1], 18, 20, 184, 8));
    KV_CHECK(c, rect_is(p.uv[0], 60.0f / 84, 0, 8.0f / 84, 8.0f / 40) && rect_is(p.uv[4], 68.0f / 84, 8.0f / 40, 8.0f / 84, 8.0f / 40));
    krome_image_pieces(frame, src, 84, 40, rect2{0, 0, 640, 480}, 1.0f, KromeAlign::Start, KromeAlign::Start, p);
    KV_CHECK(c, p.count == 9 && rect_is(p.rect[0], 0, 0, 8, 8) && rect_is(p.rect[8], 632, 472, 8, 8) && rect_is(p.rect[4], 8, 8, 624, 464));
    // At twice the scale the corners are twice the pixels: the same logical size.
    krome_image_pieces(frame, src, 84, 40, rect2{0, 0, 400, 200}, 2.0f, KromeAlign::Start, KromeAlign::Start, p);
    KV_CHECK(c, p.count == 9 && rect_is(p.rect[0], 0, 0, 16, 16) && rect_is(p.rect[8], 384, 184, 16, 16));
    // Smaller than its borders: they shrink in proportion, and the middle is gone.
    krome_image_pieces(frame, src, 84, 40, rect2{0, 0, 10, 6}, 1.0f, KromeAlign::Start, KromeAlign::Start, p);
    KV_CHECK(c, p.count == 4 && rect_is(p.rect[0], 0, 0, 5, 3) && rect_is(p.rect[3], 5, 3, 5, 3));
    // Borders of 0 make no piece: top and bottom only, three bands.
    KV_CHECK(c, t.set(img, "slice", KromeValue::nums(Nums<4>{{0, 8, 0, 8}}.v, 4)) == nullptr);
    krome_image_pieces(t.resolve_look(img, KromeState::Base, nullptr), src, 84, 40, rect2{0, 0, 100, 50}, 1.0f, KromeAlign::Start, KromeAlign::Start, p);
    KV_CHECK(c, p.count == 3 && rect_is(p.rect[0], 0, 0, 100, 8) && rect_is(p.rect[1], 0, 8, 100, 34) && rect_is(p.rect[2], 0, 42, 100, 8));

    // Tiled: one piece, the tile at the scale, from the rect's top left.
    KV_CHECK(c, t.set(img, "fit", KromeValue::str("tile")) == nullptr);
    krome_image_pieces(t.resolve_look(img, KromeState::Base, nullptr), rect2{60, 24, 8, 8}, 84, 40, rect2{0, 0, 640, 480}, 1.5f, KromeAlign::Start, KromeAlign::Start, p);
    KV_CHECK(c, p.count == 1 && rect_is(p.rect[0], 0, 0, 640, 480) && p.tile.x == 12.0f && p.tile.y == 12.0f);
    KV_CHECK(c, rect_is(p.uv[0], 60.0f / 84, 24.0f / 40, 8.0f / 84, 8.0f / 40));

    // Natural: its own size at the scale, placed by the node's alignment, clipped when larger.
    KV_CHECK(c, t.set(img, "fit", KromeValue::str("natural")) == nullptr);
    KromeLook nat = t.resolve_look(img, KromeState::Base, nullptr);
    krome_image_pieces(nat, rect2{0, 0, 20, 20}, 84, 40, rect2{10, 10, 100, 50}, 2.0f, KromeAlign::Center, KromeAlign::Center, p);
    KV_CHECK(c, p.count == 1 && !p.clip && rect_is(p.rect[0], 40, 15, 40, 40));
    krome_image_pieces(nat, rect2{0, 0, 20, 20}, 84, 40, rect2{10, 10, 100, 50}, 2.0f, KromeAlign::End, KromeAlign::Start, p);
    KV_CHECK(c, rect_is(p.rect[0], 70, 10, 40, 40));
    krome_image_pieces(nat, rect2{0, 0, 20, 20}, 84, 40, rect2{10, 10, 30, 30}, 2.0f, KromeAlign::Start, KromeAlign::Start, p);
    KV_CHECK(c, p.clip && rect_is(p.rect[0], 10, 10, 40, 40));
    // Stretched, the default: one piece over the rect.
    KV_CHECK(c, t.set(img, "fit", KromeValue::str("stretch")) == nullptr);
    krome_image_pieces(t.resolve_look(img, KromeState::Base, nullptr), src, 84, 40, rect2{0, 0, 33, 44}, 1.0f, KromeAlign::Start, KromeAlign::Start, p);
    KV_CHECK(c, p.count == 1 && rect_is(p.rect[0], 0, 0, 33, 44) && p.tile.x == 0.0f);
    t.free();
}

} // namespace

void test_pictures(kv::TestCtx& c) {
    test_regions(c);
    test_regions_kim(c);
    test_fit(c);
}
