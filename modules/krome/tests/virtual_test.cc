// Many rows, and motion (docs/PRDs/Krome-Slice-4-PRD.md, milestone 3): a virtualized each
// over 10,000 rows builds only what is in view and a screenful each side, with spacers the
// height of the rest; scrolling builds the newly visible; a gap is counted; the list
// changing keeps the window. Transitions ease colors between states on the Frame cycle's
// clock, interrupted from where they are.
#include "korvid/krome/krome.h"
#include "korvid/kim/kim.h"
#include "../src/internal.h"
#include "korvid/asset/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/kbor.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include "korvid/platform/module.h"
#include "korvid/platform_hid/module.h"
#include <string.h>

namespace {

using namespace kv;

const char* k_screen = R"({"kim": 1, "file": "v/s.kim", "scripts": ["v/s.lua"],
  "screens": { "main": { "w": "box", "p": { "dir": "col", "pad": 10, "gap": 10 }, "c": [
    { "w": "box", "id": "list", "f": ["scroll"], "p": { "dir": "col", "w": 300, "h": 240 }, "c": [
      { "w": "each", "v": "row", "in": "rows", "height": 24, "c": [
        { "w": "text", "t": "{row.name}", "p": { "h": 24 } }
      ] }
    ] },
    { "w": "box", "id": "gapped", "f": ["scroll"], "p": { "dir": "col", "w": 300, "h": 100, "gap": 2 }, "c": [
      { "w": "each", "v": "row", "in": "few", "height": 20, "c": [
        { "w": "box", "p": { "h": 20, "w": 10 } }
      ] }
    ] },
    { "w": "hit", "id": "fade", "e": "x", "p": { "w": 50, "h": 20, "bg": "#000000", "transition": 0.2 },
      "looks": { "hover": { "bg": "#ffffff" } } }
  ] } } })";
const char* k_script = R"lua(
local rows, few = {}, {}
for i = 1, 10000 do rows[i] = { name = "row " .. i } end
for i = 1, 50 do few[i] = { name = i } end
return { model = { rows = rows, few = few }, on = {} }
)lua";

void provide_json(Engine& e, const char* path, const char* json) {
    JsonDoc doc;
    if (!json_parse(json, strlen(json), mem_scratch(), doc)) { KV_ERROR("test", "%s: %s at line %u", path, doc.error, doc.error_line); return; }
    Array<u8> k{};
    kbor_write(doc, k);
    krome_provide(e, path, k.data, k.len);
    k.free();
}

void frames(Engine& e, u32 n) {
    for (u32 i = 0; i < n; ++i) { platform_pump(e); engine_frame(e, 1.0 / 60.0); engine_frame_cycle(e, 1.0 / 60.0); }
}

// The live text node showing this, or KROME_NONE.
u32 text_node(const KromeTree& t, const char* want) {
    for (u32 i = 0; i < t.nodes.len; ++i)
        if (t.nodes[i].type == KromeType::Text && !(t.nodes[i].flags & KROME_DEAD) && !strcmp(t.text(i), want)) return i;
    return KROME_NONE;
}

u32 live_texts(const KromeTree& t) {
    u32 n = 0;
    for (u32 i = 0; i < t.nodes.len; ++i) if (t.nodes[i].type == KromeType::Text && !(t.nodes[i].flags & KROME_DEAD)) ++n;
    return n;
}

const KromeRegion* region_of(KromeScreen& sc, f32 height) {
    for (const KromeRegion& r : sc.regions) if (r.alive && r.height == height) return &r;
    return nullptr;
}

// The fill of the box painted at this rect.
u32 fill_at(KromeModule& s, KromeScreen& sc, rect2 r) {
    UiList l;
    l.init(mem_general());
    krome_paint_screen(s, sc, l);
    u32 fill = 0;
    for (u32 i = 0; i < l.count(); ++i) {
        const u32* w = &l.words[l.offsets[i]];
        if ((w[0] & 3) != 0) continue;
        f32 b[4];
        memcpy(b, w + 6, sizeof b);
        if (b[0] == r.x && b[1] == r.y && b[2] == r.w && b[3] == r.h) fill = w[2];
    }
    l.free();
    return fill;
}


// A sparse each (KR22): fifty thousand rows, of which the model holds a window.
const char* k_sparse = R"({"kim": 1, "file": "v/p.kim", "scripts": ["v/p.lua"],
  "screens": { "main": { "w": "box", "p": { "dir": "col" }, "c": [
    { "w": "box", "id": "list", "f": ["scroll"], "p": { "dir": "col", "w": 300, "h": 240 }, "c": [
      { "w": "each", "v": "row", "in": "rows", "height": 24, "count": "@total", "from": "@first", "e": "view", "c": [
        { "w": "text", "t": "{row.name}", "p": { "h": 24 } },
        { "w": "placeholder", "c": [ { "w": "text", "t": "...", "p": { "h": 24 } } ] }
      ] }
    ] }
  ] } } })";
const char* k_sparse_script = R"lua(
return { model = { total = 50000, first = 0, rows = {}, asked = -1, asked_n = 0, views = 0 },
         on = { view = function(first, count, total) model.asked = first model.asked_n = count model.views = model.views + 1 model.total_seen = total end } }
)lua";

u32 count_texts(const KromeTree& t, const char* want) {
    u32 n = 0;
    for (u32 i = 0; i < t.nodes.len; ++i)
        if (t.nodes[i].type == KromeType::Text && !(t.nodes[i].flags & KROME_DEAD) && !strcmp(t.text(i), want)) ++n;
    return n;
}

// The model's list as the owner would hand it: rows [from, from + n), "track <row + 1>".
void give_rows(Engine& e, Name scn, u32 from, u32 n, u32 renamed = ~0u) {
    Array<char> j{};
    auto put = [&](const char* text) { usize l = strlen(text); memcpy(j.push_n((u32)l), text, l); };
    put("[");
    char item[64];
    for (u32 i = 0; i < n; ++i) {
        if (from + i == renamed) format(item, sizeof item, "%s{\"name\": \"renamed\"}", i ? "," : "");
        else format(item, sizeof item, "%s{\"name\": \"track %u\"}", i ? "," : "", from + i + 1);
        put(item);
    }
    put("]");
    JsonDoc doc;
    if (json_parse(j.data, j.len, mem_scratch(), doc)) {
        Array<u8> k{};
        kbor_write(doc, k);
        krome_set_doc(e, scn, "rows", k.data, k.len);
        k.free();
    }
    krome_set(e, scn, "first", (f64)from);
    j.free();
}

f64 model_num(KromeModule& s, KromeScreen& sc, const char* path) {
    f64 v = -99.0;
    krome_model_number(s, sc, path, &v);
    return v;
}
} // namespace

void test_virtual(kv::TestCtx& c) {
    using namespace kv;
    c.begin("krome virtual");
    const ModuleDesc* const list[] = {&platform_module, &platform_hid_module, &asset_module, &krome_module};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    KromeModule& s = *krome_state(e);
    krome_viewport(e, 800, 600, 1.0f);
    provide_json(e, "v/s.kimb", k_screen);
    krome_provide(e, "v/s.lua", k_script, strlen(k_script));
    Name scn = krome_open(e, "v/s.kimb", "main");
    frames(e, 3);
    KV_CHECK(c, krome_ready(e, scn));
    KromeScreen& sc = *krome_screen(s, scn);
    KromeTree& t = sc.tree;
    u32 list_n = krome_find(t, name_intern("list"));

    // Only a few screens' worth (the first build guesses from the window; the view keeps it
    // while it is no more than three times what it needs).
    const KromeRegion* r = region_of(sc, 24.0f);
    KV_CHECK(c, r && r->len == 10000 && r->first == 0 && r->want <= 100);
    KV_CHECK(c, live_texts(t) <= 100 && text_node(t, "row 1") != KROME_NONE && text_node(t, "row 200") == KROME_NONE);
    // The box scrolls over all of them.
    KV_CHECK(c, t.nodes[list_n].content.y == 240000.0f);

    // A jump to the middle: what is in view is built, in its place.
    f64 t0 = time_now();
    t.nodes[list_n].scroll.y = 120000.0f;
    t.dirty = true;
    frames(e, 1);
    f64 jump_ms = (time_now() - t0) * 1000.0;
    r = region_of(sc, 24.0f);
    u32 mid = text_node(t, "row 5001");
    KV_CHECK(c, r && r->first > 4950 && r->first < 5000 && mid != KROME_NONE);
    KV_CHECK(c, mid != KROME_NONE && t.nodes[mid].rect.y == t.nodes[list_n].rect.y);
    KV_CHECK(c, live_texts(t) <= 40 && text_node(t, "row 1") == KROME_NONE);
    // The wheel a row at a time builds nothing while the window covers it.
    u64 regions_before = s.rebuilt_regions;
    krome_pointer(s, t.nodes[list_n].rect.x + 10, t.nodes[list_n].rect.y + 10, false, -0.5f, false);
    frames(e, 1);
    KV_CHECK(c, s.rebuilt_regions == regions_before && text_node(t, "row 5001") != KROME_NONE);
    // To the end: the last row at the bottom.
    t.nodes[list_n].scroll.y = 1e9f;
    t.dirty = true;
    frames(e, 1);
    u32 last = text_node(t, "row 10000");
    KV_CHECK(c, last != KROME_NONE && t.nodes[last].rect.y + 24.0f == t.nodes[list_n].rect.y + 240.0f);
    KV_INFO("test", "virtual each over 10,000: a jump to the middle built and laid out in %.2f ms (%u texts live)", jump_ms, live_texts(t));

    // With a gap: each row a height and a gap apart, the spacers counting it.
    u32 g = krome_find(t, name_intern("gapped"));
    KV_CHECK(c, t.nodes[g].content.y == 50.0f * 22.0f - 2.0f);
    t.nodes[g].scroll.y = 22.0f * 30.0f;
    t.dirty = true;
    frames(e, 1);
    {
        // The first row in view is at the box's top.
        bool found = false;
        for (u32 ch = t.nodes[g].first; ch != KROME_NONE; ch = t.nodes[ch].next)
            if (!(t.nodes[ch].flags & KROME_HIDDEN) && t.nodes[ch].w.mode == KromeSize::Fixed && t.nodes[ch].rect.y == t.nodes[g].rect.y) found = true;
        KV_CHECK(c, found);
    }

    // The list changing keeps the window where it is, within the new length.
    krome_set(e, scn, "rows", "none"); // not a list: none
    frames(e, 1);
    r = region_of(sc, 24.0f);
    KV_CHECK(c, r && r->len == 0 && live_texts(t) == 0);

    // A transition: hover eases the fill from black to white over 0.2 s.
    u32 fade = krome_find(t, name_intern("fade"));
    rect2 fr = krome_pixel_rect(t.nodes[fade].rect, 1.0f);
    KV_CHECK(c, fill_at(s, sc, fr) == 0xFF000000u);
    krome_pointer(s, fr.x + 5, fr.y + 5, false, 0, false);
    // (The frames would move the pointer to the window's, which a test has not: paint
    // directly.) The paint that sees the change starts it.
    KV_CHECK(c, fill_at(s, sc, fr) == 0xFF000000u);
    s.clock += 0.1;
    u32 half = fill_at(s, sc, fr);
    u32 hr = half & 0xFF;
    KV_CHECK(c, hr > 0x40 && hr < 0xC0 && s.animating);
    s.clock += 0.15;
    KV_CHECK(c, fill_at(s, sc, fr) == 0xFFFFFFFFu);
    // Interrupted: leaving mid-way eases back from where it was, not from white.
    krome_pointer(s, 700, 500, false, 0, false);
    fill_at(s, sc, fr);
    s.clock += 0.02;
    u32 back = fill_at(s, sc, fr) & 0xFF;
    KV_CHECK(c, back > 0xE0 && back < 0xFF);
    krome_pointer(s, fr.x + 5, fr.y + 5, false, 0, false);
    KV_CHECK(c, (fill_at(s, sc, fr) & 0xFF) == back); // turned around where it was
    s.clock += 0.01;
    u32 again = fill_at(s, sc, fr) & 0xFF;
    KV_CHECK(c, again >= back);
    s.clock += 1.0;
    KV_CHECK(c, fill_at(s, sc, fr) == 0xFFFFFFFFu);
    engine_shutdown(e);
}

void test_sparse(kv::TestCtx& c) {
    using namespace kv;
    c.begin("krome sparse");
    const ModuleDesc* const list[] = {&platform_module, &platform_hid_module, &asset_module, &krome_module};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    KromeModule& s = *krome_state(e);
    krome_viewport(e, 800, 600, 1.0f);
    provide_json(e, "v/p.kimb", k_sparse);
    krome_provide(e, "v/p.lua", k_sparse_script, strlen(k_sparse_script));
    Name scn = krome_open(e, "v/p.kimb", "main");
    frames(e, 3);
    KV_CHECK(c, krome_ready(e, scn));
    KromeScreen& sc = *krome_screen(s, scn);
    KromeTree& t = sc.tree;
    u32 list_n = krome_find(t, name_intern("list"));

    // Laid out as if every row were there; nothing held, so placeholders, and the rows
    // built asked for.
    const KromeRegion* r = region_of(sc, 24.0f);
    KV_CHECK(c, r && r->sparse && r->len == 50000 && r->rows_len == 0);
    KV_CHECK(c, t.nodes[list_n].content.y == 50000.0f * 24.0f);
    u32 built = r ? (r->first + r->want < r->len ? r->want : r->len - r->first) : 0;
    KV_CHECK(c, built > 10 && built <= 100 && count_texts(t, "...") == built && text_node(t, "track 1") == KROME_NONE);
    KV_CHECK(c, model_num(s, sc, "asked") == 0.0 && model_num(s, sc, "asked_n") == (f64)built && model_num(s, sc, "total_seen") == 50000.0);
    f64 views = model_num(s, sc, "views");
    KV_CHECK(c, views == 1.0);

    // The owner hands them a tick later: each row in place, no placeholder left, no new ask.
    give_rows(e, scn, 0, built);
    frames(e, 1);
    r = region_of(sc, 24.0f);
    KV_CHECK(c, r && r->rows_len == built && count_texts(t, "...") == 0 && text_node(t, "track 1") != KROME_NONE && text_node(t, "track 2") != KROME_NONE);
    KV_CHECK(c, model_num(s, sc, "views") == views && t.nodes[list_n].scroll.y == 0.0f);

    // A jump to the middle by the scrollbar: placeholders where the rows have not come, in
    // their places, and an ask for the new ones.
    t.nodes[list_n].scroll.y = 600000.0f;
    t.dirty = true;
    frames(e, 1);
    r = region_of(sc, 24.0f);
    f64 asked = model_num(s, sc, "asked");
    KV_CHECK(c, r && asked > 24900.0 && asked < 25000.0 && model_num(s, sc, "views") == views + 1.0);
    KV_CHECK(c, text_node(t, "track 1") == KROME_NONE && count_texts(t, "...") > 0);
    u32 n = (u32)model_num(s, sc, "asked_n");
    give_rows(e, scn, (u32)asked, n);
    frames(e, 1);
    u32 mid = text_node(t, "track 25001");
    KV_CHECK(c, mid != KROME_NONE && t.nodes[mid].rect.y == t.nodes[list_n].rect.y && count_texts(t, "...") == 0);
    KV_CHECK(c, t.nodes[list_n].scroll.y == 600000.0f);

    // The same rows handed over again, at rest: every row kept, node for node, and only
    // the spacers built; the scroll where it was.
    {
        u32 before_id = text_node(t, "track 25001");
        u64 nodes_before = s.rebuilt_nodes;
        give_rows(e, scn, (u32)asked, n);
        frames(e, 1);
        KV_CHECK(c, text_node(t, "track 25001") == before_id && t.nodes[list_n].scroll.y == 600000.0f);
        KV_CHECK(c, s.rebuilt_nodes - nodes_before <= 4); // the spacers, twice over (the list, then `from`)
    }
    // Handed over starting a row earlier: one row new at the top and the rest kept, now each
    // a list element further on; what reads them follows (a row's name changed after shows).
    {
        u32 kept_id = text_node(t, "track 25010");
        give_rows(e, scn, (u32)asked - 1, n);
        frames(e, 1);
        KV_CHECK(c, text_node(t, "track 25010") == kept_id && text_node(t, "track 25001") != KROME_NONE);
        // One row's value changed: that row built again, the rest kept.
        u32 other = text_node(t, "track 25003");
        give_rows(e, scn, (u32)asked - 1, n, 25009);
        frames(e, 1);
        KV_CHECK(c, text_node(t, "track 25010") == KROME_NONE && text_node(t, "renamed") != KROME_NONE && text_node(t, "track 25003") == other);
    }

    // Rows only partly held: the held ones real, the rest placeholders.
    give_rows(e, scn, (u32)asked, n / 2);
    frames(e, 1);
    KV_CHECK(c, text_node(t, "track 25001") != KROME_NONE && count_texts(t, "...") == n - n / 2);

    // The scrollbar dragged end to end over a second and a half of frames, the owner
    // handing each window over the frame after it is asked for: the longest frame.
    {
        f64 worst = 0.0, total_ms = 0.0;
        u32 steps = 180, placeheld = 0;
        f32 end = t.nodes[list_n].content.y - 240.0f;
        for (u32 i = 0; i <= steps; ++i) {
            f64 t0 = time_now();
            t.nodes[list_n].scroll.y = end * (f32)i / (f32)steps;
            t.dirty = true;
            f64 before = model_num(s, sc, "views");
            frames(e, 1);
            if (model_num(s, sc, "views") != before) {
                if (count_texts(t, "...") > 0) ++placeheld;
                give_rows(e, scn, (u32)model_num(s, sc, "asked"), (u32)model_num(s, sc, "asked_n"));
            }
            f64 ms = (time_now() - t0) * 1000.0;
            total_ms += ms;
            if (ms > worst) worst = ms;
        }
        frames(e, 1);
        u32 last = text_node(t, "track 50000");
        KV_CHECK(c, last != KROME_NONE && count_texts(t, "...") == 0);
        KV_INFO("test", "sparse each over 50,000: a drag end to end in %u frames, %.2f ms each on average, %.2f ms the longest; %u showed placeholders",
                steps + 1, total_ms / (steps + 1), worst, placeheld);
#if !KV_DEBUG
        KV_CHECK(c, worst < 8.3); // a 120 Hz display's refresh
#endif
    }

    // A shorter list: the count is the model's.
    krome_set(e, scn, "total", 100.0);
    frames(e, 2);
    r = region_of(sc, 24.0f);
    KV_CHECK(c, r && r->len == 100 && t.nodes[list_n].content.y == 2400.0f && model_num(s, sc, "total_seen") == 100.0);
    engine_shutdown(e);
}

// A sparse each from .kim text (KR27): compiled by kim, checked (a reload is, and the check
// used to refuse the placeholder), and built with its placeholders.
void test_sparse_text(kv::TestCtx& c) {
    using namespace kv;
    c.begin("krome sparse from text");
    const char* text =
        "script \"v/t.lua\"\n"
        "screen main { box dir=col {\n"
        "  box id=list scroll dir=col w=300 h=240 {\n"
        "    each row in rows height=24 count=@n from=@f {\n"
        "      text \"{row.name}\" h=24\n"
        "      placeholder { text \"...\" h=24 }\n"
        "    }\n"
        "  }\n"
        "} }\n";
    const char* script = "return { model = { n = 1000, f = 0, rows = {} }, on = {} }";
    Array<u8> k{};
    KimErrors errs;
    KV_CHECK(c, kim_compile(text, strlen(text), "v/t.kim", k, errs) && errs.count == 0);
    {
        // The check on its own, as goldberg runs it when it cooks.
        JsonDoc doc;
        KimErrors e2;
        KimbFile f;
        KimbScope scope;
        KV_CHECK(c, kim_parse(text, strlen(text), "v/t.kim", mem_scratch(), doc, e2) && kimb_from_doc(doc, f, e2, "v/t.kim"));
        KV_CHECK(c, kimb_scope(scope, f, [](void*, const char*) -> const KimbFile* { return nullptr; }, nullptr, e2) && kimb_check(scope, e2));
    }

    const ModuleDesc* const list[] = {&platform_module, &platform_hid_module, &asset_module, &krome_module};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    KromeModule& s = *krome_state(e);
    krome_viewport(e, 800, 600, 1.0f);
    krome_provide(e, "v/t.kimb", k.data, k.len);
    krome_provide(e, "v/t.lua", script, strlen(script));
    Name scn = krome_open(e, "v/t.kimb", "main");
    frames(e, 3);
    KV_CHECK(c, krome_ready(e, scn));
    KromeScreen& sc = *krome_screen(s, scn);
    const KromeRegion* r = region_of(sc, 24.0f);
    KV_CHECK(c, r && r->sparse && r->len == 1000 && count_texts(sc.tree, "...") > 10);

    // Given again, as a save is: checked in Krome's own scope and put in force.
    krome_provide(e, "v/t.kimb", k.data, k.len);
    frames(e, 3);
    KV_CHECK(c, !sc.held && !s.reload_error[0] && count_texts(sc.tree, "...") > 10);
    k.free();
    engine_shutdown(e);
}
