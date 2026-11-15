// JsonBuilder (json.h): values made in a working list, laid out breadth first by finish()
// so that every container's children are consecutive, as the parsers lay them out.
#include "korvid/core/json.h"

namespace kv {

JsonRef JsonBuilder::make(JsonType t) {
    Item* it = items.try_push(Item{});
    if (!it) { failed = true; return KV_JSON_NO_REF; }
    it->v.type = t;
    return items.len - 1;
}

const char* JsonBuilder::copy(const char* s, u32 len, bool with_len) {
    u32 head = with_len ? (u32)sizeof(u32) : 0;
    char* d = (char*)alloc_in(alloc, head + (usize)len + 1, with_len ? (u32)alignof(u32) : 1);
    if (!d) { failed = true; return nullptr; }
    d += head;
    if (len) memcpy(d, s, len);
    d[len] = 0;
    if (with_len) memcpy(d - sizeof len, &len, sizeof len);
    return d;
}

JsonRef JsonBuilder::null() { return make(JsonType::Null); }

JsonRef JsonBuilder::boolean(bool v) {
    JsonRef r = make(JsonType::Bool);
    if (r != KV_JSON_NO_REF) items[r].v.as.num = v ? 1.0 : 0.0;
    return r;
}

JsonRef JsonBuilder::integer(i64 v) {
    JsonRef r = make(JsonType::Number);
    if (r == KV_JSON_NO_REF) return r;
    items[r].v.as.bits = (u64)v;
    items[r].v.flags = KV_JSON_VALUE_INTEGER | (v < 0 ? KV_JSON_VALUE_NEGATIVE : 0);
    return r;
}

JsonRef JsonBuilder::unsigned_integer(u64 v) {
    JsonRef r = make(JsonType::Number);
    if (r == KV_JSON_NO_REF) return r;
    items[r].v.as.bits = v;
    items[r].v.flags = KV_JSON_VALUE_INTEGER;
    return r;
}

JsonRef JsonBuilder::number(f64 v) {
    JsonRef r = make(JsonType::Number);
    if (r != KV_JSON_NO_REF) items[r].v.as.num = v;
    return r;
}

JsonRef JsonBuilder::string(const char* s) { return string(s, s ? (u32)strlen(s) : 0); }

JsonRef JsonBuilder::string(const char* s, u32 len) {
    const char* t = copy(s, len, true);
    if (!t) return KV_JSON_NO_REF;
    JsonRef r = make(JsonType::String);
    if (r != KV_JSON_NO_REF) items[r].v.as.text = t;
    return r;
}

JsonRef JsonBuilder::bytes(const void* data, u32 len) {
    const char* t = copy((const char*)data, len, true);
    if (!t) return KV_JSON_NO_REF;
    JsonRef r = make(JsonType::Bytes);
    if (r != KV_JSON_NO_REF) items[r].v.as.text = t;
    return r;
}

JsonRef JsonBuilder::array() { return make(JsonType::Array); }
JsonRef JsonBuilder::object() { return make(JsonType::Object); }

bool JsonBuilder::push(JsonRef a, JsonRef v) {
    if (a >= items.len || v >= items.len || a == v || items[v].attached || items[a].v.type != JsonType::Array) { failed = true; return false; }
    Item& c = items[a];
    if (c.last == KV_JSON_NO_REF) c.first = v;
    else items[c.last].next = v;
    c.last = v;
    ++c.count;
    items[v].attached = true;
    return true;
}

bool JsonBuilder::set(JsonRef o, const char* key, JsonRef v) {
    if (o >= items.len || v >= items.len || o == v || items[v].attached || items[o].v.type != JsonType::Object || !key) { failed = true; return false; }
    const char* k = copy(key, (u32)strlen(key), false);
    if (!k) return false;
    items[v].v.key = k;
    Item& c = items[o];
    if (c.last == KV_JSON_NO_REF) c.first = v;
    else items[c.last].next = v;
    c.last = v;
    ++c.count;
    items[v].attached = true;
    return true;
}

void JsonBuilder::line(JsonRef v, u32 l) {
    if (v < items.len) items[v].v.line = l;
}

bool JsonBuilder::finish(JsonRef root, JsonDoc& doc) {
    doc = JsonDoc{};
    auto lone_null = [&](const char* why) {
        static JsonValue none;
        none = JsonValue{};
        none.flags = KV_JSON_VALUE_LAST;
        JsonValue* n = (JsonValue*)alloc_in(alloc, sizeof(JsonValue), alignof(JsonValue));
        if (n) *n = none;
        doc.nodes = n ? n : &none;
        doc.count = 1;
        doc.error = why;
        return false;
    };
    if (failed) return lone_null("the builder failed (out of memory, or a value attached twice)");
    if (root >= items.len) return lone_null("no root");

    // Count what is reachable, then lay it out level by level: a queue of item refs whose
    // position is the node's index in the document.
    Array<u32> order{};
    if (!order.try_push(root)) return lone_null("out of memory");
    for (u32 i = 0; i < order.len; ++i)
        for (u32 k = items[order[i]].first; k != KV_JSON_NO_REF; k = items[k].next)
            if (!order.try_push(k)) { order.free(); return lone_null("out of memory"); }

    JsonValue* nodes = (JsonValue*)alloc_in(alloc, (usize)order.len * sizeof(JsonValue), alignof(JsonValue));
    if (!nodes) { order.free(); return lone_null("out of memory"); }
    // Children of the item at position i start where the queue first met them; since the
    // queue appended each container's children together and in order, they are consecutive.
    u32 next = 1;
    for (u32 i = 0; i < order.len; ++i) {
        const Item& it = items[order[i]];
        JsonValue v = it.v;
        v.flags &= (u8)~KV_JSON_VALUE_LAST;
        if (v.type == JsonType::Array || v.type == JsonType::Object) {
            v.as.kids.first = it.count ? next : 0;
            v.as.kids.count = it.count;
            next += it.count;
        }
        if (i == 0 || it.next == KV_JSON_NO_REF) v.flags |= KV_JSON_VALUE_LAST;
        nodes[i] = v;
    }
    doc.nodes = nodes;
    doc.count = order.len;
    order.free();
    return true;
}

} // namespace kv
