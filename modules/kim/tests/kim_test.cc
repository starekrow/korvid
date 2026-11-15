#include "korvid/kim/kim.h"
#include "korvid/core/arena.h"
#include "korvid/core/kbor.h"
#include "korvid/core/test.h"
#include <string.h>

namespace {

using namespace kv;

const char* json_of(const JsonDoc& doc) {
    Array<u8> t{};
    json_write(doc, t, 0);
    char* s = (char*)alloc_in(mem_scratch(), t.len + 1, 1);
    memcpy(s, t.data, t.len);
    s[t.len] = 0;
    t.free();
    return s;
}

void show(const KimErrors& e) {
    for (u32 i = 0; i < e.count; ++i) {
        char buf[300];
        e.format_one(i, buf, sizeof buf);
        KV_INFO("test", "  %s", buf);
    }
}

// The compiled document as compact JSON, or "(errors)".
const char* doc_of(const char* src, u32 flags = 0) {
    JsonDoc doc;
    KimErrors e;
    if (!kim_parse(src, strlen(src), "t.kim", mem_scratch(), doc, e, flags)) { show(e); return "(errors)"; }
    return json_of(doc);
}

bool same(const char* got, const char* want) {
    if (!strcmp(got, want)) return true;
    KV_ERROR("test", "kim:\n  got  %s\n  want %s", got, want);
    return false;
}

Array<u8> bytes_of(const char* src, u32 flags = 0) {
    Array<u8> out{};
    KimErrors e;
    if (!kim_compile(src, strlen(src), "t.kim", out, e, flags)) show(e);
    return out;
}

bool same_bytes(Array<u8>& a, Array<u8>& b) {
    bool ok = a.len && a.len == b.len && memcmp(a.data, b.data, a.len) == 0;
    a.free();
    b.free();
    return ok;
}

} // namespace

void test_kim(kv::TestCtx& c) {
    c.begin("kim");
    using namespace kv;

    // A node with one of everything, and a block.
    KV_CHECK(c, same(doc_of("screen s { row fill gap=8 [a b] \"Hi {name}\" \"two\" 1..99 @on -> go id=r1 { text \"x\" } }"),
        R"({"kim":1,"file":"t.kim","screens":{"s":{"w":"row","id":"r1","f":["fill"],"p":{"gap":8},"s":["a","b"],"t":"Hi {name}","a":["two"],"r":[1,99],"b":"on","e":"go","c":[{"w":"text","t":"x"}]}},"lines":[1,1,1]})"));

    // Values: numbers, floats, negatives, colors, ranges, lists, words, strings.
    KV_CHECK(c, same(doc_of("screen s {\n box w=120..400 h=..60 x=-2 o=0.5 bg=#10131aff tint=#ffffff pad=14,8 r=4,8,4,8 bg2=panel2 font=\"text/a.ttf\" m=10.. \n}"),
        R"({"kim":1,"file":"t.kim","screens":{"s":{"w":"box","p":{"bg":"#10131aff","bg2":"panel2","font":"text/a.ttf","h":"..60","m":"10..","o":0.5,"pad":[14,8],"r":[4,8,4,8],"tint":"#ffffff","w":"120..400","x":-2}}},"lines":[1,2]})"));

    // Open-ended bare ranges.
    KV_CHECK(c, same(doc_of("screen s { slider ..10 }"), R"({"kim":1,"file":"t.kim","screens":{"s":{"w":"slider","r":[null,10]}},"lines":[1,1]})"));

    // Every top-level form, out of order, and the walk the lines take.
    const char* whole =
        "// a whole file\n"                                   // 1
        "use \"krome/std\"\n"                                 // 2
        "script \"game/form.lua\"\n"                          // 3
        "prop size number\n"                                  // 4
        "prop label string\n"                                 // 5
        "theme dark {\n"                                      // 6
        "    panel2=#202430 accent=#2f6fed\n"                 // 7
        "    space=8\n"                                       // 8
        "}\n"                                                 // 9
        "style button [base] {\n"                             // 10
        "    bg=panel2 radius=4 fill\n"                       // 11
        "    hover { bg=panel3 }\n"                           // 12
        "    disabled {\n"                                    // 13
        "        opacity=0.5\n"                               // 14
        "    }\n"                                             // 15
        "}\n"                                                 // 16
        "def field label value {\n"                           // 17
        "    row gap=8 {\n"                                   // 18
        "        text \"{label}\"\n"                          // 19
        "        slot\n"                                      // 20
        "    }\n"                                             // 21
        "}\n"                                                 // 22
        "screen form {\n"                                     // 23
        "    col [page] {\n"                                  // 24
        "        field \"Name\" \"{name}\"\n"                 // 25
        "        each item in assets {\n"                     // 26
        "            row -> pick { text \"{item.name}\" }\n"  // 27
        "        }\n"                                         // 28
        "        if dirty { badge \"unsaved\" }\n"            // 29
        "    }\n"                                             // 30
        "}\n";                                                // 31
    KV_CHECK(c, same(doc_of(whole),
        R"({"kim":1,"file":"t.kim","uses":["krome/std"],"scripts":["game/form.lua"],)"
        R"("props":[{"name":"label","type":"string"},{"name":"size","type":"number"}],)"
        R"("screens":{"form":{"w":"col","s":["page"],"c":[{"w":"field","t":"Name","a":["{name}"]},)"
        R"({"w":"each","v":"item","in":"assets","c":[{"w":"row","e":"pick","c":[{"w":"text","t":"{item.name}"}]}]},)"
        R"({"w":"if","on":"dirty","c":[{"w":"badge","t":"unsaved"}]}]}},)"
        R"("defs":{"field":{"params":["label","value"],"body":{"w":"row","p":{"gap":8},"c":[{"w":"text","t":"{label}"},{"w":"slot"}]}}},)"
        R"("styles":{"button":{"s":["base"],"f":["fill"],"p":{"bg":"panel2","radius":4},"looks":{"disabled":{"opacity":0.5},"hover":{"bg":"panel3"}}}},)"
        R"("themes":{"dark":{"accent":"#2f6fed","panel2":"#202430","space":8}},)"
        R"("lines":[2,3,5,4,23,24,25,26,27,27,29,29,17,18,19,20,10,13,12,6,7,7,8]})"));

    // Canonical: comments, spacing, property order and a repeated property change nothing.
    {
        Array<u8> a = bytes_of("screen s {\n  row gap=8 pad=4 {\n    text \"a\"\n  }\n}\n");
        Array<u8> b = bytes_of("// x\nscreen s { row  pad=2 gap=8   pad=4 {   // y\n text \"a\" } }");
        Array<u8> c2 = bytes_of("screen s {\n  row gap=8 pad=4 {\n    text \"a\"\n  }\n}\n", KV_KIM_NO_LINES);
        Array<u8> d = bytes_of("screen s { row pad=4 gap=8 { text \"a\" } }", KV_KIM_NO_LINES);
        // The two with lines differ only if their lines do: both are 1,2,3 against 2,2,2.
        KV_CHECK(c, a.len == b.len);
        a.free(); b.free();
        KV_CHECK(c, same_bytes(c2, d));
    }
    {
        Array<u8> a = bytes_of("screen s { row gap=8 pad=4 { text \"a\" } }");
        Array<u8> b = bytes_of("screen s { row pad=4 gap=8 { text \"a\" } }");
        KV_CHECK(c, same_bytes(a, b));
        // Styles and themes declared in another order, and tokens too.
        Array<u8> x = bytes_of("style b { p=1 }\nstyle a { q=2 }\ntheme t { y=1 x=2 }");
        Array<u8> y = bytes_of("theme t { x=2 y=1 }\nstyle a { q=2 }\nstyle b { p=1 }", KV_KIM_NO_LINES);
        Array<u8> z = bytes_of("style b { p=1 }\nstyle a { q=2 }\ntheme t { y=1 x=2 }", KV_KIM_NO_LINES);
        x.free();
        KV_CHECK(c, same_bytes(y, z));
    }

    // Stripping the lines changes no other byte: the doc without them is the doc with them
    // less its last member.
    {
        const char* with = doc_of(whole);
        const char* without = doc_of(whole, KV_KIM_NO_LINES);
        const char* cut = strstr(with, ",\"lines\":");
        KV_CHECK(c, cut && (usize)(cut - with) + 1 == strlen(without) && !strncmp(with, without, (usize)(cut - with)));
        // And the compiled bytes read back to the same document.
        Array<u8> k = bytes_of(whole);
        JsonDoc back;
        KV_CHECK(c, kbor_parse(k.data, k.len, mem_scratch(), back));
        KV_CHECK(c, same(json_of(back), with));
        k.free();
    }

    // A repeated look merges; a repeated token takes its last value with its line.
    KV_CHECK(c, same(doc_of("style b {\n hover { a=1 b=2 }\n hover { a=3 }\n}\ntheme t {\n x=1\n x=2\n}"),
        R"({"kim":1,"file":"t.kim","styles":{"b":{"looks":{"hover":{"a":3,"b":2}}}},"themes":{"t":{"x":2}},"lines":[1,2,5,7]})"));

    // Escapes; a string with a brace stays as written.
    KV_CHECK(c, same(doc_of("screen s { text \"a \\\"q\\\" \\\\ {{x}}\" }"),
        R"({"kim":1,"file":"t.kim","screens":{"s":{"w":"text","t":"a \"q\" \\ {{x}}"}},"lines":[1,1]})"));
}

void test_kim_errors(kv::TestCtx& c) {
    c.begin("kim_errors");
    using namespace kv;

    // The first error's line, column and text; and that nothing was compiled.
    auto first = [&](const char* src, u32 line, u32 col, const char* text) {
        JsonDoc doc;
        KimErrors e;
        bool ok = kim_parse(src, strlen(src), "t.kim", mem_scratch(), doc, e);
        bool good = !ok && e.count >= 1 && e.list[0].line == line && e.list[0].col == col && strstr(e.list[0].text, text)
            && doc.count == 1 && doc.root().is_null();
        if (!good) {
            KV_ERROR("test", "kim '%s': wanted %u:%u '%s'", src, line, col, text);
            show(e);
        }
        return good;
    };
    KV_CHECK(c, first("screen s {\n  row {\n    text \"a\"\n", 2, 0, "never closed"));
    KV_CHECK(c, first("screen s { text \"abc }", 1, 17, "unterminated string"));
    KV_CHECK(c, first("screen s { box bg=#12345 }", 1, 19, "#rrggbb"));
    KV_CHECK(c, first("screen s { box w=8px }", 1, 18, "not a number"));
    KV_CHECK(c, first("screen s { box 8 }", 1, 16, "bare range"));
    KV_CHECK(c, first("screen s {\n  each item assets { text \"x\" }\n}", 2, 3, "each item in list"));
    KV_CHECK(c, first("screen s {\n  if { text \"x\" }\n}", 2, 3, "if name"));
    KV_CHECK(c, first("screen s { row\n}\nscreen s { col }", 3, 0, "screen s is defined twice (first at line 1)"));
    KV_CHECK(c, first("style a { x=1 }\nstyle a { y=1 }", 2, 0, "style a is defined twice"));
    KV_CHECK(c, first("screen s { }", 1, 0, "holds one node, not 0"));
    KV_CHECK(c, first("screen s {\n row\n col\n}", 1, 0, "holds one node, not 2"));
    KV_CHECK(c, first("widget w { }", 1, 1, "is not use, script"));
    KV_CHECK(c, first("}", 1, 1, "nothing open"));
    KV_CHECK(c, first("use krome", 1, 5, "path in quotes"));
    KV_CHECK(c, first("use \"a\"\nuse \"a\"", 2, 0, "written twice"));
    KV_CHECK(c, first("def b x x { box }", 1, 9, "named twice"));
    KV_CHECK(c, first("theme t { a=1 b }", 1, 15, "expected name=value"));
    KV_CHECK(c, first("screen s { text \"a\\q\" }", 1, 19, "unknown escape"));
    KV_CHECK(c, first("screen s { row @ }", 1, 16, "binding is @name"));

    // Several errors in one file, each kept, and the parse goes on past them.
    {
        const char* src = "screen s {\n  col {\n    box bg=#1 w=1x\n    text \"ok\"\n    box 7\n  }\n}\n";
        JsonDoc doc;
        KimErrors e;
        KV_CHECK(c, !kim_parse(src, strlen(src), "t.kim", mem_scratch(), doc, e));
        KV_CHECK(c, e.count == 3 && e.list[0].line == 3 && e.list[1].line == 3 && e.list[2].line == 5);
        char buf[300];
        e.format_one(0, buf, sizeof buf);
        KV_CHECK(c, !strcmp(buf, "t.kim:3:12: a color is #rrggbb or #rrggbbaa (or #rgb, #rgba)"));
    }

    // Past the cap they are counted, not kept.
    {
        char src[4096] = "screen s { col {\n";
        for (int i = 0; i < 40; ++i) strcat(src, " box w=1x\n");
        strcat(src, "} }\n");
        JsonDoc doc;
        KimErrors e;
        KV_CHECK(c, !kim_parse(src, strlen(src), "t.kim", mem_scratch(), doc, e));
        KV_CHECK(c, e.count == KV_KIM_MAX_ERRORS && e.total == 40);
    }
}
