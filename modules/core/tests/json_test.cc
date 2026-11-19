#include "korvid/core/json.h"
#include "korvid/core/arena.h"
#include "korvid/core/test.h"
#include <string.h>

void test_json(kv::TestCtx& c) {
    using namespace kv;
    c.begin("json");
    MemScope scope;

    // A material file's worth of structure, with every value kind and some escapes.
    const char* text = R"({
        "texture": "rocks/rock.png", "blend": "alpha",
        "tint": [1, 0.5, 0.25, 1], "params": [], "name": "a \"quoted\" \u00e9\n", "opaque": false, "nothing": null,
        "nested": {"n": -1.5e2, "list": [[1], {"k": true}]}
    })";
    JsonDoc doc;
    KV_CHECK(c, json_parse(text, strlen(text), mem_scratch(), doc) && doc.error == nullptr);
    const JsonValue& root = doc.root();
    KV_CHECK(c, root.is_object() && root.count() == 8);
    KV_CHECK(c, strcmp(json_string(json_get(doc, root, "texture"), ""), "rocks/rock.png") == 0);
    KV_CHECK(c, strcmp(json_string(json_get(doc, root, "blend"), ""), "alpha") == 0);
    f32 tint[4] = {};
    KV_CHECK(c, json_numbers(doc, json_get(doc, root, "tint"), tint, 4) == 4 && tint[1] == 0.5f && tint[2] == 0.25f);
    KV_CHECK(c, json_numbers(doc, json_get(doc, root, "params"), tint, 4) == 0);
    KV_CHECK(c, json_numbers(doc, json_get(doc, root, "blend"), tint, 4) == 0); // not an array
    const JsonValue* name = json_get(doc, root, "name");
    KV_CHECK(c, name && name->is_string() && strcmp(name->str(), "a \"quoted\" \xc3\xa9\n") == 0 && name->len() == 14);
    KV_CHECK(c, json_bool(json_get(doc, root, "opaque"), true) == false && json_bool(json_get(doc, root, "tint"), true) == true);
    KV_CHECK(c, json_get(doc, root, "nothing")->is_null() && json_get(doc, root, "missing") == nullptr);
    const JsonValue* nested = json_get(doc, root, "nested");
    KV_CHECK(c, nested && json_number(json_get(doc, *nested, "n"), 0.0) == -150.0);
    const JsonValue* list = json_get(doc, *nested, "list");
    KV_CHECK(c, list && list->is_array() && list->count() == 2);
    KV_CHECK(c, json_at(doc, *list, 0)->is_array() && json_number(json_first(doc, *json_at(doc, *list, 0)), 0.0) == 1.0);
    KV_CHECK(c, json_bool(json_get(doc, *json_at(doc, *list, 1), "k"), false) == true);
    KV_CHECK(c, json_at(doc, *list, 2) == nullptr && json_first(doc, *name) == nullptr);

    // Scalars at the top level, and whitespace around them.
    KV_CHECK(c, json_parse("  42 ", 5, mem_scratch(), doc) && doc.root().is_number() && doc.root().number() == 42.0);
    KV_CHECK(c, json_parse("\"\\ud83d\\ude00\"", 14, mem_scratch(), doc) && doc.root().len() == 4 && memcmp(doc.root().str(), "\xf0\x9f\x98\x80", 4) == 0);

    // Errors name the offset.
    struct Bad { const char* text; u32 at; };
    const Bad bad[] = {
        {"{\"a\": 1,}", 8},     // trailing comma
        {"[1 2]", 3},           // missing comma
        {"{\"a\" 1}", 5},       // missing colon
        {"\"abc", 0},           // unterminated string
        {"tru", 0},             // bad literal
        {"[1] x", 4},           // text after the value
        {"{\"a\": 01}", 7},     // leading zero: the number ends, then 1 is unexpected
        {"\"\\x\"", 1},         // unknown escape
        {"", 0},                // nothing
    };
    for (const Bad& b : bad) {
        bool ok = json_parse(b.text, strlen(b.text), mem_scratch(), doc);
        KV_CHECK(c, !ok && doc.error != nullptr && doc.error_at == b.at);
        if (ok || doc.error_at != b.at) KV_ERROR("test", "json '%s': %s at %u", b.text, doc.error ? doc.error : "(ok)", doc.error_at);
    }

    // Depth is bounded.
    char deep[2 * (KV_JSON_MAX_DEPTH + 2) + 1];
    for (u32 i = 0; i < KV_JSON_MAX_DEPTH + 2; ++i) { deep[i] = '['; deep[KV_JSON_MAX_DEPTH + 2 + i] = ']'; }
    deep[sizeof deep - 1] = 0;
    KV_CHECK(c, !json_parse(deep, strlen(deep), mem_scratch(), doc) && strcmp(doc.error, "nesting too deep") == 0);
    deep[1] = ']'; deep[2] = 0; // "[]"
    KV_CHECK(c, json_parse(deep, 2, mem_scratch(), doc) && doc.root().is_array() && doc.root().count() == 0);

    // Integers keep their exact value past 2^53; fractions and exponents are not integers.
    const char* ints = "[18446744073709551615, -9223372036854775808, 9007199254740993, 3.5, 1e3, 18446744073709551616, -2]";
    KV_CHECK(c, json_parse(ints, strlen(ints), mem_scratch(), doc));
    const JsonValue* iv[7];
    for (u32 i = 0; i < 7; ++i) iv[i] = json_at(doc, doc.root(), i);
    KV_CHECK(c, iv[0]->integer() && json_u64(iv[0], 0) == ~0ull);
    KV_CHECK(c, iv[1]->integer() && json_i64(iv[1], 0) == (-0x7FFFFFFFFFFFFFFFll - 1) && json_u64(iv[1], 7) == 0);
    KV_CHECK(c, json_u64(iv[2], 0) == 9007199254740993ull && (u64)iv[2]->number() != 9007199254740993ull);
    KV_CHECK(c, !iv[3]->integer() && json_i64(iv[3], 0) == 3 && !iv[4]->integer() && json_u64(iv[4], 0) == 1000);
    KV_CHECK(c, !iv[5]->integer() && json_u64(iv[5], 0) == ~0ull); // past u64: the double, clamped
    KV_CHECK(c, json_i64(iv[6], 0) == -2 && json_u64(iv[6], 9) == 0 && json_i64(nullptr, 4) == 4);

    // An integer reads as the number strtod would have made of it, sign of zero and all.
    const char* nums = "[-0, 9007199254740993, -9007199254740993, 18446744073709551615, -7, 0, true]";
    KV_CHECK(c, json_parse(nums, strlen(nums), mem_scratch(), doc));
    const f64 want[7] = {-0.0, 9007199254740993.0, -9007199254740993.0, 18446744073709551615.0, -7.0, 0.0, 1.0};
    for (u32 i = 0; i < 7; ++i) {
        f64 got = json_at(doc, doc.root(), i)->number();
        KV_CHECK(c, memcmp(&got, &want[i], sizeof got) == 0);
    }
    KV_CHECK(c, json_number(json_at(doc, doc.root(), 6), 5.0) == 5.0 && json_at(doc, doc.root(), 0)->integer());

    // The nodes: 24 bytes each, exactly as many as there are values, a container's children
    // side by side in the order written, and every accessor safe on every type.
    KV_CHECK(c, sizeof(JsonValue) == 24);
    const char* shape = "{\"a\": [1, [2, 3], {\"k\": \"v\"}], \"b\": \"x\\u0000y\", \"c\": {}}";
    KV_CHECK(c, json_parse(shape, strlen(shape), mem_scratch(), doc) && doc.count == 10);
    const JsonValue* sa = json_get(doc, doc.root(), "a");
    const JsonValue* sb = json_get(doc, doc.root(), "b");
    const JsonValue* sc = json_get(doc, doc.root(), "c");
    KV_CHECK(c, sa && sb == sa + 1 && sc == sb + 1 && json_next(doc, *sa) == sb && json_next(doc, *sc) == nullptr && json_next(doc, doc.root()) == nullptr);
    KV_CHECK(c, json_at(doc, *sa, 1) == json_first(doc, *sa) + 1 && json_at(doc, *sa, 2)->is_object() && json_at(doc, *sa, 3) == nullptr);
    u32 walked = 0;
    for (const JsonValue* v = json_first(doc, *sa); v; v = json_next(doc, *v)) ++walked;
    KV_CHECK(c, walked == 3 && json_number(json_at(doc, *json_at(doc, *sa, 1), 1), 0.0) == 3.0);
    KV_CHECK(c, sb->len() == 3 && memcmp(sb->str(), "x\0y", 4) == 0); // the length is kept, not measured
    KV_CHECK(c, sc->count() == 0 && json_first(doc, *sc) == nullptr && sc->str() == nullptr && sc->len() == 0 && sc->number() == 0.0 && !sc->integer());
    KV_CHECK(c, sb->count() == 0 && sb->number() == 0.0 && sb->bits() == 0 && json_first(doc, *sb) == nullptr);

    // One allocation for the nodes, of exactly their size, and one per string: nothing grows.
    struct Tally { Allocator a; u32 calls = 0; usize node_bytes = 0; };
    Tally tally;
    tally.a.alloc = [](Allocator* a, usize size, u32 align) {
        Tally* t = (Tally*)a;
        ++t->calls;
        if (align >= alignof(JsonValue) && size % sizeof(JsonValue) == 0) t->node_bytes += size;
        return alloc_in(mem_scratch(), size, align);
    };
    tally.a.free = [](Allocator*, void*, usize) {};
    KV_CHECK(c, json_parse(shape, strlen(shape), &tally.a, doc) && doc.count == 10);
    KV_CHECK(c, tally.calls == 1 + 4 + 2 && tally.node_bytes == sizeof(JsonValue) * 10); // the block, four names, two strings
    // Commas and brackets in strings and comments are not values; empty containers have no children.
    const char* tricky = "// a, b, [\n[\"a,b[{\", \"q\\\",\", [ // , ,\n ], { }, [[]]] // , [";
    tally = Tally{tally.a};
    KV_CHECK(c, json_parse(tricky, strlen(tricky), &tally.a, doc, KV_JSON_LINE_COMMENTS) && doc.count == 7 && tally.node_bytes == sizeof(JsonValue) * 7);
    KV_CHECK(c, doc.root().count() == 5 && strcmp(json_string(json_at(doc, doc.root(), 1), ""), "q\",") == 0 && json_at(doc, *json_at(doc, doc.root(), 4), 0)->count() == 0);
    // Wide and deep at once, so the stack at the back and the document at the front meet in the middle.
    const char* wide = "[[1,2,3,[4,5,[6,7],8],9],[10,[11,12]],13,{\"a\":[14,15],\"b\":16}]";
    KV_CHECK(c, json_parse(wide, strlen(wide), mem_scratch(), doc) && doc.count == 24);
    u32 seen = 0; f64 sum = 0;
    struct Walk { static void go(const JsonDoc& d, const JsonValue& v, u32& seen, f64& sum) {
        if (v.is_number()) { ++seen; if (v.number() != (f64)seen) sum = -1e9; sum += v.number(); }
        for (const JsonValue* k = json_first(d, v); k; k = json_next(d, *k)) go(d, *k, seen, sum);
    } };
    Walk::go(doc, doc.root(), seen, sum);
    KV_CHECK(c, seen == 16 && sum == 136.0); // every number, in the order written

    // A failed parse still has a root to ask, and nothing under it.
    KV_CHECK(c, !json_parse("[1, [2, 3], oops]", 17, mem_scratch(), doc) && doc.count == 1 && doc.root().is_null() && json_first(doc, doc.root()) == nullptr);

    // Lines: every node knows where it began, a member by its name; an error knows too.
    const char* lined = "{\n  \"meta\": { \"type\": \"sprite\" },\n  \"rules\": [\n    { \"match\": \"*.png\",\n      \"meta\":\n        { \"cook\": false } }\n  ]\n}";
    KV_CHECK(c, json_parse(lined, strlen(lined), mem_scratch(), doc) && doc.error_line == 0 && doc.root().line == 1);
    KV_CHECK(c, json_get(doc, doc.root(), "meta")->line == 2 && json_get(doc, *json_get(doc, doc.root(), "meta"), "type")->line == 2);
    const JsonValue* rules = json_get(doc, doc.root(), "rules");
    KV_CHECK(c, rules->line == 3 && json_at(doc, *rules, 0)->line == 4);
    const JsonValue* rmeta = json_get(doc, *json_at(doc, *rules, 0), "meta");
    KV_CHECK(c, rmeta->line == 5 && json_get(doc, *rmeta, "cook")->line == 6);
    const char* broken = "{\n  \"a\": 1,\n  \"b\" 2\n}";
    KV_CHECK(c, !json_parse(broken, strlen(broken), mem_scratch(), doc) && doc.error_line == 3);
    KV_CHECK(c, !json_parse("[1,\r\n2,\r\n]", 10, mem_scratch(), doc) && doc.error_line == 3);

    // Line comments, only when asked for; lines still count through them, and a // inside
    // a string is text.
    const char* commented = "// a rule file\n{\n  \"url\": \"http://x\", // why\n  // between members\n  \"n\": 2\n}\n// the end";
    KV_CHECK(c, !json_parse(commented, strlen(commented), mem_scratch(), doc));
    KV_CHECK(c, json_parse(commented, strlen(commented), mem_scratch(), doc, KV_JSON_LINE_COMMENTS) && doc.root().count() == 2);
    KV_CHECK(c, strcmp(json_string(json_get(doc, doc.root(), "url"), ""), "http://x") == 0 && json_get(doc, doc.root(), "n")->line == 5);
    KV_CHECK(c, !json_parse("[1, / 2]", 8, mem_scratch(), doc, KV_JSON_LINE_COMMENTS) && !json_parse("[1] /", 5, mem_scratch(), doc, KV_JSON_LINE_COMMENTS));

    // json_write's leaves on their own: the escapes, a float's spelling, and an f32 that
    // reads back through json_parse (strtod, then narrowed) to the same bits.
    Array<u8> out{};
    auto wrote = [&](const char* expect) {
        bool ok = out.len == strlen(expect) && memcmp(out.data, expect, out.len) == 0;
        out.clear();
        return ok;
    };
    const char* raw = "q\"\\\n\r\t\b\f\x01 \xc3\xa9";
    json_write_string(out, raw, strlen(raw));
    KV_CHECK(c, wrote("\"q\\\"\\\\\\n\\r\\t\\b\\f\\u0001 \xc3\xa9\""));
    json_write_f64(out, 1.0);    KV_CHECK(c, wrote("1.0"));
    json_write_f64(out, 0.1);    KV_CHECK(c, wrote("0.1"));
    json_write_f64(out, -0.0);   KV_CHECK(c, wrote("-0.0"));
    u64 inf_bits = 0x7ff0000000000000ull;
    f64 inf;
    memcpy(&inf, &inf_bits, 8);
    json_write_f64(out, -inf);   KV_CHECK(c, wrote("\"-inf\""));
    json_write_f32(out, 0.15f);  KV_CHECK(c, wrote("0.15"));
    json_write_f32(out, -2.0f);  KV_CHECK(c, wrote("-2.0"));
    json_write_f64(out, 0.15f);  KV_CHECK(c, wrote("0.15000000596046448"));
    u32 bits = 0x15ae43fd; // 7.038531e-26 reads back as this f32 by strtof, not by strtod
    f32 narrowing;
    memcpy(&narrowing, &bits, 4);
    json_write_f32(out, narrowing);
    KV_CHECK(c, json_parse((const char*)out.data, out.len, mem_scratch(), doc) && (f32)doc.root().number() == narrowing);
    out.free();
}
