// Placed, moving, and not a rectangle (docs/PRDs/Krome-Slice-8-PRD.md), through a running
// Krome: edges and offsets written in .kim and bound to the model.
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

void frames(Engine& e, u32 n) {
    for (u32 i = 0; i < n; ++i) { platform_pump(e); engine_frame(e, 1.0 / 60.0); engine_frame_cycle(e, 1.0 / 60.0); }
}

bool has_error(Engine& e, const char* want) {
    const KimErrors* errs = krome_errors(e);
    char line[300];
    for (u32 i = 0; errs && i < errs->count; ++i) {
        errs->format_one(i, line, sizeof line);
        if (strstr(line, want)) return true;
    }
    KV_ERROR("test", "no error '%s' among:", want);
    for (u32 i = 0; errs && i < errs->count; ++i) { errs->format_one(i, line, sizeof line); KV_ERROR("test", "  %s", line); }
    return false;
}

bool rect_is(const rect2& r, f32 x, f32 y, f32 w, f32 h) {
    auto near = [](f32 a, f32 b) { return a - b < 0.001f && b - a < 0.001f; };
    if (near(r.x, x) && near(r.y, y) && near(r.w, w) && near(r.h, h)) return true;
    KV_ERROR("test", "rect (%g, %g, %g, %g), wanted (%g, %g, %g, %g)", r.x, r.y, r.w, r.h, x, y, w, h);
    return false;
}

const rect2& rect_of(Engine& e, Name screen, const char* id) {
    static const rect2 none{-1, -1, -1, -1};
    const KromeTree* t = krome_tree(e, screen);
    u32 n = t ? krome_find(*t, name_intern(id)) : KROME_NONE;
    return n == KROME_NONE ? none : t->nodes[n].rect;
}

// Compiled from .kim text, given to Krome, opened.
Name open_kim(TestCtx& c, Engine& e, const char* file, const char* text, const char* screen) {
    Array<u8> k{};
    KimErrors errs;
    KV_CHECK(c, kim_compile(text, strlen(text), file, k, errs) && errs.count == 0);
    char path[128];
    usize n = strlen(file);
    memcpy(path, file, n);
    memcpy(path + n, "b", 2); // t/x.kim -> t/x.kimb
    krome_provide(e, path, k.data, k.len);
    k.free();
    return krome_open(e, path, screen);
}

const ModuleDesc* const k_list[] = {&platform_module, &platform_hid_module, &asset_module, &krome_module};

void test_edges_kim(TestCtx& c) {
    c.begin("krome placed: edges in .kim");
    Engine e;
    KV_CHECK(c, engine_init(e, k_list));
    krome_viewport(e, 300, 200, 1.0f);
    // A drawer bound to the model, a bar across, a badge at a corner, a mark two thirds along.
    const char* text =
        "screen main { box dir=stack pad=10 {\n"
        "  box id=bar l=10 r=10 h=20\n"
        "  box id=badge r=30 b=8 w=16 h=16\n"
        "  box id=mark l=66.7% t=100%-12 w=4 h=12\n"
        "  box id=drawer w=100 h=50 x=@drawer\n"
        "} }\n";
    Name scn = open_kim(c, e, "t/edges.kim", text, "main");
    frames(e, 3);
    KV_CHECK(c, krome_ready(e, scn));
    KV_CHECK(c, rect_is(rect_of(e, scn, "bar"), 20, 10, 260, 20));
    KV_CHECK(c, rect_is(rect_of(e, scn, "badge"), 244, 166, 16, 16));
    KV_CHECK(c, rect_is(rect_of(e, scn, "mark"), 10 + 280 * 0.667f, 178, 4, 12));
    KV_CHECK(c, rect_is(rect_of(e, scn, "drawer"), 10, 10, 100, 50)); // nothing in the model: no offset
    // The window resized: each follows its edge.
    krome_viewport(e, 400, 250, 1.0f);
    frames(e, 1);
    KV_CHECK(c, rect_is(rect_of(e, scn, "bar"), 20, 10, 360, 20));
    KV_CHECK(c, rect_is(rect_of(e, scn, "badge"), 344, 216, 16, 16));
    // Bound: the model moves it.
    krome_set(e, scn, "drawer", -120.0);
    frames(e, 2);
    KV_CHECK(c, rect_is(rect_of(e, scn, "drawer"), -110, 10, 100, 50));

    // In a row or a column, or at the root, each is a mistake naming why.
    krome_clear_errors(e);
    const char* bad =
        "screen main { box dir=row {\n"
        "  box id=a l=4 w=10\n"
        "  box dir=col { box id=b y=3 }\n"
        "} }\n";
    Name b = open_kim(c, e, "t/notstack.kim", bad, "main");
    frames(e, 3);
    KV_CHECK(c, krome_ready(e, b));
    KV_CHECK(c, has_error(e, "t/notstack.kim:2: l= places a child of a stack, and this one's parent is a row"));
    KV_CHECK(c, has_error(e, "t/notstack.kim:3: y= places a child of a stack, and this one's parent is a col"));
    KV_CHECK(c, rect_is(rect_of(e, b, "a"), 0, 0, 10, 0));
    engine_shutdown(e);
}

} // namespace

void test_placed(TestCtx& c) {
    test_edges_kim(c);
}
