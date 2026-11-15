// The check (kimb_check) over .kim text: the parser's document, read as a .kimb is, in a
// scope of preludes also written as text.
#include "korvid/kim/kim.h"
#include "korvid/core/arena.h"
#include "korvid/core/test.h"
#include <stdio.h>
#include <string.h>

namespace {

using namespace kv;

struct Files {
    KimbFile    files[8];
    const char* names[8];
    u32         count = 0;
};
Files g_files;

const KimbFile* find(void*, const char* use) {
    for (u32 i = 0; i < g_files.count; ++i)
        if (!strcmp(g_files.names[i], use)) return &g_files.files[i];
    return nullptr;
}

KimbFile* add(const char* name, const char* src) {
    JsonDoc doc;
    KimErrors e;
    char file[64];
    snprintf(file, sizeof file, "%s.kim", name);
    char* keep = (char*)alloc_in(mem_scratch(), strlen(file) + 1, 1);
    memcpy(keep, file, strlen(file) + 1);
    if (!kim_parse(src, strlen(src), keep, mem_scratch(), doc, e)) {
        char buf[300];
        for (u32 i = 0; i < e.count; ++i) { e.format_one(i, buf, sizeof buf); KV_ERROR("test", "%s", buf); }
        return nullptr;
    }
    KimbFile& f = g_files.files[g_files.count];
    g_files.names[g_files.count++] = name;
    kimb_from_doc(doc, f, e, keep);
    return &f;
}

// The errors the check finds in `src` against the preludes added so far, one to a line.
const char* check(const char* src) {
    static char out[4096];
    out[0] = 0;
    KimbFile* f = add("t", src);
    if (!f) return "(did not parse)";
    --g_files.count; // not a prelude for the next
    KimErrors e;
    KimbScope s;
    kimb_scope(s, *f, find, nullptr, e);
    kimb_check(s, e);
    for (u32 i = 0; i < e.count; ++i) {
        char buf[300];
        e.format_one(i, buf, sizeof buf);
        strcat(out, buf);
        strcat(out, "\n");
    }
    return out;
}

bool same(const char* got, const char* want) {
    if (!strcmp(got, want)) return true;
    KV_ERROR("test", "check:\n--- got\n%s--- want\n%s", got, want);
    return false;
}

} // namespace

void test_kim_check(kv::TestCtx& c) {
    c.begin("kim_check");
    using namespace kv;
    g_files.count = 0;

    add("krome/std",
        "prop label string\n"                                  // 1
        "theme dark { panel=#202430 text=#e6e9ef space=8 }\n"  // 2
        "theme light { panel=#f0f0f0 text=#101010 space=8 }\n" // 3
        "style button { bg=panel pad=8,4 radius=4\n"           // 4
        "  hover { bg=#2f6fed }\n"                             // 5
        "}\n"                                                  // 6
        "def row { box dir=row {\n"                            // 7
        "    slot\n"                                           // 8
        "} }\n"                                                // 9
        "def button label { hit [button] { text \"{label}\" } }\n"); // 10
    add("game/extra", "use \"krome/std\"\ntheme dark { accent=#ff0000 }\n");

    // A file with nothing wrong: kernel nodes, definitions with children and parameters,
    // styles and tokens from a prelude, each and if.
    KV_CHECK(c, same(check(
        "use \"krome/std\"\n"
        "screen s {\n"
        "  row gap=space {\n"
        "    button \"Save\" -> save\n"
        "    button label=\"Later\" disabled\n"
        "    text \"{name}\" color=text\n"
        "    each item in list { text \"{item.name}\" }\n"
        "    if dirty { box bg=panel }\n"
        "  }\n"
        "}\n"), ""));

    // Each kind of mistake, at its line.
    KV_CHECK(c, same(check(
        "use \"krome/std\"\n"                      // 1
        "screen s {\n"                             // 2
        "  row {\n"                                // 3
        "    buton \"Save\"\n"                     // 4
        "    box [buttn] bg=panl\n"                // 5
        "    box colour=#fff fill frobbed\n"       // 6
        "    button \"a\" \"b\"\n"                 // 7
        "    button size=4\n"                      // 8: a kernel property, onto the body's root
        "    button { text \"x\" }\n"              // 9
        "    text \"a\" { box }\n"                 // 10
        "    box \"words\"\n"                      // 11
        "    box w=wide pad=1,2,3\n"               // 12
        "    slot\n"                               // 13
        "  }\n"
        "}\n"),
        "t.kim:4: 'buton' is neither a kernel node (box, text, image, hit) nor a definition\n"
        "t.kim:5: no style 'buttn'\n"
        "t.kim:5: bg: 'panl' is not a color, nor a token of every theme\n"
        "t.kim:6: 'frobbed' is not a flag\n"
        "t.kim:6: 'colour' is not a property\n"
        "t.kim:7: button takes 1 parameter, and is given 2 strings\n"
        "t.kim:9: button takes no children: its body has no slot\n"
        "t.kim:10: a text takes no children\n"
        "t.kim:11: a box takes no content: put a text in it\n"
        "t.kim:12: pad takes one, two or four numbers\n"
        "t.kim:12: w takes a number, hug, fill or a range\n"
        "t.kim:13: slot is only in a definition's body\n"));

    // A token must be in every theme: game/extra's dark has accent, light has none.
    KV_CHECK(c, same(check("use \"game/extra\"\nscreen s { box bg=accent }\n"),
        "t.kim:2: bg: the token 'accent' is not in theme light\n"));
    // A use that is not there, a theme's value of the wrong kind.
    KV_CHECK(c, same(check("use \"krome/nope\"\nscreen s { box }\n"), "t.kim:1: use \"krome/nope\": no such prelude\n"));
    KV_CHECK(c, same(check("use \"krome/std\"\nscreen s { box bg=space }\n"), "t.kim:2: bg: the token 'space' in theme dark is not a color\n"));

    // Styles: their own mistakes, looks that set layout, states that are not, cycles.
    KV_CHECK(c, same(check(
        "style a [b] { bg=#fff\n"                  // 1
        "  hover { pad=4 }\n"                      // 2
        "  hovered { bg=#000 }\n"                  // 3
        "}\n"
        "style b [a] { frob=1 }\n"),               // 5
        "t.kim:2: hover.pad is layout, which a look cannot set\n"
        "t.kim:3: 'hovered' is not a state (hover, pressed, focused, disabled, selected)\n"
        "t.kim:1: style a starts from itself\n"
        "t.kim:5: 'frob' is not a property\n"
        "t.kim:5: style b starts from itself\n"));

    // Declarations: a type that is not, a kernel property declared again, a kernel word defined.
    KV_CHECK(c, same(check("prop size number\nprop label strng\ndef box { text }\n"),
        "t.kim:2: prop label: 'strng' is not a type (number, color, string, size, any)\n"
        "t.kim:1: prop size: the kernel's property, which a prelude does not declare again\n"
        "t.kim:3: def box: a kernel word, which a definition cannot take\n"));

    // A typed parameter by name.
    KV_CHECK(c, same(check("use \"krome/std\"\nscreen s { button label=4 }\n"), "t.kim:2: label takes a string\n"));
}
