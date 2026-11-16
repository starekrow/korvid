// patherizer: the .kshape file (docs/PRDs/Krome-Slice-5-PRD.md, "The shape asset"). KBOR, one
// map:
//
//   kshape      1, the format's version
//   view        [x, y, w, h]
//   one_colour  whether every paint is currentColor
//   verbs       bytes: one PathVerb each
//   points      bytes: f32 x, y pairs
//   paints      bytes: 40-byte records (kind, spread, linear_rgb, rgba, g[5], transform[6],
//               first_stop, stop_count; see put_paint)
//   stops       bytes: f32 offset, rgba
//   strokes     bytes: f32 width, u8 join, u8 cap, f32 miter_limit, u32 first_dash,
//               u32 dash_count, f32 dash_offset
//   dashes      bytes: f32
//   nodes       bytes: 64-byte records (see put_node)
//   clips       bytes: u32 node indices
//   lines       bytes: u32 a node, the source's line (a sidecar: strip it and nothing breaks)
//
// Every number is little-endian and written field by field, so the bytes are the same from
// any platform and read back on any.
#include "korvid/patherizer/patherizer.h"
#include "korvid/core/json.h"
#include "korvid/core/kbor.h"
#include "korvid/core/arena.h"
#include "korvid/core/memory.h"
#include <string.h>

namespace kv {

namespace {

struct Put {
    Array<u8>& b;
    void u8_(u8 v) { b.push(v); }
    void u32_(u32 v) { u8* p = b.push_n(4); p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
    void f32_(f32 v) { u32 u; memcpy(&u, &v, 4); u32_(u); }
};

struct Get {
    const u8* p;
    const u8* end;
    bool ok = true;
    u8  u8_() { if (p + 1 > end) { ok = false; return 0; } return *p++; }
    u32 u32_() { if (p + 4 > end) { ok = false; return 0; } u32 v = (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; p += 4; return v; }
    f32 f32_() { u32 u = u32_(); f32 v; memcpy(&v, &u, 4); return v; }
};

constexpr u32 kPaintBytes = 4 + 4 + 20 + 24 + 8;  // 60
constexpr u32 kNodeBytes = 4 + 24 + 12 + 12 + 8 + 4 + 4 + 8 + 4; // 80
constexpr u32 kStrokeBytes = 4 + 2 + 4 + 4 + 4 + 4;

void put_paint(Put& o, const PathPaint& p) {
    o.u8_((u8)p.kind); o.u8_((u8)p.spread); o.u8_(p.linear_rgb ? 1 : 0); o.u8_(0);
    for (u32 k = 0; k < 4; ++k) o.u8_(p.rgba[k]);
    for (u32 k = 0; k < 5; ++k) o.f32_(p.g[k]);
    for (u32 k = 0; k < 6; ++k) o.f32_(p.transform.m[k]);
    o.u32_(p.first_stop); o.u32_(p.stop_count);
}

void get_paint(Get& g, PathPaint& p) {
    p.kind = (PathPaint::Kind)g.u8_(); p.spread = (GradientSpread)g.u8_(); p.linear_rgb = g.u8_() != 0; g.u8_();
    for (u32 k = 0; k < 4; ++k) p.rgba[k] = g.u8_();
    for (u32 k = 0; k < 5; ++k) p.g[k] = g.f32_();
    for (u32 k = 0; k < 6; ++k) p.transform.m[k] = g.f32_();
    p.first_stop = g.u32_(); p.stop_count = g.u32_();
}

void put_node(Put& o, const PathNode& n) {
    o.u8_((u8)n.kind); o.u8_((u8)n.rule); o.u8_(0); o.u8_(0);
    for (u32 k = 0; k < 6; ++k) o.f32_(n.transform.m[k]);
    o.u32_(n.first_verb); o.u32_(n.verb_count); o.u32_(n.first_point);
    o.u32_(n.fill); o.u32_(n.stroke); o.u32_(n.stroke_style);
    o.f32_(n.fill_opacity); o.f32_(n.stroke_opacity);
    o.u32_(n.count);
    o.f32_(n.opacity);
    o.u32_(n.clip); o.u32_(0);
    o.u32_(0);
}

void get_node(Get& g, PathNode& n) {
    n.kind = (PathNode::Kind)g.u8_(); n.rule = (FillRule)g.u8_(); g.u8_(); g.u8_();
    for (u32 k = 0; k < 6; ++k) n.transform.m[k] = g.f32_();
    n.first_verb = g.u32_(); n.verb_count = g.u32_(); n.first_point = g.u32_();
    n.fill = g.u32_(); n.stroke = g.u32_(); n.stroke_style = g.u32_();
    n.fill_opacity = g.f32_(); n.stroke_opacity = g.f32_();
    n.count = g.u32_();
    n.opacity = g.f32_();
    n.clip = g.u32_(); g.u32_();
    g.u32_();
}

} // namespace

bool patherizer_write_kshape(const PathShape& s, Array<u8>& out, bool lines) {
    MemScope scope;
    JsonBuilder b(&scope.arena->a);
    JsonRef root = b.object();
    b.set(root, "kshape", b.integer(1));
    JsonRef view = b.array();
    for (u32 k = 0; k < 4; ++k) b.push(view, b.number(s.view[k]));
    b.set(root, "view", view);
    b.set(root, "one_colour", b.boolean(s.one_colour()));
    Array<u8> buf;
    buf.alloc = mem_general();
    Put o{buf};
    auto bytes = [&](const char* key) { b.set(root, key, b.bytes(buf.data, buf.len)); buf.clear(); };
    for (PathVerb v : s.verbs) o.u8_((u8)v);
    bytes("verbs");
    for (f32 v : s.points) o.f32_(v);
    bytes("points");
    for (const PathPaint& p : s.paints) put_paint(o, p);
    bytes("paints");
    for (const PathStop& st : s.stops) { o.f32_(st.offset); for (u32 k = 0; k < 4; ++k) o.u8_(st.rgba[k]); }
    bytes("stops");
    for (const PathStroke& st : s.strokes) {
        o.f32_(st.width); o.u8_((u8)st.join); o.u8_((u8)st.cap); o.f32_(st.miter_limit);
        o.u32_(st.first_dash); o.u32_(st.dash_count); o.f32_(st.dash_offset);
    }
    bytes("strokes");
    for (f32 v : s.dashes) o.f32_(v);
    bytes("dashes");
    for (const PathNode& n : s.nodes) put_node(o, n);
    bytes("nodes");
    for (u32 c : s.clips) o.u32_(c);
    bytes("clips");
    if (lines) {
        for (const PathNode& n : s.nodes) o.u32_(n.line);
        bytes("lines");
    }
    JsonDoc doc;
    bool ok = b.finish(root, doc);
    if (ok) kbor_write(doc, out);
    b.free();
    buf.free();
    return ok;
}

bool patherizer_read_kshape(const void* data, usize size, PathShape& s, const char** why) {
    MemScope scope;
    JsonDoc doc;
    if (!kbor_parse(data, size, &scope.arena->a, doc)) { *why = doc.error ? doc.error : "not KBOR"; return false; }
    bool ok = false;
    s.clear();
    s.paints.clear();
    s.strokes.clear();
    do {
        const JsonValue& root = doc.nodes[0];
        const JsonValue* ver = root.is_object() ? json_get(doc, root, "kshape") : nullptr;
        if (!ver || ver->number() != 1.0) { *why = "not a .kshape (or not version 1)"; break; }
        const JsonValue* view = json_get(doc, root, "view");
        if (!view || view->count() != 4) { *why = "no view"; break; }
        for (u32 k = 0; k < 4; ++k) s.view[k] = (f32)json_at(doc, *view, k)->number();
        auto field = [&](const char* key, u32 record, Get& g, u32* count) {
            const JsonValue* v = json_get(doc, root, key);
            if (!v) { g = Get{nullptr, nullptr, true}; *count = 0; return true; }
            if (!v->is_bytes() || v->len() % record) return false;
            g = Get{v->bytes(), v->bytes() + v->len(), true};
            *count = v->len() / record;
            return true;
        };
        Get g{nullptr, nullptr};
        u32 n = 0;
        bool good = true;
        good = good && field("verbs", 1, g, &n);
        for (u32 i = 0; good && i < n; ++i) { u8 v = g.u8_(); if (v > (u8)PathVerb::Close) good = false; s.verbs.push((PathVerb)v); }
        good = good && field("points", 4, g, &n);
        for (u32 i = 0; good && i < n; ++i) s.points.push(g.f32_());
        good = good && field("paints", kPaintBytes, g, &n);
        for (u32 i = 0; good && i < n; ++i) { PathPaint p; get_paint(g, p); s.paints.push(p); }
        good = good && field("stops", 8, g, &n);
        for (u32 i = 0; good && i < n; ++i) { PathStop st; st.offset = g.f32_(); for (u32 k = 0; k < 4; ++k) st.rgba[k] = g.u8_(); s.stops.push(st); }
        good = good && field("strokes", kStrokeBytes, g, &n);
        for (u32 i = 0; good && i < n; ++i) {
            PathStroke st;
            st.width = g.f32_(); st.join = (LineJoin)g.u8_(); st.cap = (LineCap)g.u8_(); st.miter_limit = g.f32_();
            st.first_dash = g.u32_(); st.dash_count = g.u32_(); st.dash_offset = g.f32_();
            s.strokes.push(st);
        }
        good = good && field("dashes", 4, g, &n);
        for (u32 i = 0; good && i < n; ++i) s.dashes.push(g.f32_());
        good = good && field("nodes", kNodeBytes, g, &n);
        for (u32 i = 0; good && i < n; ++i) { PathNode nd; get_node(g, nd); s.nodes.push(nd); }
        good = good && field("clips", 4, g, &n);
        for (u32 i = 0; good && i < n; ++i) s.clips.push(g.u32_());
        good = good && field("lines", 4, g, &n);
        for (u32 i = 0; good && i < n && i < s.nodes.len; ++i) s.nodes[i].line = g.u32_();
        if (!good) { *why = "a field is the wrong size"; break; }
        if (!s.paints.len) s.paints.push(PathPaint{});
        if (!s.strokes.len) s.strokes.push(PathStroke{});
        // Every index in range, so drawing need check nothing.
        for (const PathNode& nd : s.nodes) {
            if (nd.kind == PathNode::Path) {
                if ((u64)nd.first_verb + nd.verb_count > s.verbs.len || nd.fill >= s.paints.len || nd.stroke >= s.paints.len || nd.stroke_style >= s.strokes.len) good = false;
            } else if (nd.clip > s.nodes.len) good = false;
        }
        for (const PathPaint& p : s.paints) if ((u64)p.first_stop + p.stop_count > s.stops.len) good = false;
        for (const PathStroke& st : s.strokes) if ((u64)st.first_dash + st.dash_count > s.dashes.len) good = false;
        for (u32 c : s.clips) if (c >= s.nodes.len) good = false;
        // The points the verbs need, path by path.
        for (const PathNode& nd : s.nodes) {
            if (nd.kind != PathNode::Path) continue;
            u64 need = 0;
            for (u32 i = 0; i < nd.verb_count; ++i) {
                PathVerb v = s.verbs[nd.first_verb + i];
                need += v == PathVerb::Move || v == PathVerb::Line ? 2 : v == PathVerb::Quad ? 4 : v == PathVerb::Cubic ? 6 : 0;
            }
            if (nd.first_point + need > s.points.len) good = false;
        }
        if (!good) { *why = "an index is out of range"; break; }
        ok = true;
    } while (false);
    if (!ok) s.clear();
    return ok;
}

} // namespace kv
