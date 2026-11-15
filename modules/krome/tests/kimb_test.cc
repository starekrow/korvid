// Krome and .kimb (docs/PRDs/Krome-Slice-2-PRD.md, milestone 2): a screen of a .kimb with
// its preludes and scripts, styles merged with the node over them, tokens from the theme,
// a theme switch, and mistakes with their files and lines. The documents are written as
// JSON here and given as KBOR (krome_provide): Krome does not link the parser.
#include "korvid/krome/krome.h"
#include "../src/internal.h"
#include "korvid/core/engine.h"
#include "korvid/core/kbor.h"
#include "korvid/core/test.h"
#include "korvid/platform/module.h"
#include "korvid/platform_hid/module.h"
#include "korvid/asset/module.h"
#include <string.h>

namespace {

using namespace kv;

void provide_json(Engine& e, const char* path, const char* json) {
    JsonDoc doc;
    if (!json_parse(json, strlen(json), mem_scratch(), doc)) { KV_ERROR("test", "%s: %s at line %u", path, doc.error, doc.error_line); return; }
    Array<u8> k{};
    kbor_write(doc, k);
    krome_provide(e, path, k.data, k.len);
    k.free();
}
void provide_text(Engine& e, const char* path, const char* text) { krome_provide(e, path, text, strlen(text)); }

void frames(Engine& e, u32 n) {
    for (u32 i = 0; i < n; ++i) { platform_pump(e); engine_frame(e, 1.0 / 60.0); engine_frame_cycle(e, 1.0 / 60.0); }
}

u32 rgba(color c) {
    auto b = [](f32 v) { return (u32)(v * 255.0f + 0.5f); };
    return b(c.r) << 24 | b(c.g) << 16 | b(c.b) << 8 | b(c.a);
}

bool has_error(Engine& e, const char* want) {
    const KimErrors* errs = krome_errors(e);
    char line[300];
    for (u32 i = 0; errs && i < errs->count; ++i) {
        errs->format_one(i, line, sizeof line);
        if (!strcmp(line, want)) return true;
    }
    KV_ERROR("test", "no error '%s' among:", want);
    for (u32 i = 0; errs && i < errs->count; ++i) { errs->format_one(i, line, sizeof line); KV_ERROR("test", "  %s", line); }
    return false;
}

const char* k_std = R"({"kim": 1, "file": "krome/std.kim",
  "styles": {
    "card": {"p": {"bg": "panel", "pad": [8, 4], "radius": 4}, "looks": {"hover": {"bg": "#2f6fed"}}},
    "loud": {"s": ["card"], "p": {"color": "#ff0000"}}},
  "themes": {
    "dark":  {"gap": 8, "ink": "#e6e9ef", "panel": "#202430"},
    "light": {"gap": 4, "ink": "#101010", "panel": "#f0f0f0"}}})";

const char* k_form = R"({"kim": 1, "file": "t/form.kim", "uses": ["krome/std"], "scripts": ["t/a.lua", "t/b.lua"],
  "screens": {"form": {"w": "box", "p": {"dir": "col", "gap": "gap"}, "c": [
    {"w": "hit", "id": "go", "e": "go", "s": ["loud"], "p": {"radius": 6}, "c": [{"w": "text", "t": "Hi {name}"}]},
    {"w": "text", "id": "count", "t": "{count}", "p": {"color": "ink"}}]}},
  "lines": [1, 2, 3, 5, 6, 7, 8, 9]})";

} // namespace

void test_kimb(kv::TestCtx& c) {
    using namespace kv;
    c.begin("krome kimb");
    const ModuleDesc* const list[] = {&platform_module, &platform_hid_module, &asset_module, &krome_module};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    KromeModule* s = krome_state(e);

    provide_json(e, "krome/std.kimb", k_std);
    provide_json(e, "t/form.kimb", k_form);
    provide_text(e, "t/a.lua", "return { model = { name = 'Ada', count = 0 }, on = { go = function() model.count = model.count + 1 end } }");
    provide_text(e, "t/b.lua", "return { model = { count = 10 } }");
    Name form = krome_open(e, "t/form.kimb", "form");
    KV_CHECK(c, form == name_intern("t/form.kimb#form") && !krome_ready(e, form));
    frames(e, 3);
    KV_CHECK(c, krome_ready(e, form) && s->script_errors == 0);
    const KromeTree* t = krome_tree(e, form);
    const KromeNode& root = t->nodes[t->root];
    // A token from the first theme; lines from the sidecar.
    KV_CHECK(c, root.dir == KromeDir::Col && root.gap == 8 && root.line == 6);
    u32 go = krome_find(*t, name_intern("go"));
    KV_CHECK(c, go != KROME_NONE && t->nodes[go].line == 7);
    // loud starts from card: card's bg (a token) and pad, loud's color, the node's radius over card's.
    KromeLook base = t->resolve_look(go, KromeState::Base, nullptr);
    KV_CHECK(c, rgba(base.bg) == 0x202430FFu && rgba(base.text_color) == 0xFF0000FFu && base.radius[0] == 6 && t->nodes[go].pad[0] == 8 && t->nodes[go].pad[1] == 4);
    KV_CHECK(c, rgba(t->resolve_look(go, KromeState::Hover, nullptr).bg) == 0x2F6FEDFFu);
    // Both scripts ran in one environment: a's handler, b's count over a's.
    u32 hi = t->nodes[go].first, count = krome_find(*t, name_intern("count"));
    KV_CHECK(c, strcmp(t->text(hi), "Hi Ada") == 0 && strcmp(t->text(count), "10") == 0);
    KV_CHECK(c, krome_press(e, form, "go") && strcmp(krome_tree(e, form)->text(count), "11") == 0);
    KV_CHECK(c, !strcmp(krome_current_theme(e, form), "dark"));

    // A theme switch: rebuilt from the documents, nothing read again; the model stays.
    u64 reads = s->docs.len;
    krome_theme(e, "light");
    frames(e, 1);
    t = krome_tree(e, form);
    go = krome_find(*t, name_intern("go"));
    count = krome_find(*t, name_intern("count"));
    KV_CHECK(c, t->nodes[t->root].gap == 4 && rgba(t->resolve_look(go, KromeState::Base, nullptr).bg) == 0xF0F0F0FFu);
    KV_CHECK(c, rgba(t->resolve_look(count, KromeState::Base, nullptr).text_color) == 0x101010FFu && strcmp(t->text(count), "11") == 0);
    KV_CHECK(c, !strcmp(krome_current_theme(e, form), "light") && s->docs.len == reads);
    // From Lua too.
    provide_text(e, "t/c.lua", "return { on = { dark = function() theme('dark') end } }");
    provide_json(e, "t/two.kimb", R"({"kim": 1, "file": "t/two.kim", "uses": ["krome/std"], "scripts": ["t/c.lua"],
      "screens": {"s": {"w": "hit", "id": "d", "e": "dark", "s": ["card"]}}})");
    Name two = krome_open(e, "t/two.kimb", "s");
    frames(e, 3);
    KV_CHECK(c, krome_ready(e, two) && rgba(krome_tree(e, two)->looks[krome_tree(e, two)->nodes[0].look].bg) == 0xF0F0F0FFu);
    KV_CHECK(c, krome_press(e, two, "d"));
    frames(e, 1);
    KV_CHECK(c, rgba(krome_tree(e, two)->looks[krome_tree(e, two)->nodes[0].look].bg) == 0x202430FFu && !strcmp(krome_current_theme(e, form), "dark"));

    // A Lua screen with uses: the same styles and tokens.
    Name lua = krome_open_source(e, "t/lua", "return { uses = { 'krome/std' }, root = box { style = 'card', gap = 'gap' } }");
    frames(e, 2);
    KV_CHECK(c, krome_ready(e, lua) && krome_tree(e, lua)->nodes[0].gap == 8 && krome_tree(e, lua)->nodes[0].pad[0] == 8);

    // Mistakes, with their files and lines: a style that is not there, a token no theme
    // has, a property that is not, a word that is no node; the screen opens with the rest.
    krome_clear_errors(e);
    u64 before = s->script_errors;
    provide_json(e, "t/bad.kimb", R"({"kim": 1, "file": "t/bad.kim", "uses": ["krome/std"],
      "screens": {"s": {"w": "box", "c": [
        {"w": "box", "s": ["lound"]}, {"w": "box", "p": {"bg": "panl"}}, {"w": "box", "p": {"colour": "#fff"}}, {"w": "buton"}]}},
      "lines": [1, 3, 4, 5, 6, 7, 8]})");
    Name bad = krome_open(e, "t/bad.kimb", "s");
    frames(e, 3);
    KV_CHECK(c, krome_ready(e, bad) && s->script_errors == before + 4);
    KV_CHECK(c, has_error(e, "t/bad.kim:5: no style 'lound'"));
    KV_CHECK(c, has_error(e, "t/bad.kim:6: bg: 'panl' is not a color, nor a token of theme dark"));
    KV_CHECK(c, has_error(e, "t/bad.kim:7: 'colour' is not a property"));
    KV_CHECK(c, has_error(e, "t/bad.kim:8: 'buton' is neither a kernel node (box, text, image, hit) nor a definition"));

    // A use that is not there fails the screen; one event handled in two scripts is said.
    krome_clear_errors(e);
    provide_json(e, "t/nouse.kimb", R"({"kim": 1, "file": "t/nouse.kim", "uses": ["krome/nope"], "screens": {"s": {"w": "box"}}, "lines": [2, 3, 4]})");
    Name nouse = krome_open(e, "t/nouse.kimb", "s");
    provide_text(e, "t/d.lua", "return { on = { go = function() end } }");
    provide_json(e, "t/twice.kimb", R"({"kim": 1, "file": "t/twice.kim", "scripts": ["t/a.lua", "t/d.lua"], "screens": {"s": {"w": "box"}}})");
    Name twice = krome_open(e, "t/twice.kimb", "s");
    frames(e, 4);
    KV_CHECK(c, !krome_ready(e, nouse) && krome_screen(*s, nouse)->failed && has_error(e, "t/nouse.kim:2: use \"krome/nope\": no such prelude"));
    KV_CHECK(c, krome_ready(e, twice) && has_error(e, "t/d.lua:0: the event 'go' is handled here and in t/a.lua"));
    // A screen the file does not have; a file that is not a .kimb.
    Name none = krome_open(e, "t/form.kimb", "nope");
    krome_provide(e, "t/junk.kimb", "junk", 4);
    Name junk = krome_open(e, "t/junk.kimb", "s");
    frames(e, 2);
    KV_CHECK(c, !krome_ready(e, none) && has_error(e, "t/form.kim:0: no screen 'nope'"));
    KV_CHECK(c, !krome_ready(e, junk) && krome_screen(*s, junk)->failed);
    engine_shutdown(e);
}
