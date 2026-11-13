// The writer: JSON text as the calls describe it, pretty or not; strings escaped; floats
// in the fewest digits that read back to the same bits; mistakes caught at finish().
#include "korvid/save/writer.h"
#include "korvid/core/arena.h"
#include "korvid/core/test.h"
#include "korvid/math/scalar.h"
#include <stdlib.h>
#include <string.h>

namespace {

using namespace kv;

bool text_is(SaveWriter& w, const char* want) {
    return w.out.len == strlen(want) && memcmp(w.out.data, want, w.out.len) == 0;
}

} // namespace

void test_writer(kv::TestCtx& c) {
    using namespace kv;
    c.begin("writer");
    SaveWriter w;
    w.init(mem_general(), SaveFormat::Json, false);
    w.begin_object();
    w.key("a"); w.int_value(-3);
    w.key("b"); w.begin_array(); w.bool_value(true); w.null_value(); w.uint_value(18446744073709551615ull); w.end_array();
    w.key("s"); w.string_value("q\"\\\n\x01 \xc3\xa9");
    w.key("e"); w.begin_object(); w.end_object();
    w.end_object();
    KV_CHECK(c, w.finish());
    KV_CHECK(c, text_is(w, "{\"a\":-3,\"b\":[true,null,18446744073709551615],\"s\":\"q\\\"\\\\\\n\\u0001 \xc3\xa9\",\"e\":{}}"));

    // Pretty: a member per line, compact containers on one line.
    w.init(mem_general());
    w.begin_object();
    w.key("p"); w.begin_array(true); w.f32_value(1.5f); w.f32_value(-2.0f); w.end_array();
    w.key("rows"); w.begin_array(); w.begin_object(); w.key("x"); w.int_value(1); w.end_object(); w.end_array();
    w.end_object();
    KV_CHECK(c, w.finish());
    KV_CHECK(c, text_is(w, "{\n  \"p\": [1.5, -2],\n  \"rows\": [\n    {\n      \"x\": 1\n    }\n  ]\n}\n"));

    // Floats: short where short reads back, every bit where it does not; JSON-less values as strings.
    const f32 f32s[] = {0.15f, 1.0f / 3.0f, 3.4028235e38f, 1.0e-45f, -0.0f, 16777217.0f};
    const f64 f64s[] = {0.1, 1.0 / 3.0, 1.7976931348623157e308, 5e-324, 9007199254740993.0};
    for (f32 v : f32s) {
        w.reset(); w.f32_value(v); KV_CHECK(c, w.finish());
        w.out.push(0);
        KV_CHECK(c, strtof((const char*)w.out.data, nullptr) == v && kv::signbit(strtof((const char*)w.out.data, nullptr)) == kv::signbit(v));
    }
    w.reset(); w.f32_value(0.15f); KV_CHECK(c, w.finish() && text_is(w, "0.15\n"));
    for (f64 v : f64s) {
        w.reset(); w.f64_value(v); KV_CHECK(c, w.finish());
        w.out.push(0);
        KV_CHECK(c, strtod((const char*)w.out.data, nullptr) == v);
    }
    w.reset(); w.begin_array(true); w.f32_value(kv::KV_NAN); w.f64_value(-kv::KV_INF); w.end_array();
    KV_CHECK(c, w.finish() && text_is(w, "[\"nan\", \"-inf\"]\n"));

    // Mistakes: a value without a key, an unbalanced end, two top-level values, an open container.
    w.reset(); w.begin_object(); w.int_value(1); w.end_object(); KV_CHECK(c, !w.finish());
    w.reset(); w.begin_array(); w.end_object(); KV_CHECK(c, !w.finish());
    w.reset(); w.int_value(1); w.int_value(2); KV_CHECK(c, !w.finish());
    w.reset(); w.begin_array(); KV_CHECK(c, !w.finish());
    w.reset(); KV_CHECK(c, !w.finish());
    w.reset(); w.begin_array(); w.key("k"); w.end_array(); KV_CHECK(c, !w.finish());

    // What it writes, the reader reads.
    w.reset();
    w.begin_object(); w.key("n"); w.uint_value(9007199254740993ull); w.key("t"); w.string_value("tab\there"); w.end_object();
    KV_CHECK(c, w.finish());
    JsonDoc doc;
    KV_CHECK(c, save_parse(SaveFormat::Json, w.out.data, w.out.len, mem_scratch(), doc));
    KV_CHECK(c, json_u64(json_get(doc, doc.root(), "n"), 0) == 9007199254740993ull);
    KV_CHECK(c, !strcmp(json_string(json_get(doc, doc.root(), "t"), ""), "tab\there"));
    w.free();
}
