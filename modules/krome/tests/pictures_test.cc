// Pictures (docs/PRDs/Krome-Slice-7-PRD.md): what Krome draws of an image besides the whole
// of it. Regions of a sheet, a look's per state, measured as the region; `pixel`, the flag
// that inherits; a node's state looks written in .kim among its children.
#include "korvid/krome/krome.h"
#include "../src/internal.h"
#include "korvid/asset/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/test.h"
#include "korvid/core/format.h"
#include "korvid/core/time.h"
#include "korvid/image/png.h"
#include "korvid/kim/kim.h"
#include "korvid/platform/io.h"
#include "korvid/render/ui.h"
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
    KV_CHECK(c, krome_image_source(base, 200, 40, false, 0, &src) && src.x == 40 && src.y == 0 && src.w == 20 && src.h == 20);
    KromeLook none = t.resolve_look(whole, KromeState::Base, nullptr);
    KV_CHECK(c, krome_image_source(none, 200, 40, false, 0, &src) && src.x == 0 && src.w == 200 && src.h == 40);
    KV_CHECK(c, krome_image_source(base, 200, 40, false, 0, &src) && !krome_image_source(base, 59, 40, false, 0, &src) && !krome_image_source(base, 200, 19, false, 0, &src));

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

// Frames (KR29): which frame of a strip, still or playing, and where it is in the file.
void test_frames(TestCtx& c) {
    c.begin("krome pictures: frames");
    KromeTree t;
    t.init(mem_general());
    u32 root = t.add(KromeType::Box);
    t.set(root, "dir", KromeValue::str("row"));
    u32 knob = t.add(KromeType::Image, root);
    t.set(knob, "image", KromeValue::str("t/knob.png"));
    KV_CHECK(c, t.set(knob, "frames", KromeValue::num(0)) != nullptr && t.set(knob, "frames", KromeValue::num(2.5f)) != nullptr);
    KV_CHECK(c, t.set(knob, "fps", KromeValue::num(-1)) != nullptr && t.set(knob, "loops", KromeValue::num(1.5f)) != nullptr);
    KV_CHECK(c, t.set(knob, "frames", KromeValue::num(64)) == nullptr);
    auto still = [&](const char* prop, f32 v) {
        t.set(knob, prop, KromeValue::num(v));
        return krome_still_frame(t.resolve_look(knob, KromeState::Base, nullptr));
    };
    KV_CHECK(c, krome_still_frame(t.resolve_look(knob, KromeState::Base, nullptr)) == 0);
    // frame-at: 0 the first, 1 the last, held inside; frame: by number, held inside.
    KV_CHECK(c, still("frame-at", 0.5f) == 32 && still("frame-at", 0.0f) == 0 && still("frame-at", 1.0f) == 63 && still("frame-at", 1.5f) == 63 && still("frame-at", -1.0f) == 0);
    KV_CHECK(c, still("frame", 3) == 3 && still("frame", 99) == 63 && still("frame", -2) == 0);

    // A strip down a region, and across a file: the frame's cell.
    rect2 src;
    KromeTree u;
    u.init(mem_general());
    u32 a = u.add(KromeType::Image);
    u.set(a, "region", KromeValue::nums(Nums<4>{{100, 0, 20, 240}}.v, 4));
    u.set(a, "frames", KromeValue::num(12));
    KromeLook down = u.resolve_look(a, KromeState::Base, nullptr);
    KV_CHECK(c, krome_image_source(down, 200, 240, false, 3, &src) && rect_is(src, 100, 60, 20, 20));
    KV_CHECK(c, krome_image_source(down, 200, 240, false, 40, &src) && rect_is(src, 100, 220, 20, 20)); // held to the last
    KromeTree v;
    v.init(mem_general());
    u32 b = v.add(KromeType::Image);
    v.set(b, "frames", KromeValue::num(12));
    KV_CHECK(c, krome_image_source(v.resolve_look(b, KromeState::Base, nullptr), 240, 20, true, 3, &src) && rect_is(src, 60, 0, 20, 20));

    // Playing: 12 frames at 10 a second, twice, resting on the last; for ever, round and round.
    bool done = false;
    f64 next = 0.0;
    KV_CHECK(c, krome_strip_frame(12, 0, 10, 2, 0.0, &done, &next) == 0 && !done && next > 0.0999 && next < 0.1001);
    KV_CHECK(c, krome_strip_frame(12, 0, 10, 2, 0.35, &done, &next) == 3 && !done && next > 0.3999 && next < 0.4001);
    KV_CHECK(c, krome_strip_frame(12, 0, 10, 2, 0.1, &done, &next) == 1); // due at the clock's time: shown at it
    KV_CHECK(c, krome_strip_frame(12, 0, 10, 2, 1.25, &done, &next) == 0 && !done);
    KV_CHECK(c, krome_strip_frame(12, 0, 10, 2, 2.39, &done, &next) == 11 && !done && next > 2.3999 && next < 2.4001);
    KV_CHECK(c, krome_strip_frame(12, 0, 10, 2, 2.4, &done, &next) == 11 && done && next == 0.0);
    KV_CHECK(c, krome_strip_frame(12, 0, 10, 2, 50.0, &done, &next) == 11 && done && next == 0.0);
    KV_CHECK(c, krome_strip_frame(12, 0, 10, 0, 100.05, &done, &next) == 4 && !done);
    KV_CHECK(c, krome_strip_frame(12, 5, 10, 1, 0.65, &done, &next) == 11 && !done); // from where it was: 5 to 11, once
    KV_CHECK(c, krome_strip_frame(12, 5, 10, 1, 0.7, &done, &next) == 11 && done);
    KV_CHECK(c, krome_strip_frame(12, 7, 0, 1, 9.0, &done, &next) == 7 && !done && next == 0.0); // still

    // A strip measures as one frame: the region's down, the file's across.
    t.set(root, "dir", KromeValue::str("row"));
    u32 strip = t.add(KromeType::Image, root);
    t.set(strip, "image", KromeValue::str("t/sheet.png"));
    t.set(strip, "frames", KromeValue::num(10));
    t.flag(strip, "across");
    u32 tall = t.add(KromeType::Image, root);
    t.set(tall, "region", KromeValue::nums(Nums<4>{{100, 0, 20, 240}}.v, 4));
    t.set(tall, "frames", KromeValue::num(12));
    krome_layout(t, root, rect2{0, 0, 640, 480}, k_measure, false);
    KV_CHECK(c, rect_is(t.nodes[knob].rect, 0, 0, 200, 40.0f / 64) && rect_is(t.nodes[strip].rect, 200, 0, 20, 40) && t.nodes[tall].rect.w == 20 && t.nodes[tall].rect.h == 20);
    t.free();
    u.free();
    v.free();
}

// A knob bound to the model, and a strip played: what wakes the window while it plays and
// after, and its event, once.
void test_frames_playing(TestCtx& c) {
    c.begin("krome pictures: frames playing");
    const char* text =
        "script \"t/p.lua\"\n"
        "screen main { box dir=row {\n"
        "  image \"t/knob.png\" id=knob frames=64 frame-at=@level\n"
        "  image \"t/strip.png\" id=spin frames=12 fps=@fps loops=2 across -> spun\n"
        "} }\n";
    const char* script = "return { model = { level = 0.5, fps = 0, spun = 0 }, on = { spun = function() model.spun = model.spun + 1 end } }";
    Array<u8> k{};
    KimErrors errs;
    KV_CHECK(c, kim_compile(text, strlen(text), "t/p.kim", k, errs) && errs.count == 0);
    const ModuleDesc* const list[] = {&platform_module, &platform_hid_module, &asset_module, &krome_module};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    KromeModule& s = *krome_state(e);
    krome_viewport(e, 400, 300, 1.0f);
    krome_provide(e, "t/p.kimb", k.data, k.len);
    krome_provide(e, "t/p.lua", script, strlen(script));
    Name scn = krome_open(e, "t/p.kimb", "main");
    for (u32 i = 0; i < 200 && !(krome_ready(e, scn) && krome_settled(e)); ++i) frames(e, 1);
    KV_CHECK(c, krome_ready(e, scn) && krome_settled(e));
    KromeScreen& sc = *krome_screen(s, scn);
    u32 knob = krome_find(sc.tree, name_intern("knob"));
    KV_CHECK(c, knob != KROME_NONE && krome_still_frame(sc.tree.resolve_look(knob, KromeState::Base, nullptr)) == 32);
    // Bound: the model moves the knob, with no rebuild.
    u64 nodes = krome_stats(e).rebuilt_nodes;
    krome_set(e, scn, "level", 1.0);
    frames(e, 2);
    KV_CHECK(c, krome_still_frame(sc.tree.resolve_look(knob, KromeState::Base, nullptr)) == 63 && krome_stats(e).rebuilt_nodes == nodes);
    // Still, it costs nothing: no frame asked for, no paint.
    u64 paints = krome_stats(e).paints;
    e.frame_due = 0.0;
    frames(e, 30);
    KV_CHECK(c, krome_stats(e).paints == paints && e.frame_due == 0.0 && s.plays.len == 0);

    // Played at 10 a second, twice: a paint for each of its 24 frames, and one for the rest
    // where it says so; a frame asked for while it plays, never after.
    krome_set(e, scn, "fps", 10.0);
    frames(e, 1);
    paints = krome_stats(e).paints;
    KV_CHECK(c, s.plays.len == 1 && s.plays[0].due > 0.0);
    u32 asked = 0, asked_after = 0, playing = 0;
    for (u32 i = 0; i < 60 * 4; ++i) {
        e.frame_due = 0.0;
        frames(e, 1);
        bool rests = s.plays.len == 1 && s.plays[0].done;
        if (e.frame_due > 0.0) (rests ? asked_after : asked) += 1;
        if (!rests) ++playing;
    }
    u64 painted = krome_stats(e).paints - paints;
    KV_CHECK(c, painted == 24 && asked == playing && asked_after == 0);
    if (painted != 24 || asked != playing || asked_after) KV_ERROR("test", "paints %llu, frames asked %u while playing %u, %u after", (unsigned long long)painted, asked, playing, asked_after);
    KV_CHECK(c, playing > 140 && playing < 146); // 2.4 seconds at 60 a second
    KV_CHECK(c, !strcmp(krome_model_text(e, scn, "spun"), "1"));
    // At rest it stays: nothing painted, nothing asked, no second event.
    paints = krome_stats(e).paints;
    frames(e, 60);
    KV_CHECK(c, krome_stats(e).paints == paints && !strcmp(krome_model_text(e, scn, "spun"), "1"));
    // Still again (fps 0): the record goes with the play.
    krome_set(e, scn, "fps", 0.0);
    frames(e, 2);
    KV_CHECK(c, s.plays.len == 0);
    k.free();
    engine_shutdown(e);
}

// Bitmap fonts (KR32): a .kbf, and one written inline, laid out and painted by a text node
// as any font is; a character one lacks; `pixel` glyphs. 3 x 5 cells, drawn at size 10.
bool write_digits(const char* dir) {
    u8 px[15 * 15 * 4] = {};
    for (u32 y = 0; y < 15; ++y)
        for (u32 x = 0; x < 15; ++x) {
            u32 cell = (y / 5) * 5 + x / 3;
            u8* p = px + (y * 15 + x) * 4;
            if (cell < 13) { p[0] = p[1] = p[2] = 255; p[3] = (x + y) & 1 ? 255 : 0; }
        }
    Array<u8> out{};
    out.alloc = mem_general();
    const char* why = nullptr;
    char path[600];
    format(path, sizeof path, "%s/bm/digits.png", dir);
    bool ok = png_encode(px, 15, 15, 4, out, &why) && io_write_file(path, out.data, out.len);
    out.free();
    const char* kbf = "image \"digits.png\"\ncell 3 5\nchars \"0123456789:- \"\nmissing \" \"\n";
    format(path, sizeof path, "%s/bm/digits.kbf", dir);
    return ok && io_write_file(path, kbf, strlen(kbf));
}

void test_bitmap_fonts(TestCtx& c) {
    c.begin("krome pictures: bitmap fonts");
    char dir[400], sub[500];
    KV_CHECK(c, io_make_temp_dir("kv-krome-bitmap", dir, sizeof dir));
    format(sub, sizeof sub, "%s/bm", dir);
    io_make_dirs(sub);
    KV_CHECK(c, write_digits(dir));
    const char* text =
        "script \"t/f.lua\"\n"
        "font digits { image=\"bm/digits.png\" cell=3,5 chars=\"0123456789:\" }\n"
        "screen main { box dir=col {\n"
        "  text \"{time}\" id=time font=\"bm/digits.kbf\" size=10\n"
        "  text \"12a34\" id=lacks font=\"bm/digits.kbf\" size=10\n"
        "  text \"12:34\" id=inline font=digits size=10\n"
        "  text \"12a34\" id=nothing font=digits size=10\n"
        "  box pixel { text \"1\" id=square font=digits size=20 }\n"
        "} }\n";
    const char* script = "return { model = { time = '12:34' }, on = {} }";
    Array<u8> k{};
    KimErrors errs;
    KV_CHECK(c, kim_compile(text, strlen(text), "t/f.kim", k, errs) && errs.count == 0);
    {
        // The check: a font's words, and what it needs.
        const char* bad = "font a { image=\"x.png\" cell=3 chars=\"0\" size=4 }\nfont b { image=\"x.png\" }\nscreen s { box }\n";
        JsonDoc doc;
        KimErrors e2;
        KimbFile f;
        KimbScope scope;
        KV_CHECK(c, kim_parse(bad, strlen(bad), "t/bad.kim", mem_scratch(), doc, e2) && kimb_from_doc(doc, f, e2, "t/bad.kim"));
        KV_CHECK(c, kimb_scope(scope, f, [](void*, const char*) -> const KimbFile* { return nullptr; }, nullptr, e2) && !kimb_check(scope, e2));
        char line[300];
        bool cell = false, word = false, needs = false;
        for (u32 i = 0; i < e2.count; ++i) {
            e2.format_one(i, line, sizeof line);
            if (strstr(line, "t/bad.kim:1: font a: cell takes a width and a height")) cell = true;
            if (strstr(line, "t/bad.kim:1: font a: 'size' is not a font's word")) word = true;
            if (strstr(line, "t/bad.kim:2: font b needs image, cell and chars")) needs = true;
        }
        KV_CHECK(c, cell && word && needs);
    }

    const ModuleDesc* const list[] = {&platform_module, &platform_hid_module, &asset_module, &krome_module};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    KV_CHECK(c, platform_io(e)->mount_dir(dir));
    KromeModule& s = *krome_state(e);
    krome_viewport(e, 400, 300, 1.0f);
    krome_provide(e, "t/f.kimb", k.data, k.len);
    krome_provide(e, "t/f.lua", script, strlen(script));
    Name scn = krome_open(e, "t/f.kimb", "main");
    for (u32 i = 0; i < 2000 && !(krome_ready(e, scn) && krome_settled(e)); ++i) { frames(e, 1); time_sleep(0.001); }
    KV_CHECK(c, krome_ready(e, scn) && krome_settled(e));
    KromeScreen& sc = *krome_screen(s, scn);
    auto rect_of = [&](const char* id) { u32 n = krome_find(sc.tree, name_intern(id)); return n == KROME_NONE ? rect2{} : sc.tree.nodes[n].rect; };
    // A 5-pixel cell at 10: 6 x 10 a character; {time} filled; 'a' drawn as `missing`, a space.
    KV_CHECK(c, rect_is(rect_of("time"), 0, 0, 30, 10) && rect_is(rect_of("lacks"), 0, 10, 30, 10));
    // Inline: the same; one with no `missing` lays out without what it lacks.
    KV_CHECK(c, rect_is(rect_of("inline"), 0, 20, 30, 10) && rect_is(rect_of("nothing"), 0, 30, 24, 10));
    KV_CHECK(c, rect_is(rect_of("square"), 0, 40, 6 * 2, 20));
    // Painted: glyphs of the cell's size times two, nearest under `pixel`, linear elsewhere.
    UiList l;
    l.init(mem_general());
    krome_paint_screen(s, sc, l);
    u32 glyphs = 0, nearest = 0, big = 0;
    for (u32 i = 0; i < l.count(); ++i) {
        const u32* w = l.words.data + l.offsets[i];
        if ((w[0] & 3u) != 1u) continue;
        ++glyphs;
        f32 gw, gh;
        memcpy(&gw, &w[5], 4);
        memcpy(&gh, &w[6], 4);
        if (w[0] & (1u << 20)) { ++nearest; if (gw == 12.0f && gh == 20.0f) ++big; }
    }
    // Five each but `nothing`'s four (its 'a' is not drawn; `lacks`' is the space's cell), and
    // the one under `pixel`, 12 x 20.
    KV_CHECK(c, glyphs == 5 + 5 + 5 + 4 + 1 && nearest == 1 && big == 1);
    l.free();
    k.free();
    engine_shutdown(e);
    io_remove_tree(dir);
}

// A look computed from the pixels (KR34): its properties; a hover's gain eased by a
// transition; a theme's token for the hue, and the theme switched.
void test_computed(TestCtx& c) {
    c.begin("krome pictures: computed looks");
    KromeTree t;
    t.init(mem_general());
    u32 img = t.add(KromeType::Image);
    KV_CHECK(c, t.set(img, "gain", KromeValue::nums(Nums<2>{{1, 2}}.v, 2)) != nullptr && t.set(img, "gain", KromeValue::num(-1)) != nullptr);
    KV_CHECK(c, t.set(img, "saturation", KromeValue::num(-0.5f)) != nullptr && t.set(img, "hue", KromeValue::str("red")) != nullptr);
    KV_CHECK(c, t.set(img, "gain", KromeValue::nums(Nums<3>{{1.2f, 1.1f, 1.0f}}.v, 3)) == nullptr);
    KV_CHECK(c, t.set(img, "gain", KromeValue::num(1.5f), KromeState::Hover) == nullptr);
    KV_CHECK(c, t.set(img, "hue", KromeValue::num(30)) == nullptr && t.set(img, "saturation", KromeValue::num(0.5f)) == nullptr);
    KromeLook base = t.resolve_look(img, KromeState::Base, nullptr), hover = t.resolve_look(img, KromeState::Hover, nullptr);
    KV_CHECK(c, base.gain[0] == 1.2f && base.gain[1] == 1.1f && base.gain[2] == 1.0f && base.hue == 30.0f && base.saturation == 0.5f);
    KV_CHECK(c, hover.gain[0] == 1.5f && hover.gain[2] == 1.5f && hover.hue == 30.0f);
    KromeLook none{};
    KV_CHECK(c, none.gain[0] == 1.0f && none.hue == 0.0f && none.saturation == 1.0f); // as drawn
    t.free();

    const char* text =
        "theme dusk { skin-hue=40 }\n" // first by name: the screen starts in it
        "theme noon { skin-hue=200 }\n"
        "screen main { box dir=row {\n"
        "  image \"t/sheet.png\" id=lit w=20 h=20 hue=skin-hue transition=1 -> lit {\n"
        "    hover { gain=1.2 }\n"
        "  }\n"
        "} }\n";
    Array<u8> k{};
    KimErrors errs;
    KV_CHECK(c, kim_compile(text, strlen(text), "t/c.kim", k, errs) && errs.count == 0);
    const ModuleDesc* const list[] = {&platform_module, &platform_hid_module, &asset_module, &krome_module};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    KromeModule& s = *krome_state(e);
    krome_viewport(e, 400, 300, 1.0f);
    krome_provide(e, "t/c.kimb", k.data, k.len);
    Name scn = krome_open(e, "t/c.kimb", "main");
    for (u32 i = 0; i < 200 && !(krome_ready(e, scn) && krome_settled(e)); ++i) frames(e, 1);
    KromeScreen& sc = *krome_screen(s, scn);
    u32 lit = krome_find(sc.tree, name_intern("lit"));
    KV_CHECK(c, lit != KROME_NONE && sc.tree.resolve_look(lit, KromeState::Base, nullptr).hue == 40.0f);
    // Hovered: its gain eases from 1 to 1.2 over a second, not at once.
    krome_pointer(s, 10, 10, false, 0, false);
    frames(e, 2);
    KromeModule::Anim* a = nullptr;
    for (KromeModule::Anim& x : s.anims) if (x.node == lit) a = &x;
    KV_CHECK(c, a && a->from.gain[0] == 1.0f && a->to.gain[0] == 1.2f && a->to.hue == 40.0f && s.animating);
    frames(e, 70);
    KV_CHECK(c, !s.animating);
    // The theme switched: every image naming the token takes its value there.
    krome_theme(e, "noon");
    frames(e, 3);
    lit = krome_find(sc.tree, name_intern("lit"));
    KV_CHECK(c, lit != KROME_NONE && sc.tree.resolve_look(lit, KromeState::Base, nullptr).hue == 200.0f);
    k.free();
    engine_shutdown(e);
}

} // namespace

void test_pictures(kv::TestCtx& c) {
    test_regions(c);
    test_regions_kim(c);
    test_fit(c);
    test_frames(c);
    test_frames_playing(c);
    test_bitmap_fonts(c);
    test_computed(c);
}
