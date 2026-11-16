// patherizer: the shape's data and its builder, and SVG path data (patherizer.h).
#include "korvid/patherizer/patherizer.h"
#include "korvid/core/memory.h"
#include "korvid/math/scalar.h"
#include <string.h>

namespace kv {

// ---- transforms --------------------------------------------------------------------------------

PathTransform PathTransform::then(const PathTransform& t) const {
    // t after this: t.m * this.m.
    const f32* a = t.m;
    const f32* b = m;
    PathTransform r;
    r.m[0] = a[0] * b[0] + a[2] * b[1];
    r.m[1] = a[1] * b[0] + a[3] * b[1];
    r.m[2] = a[0] * b[2] + a[2] * b[3];
    r.m[3] = a[1] * b[2] + a[3] * b[3];
    r.m[4] = a[0] * b[4] + a[2] * b[5] + a[4];
    r.m[5] = a[1] * b[4] + a[3] * b[5] + a[5];
    return r;
}

bool PathTransform::invert(PathTransform* out) const {
    f32 det = m[0] * m[3] - m[2] * m[1];
    if (!(kv::fabs(det) > 1e-20f)) return false;
    f32 id = 1.0f / det;
    out->m[0] = m[3] * id;
    out->m[1] = -m[1] * id;
    out->m[2] = -m[2] * id;
    out->m[3] = m[0] * id;
    out->m[4] = -(out->m[0] * m[4] + out->m[2] * m[5]);
    out->m[5] = -(out->m[1] * m[4] + out->m[3] * m[5]);
    return true;
}

// ---- the shape ----------------------------------------------------------------------------------

void PathShape::init(Allocator* a) {
    verbs.alloc = a; points.alloc = a; paints.alloc = a; stops.alloc = a;
    strokes.alloc = a; dashes.alloc = a; nodes.alloc = a; clips.alloc = a;
    clear();
}

void PathShape::free() {
    verbs.free(); points.free(); paints.free(); stops.free();
    strokes.free(); dashes.free(); nodes.free(); clips.free();
}

void PathShape::clear() {
    verbs.clear(); points.clear(); paints.clear(); stops.clear();
    strokes.clear(); dashes.clear(); nodes.clear(); clips.clear();
    paints.push(PathPaint{}); // None
    strokes.push(PathStroke{});
}

u32 PathShape::add_solid(u8 r, u8 g, u8 b, u8 a) {
    PathPaint p;
    p.kind = PathPaint::Solid;
    p.rgba[0] = r; p.rgba[1] = g; p.rgba[2] = b; p.rgba[3] = a;
    paints.push(p);
    return paints.len - 1;
}

u32 PathShape::add_current() {
    PathPaint p;
    p.kind = PathPaint::Current;
    paints.push(p);
    return paints.len - 1;
}

u32 PathShape::add_gradient(const PathPaint& g, const PathStop* s, u32 count) {
    PathPaint p = g;
    p.first_stop = stops.len;
    p.stop_count = count;
    for (u32 i = 0; i < count; ++i) stops.push(s[i]);
    paints.push(p);
    return paints.len - 1;
}

bool PathShape::one_colour() const {
    for (const PathNode& n : nodes) {
        if (n.kind != PathNode::Path) continue;
        u32 used[2] = {n.fill, n.stroke};
        for (u32 p : used)
            if (p && p < paints.len && paints[p].kind != PathPaint::Current && paints[p].kind != PathPaint::None) return false;
    }
    return true;
}

// ---- the builder ---------------------------------------------------------------------------------

PathBuilder::PathBuilder(PathShape& shape) : s(shape), verb0(shape.verbs.len), point0(shape.points.len) {}

void PathBuilder::move_to(f32 x, f32 y) {
    s.verbs.push(PathVerb::Move);
    s.points.push(x); s.points.push(y);
    cx = sx = x; cy = sy = y;
}

void PathBuilder::line_to(f32 x, f32 y) {
    s.verbs.push(PathVerb::Line);
    s.points.push(x); s.points.push(y);
    cx = x; cy = y;
}

void PathBuilder::quad_to(f32 x1, f32 y1, f32 x, f32 y) {
    s.verbs.push(PathVerb::Quad);
    s.points.push(x1); s.points.push(y1); s.points.push(x); s.points.push(y);
    cx = x; cy = y;
}

void PathBuilder::cubic_to(f32 x1, f32 y1, f32 x2, f32 y2, f32 x, f32 y) {
    s.verbs.push(PathVerb::Cubic);
    s.points.push(x1); s.points.push(y1); s.points.push(x2); s.points.push(y2); s.points.push(x); s.points.push(y);
    cx = x; cy = y;
}

void PathBuilder::close() {
    s.verbs.push(PathVerb::Close);
    cx = sx; cy = sy;
}

u32 PathBuilder::end_path() {
    PathNode n;
    n.kind = PathNode::Path;
    n.transform = transform;
    n.first_verb = verb0;
    n.verb_count = s.verbs.len - verb0;
    n.first_point = point0;
    s.nodes.push(n);
    verb0 = s.verbs.len;
    point0 = s.points.len;
    return s.nodes.len - 1;
}

u32 PathBuilder::fill(u32 paint, FillRule rule, f32 opacity) {
    u32 i = end_path();
    s.nodes[i].fill = paint;
    s.nodes[i].rule = rule;
    s.nodes[i].fill_opacity = opacity;
    return i;
}

u32 PathBuilder::stroke(u32 paint, const PathStroke& style, f32 opacity, const f32* dashes, u32 dash_count) {
    u32 i = end_path();
    PathStroke st = style;
    st.first_dash = s.dashes.len;
    st.dash_count = dash_count;
    for (u32 k = 0; k < dash_count; ++k) s.dashes.push(dashes[k]);
    s.strokes.push(st);
    s.nodes[i].stroke = paint;
    s.nodes[i].stroke_style = s.strokes.len - 1;
    s.nodes[i].stroke_opacity = opacity;
    return i;
}

u32 PathBuilder::fill_and_stroke(u32 fill_paint, FillRule rule, u32 stroke_paint, const PathStroke& style) {
    u32 i = end_path();
    s.nodes[i].fill = fill_paint;
    s.nodes[i].rule = rule;
    s.strokes.push(style);
    s.nodes[i].stroke = stroke_paint;
    s.nodes[i].stroke_style = s.strokes.len - 1;
    return i;
}

u32 PathBuilder::begin_group(f32 opacity, u32 clip) {
    PathNode g;
    g.kind = PathNode::Group;
    g.opacity = opacity;
    g.clip = clip;
    s.nodes.push(g);
    return s.nodes.len - 1;
}

void PathBuilder::end_group(u32 group) {
    s.nodes[group].count = s.nodes.len - group - 1;
}

// ---- SVG path data ---------------------------------------------------------------------------------

bool patherizer_number(const char** at, f32* out) {
    const char* p = *at;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') ++p;
    const char* q = p;
    if (*q == '+' || *q == '-') ++q;
    bool digits = false;
    while (*q >= '0' && *q <= '9') { ++q; digits = true; }
    if (*q == '.') { ++q; while (*q >= '0' && *q <= '9') { ++q; digits = true; } }
    if (!digits) return false;
    if (*q == 'e' || *q == 'E') {
        const char* e = q + 1;
        if (*e == '+' || *e == '-') ++e;
        if (*e >= '0' && *e <= '9') { q = e; while (*q >= '0' && *q <= '9') ++q; }
    }
    // By hand, as decimal digits and an exponent, so every platform reads the same float: a
    // mantissa of up to 18 digits, then one scale by a power of ten in double.
    const char* r = p;
    bool neg = false;
    if (*r == '+' || *r == '-') neg = *r++ == '-';
    u64 mant = 0;
    i32 exp10 = 0, kept = 0;
    while (*r >= '0' && *r <= '9') { if (kept < 18) { mant = mant * 10 + (u64)(*r - '0'); if (mant) ++kept; } else ++exp10; ++r; }
    if (*r == '.') {
        ++r;
        while (*r >= '0' && *r <= '9') { if (kept < 18) { mant = mant * 10 + (u64)(*r - '0'); if (mant) ++kept; --exp10; } ++r; }
    }
    if (*r == 'e' || *r == 'E') {
        ++r;
        bool eneg = false;
        if (*r == '+' || *r == '-') eneg = *r++ == '-';
        i32 e = 0;
        while (*r >= '0' && *r <= '9') { if (e < 10000) e = e * 10 + (*r - '0'); ++r; }
        exp10 += eneg ? -e : e;
    }
    f64 v = (f64)mant, scale = 1.0, ten = 10.0;
    i32 ae = exp10 < 0 ? -exp10 : exp10;
    if (ae > 300) ae = 300;
    for (i32 k = ae; k; k >>= 1, ten *= ten) if (k & 1) scale *= ten;
    v = exp10 < 0 ? v / scale : v * scale;
    *out = (f32)(neg ? -v : v);
    *at = q;
    return true;
}

namespace {

struct Reader {
    const char* p;
    const char* start;
    void space() { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') ++p; }
    bool number(f32* out) {
        space();
        const char* q0 = p;
        if (!patherizer_number(&q0, out)) return false;
        p = q0;
        return true;
    }
    bool flag(bool* out) {
        space();
        if (*p == '0' || *p == '1') { *out = *p++ == '1'; return true; }
        return false;
    }
    bool more() { space(); return (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '+'; }
};

// An elliptical arc (SVG's endpoint form) as cubics, a quarter turn or less each.
void arc_to(PathBuilder& b, f32 rx, f32 ry, f32 angle_deg, bool large, bool sweep, f32 x, f32 y) {
    f32 x0 = b.cx, y0 = b.cy;
    if (x0 == x && y0 == y) return;
    rx = kv::fabs(rx); ry = kv::fabs(ry);
    if (rx == 0.0f || ry == 0.0f) { b.line_to(x, y); return; }
    const f32 pi = 3.14159265358979f;
    f32 phi = angle_deg * pi / 180.0f;
    f32 cp = kv::cos(phi), sp = kv::sin(phi);
    f32 dx = (x0 - x) * 0.5f, dy = (y0 - y) * 0.5f;
    f32 x1p = cp * dx + sp * dy, y1p = -sp * dx + cp * dy;
    f32 lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lambda > 1.0f) { f32 l = kv::sqrt(lambda); rx *= l; ry *= l; }
    f32 num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    f32 den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    f32 coef = den > 0.0f && num > 0.0f ? kv::sqrt(num / den) : 0.0f;
    if (large == sweep) coef = -coef;
    f32 cxp = coef * rx * y1p / ry, cyp = -coef * ry * x1p / rx;
    f32 ccx = cp * cxp - sp * cyp + (x0 + x) * 0.5f, ccy = sp * cxp + cp * cyp + (y0 + y) * 0.5f;
    auto angle = [](f32 ux, f32 uy, f32 vx, f32 vy) {
        f32 a = kv::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
        return a;
    };
    f32 t1 = angle(1.0f, 0.0f, (x1p - cxp) / rx, (y1p - cyp) / ry);
    f32 dt = angle((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry);
    if (!sweep && dt > 0.0f) dt -= 2.0f * pi;
    else if (sweep && dt < 0.0f) dt += 2.0f * pi;
    u32 segs = (u32)kv::ceil(kv::fabs(dt) / (pi * 0.5f) - 1e-4f);
    if (segs < 1) segs = 1;
    if (segs > 4) segs = 4;
    f32 step = dt / (f32)segs;
    f32 k = 4.0f / 3.0f * kv::tan(step * 0.25f);
    for (u32 i = 0; i < segs; ++i) {
        f32 a0 = t1 + step * (f32)i, a1 = a0 + step;
        f32 c0 = kv::cos(a0), s0 = kv::sin(a0), c1 = kv::cos(a1), s1 = kv::sin(a1);
        // On the unit circle, then scaled, turned and moved.
        f32 px[3] = {c0 - k * s0, c1 + k * s1, c1};
        f32 py[3] = {s0 + k * c0, s1 - k * c1, s1};
        f32 out[6];
        for (u32 j = 0; j < 3; ++j) {
            f32 ex = px[j] * rx, ey = py[j] * ry;
            out[j * 2] = cp * ex - sp * ey + ccx;
            out[j * 2 + 1] = sp * ex + cp * ey + ccy;
        }
        if (i == segs - 1) { out[4] = x; out[5] = y; } // land exactly on the end point
        b.cubic_to(out[0], out[1], out[2], out[3], out[4], out[5]);
    }
}

} // namespace

bool PathBuilder::svg_path(const char* d, u32* at) {
    Reader r{d, d};
    char cmd = 0;
    f32 lcx = cx, lcy = cy; // the last control point, for S and T
    char last = 0;
    for (;;) {
        r.space();
        if (!*r.p) break;
        char c = *r.p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) { cmd = c; ++r.p; }
        else if (!cmd || !r.more()) { if (at) *at = (u32)(r.p - d); return false; }
        bool rel = cmd >= 'a';
        char C = rel ? (char)(cmd - 32) : cmd;
        f32 ox = rel ? cx : 0.0f, oy = rel ? cy : 0.0f;
        f32 v[7];
        auto need = [&](u32 n) {
            for (u32 i = 0; i < n; ++i) if (!r.number(&v[i])) return false;
            return true;
        };
        bool ok = true;
        switch (C) {
        case 'M':
            ok = need(2);
            if (ok) { move_to(ox + v[0], oy + v[1]); cmd = rel ? 'l' : 'L'; } // pairs after M are lines
            break;
        case 'L': ok = need(2); if (ok) line_to(ox + v[0], oy + v[1]); break;
        case 'H': ok = need(1); if (ok) line_to(ox + v[0], cy); break;
        case 'V': ok = need(1); if (ok) line_to(cx, (rel ? cy : 0.0f) + v[0]); break;
        case 'C':
            ok = need(6);
            if (ok) { cubic_to(ox + v[0], oy + v[1], ox + v[2], oy + v[3], ox + v[4], oy + v[5]); lcx = ox + v[2]; lcy = oy + v[3]; }
            break;
        case 'S':
            ok = need(4);
            if (ok) {
                f32 x1 = (last == 'C' || last == 'S') ? 2.0f * cx - lcx : cx, y1 = (last == 'C' || last == 'S') ? 2.0f * cy - lcy : cy;
                cubic_to(x1, y1, ox + v[0], oy + v[1], ox + v[2], oy + v[3]);
                lcx = ox + v[0]; lcy = oy + v[1];
            }
            break;
        case 'Q':
            ok = need(4);
            if (ok) { quad_to(ox + v[0], oy + v[1], ox + v[2], oy + v[3]); lcx = ox + v[0]; lcy = oy + v[1]; }
            break;
        case 'T':
            ok = need(2);
            if (ok) {
                f32 x1 = (last == 'Q' || last == 'T') ? 2.0f * cx - lcx : cx, y1 = (last == 'Q' || last == 'T') ? 2.0f * cy - lcy : cy;
                quad_to(x1, y1, ox + v[0], oy + v[1]);
                lcx = x1; lcy = y1;
            }
            break;
        case 'A': {
            bool large = false, sweep = false;
            ok = r.number(&v[0]) && r.number(&v[1]) && r.number(&v[2]) && r.flag(&large) && r.flag(&sweep) && r.number(&v[3]) && r.number(&v[4]);
            if (ok) arc_to(*this, v[0], v[1], v[2], large, sweep, ox + v[3], oy + v[4]);
            break;
        }
        case 'Z': close(); cmd = 0; last = 'Z'; continue;
        default: ok = false;
        }
        if (!ok) { if (at) *at = (u32)(r.p - d); return false; }
        last = C;
    }
    return true;
}

} // namespace kv
