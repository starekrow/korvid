#include "korvid/core/kbor.h"
#include "korvid/core/arena.h"
#include "korvid/core/base64.h"
#include "korvid/core/format.h"
#include "korvid/core/test.h"
#include <string.h>

namespace {

using namespace kv;

// Bytes from hex pairs, with anything else (spaces) between them ignored, and 'text' runs
// copied as they are: "A2 'name' 61".
struct Bytes {
    u8  b[512];
    u32 n = 0;
};
Bytes hex(const char* s) {
    Bytes out;
    auto digit = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    while (*s) {
        if (*s == '\'') {
            for (++s; *s && *s != '\''; ++s) out.b[out.n++] = (u8)*s;
            if (*s) ++s;
            continue;
        }
        int a = digit(s[0]);
        if (a < 0) { ++s; continue; }
        out.b[out.n++] = (u8)(a * 16 + digit(s[1]));
        s += 2;
    }
    return out;
}

bool parse(const Bytes& b, JsonDoc& doc, KborInfo* info = nullptr) { return kbor_parse(b.b, b.n, mem_scratch(), doc, info); }

// Parses and expects failure with this message.
bool refused(const char* bytes, const char* why) {
    JsonDoc doc;
    Bytes b = hex(bytes);
    bool ok = parse(b, doc);
    if (ok || !doc.error || strcmp(doc.error, why) != 0) {
        KV_ERROR("test", "kbor '%s': %s, wanted '%s'", bytes, ok ? "parsed" : doc.error, why);
        return false;
    }
    return doc.count == 1 && doc.root().is_null();
}

bool same_bytes(const Array<u8>& a, const Bytes& b) {
    if (a.len != b.n || memcmp(a.data, b.b, b.n) != 0) {
        char got[1200] = {};
        u32 at = 0;
        for (u32 i = 0; i < a.len && at + 4 < sizeof got; ++i) at += (u32)format(got + at, sizeof got - at, "%02X ", a.data[i]);
        KV_ERROR("test", "kbor bytes: got %s", got);
        return false;
    }
    return true;
}

// JSON text in; KBOR; back to JSON text (pretty unless told), as a NUL-terminated string
// in the scratch arena.
const char* round_trip(const char* json, u32 flags = KV_JSON_WRITE_PRETTY, KborInfo* info = nullptr) {
    JsonDoc a, b;
    if (!json_parse(json, strlen(json), mem_scratch(), a)) return "(json did not parse)";
    Array<u8> k{};
    kbor_write(a, k);
    bool ok = kbor_parse(k.data, k.len, mem_scratch(), b, info);
    k.free();
    if (!ok) return b.error;
    Array<u8> t{};
    json_write(b, t, flags);
    char* s = (char*)alloc_in(mem_scratch(), t.len + 1, 1);
    memcpy(s, t.data, t.len);
    s[t.len] = 0;
    t.free();
    return s;
}

bool text_is(const char* got, const char* want) {
    if (strcmp(got, want) == 0) return true;
    KV_ERROR("test", "kbor text:\n%s\nwanted:\n%s", got, want);
    return false;
}

void test_base64(TestCtx& c) {
    // RFC 4648's vectors.
    const char* plain[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    const char* coded[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    for (u32 i = 0; i < 7; ++i) {
        char out[16] = {};
        usize n = strlen(plain[i]);
        base64_encode(plain[i], n, out);
        KV_CHECK(c, base64_encoded_len(n) == strlen(coded[i]) && strcmp(out, coded[i]) == 0);
        u8 back[16];
        usize len = 0;
        KV_CHECK(c, base64_decode(coded[i], strlen(coded[i]), back, &len) && len == n && memcmp(back, plain[i], n) == 0);
        // Unpadded reads the same.
        usize m = strlen(coded[i]);
        while (m && coded[i][m - 1] == '=') --m;
        KV_CHECK(c, base64_decode(coded[i], m, back, &len) && len == n && memcmp(back, plain[i], n) == 0);
    }
    // Both alphabets, never mixed in one text.
    const u8 high[] = {0xFB, 0xFF, 0xBF};
    char std_text[8] = {};
    base64_encode(high, 3, std_text);
    KV_CHECK(c, strcmp(std_text, "+/+/") == 0);
    u8 back[8];
    usize len = 0;
    KV_CHECK(c, base64_decode("-_-_", 4, back, &len) && len == 3 && memcmp(back, high, 3) == 0);
    KV_CHECK(c, !base64_decode("+_-/", 4, back, &len));
    KV_CHECK(c, !base64_decode("Zm9v!", 5, back, &len));   // not base64
    KV_CHECK(c, !base64_decode("Z", 1, back, &len));       // a lone character
    KV_CHECK(c, !base64_decode("Zh==", 4, back, &len));    // bits left over
    KV_CHECK(c, !base64_decode("Zg=", 3, back, &len));     // padding short
    KV_CHECK(c, !base64_decode("Zm9v=", 5, back, &len));   // padding where none belongs
    KV_CHECK(c, !base64_decode("Zg==Zg==", 8, back, &len)); // padding in the middle
}

void test_reading(TestCtx& c) {
    // The design's example: the second object names its keys by index.
    JsonDoc doc;
    KborInfo info;
    Bytes ex = hex("82 A2 64'name' 61'a' 62'hp' 01 A2 00 61'b' 01 02");
    KV_CHECK(c, parse(ex, doc, &info) && doc.error == nullptr);
    const JsonValue& root = doc.root();
    KV_CHECK(c, root.is_array() && root.count() == 2 && doc.count == 7);
    const JsonValue* second = json_at(doc, root, 1);
    KV_CHECK(c, second && strcmp(json_string(json_get(doc, *second, "name"), ""), "b") == 0 && json_i64(json_get(doc, *second, "hp"), 0) == 2);
    KV_CHECK(c, json_get(doc, *json_at(doc, root, 0), "name")->key == json_get(doc, *second, "name")->key); // one copy of the name
    KV_CHECK(c, !info.summarized && !info.indefinite && info.index_keys == 2 && info.names == 2);
    KV_CHECK(c, info.counts.nodes == 7 && info.counts.keys == 2 && info.counts.strings == 2 && info.counts.bytes == 8);

    // A stock encoder's CBOR: no indices, a name in full every time, indefinite lengths.
    // Every text key appends, so index 2 is the third text key ("name" again).
    Bytes stock = hex("9F A2 64'name' 61'a' 62'hp' 01 BF 64'name' 61'b' 62'hp' 02 FF A1 02 F6 FF");
    KV_CHECK(c, parse(stock, doc, &info) && doc.root().count() == 3);
    KV_CHECK(c, info.indefinite && info.counts.keys == 4 && info.names == 2 && info.index_keys == 1);
    KV_CHECK(c, json_get(doc, *json_at(doc, doc.root(), 2), "name") && json_get(doc, *json_at(doc, doc.root(), 2), "name")->is_null());
    KV_CHECK(c, json_get(doc, *json_at(doc, doc.root(), 1), "hp")->as.bits == 2);

    // Integers, exact across the range, in every head size.
    Bytes ints = hex("8B 00 17 18 18 18 FF 19 0100 1A 00010000 1B FFFFFFFFFFFFFFFF 20 37 38 18 3B 7FFFFFFFFFFFFFFF");
    KV_CHECK(c, parse(ints, doc));
    const u64 want_u[] = {0, 23, 24, 255, 256, 65536, ~0ull};
    for (u32 i = 0; i < 7; ++i) KV_CHECK(c, json_at(doc, doc.root(), i)->integer() && json_at(doc, doc.root(), i)->bits() == want_u[i]);
    const i64 want_i[] = {-1, -24, -25, (i64)0x8000000000000000ull};
    for (u32 i = 0; i < 4; ++i) KV_CHECK(c, json_i64(json_at(doc, doc.root(), 7 + i), 0) == want_i[i] && json_at(doc, doc.root(), 7 + i)->number() < 0);

    // Floats: half (one, the smallest subnormal, infinity, NaN), single, double.
    Bytes floats = hex("87 F9 3C00 F9 0001 F9 7C00 F9 7E00 F9 8000 FA 3FC00000 FB 3FB999999999999A");
    KV_CHECK(c, parse(floats, doc));
    const JsonValue* f = json_first(doc, doc.root());
    KV_CHECK(c, !f->integer() && f->number() == 1.0);
    f = json_next(doc, *f); KV_CHECK(c, f->number() == 5.9604644775390625e-8);
    f = json_next(doc, *f); KV_CHECK(c, f->number() > 1e308);
    f = json_next(doc, *f); KV_CHECK(c, f->number() != f->number());
    f = json_next(doc, *f); KV_CHECK(c, f->number() == 0.0 && (f->as.bits >> 63) == 1); // -0
    f = json_next(doc, *f); KV_CHECK(c, f->number() == 1.5);
    f = json_next(doc, *f); KV_CHECK(c, f->number() == 0.1);

    // The other scalars, byte strings, and an empty array and map.
    Bytes misc = hex("88 F4 F5 F6 43 00FF10 40 60 80 A0");
    KV_CHECK(c, parse(misc, doc));
    f = json_first(doc, doc.root());
    KV_CHECK(c, f->is_bool() && !json_bool(f, true));
    f = json_next(doc, *f); KV_CHECK(c, json_bool(f, false));
    f = json_next(doc, *f); KV_CHECK(c, f->is_null());
    f = json_next(doc, *f); KV_CHECK(c, f->is_bytes() && f->len() == 3 && f->bytes()[1] == 0xFF && f->str() == nullptr && json_string(f, nullptr) == nullptr);
    f = json_next(doc, *f); KV_CHECK(c, f->is_bytes() && f->len() == 0);
    f = json_next(doc, *f); KV_CHECK(c, f->is_string() && f->len() == 0 && f->str()[0] == 0);
    f = json_next(doc, *f); KV_CHECK(c, f->is_array() && f->count() == 0);
    f = json_next(doc, *f); KV_CHECK(c, f->is_object() && f->count() == 0 && !json_next(doc, *f));
    KV_CHECK(c, parse(hex("01"), doc) && doc.count == 1 && doc.root().bits() == 1); // a scalar file
}

void test_errors(TestCtx& c) {
    KV_CHECK(c, refused("", "truncated"));
    KV_CHECK(c, refused("82 01", "a length past the end"));
    KV_CHECK(c, refused("19 01", "truncated"));
    KV_CHECK(c, refused("63 'ab'", "a string past the end"));
    KV_CHECK(c, refused("9B 00000000FFFFFFFF", "a length past the end"));
    KV_CHECK(c, refused("A1 00 01", "a key index past the table"));
    KV_CHECK(c, refused("A2 61'a' 01 01 02", "a key index past the table")); // an index refers backwards only
    KV_CHECK(c, refused("A1 20 01", "a map key that is neither text nor an index"));
    KV_CHECK(c, refused("A1 41'a' 01", "a map key that is neither text nor an index"));
    KV_CHECK(c, refused("A1 F9 3C00 01", "a map key that is neither text nor an index"));
    KV_CHECK(c, refused("F7", "undefined, which JSON cannot say"));
    KV_CHECK(c, refused("E0", "a simple value JSON cannot say"));
    KV_CHECK(c, refused("F8 20", "a simple value JSON cannot say"));
    KV_CHECK(c, refused("FF", "a break outside an indefinite-length container"));
    KV_CHECK(c, refused("82 01 FF", "a break outside an indefinite-length container"));
    KV_CHECK(c, refused("7F 61'a' FF", "an indefinite-length string"));
    KV_CHECK(c, refused("82 C1 01 02", "a tag inside the document"));
    KV_CHECK(c, refused("C1 01", "a tag that is not the summary's"));
    KV_CHECK(c, refused("01 02", "bytes after the document"));
    KV_CHECK(c, refused("3B 8000000000000000", "an integer below -2^63"));
    KV_CHECK(c, refused("62 C328", "a string that is not UTF-8"));
    KV_CHECK(c, refused("A1 62 C328 01", "a key that is not UTF-8"));
    KV_CHECK(c, refused("A1 63 61 00 62 01", "a key holding a NUL"));
    KV_CHECK(c, refused("1C", "a reserved additional information value"));
    KV_CHECK(c, refused("9F 01", "truncated"));
    {   // Nesting: 64 levels read, 65 do not.
        char deep[400] = {};
        for (u32 i = 0; i < 64; ++i) strcat(deep, "81 ");
        strcat(deep, "01");
        JsonDoc doc;
        KV_CHECK(c, parse(hex(deep), doc));
        char deeper[400] = "81 ";
        strcat(deeper, deep);
        KV_CHECK(c, refused(deeper, "nesting too deep"));
    }

    // The summary: what it says must be what is there, in each count and either way.
    const char* body = "82 A2 64'name' 61'a' 62'hp' 01 A2 00 61'b' 01 02";
    char text[300];
    format(text, sizeof text, "D9 CB0B 82 84 07 02 02 08 %s", body);
    JsonDoc doc;
    KborInfo info;
    KV_CHECK(c, parse(hex(text), doc, &info) && info.summarized && doc.count == 7);
    format(text, sizeof text, "D9 CB0B 82 85 07 02 02 08 19 1234 %s", body); // a later count, ignored
    KV_CHECK(c, parse(hex(text), doc, &info) && info.summarized);
    format(text, sizeof text, "D9 CB0B 82 84 06 02 02 08 %s", body);
    KV_CHECK(c, refused(text, "more values than the summary says"));
    format(text, sizeof text, "D9 CB0B 82 84 08 02 02 08 %s", body);
    KV_CHECK(c, refused(text, "a summary larger than the document"));
    format(text, sizeof text, "D9 CB0B 82 84 07 01 02 08 %s", body);
    KV_CHECK(c, refused(text, "more keys than the summary says"));
    format(text, sizeof text, "D9 CB0B 82 84 07 03 02 08 %s", body);
    KV_CHECK(c, refused(text, "a summary larger than the document"));
    format(text, sizeof text, "D9 CB0B 82 84 07 02 01 08 %s", body);
    KV_CHECK(c, refused(text, "more strings than the summary says"));
    format(text, sizeof text, "D9 CB0B 82 84 07 02 03 08 %s", body);
    KV_CHECK(c, refused(text, "a summary larger than the document"));
    format(text, sizeof text, "D9 CB0B 82 84 07 02 02 07 %s", body);
    KV_CHECK(c, refused(text, "more string bytes than the summary says"));
    format(text, sizeof text, "D9 CB0B 82 84 07 02 02 09 %s", body);
    KV_CHECK(c, refused(text, "a summary larger than the document"));
    KV_CHECK(c, refused("D9 CB0B 82 84 1B FFFFFFFFFFFFFFFF 00 00 00 01", "a summary larger than the file"));
    KV_CHECK(c, refused("D9 CB0B 82 83 01 00 00 01", "a summary with fewer than four counts"));
    KV_CHECK(c, refused("D9 CB0B 82 84 00 00 00 00 01", "a summary of no values"));
    KV_CHECK(c, refused("D9 CB0B 83 84 01 00 00 00 01 01", "the summary tag around something not a pair"));
    KV_CHECK(c, refused("D9 CB0B 82 A0 01", "a summary that is not an array"));
    KV_CHECK(c, refused("D9 CB0B 82 84 01 00 00 61 01", "a summary count that is not an unsigned integer"));
    KV_CHECK(c, refused("D9 CB0B 82 84 02 00 00 00 9F 01 FF", "an indefinite length in a file with a summary"));
}

void test_writing(TestCtx& c) {
    // JSON to KBOR: the design's example, byte for byte, summary and all.
    const char* json = R"([{"name": "a", "hp": 1}, {"name": "b", "hp": 2}])";
    JsonDoc doc;
    KV_CHECK(c, json_parse(json, strlen(json), mem_scratch(), doc));
    Array<u8> out{};
    KborSummary s;
    kbor_write(doc, out, &s);
    KV_CHECK(c, same_bytes(out, hex("D9 CB0B 82 84 07 02 02 08 82 A2 64'name' 61'a' 62'hp' 01 A2 00 61'b' 01 02")));
    KV_CHECK(c, s.nodes == 7 && s.keys == 2 && s.strings == 2 && s.bytes == 8);

    // Numbers: the shortest heads, a single where it is exact, a double where not.
    out.clear();
    const char* nums = "[0, 23, 24, 256, 18446744073709551615, -1, -9223372036854775808, 1.0, 0.1, -0.0, 1e300, 100000000000000000000000]";
    KV_CHECK(c, json_parse(nums, strlen(nums), mem_scratch(), doc));
    kbor_write(doc, out);
    KV_CHECK(c, same_bytes(out, hex("D9 CB0B 82 84 0D 00 00 00 8C 00 17 18 18 19 0100 1B FFFFFFFFFFFFFFFF 20 3B 7FFFFFFFFFFFFFFF "
                                    "FA 3F800000 FB 3FB999999999999A FA 80000000 FB 7E37E43C8800759C FB 44B52D02C7E14AF6")));

    // Normalizing: a file with indefinite lengths and names in full every time, written
    // again with indices, definite lengths and the summary, and read back the same.
    out.clear();
    Bytes stock = hex("9F A2 64'name' 61'a' 62'hp' 01 BF 64'name' 61'b' 62'hp' 02 FF FF");
    KV_CHECK(c, kbor_parse(stock.b, stock.n, mem_scratch(), doc));
    kbor_write(doc, out);
    KV_CHECK(c, same_bytes(out, hex("D9 CB0B 82 84 07 02 02 08 82 A2 64'name' 61'a' 62'hp' 01 A2 00 61'b' 01 02")));
    JsonDoc again;
    KborInfo info;
    KV_CHECK(c, kbor_parse(out.data, out.len, mem_scratch(), again, &info) && info.summarized && !info.indefinite && info.index_keys == 2);
    // Writing appends: a second document after the first is its own whole file.
    u32 first = out.len;
    kbor_write(again, out);
    KV_CHECK(c, out.len == first * 2 && memcmp(out.data, out.data + first, first) == 0);
    out.free();

    // Byte strings and floats JSON cannot say, back as JSON.
    Bytes odd = hex("A4 61'b' 43 666F6F 61'n' F9 7E00 61'i' F9 FC00 61'f' FA 3F800000");
    KV_CHECK(c, kbor_parse(odd.b, odd.n, mem_scratch(), doc));
    Array<u8> t{};
    json_write(doc, t, 0);
    t.push(0);
    KV_CHECK(c, text_is((const char*)t.data, R"({"b":"Zm9v","n":"nan","i":"-inf","f":1.0})"));
    t.free();
}

void test_round_trips(TestCtx& c) {
    // Pretty text as json_write writes it comes back to itself through KBOR.
    const char* pretty =
        "{\n"
        "  \"name\": \"level \\\"one\\\"\\n\\u0001\\t\\u00e9\",\n"
        "  \"size\": [640, 480],\n"
        "  \"scale\": 1.0,\n"
        "  \"ratio\": 0.1,\n"
        "  \"tiny\": 5e-324,\n"
        "  \"big\": 1.7976931348623157e+308,\n"
        "  \"neg\": -9223372036854775808,\n"
        "  \"top\": 18446744073709551615,\n"
        "  \"flags\": [true, false, null],\n"
        "  \"empty\": {},\n"
        "  \"none\": [],\n"
        "  \"objects\": [\n"
        "    {\n"
        "      \"name\": \"a\",\n"
        "      \"pos\": [1.5, -2.25]\n"
        "    },\n"
        "    {\n"
        "      \"name\": \"b\",\n"
        "      \"pos\": [0.0, -0.0]\n"
        "    }\n"
        "  ]\n"
        "}\n";
    // json_write's own escapes are \u00XX and the short forms, and é comes out as UTF-8.
    const char* want =
        "{\n"
        "  \"name\": \"level \\\"one\\\"\\n\\u0001\\t\xc3\xa9\",\n"
        "  \"size\": [640, 480],\n"
        "  \"scale\": 1.0,\n"
        "  \"ratio\": 0.1,\n"
        "  \"tiny\": 4.94065645841247e-324,\n"
        "  \"big\": 1.7976931348623157e+308,\n"
        "  \"neg\": -9223372036854775808,\n"
        "  \"top\": 18446744073709551615,\n"
        "  \"flags\": [true, false, null],\n"
        "  \"empty\": {},\n"
        "  \"none\": [],\n"
        "  \"objects\": [\n"
        "    {\n"
        "      \"name\": \"a\",\n"
        "      \"pos\": [1.5, -2.25]\n"
        "    },\n"
        "    {\n"
        "      \"name\": \"b\",\n"
        "      \"pos\": [0.0, -0.0]\n"
        "    }\n"
        "  ]\n"
        "}\n";
    KborInfo info;
    KV_CHECK(c, text_is(round_trip(pretty, KV_JSON_WRITE_PRETTY, &info), want));
    KV_CHECK(c, info.summarized && info.index_keys == 3); // both objects' "name" (the top level had it first) and the second's "pos"
    KV_CHECK(c, text_is(round_trip(want), want));
    KV_CHECK(c, text_is(round_trip("[1, 2.5, \"x\"]", 0), "[1,2.5,\"x\"]"));
    KV_CHECK(c, text_is(round_trip("\"just a string\"", 0), "\"just a string\""));

    // Many objects of one shape: every key after the first object costs a byte, and the
    // whole comes back as it went in.
    Array<char> big{};
    big.alloc = mem_scratch();
    auto add = [&](const char* s) { usize n = strlen(s); memcpy(big.push_n((u32)n), s, n); };
    add("[");
    for (u32 i = 0; i < 2000; ++i) {
        char row[200];
        format(row, sizeof row, "%s{\"kind\":\"rock\",\"position\":[%u,%u],\"velocity\":%u.5,\"health\":%u}", i ? "," : "", i, i * 3, i % 50, i % 7);
        add(row);
    }
    add("]");
    big.push(0);
    JsonDoc doc;
    KV_CHECK(c, json_parse(big.data, big.len - 1, mem_scratch(), doc));
    Array<u8> k{};
    kbor_write(doc, k);
    KV_CHECK(c, k.len * 2 < big.len - 1); // less than half the JSON, uncompressed
    JsonDoc back;
    KV_CHECK(c, kbor_parse(k.data, k.len, mem_scratch(), back, &info) && info.summarized && info.names == 4 && info.index_keys == 4 * 1999);
    KV_CHECK(c, back.count == doc.count);
    Array<u8> t{};
    json_write(back, t, 0);
    KV_CHECK(c, t.len == big.len - 1 && memcmp(t.data, big.data, t.len) == 0);
    t.free();
    k.free();
}

} // namespace

void test_kbor(kv::TestCtx& c) {
    using namespace kv;
    c.begin("kbor");
    MemScope scope;
    test_base64(c);
    test_reading(c);
    test_errors(c);
    test_writing(c);
    test_round_trips(c);
}
