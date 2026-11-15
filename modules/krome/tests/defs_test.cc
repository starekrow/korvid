// Definitions and control flow (docs/PRDs/Krome-Slice-2-PRD.md, milestone 3): parameters,
// the slot, the use line over the body's root, a mistake naming both lines; each and if
// built again alone when their key changes; bindings; lists from C++; kim() pieces.
#include "korvid/krome/krome.h"
#include "../src/internal.h"
#include "korvid/core/engine.h"
#include "korvid/core/kbor.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include "korvid/platform/module.h"
#include "korvid/platform_hid/module.h"
#include "korvid/asset/module.h"
#include <stdio.h>
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

// The texts under a node, in order, joined by '|'.
void texts(const KromeTree& t, u32 n, char* out, usize cap) {
    for (u32 c = t.nodes[n].first; c != KROME_NONE; c = t.nodes[c].next) {
        if (t.nodes[c].flags & KROME_DEAD) continue;
        if (t.nodes[c].type == KromeType::Text) {
            usize l = strlen(out);
            snprintf(out + l, cap - l, "%s%s", l ? "|" : "", t.text(c));
        }
        texts(t, c, out, cap);
    }
}
const char* texts_of(const KromeTree& t, u32 n) {
    static char buf[4096];
    buf[0] = 0;
    texts(t, n, buf, sizeof buf);
    return buf;
}
bool same(const char* got, const char* want) {
    if (!strcmp(got, want)) return true;
    KV_ERROR("test", "got '%s', wanted '%s'", got, want);
    return false;
}

const char* k_std = R"({"kim": 1, "file": "krome/std.kim",
  "props": [{"name": "label", "type": "string"}],
  "defs": {
    "broken": {"params": [], "body": {"w": "box", "p": {"bg": "nopes"}}},
    "button": {"params": ["label"], "body": {"w": "hit", "s": ["button"], "c": [{"w": "text", "t": "{label}"}]}},
    "field":  {"params": ["label", "value"], "body": {"w": "box", "p": {"dir": "row", "gap": 4}, "c": [{"w": "text", "t": "{label}"}, {"w": "text", "t": "{value}"}]}},
    "loud":   {"params": ["label"], "body": {"w": "button", "t": "{label}!", "p": {"color": "#ff0000"}}},
    "panel":  {"params": ["title"], "body": {"w": "box", "p": {"dir": "col"}, "c": [{"w": "text", "t": "{title}"}, {"w": "box", "id": "body", "c": [{"w": "slot"}]}]}},
    "row":    {"params": [], "body": {"w": "box", "p": {"dir": "row"}, "c": [{"w": "slot"}]}}},
  "styles": {"button": {"p": {"bg": "panel", "pad": [8, 4]}}},
  "themes": {"dark": {"panel": "#202430"}},
  "lines": [1, 10, 11, 20, 21, 22, 30, 31, 32, 33, 40, 41, 50, 51, 52, 53, 54, 60, 61, 62, 70, 80, 81]})";

const char* k_screen = R"({"kim": 1, "file": "t/m.kim", "uses": ["krome/std"], "scripts": ["t/m.lua"],
  "screens": {"s": {"w": "panel", "t": "Title {who}", "id": "p", "c": [
    {"w": "row", "p": {"gap": 8}, "id": "bar", "c": [
      {"w": "button", "t": "Save", "e": "save", "id": "save"},
      {"w": "loud", "t": "Go", "id": "go", "e": "go"},
      {"w": "field", "t": "Name", "a": ["{name}"]}]},
    {"w": "box", "id": "list", "c": [{"w": "each", "v": "item", "in": "items", "c": [{"w": "text", "t": "{item.name}: {item.n}"}]}]},
    {"w": "if", "on": "dirty", "c": [{"w": "text", "id": "dirty", "t": "unsaved"}]},
    {"w": "box", "id": "wrap", "b": "wrap", "e": "wrapped"},
    {"w": "broken"},
    {"w": "button", "t": "a", "a": ["b"]}]}},
  "lines": [1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17]})";

const char* k_script = R"lua(
return {
  model = { who = "Ada", name = "Rowan", items = { { name = "a", n = 1 }, { name = "b", n = 2 } }, dirty = false, wrap = true },
  on = {
    save = function() model.dirty = true end,
    wrapped = function() model.seen = model.wrap and "on" or "off" end,
  },
}
)lua";

} // namespace

void test_defs(kv::TestCtx& c) {
    using namespace kv;
    c.begin("krome defs");
    const ModuleDesc* const list[] = {&platform_module, &platform_hid_module, &asset_module, &krome_module};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    KromeModule* s = krome_state(e);
    provide_json(e, "krome/std.kimb", k_std);
    provide_json(e, "t/m.kimb", k_screen);
    krome_provide(e, "t/m.lua", k_script, strlen(k_script));
    Name m = krome_open(e, "t/m.kimb", "s");
    frames(e, 3);
    KV_CHECK(c, krome_ready(e, m));
    const KromeTree* t = krome_tree(e, m);

    // panel's body with the use line over its root: its id; its title a parameter that
    // reads the model; its slot holding the use's children.
    u32 root = t->root;
    KV_CHECK(c, t->nodes[root].id == name_intern("p") && t->nodes[root].dir == KromeDir::Col && t->nodes[root].line == 51);
    KV_CHECK(c, !strcmp(t->text(t->nodes[root].first), "Title Ada"));
    u32 body = krome_find(*t, name_intern("body"));
    u32 bar = krome_find(*t, name_intern("bar"));
    KV_CHECK(c, body != KROME_NONE && bar != KROME_NONE && t->nodes[bar].parent == body);
    // row: a box with dir row from its body, gap from the use.
    KV_CHECK(c, t->nodes[bar].type == KromeType::Box && t->nodes[bar].dir == KromeDir::Row && t->nodes[bar].gap == 8);
    // button: a hit in its style, the label in its text, the use's id and event.
    u32 save = krome_find(*t, name_intern("save"));
    KV_CHECK(c, save != KROME_NONE && t->nodes[save].type == KromeType::Hit && t->nodes[save].event == name_intern("save") && t->nodes[save].pad[0] == 8);
    KV_CHECK(c, rgba(t->resolve_look(save, KromeState::Base, nullptr).bg) == 0x202430FFu && !strcmp(t->text(t->nodes[save].first), "Save"));
    // loud is a button: its parameter into button's, its color over button's root, its
    // use's id and event over that.
    u32 go = krome_find(*t, name_intern("go"));
    KV_CHECK(c, go != KROME_NONE && t->nodes[go].type == KromeType::Hit && t->nodes[go].event == name_intern("go"));
    KV_CHECK(c, rgba(t->resolve_look(go, KromeState::Base, nullptr).text_color) == 0xFF0000FFu && rgba(t->resolve_look(go, KromeState::Base, nullptr).bg) == 0x202430FFu);
    KV_CHECK(c, !strcmp(t->text(t->nodes[go].first), "Go!"));
    KV_CHECK(c, same(texts_of(*t, bar), "Save|Go!|Name|Rowan"));
    // each: an element per item, its fields in the text; if: nothing while dirty is false.
    u32 list_box = krome_find(*t, name_intern("list"));
    KV_CHECK(c, same(texts_of(*t, list_box), "a: 1|b: 2"));
    KV_CHECK(c, krome_find(*t, name_intern("dirty")) == KROME_NONE);
    // A binding: selected while wrap is true.
    u32 wrap = krome_find(*t, name_intern("wrap"));
    KV_CHECK(c, wrap != KROME_NONE && (t->nodes[wrap].flags & KROME_SELECTED));

    // A mistake inside a definition names its line and the use's; a use given more strings
    // than its definition has parameters names the use.
    KV_CHECK(c, has_error(e, "krome/std.kim:11: bg: 'nopes' is not a color, nor a token of theme dark (used at t/m.kim:16)"));
    KV_CHECK(c, has_error(e, "t/m.kim:17: button takes 1 parameter, and is given 2 strings"));

    // The if's key changes: it alone is built again.
    KromeStats before = krome_stats(e);
    KV_CHECK(c, krome_press(e, m, "save"));
    t = krome_tree(e, m);
    u32 dirty = krome_find(*t, name_intern("dirty"));
    KV_CHECK(c, dirty != KROME_NONE && !strcmp(t->text(dirty), "unsaved"));
    KromeStats after = krome_stats(e);
    KV_CHECK(c, after.rebuilt_regions == before.rebuilt_regions + 1 && after.rebuilt_nodes == before.rebuilt_nodes + 1);
    // It is where it was written: after the list, before wrap.
    KV_CHECK(c, t->nodes[list_box].next != KROME_NONE);
    u32 order[8];
    u32 n = 0;
    for (u32 k = t->nodes[body].first; k != KROME_NONE && n < 8; k = t->nodes[k].next) if (!(t->nodes[k].flags & KROME_HIDDEN)) order[n++] = k;
    KV_CHECK(c, n >= 4 && order[0] == bar && order[1] == list_box && order[2] == dirty && order[3] == wrap);
    KV_CHECK(c, same(texts_of(*t, list_box), "a: 1|b: 2")); // the list was not touched

    // A binding's press flips the value before the handler sees it.
    KV_CHECK(c, krome_press(e, m, "wrap"));
    t = krome_tree(e, m);
    KV_CHECK(c, !(t->nodes[krome_find(*t, name_intern("wrap"))].flags & KROME_SELECTED) && !strcmp(krome_model_text(e, m, "seen"), "off"));
    KV_CHECK(c, krome_press(e, m, "wrap") && !strcmp(krome_model_text(e, m, "seen"), "on"));

    // A list from C++: the each is built again with it, and the if is not.
    {
        JsonDoc doc;
        const char* json = R"([{"name": "x", "n": 9}, {"name": "y", "n": 8}, {"name": "z", "n": 7}])";
        KV_CHECK(c, json_parse(json, strlen(json), mem_scratch(), doc));
        Array<u8> k{};
        kbor_write(doc, k);
        before = krome_stats(e);
        krome_set_doc(e, m, "items", k.data, k.len);
        k.free();
        frames(e, 1);
        t = krome_tree(e, m);
        KV_CHECK(c, same(texts_of(*t, krome_find(*t, name_intern("list"))), "x: 9|y: 8|z: 7"));
        after = krome_stats(e);
        KV_CHECK(c, after.rebuilt_regions == before.rebuilt_regions + 1 && after.rebuilt_nodes == before.rebuilt_nodes + 3);
        KV_CHECK(c, krome_find(*t, name_intern("dirty")) != KROME_NONE);
    }

    // A thousand: built again when the list is set, and not when anything else changes.
    {
        Array<u8> k{};
        JsonBuilder jb(mem_scratch());
        JsonRef arr = jb.array();
        for (u32 i = 0; i < 1000; ++i) {
            JsonRef o = jb.object();
            jb.set(o, "name", jb.string("item"));
            jb.set(o, "n", jb.integer(i));
            jb.push(arr, o);
        }
        JsonDoc doc;
        KV_CHECK(c, jb.finish(arr, doc));
        jb.free();
        kbor_write(doc, k);
        f64 t0 = time_now();
        krome_set_doc(e, m, "items", k.data, k.len);
        frames(e, 1);
        f64 t1 = time_now();
        before = krome_stats(e);
        krome_set_bool(e, m, "dirty", false);
        frames(e, 1);
        f64 t2 = time_now();
        after = krome_stats(e);
        t = krome_tree(e, m);
        u32 items = 0;
        for (u32 q = t->nodes[krome_find(*t, name_intern("list"))].first; q != KROME_NONE; q = t->nodes[q].next) if (!(t->nodes[q].flags & KROME_HIDDEN)) ++items;
        KV_CHECK(c, items == 1000 && after.rebuilt_nodes == before.rebuilt_nodes && krome_find(*t, name_intern("dirty")) == KROME_NONE);
        KV_INFO("test", "each over 1000: set and built in %.2f ms (with a frame); an if beside it changed in %.2f ms, the list untouched",
                (t1 - t0) * 1000.0, (t2 - t1) * 1000.0);
        k.free();
    }

    // A Lua tree with a piece of markup in it; changed() for a list changed in place.
    Name lua = krome_open_source(e, "t/lua", R"lua(
      return { model = { items = { { name = "q", n = 1 } }, who = "Bo", name = "N" },
               on = { grow = function() table.insert(model.items, { name = "r", n = 2 }); changed("items") end },
               root = col { text "top", kim("t/m.kimb", "s"), hit { id = "grow", event = "grow" } } })lua");
    frames(e, 2);
    KV_CHECK(c, krome_ready(e, lua));
    t = krome_tree(e, lua);
    KV_CHECK(c, krome_find(*t, name_intern("save")) != KROME_NONE && !strcmp(t->text(t->nodes[t->root].first), "top"));
    KV_CHECK(c, same(texts_of(*t, krome_find(*t, name_intern("list"))), "q: 1"));
    KV_CHECK(c, krome_press(e, lua, "grow"));
    t = krome_tree(e, lua);
    KV_CHECK(c, same(texts_of(*t, krome_find(*t, name_intern("list"))), "q: 1|r: 2"));

    // Mistakes of shape: a slot outside a definition, children for one with no slot, a
    // definition that uses itself.
    krome_clear_errors(e);
    provide_json(e, "t/odd.kimb", R"({"kim": 1, "file": "t/odd.kim", "uses": ["krome/std"],
      "defs": {"me": {"params": [], "body": {"w": "me"}}},
      "screens": {"s": {"w": "box", "c": [{"w": "slot"}, {"w": "button", "t": "x", "c": [{"w": "box"}]}, {"w": "me"}]}},
      "lines": [1, 2, 3, 4, 5, 6, 7, 8, 9]})");
    Name odd = krome_open(e, "t/odd.kimb", "s");
    frames(e, 3);
    KV_CHECK(c, krome_ready(e, odd));
    KV_CHECK(c, has_error(e, "t/odd.kim:4: slot is only in a definition's body"));
    KV_CHECK(c, has_error(e, "t/odd.kim:5: button takes no children: its body has no slot"));
    KV_CHECK(c, krome_errors(e)->count >= 3);
    (void)s;
    engine_shutdown(e);
}
