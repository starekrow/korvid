// Reading a .kimb (lines onto the values), the scope's order, and lookups through it. The
// check itself is tested over .kim text in kim's tests.
#include "korvid/kimb/kimb.h"
#include "korvid/core/arena.h"
#include "korvid/core/kbor.h"
#include "korvid/core/test.h"
#include <string.h>

namespace {

using namespace kv;

KimbFile g_files[8];
const char* g_names[8];
u32 g_count = 0;

KimbFile* add(const char* name, const char* json) {
    if (g_count == 8) return nullptr;
    JsonDoc doc;
    if (!json_parse(json, strlen(json), mem_scratch(), doc)) return nullptr;
    Array<u8> k{};
    kbor_write(doc, k);
    KimErrors e;
    KimbFile& f = g_files[g_count];
    g_names[g_count++] = name;
    bool ok = kimb_read(k.data, k.len, mem_scratch(), f, e, name);
    k.free();
    return ok ? &f : nullptr;
}

const KimbFile* find(void*, const char* use) {
    for (u32 i = 0; i < g_count; ++i) if (!strcmp(g_names[i], use)) return &g_files[i];
    return nullptr;
}

} // namespace

void test_kimb(kv::TestCtx& c) {
    c.begin("kimb");
    using namespace kv;

    // Lines move from the sidecar onto the values, in the walk kim writes them in.
    KimbFile* a = add("a", R"({"kim": 1, "file": "a.kim", "uses": ["b", "c"],
      "screens": {"s": {"w": "box", "c": [{"w": "text"}, {"w": "box", "c": [{"w": "text"}]}]}},
      "defs": {"d": {"params": [], "body": {"w": "box"}}},
      "styles": {"st": {"looks": {"hover": {}}}},
      "themes": {"dark": {"x": "#fff"}},
      "lines": [1, 2, 4, 5, 6, 7, 8, 10, 11, 13, 14, 16, 17]})");
    KV_CHECK(c, a && a->lines && !strcmp(a->file, "a.kim"));
    const JsonDoc& d = a->doc;
    const JsonValue* s = json_get(d, *a->screens, "s");
    const JsonValue* kids = json_get(d, *s, "c");
    KV_CHECK(c, json_first(d, *a->uses)->line == 1 && s->line == 5);
    KV_CHECK(c, json_at(d, *kids, 0)->line == 6 && json_at(d, *kids, 1)->line == 7 && json_first(d, *json_get(d, *json_at(d, *kids, 1), "c"))->line == 8);
    const JsonValue* def = json_get(d, *a->defs, "d");
    KV_CHECK(c, def->line == 10 && json_get(d, *def, "body")->line == 11);
    const JsonValue* st = json_get(d, *a->styles, "st");
    KV_CHECK(c, st->line == 13 && json_get(d, *json_get(d, *st, "looks"), "hover")->line == 14);
    const JsonValue* th = json_get(d, *a->themes, "dark");
    KV_CHECK(c, th->line == 16 && json_get(d, *th, "x")->line == 17);

    // The scope: the file, then its uses depth first, each once; lookups in that order.
    add("b", R"({"kim": 1, "file": "b.kim", "uses": ["c"], "themes": {"dark": {"x": "#000", "y": 1}}, "styles": {"st": {}, "bs": {}}})");
    add("c", R"({"kim": 1, "file": "c.kim", "themes": {"light": {"x": "#111"}}, "defs": {"d": {"params": []}, "cd": {"params": []}},
                 "props": [{"name": "size2", "type": "number"}]})");
    KimErrors e;
    KimbScope scope;
    KV_CHECK(c, kimb_scope(scope, *a, find, nullptr, e) && scope.count == 3);
    KV_CHECK(c, !strcmp(scope.list[0]->file, "a.kim") && !strcmp(scope.list[1]->file, "b.kim") && !strcmp(scope.list[2]->file, "c.kim"));
    KimbFound f;
    KV_CHECK(c, kimb_find_def(scope, "d", &f) && f.file == a);           // its own first
    KV_CHECK(c, kimb_find_def(scope, "cd", &f) && !strcmp(f.file->file, "c.kim"));
    KV_CHECK(c, kimb_find_style(scope, "st", &f) && f.file == a && kimb_find_style(scope, "bs", &f) && !kimb_find_style(scope, "zz", nullptr));
    // A theme of the same name in two files: each token from the first that has it.
    KV_CHECK(c, kimb_find_token(scope, "dark", "x", &f) && !strcmp(f.value->str(), "#fff"));
    KV_CHECK(c, kimb_find_token(scope, "dark", "y", &f) && f.value->number() == 1 && !kimb_find_token(scope, "light", "y", nullptr));
    KV_CHECK(c, !strcmp(kimb_first_theme(scope), "dark") && kimb_has_theme(scope, "light") && !kimb_has_theme(scope, "blue"));
    KV_CHECK(c, kimb_find_prop(scope, "size2", &f) && !kimb_find_prop(scope, "size", nullptr));
    // Resolving a value: a token first, then a literal; what is neither is refused.
    JsonDoc v;
    const char* why = nullptr;
    const char* vals = R"(["x", "#abc", "nope", 3])";
    KV_CHECK(c, json_parse(vals, strlen(vals), mem_scratch(), v));
    KV_CHECK(c, !strcmp(kimb_resolve_value(scope, "dark", KimbType::Color, *json_at(v, v.root(), 0), &why)->str(), "#fff"));
    KV_CHECK(c, !strcmp(kimb_resolve_value(scope, "light", KimbType::Color, *json_at(v, v.root(), 0), &why)->str(), "#111"));
    KV_CHECK(c, kimb_resolve_value(scope, "dark", KimbType::Color, *json_at(v, v.root(), 1), &why) == json_at(v, v.root(), 1));
    KV_CHECK(c, !kimb_resolve_value(scope, "dark", KimbType::Color, *json_at(v, v.root(), 2), &why) && why);
    KV_CHECK(c, !kimb_resolve_value(scope, "dark", KimbType::Number, *json_at(v, v.root(), 0), &why) && why); // x is a color in dark

    // A use that is not there; bytes that are not a .kimb; a version this does not read.
    KimbFile* lone = add("lone", R"({"kim": 1, "file": "lone.kim", "uses": ["gone"], "lines": [3]})");
    KimErrors e2;
    KV_CHECK(c, lone && !kimb_scope(scope, *lone, find, nullptr, e2) && e2.count == 1 && e2.list[0].line == 3);
    KimbFile bad;
    KimErrors e3;
    KV_CHECK(c, !kimb_read("xx", 2, mem_scratch(), bad, e3, "junk") && e3.count == 1);
    KV_CHECK(c, !add("v2", R"({"kim": 2, "file": "v2.kim"})"));

    // The kernel's words.
    KV_CHECK(c, kimb_kernel_node("hit") && !kimb_kernel_node("row") && kimb_kernel_flag("middle") && !kimb_kernel_flag("primary"));
    KV_CHECK(c, kimb_kernel_prop("border-color")->type == KimbType::Color && kimb_kernel_prop("w")->layout && !kimb_kernel_prop("bg")->layout);
    KV_CHECK(c, kimb_state("pressed") && !kimb_state("base"));
}
