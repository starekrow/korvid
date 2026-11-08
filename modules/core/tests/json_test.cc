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
        "texture": "example_rocks/rock.png", "blend": "alpha",
        "tint": [1, 0.5, 0.25, 1], "params": [], "name": "a \"quoted\" \u00e9\n", "opaque": false, "nothing": null,
        "nested": {"n": -1.5e2, "list": [[1], {"k": true}]}
    })";
    JsonDoc doc;
    KV_CHECK(c, json_parse(text, strlen(text), mem_scratch(), doc) && doc.error == nullptr);
    const JsonValue& root = doc.root();
    KV_CHECK(c, root.is_object() && root.count == 8);
    KV_CHECK(c, strcmp(json_string(json_get(doc, root, "texture"), ""), "example_rocks/rock.png") == 0);
    KV_CHECK(c, strcmp(json_string(json_get(doc, root, "blend"), ""), "alpha") == 0);
    f32 tint[4] = {};
    KV_CHECK(c, json_numbers(doc, json_get(doc, root, "tint"), tint, 4) == 4 && tint[1] == 0.5f && tint[2] == 0.25f);
    KV_CHECK(c, json_numbers(doc, json_get(doc, root, "params"), tint, 4) == 0);
    KV_CHECK(c, json_numbers(doc, json_get(doc, root, "blend"), tint, 4) == 0); // not an array
    const JsonValue* name = json_get(doc, root, "name");
    KV_CHECK(c, name && name->is_string() && strcmp(name->str, "a \"quoted\" \xc3\xa9\n") == 0 && name->len == 14);
    KV_CHECK(c, json_bool(json_get(doc, root, "opaque"), true) == false && json_bool(json_get(doc, root, "tint"), true) == true);
    KV_CHECK(c, json_get(doc, root, "nothing")->is_null() && json_get(doc, root, "missing") == nullptr);
    const JsonValue* nested = json_get(doc, root, "nested");
    KV_CHECK(c, nested && json_number(json_get(doc, *nested, "n"), 0.0) == -150.0);
    const JsonValue* list = json_get(doc, *nested, "list");
    KV_CHECK(c, list && list->is_array() && list->count == 2);
    KV_CHECK(c, json_at(doc, *list, 0)->is_array() && json_number(json_first(doc, *json_at(doc, *list, 0)), 0.0) == 1.0);
    KV_CHECK(c, json_bool(json_get(doc, *json_at(doc, *list, 1), "k"), false) == true);
    KV_CHECK(c, json_at(doc, *list, 2) == nullptr && json_first(doc, *name) == nullptr);

    // Scalars at the top level, and whitespace around them.
    KV_CHECK(c, json_parse("  42 ", 5, mem_scratch(), doc) && doc.root().is_number() && doc.root().number == 42.0);
    KV_CHECK(c, json_parse("\"\\ud83d\\ude00\"", 14, mem_scratch(), doc) && doc.root().len == 4 && memcmp(doc.root().str, "\xf0\x9f\x98\x80", 4) == 0);

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
    KV_CHECK(c, json_parse(deep, 2, mem_scratch(), doc) && doc.root().is_array() && doc.root().count == 0);

    // Integers keep their exact value past 2^53; fractions and exponents are not integers.
    const char* ints = "[18446744073709551615, -9223372036854775808, 9007199254740993, 3.5, 1e3, 18446744073709551616, -2]";
    KV_CHECK(c, json_parse(ints, strlen(ints), mem_scratch(), doc));
    const JsonValue* iv[7];
    for (u32 i = 0; i < 7; ++i) iv[i] = json_at(doc, doc.root(), i);
    KV_CHECK(c, iv[0]->integer && json_u64(iv[0], 0) == ~0ull);
    KV_CHECK(c, iv[1]->integer && json_i64(iv[1], 0) == (i64)0x8000000000000000ull && json_u64(iv[1], 7) == 0);
    KV_CHECK(c, json_u64(iv[2], 0) == 9007199254740993ull && (u64)iv[2]->number != 9007199254740993ull);
    KV_CHECK(c, !iv[3]->integer && json_i64(iv[3], 0) == 3 && !iv[4]->integer && json_u64(iv[4], 0) == 1000);
    KV_CHECK(c, !iv[5]->integer && json_u64(iv[5], 0) == ~0ull); // past u64: the double, clamped
    KV_CHECK(c, json_i64(iv[6], 0) == -2 && json_u64(iv[6], 9) == 0 && json_i64(nullptr, 4) == 4);
}
