#include "korvid/core/arena.h"
#include "korvid/core/json.h"
#include "korvid/core/kbor.h"
#include "korvid/core/memory.h"
#include "korvid/core/test.h"
#include <string.h>

namespace {

using namespace kv;

// The doc as compact JSON, NUL-terminated in the scratch arena.
const char* text_of(const JsonDoc& doc) {
    Array<u8> t{};
    json_write(doc, t, 0);
    char* s = (char*)alloc_in(mem_scratch(), t.len + 1, 1);
    memcpy(s, t.data, t.len);
    s[t.len] = 0;
    t.free();
    return s;
}

bool same(const char* got, const char* want) {
    if (!strcmp(got, want)) return true;
    KV_ERROR("test", "json_build: got %s, wanted %s", got, want);
    return false;
}

} // namespace

void test_json_build(kv::TestCtx& c) {
    c.begin("json_build");
    using namespace kv;

    // Built out of order: children made before their parents, a nested object attached
    // after its siblings, an empty array and object.
    {
        JsonBuilder b(mem_scratch());
        JsonRef kids = b.array();
        JsonRef name = b.string("Rowan");
        JsonRef o = b.object();
        JsonRef inner = b.object();
        b.set(inner, "deep", b.boolean(true));
        b.push(kids, b.integer(-3));
        b.push(kids, b.number(1.5));
        b.push(kids, b.null());
        b.push(kids, b.array());
        b.set(o, "name", name);
        b.set(o, "kids", kids);
        b.set(o, "big", b.unsigned_integer(18446744073709551615ull));
        b.set(o, "empty", b.object());
        b.set(o, "inner", inner);
        b.set(o, "one", b.number(1.0));
        b.line(name, 7);
        JsonDoc doc;
        KV_CHECK(c, b.finish(o, doc));
        KV_CHECK(c, doc.count == 12);
        KV_CHECK(c, same(text_of(doc), R"({"name":"Rowan","kids":[-3,1.5,null,[]],"big":18446744073709551615,"empty":{},"inner":{"deep":true},"one":1.0})"));
        const JsonValue* n = json_get(doc, doc.root(), "name");
        KV_CHECK(c, n && n->line == 7 && n->len() == 5);
        // Through KBOR and back: the same text, so the layout is a parser's.
        Array<u8> k{};
        kbor_write(doc, k);
        JsonDoc back;
        KV_CHECK(c, kbor_parse(k.data, k.len, mem_scratch(), back));
        KV_CHECK(c, same(text_of(back), text_of(doc)));
        // And the same bytes as KBOR written from the parsed text.
        JsonDoc parsed;
        const char* t = text_of(doc);
        KV_CHECK(c, json_parse(t, strlen(t), mem_scratch(), parsed));
        Array<u8> k2{};
        kbor_write(parsed, k2);
        KV_CHECK(c, k.len == k2.len && memcmp(k.data, k2.data, k.len) == 0);
        k.free();
        k2.free();
        b.free();
    }

    // Bytes, and a string with a NUL inside.
    {
        JsonBuilder b(mem_scratch());
        JsonRef a = b.array();
        const u8 raw[3] = {0, 255, 7};
        b.push(a, b.bytes(raw, 3));
        b.push(a, b.string("a\0b", 3));
        JsonDoc doc;
        KV_CHECK(c, b.finish(a, doc));
        KV_CHECK(c, doc.nodes[1].is_bytes() && doc.nodes[1].len() == 3 && doc.nodes[1].bytes()[1] == 255);
        KV_CHECK(c, doc.nodes[2].len() == 3);
        b.free();
    }

    // Refusals: attached twice, into a scalar, a value into itself.
    {
        JsonBuilder b(mem_scratch());
        JsonRef a = b.array();
        JsonRef v = b.integer(1);
        KV_CHECK(c, b.push(a, v));
        KV_CHECK(c, !b.push(a, v));
        JsonDoc doc;
        KV_CHECK(c, !b.finish(a, doc) && doc.error && doc.count == 1 && doc.root().is_null());
        b.reset();
        JsonRef s = b.string("x");
        KV_CHECK(c, !b.push(s, b.null()));
        b.reset();
        JsonRef self = b.array();
        KV_CHECK(c, !b.push(self, self));
        b.free();
    }
}
