// patherizer: strokes as fills (patherizer.h, docs/PRDs/Krome-Slice-5-PRD.md). A stroke is
// widened in the path's own space, as SVG widens it before the transform, into outlines that
// the fill draws with the nonzero rule:
//   - an open piece is one outline: its left side forward with the joins, the end cap, its
//     right side back, the start cap;
//   - a closed subpath is two outlines wound opposite ways, its outer and inner sides, so
//     the ring between them winds once and its middle not at all;
//   - on a turn's inner side the outline pivots through the vertex itself: its overlap is
//     inside the stroke, where nonzero fills it anyway.
// Dashes cut each subpath into open pieces first.
#include "stroke.h"
#include "korvid/math/scalar.h"

namespace kv {

namespace {

const f32 kPi = 3.14159265358979f;

struct V2 { f32 x, y; };

V2 sub(V2 a, V2 b) { return V2{a.x - b.x, a.y - b.y}; }
V2 add(V2 a, V2 b) { return V2{a.x + b.x, a.y + b.y}; }
V2 mul(V2 a, f32 k) { return V2{a.x * k, a.y * k}; }
f32 dot(V2 a, V2 b) { return a.x * b.x + a.y * b.y; }
f32 cross(V2 a, V2 b) { return a.x * b.y - a.y * b.x; }
f32 len(V2 a) { return kv::sqrt(a.x * a.x + a.y * a.y); }

struct Out {
    StrokeOutlines& o;
    void begin() { o.starts.push(o.points.len); }
    void pt(V2 p) { o.points.push(p.x); o.points.push(p.y); }
};

// Points on an arc about `c` of radius r from angle a0 through `sweep` (signed), excluding the
// start, including the end; the step keeps each chord within the tolerance.
void arc(Out& out, V2 c, f32 r, f32 a0, f32 sweep, f32 tol) {
    f32 step = r > tol ? 2.0f * kv::acos(1.0f - tol / r) : kPi / 2.0f;
    if (!(step > 0.01f)) step = 0.01f;
    u32 n = (u32)kv::ceil(kv::fabs(sweep) / step);
    if (n < 1) n = 1;
    if (n > 512) n = 512;
    for (u32 i = 1; i <= n; ++i) {
        f32 a = a0 + sweep * (f32)i / (f32)n;
        out.pt(V2{c.x + r * kv::cos(a), c.y + r * kv::sin(a)});
    }
}

// The join at vertex v on the side `side` (+1 left, -1 right) going from direction d0 to d1
// (unit): the outline is at v + side n0 hw and goes to v + side n1 hw.
// l0 and l1 are the two segments' lengths.
void join(Out& out, V2 v, V2 d0, V2 d1, f32 l0, f32 l1, f32 hw, f32 side, const PathStroke& st, f32 tol) {
    V2 n0{-d0.y * side, d0.x * side}, n1{-d1.y * side, d1.x * side};
    V2 a = add(v, mul(n0, hw)), b = add(v, mul(n1, hw));
    f32 turn = cross(d0, d1) * side; // > 0: this side is the inner one
    if (turn >= -1e-6f) {
        // The inner side (or straight on). The outline turns where the two offset edges
        // cross: always for a gentle turn (a curve's), whose crossing is near even when it
        // falls past a short segment (where a dash cut a chord), and for a sharp one when it
        // falls within both segments. Past them at a sharp turn it pivots through the vertex.
        // A pivot's loop overlaps the stroke, and the cell method counts an overlap twice
        // inside a pixel: kept for where nothing else will do.
        if (turn > 1e-6f) {
            V2 m = add(n0, n1);
            f32 ml = len(m), c = ml > 1e-6f ? dot(mul(m, 1.0f / ml), n0) : 0.0f;
            f32 reach = c > 1e-6f ? hw * kv::sqrt(1.0f - c * c) / c : 1e30f; // along each segment from the vertex
            if (c > 0.866f || (reach <= l0 && reach <= l1)) { out.pt(add(v, mul(m, hw / (c * ml)))); return; }
            out.pt(v);
        }
        out.pt(b);
        return;
    }
    if (st.join == LineJoin::Round) {
        // The outer side's normals turn the short way from n0 to n1.
        f32 a0 = kv::atan2(n0.y, n0.x), sweep = kv::atan2(cross(n0, n1), dot(n0, n1));
        out.pt(a);
        arc(out, v, hw, a0, sweep, tol);
        return;
    }
    if (st.join == LineJoin::Miter) {
        V2 m = add(n0, n1);
        f32 ml = len(m);
        if (ml > 1e-6f) {
            m = mul(m, 1.0f / ml);
            f32 c = dot(m, n0); // cos of half the angle between the normals
            if (c > 1e-6f && 1.0f / c <= st.miter_limit) {
                out.pt(add(v, mul(m, hw / c)));
                out.pt(b);
                return;
            }
        }
    }
    out.pt(a);
    out.pt(b); // bevel
}

// The cap at p, d pointing out of the piece: from the side `from_side` (+1: the left of d) round
// the end to the other side.
void cap(Out& out, V2 p, V2 d, f32 hw, f32 from_side, const PathStroke& st, f32 tol) {
    V2 n{-d.y, d.x};
    V2 a = add(p, mul(n, hw * from_side)), b = add(p, mul(n, -hw * from_side));
    if (st.cap == LineCap::Square) {
        V2 e = mul(d, hw);
        out.pt(add(a, e));
        out.pt(add(b, e));
        out.pt(b);
    } else if (st.cap == LineCap::Round) {
        f32 a0 = kv::atan2(a.y - p.y, a.x - p.x);
        // Round the outside: the half turn through d.
        f32 sweep = from_side * cross(n, d) > 0.0f ? kPi : -kPi;
        arc(out, p, hw, a0, sweep, tol);
    } else {
        out.pt(b);
    }
}

// How far along each segment from vertex b the inner offset edges of a -> b -> c cross.
f32 inner_reach(V2 a, V2 b, V2 c, f32 hw) {
    V2 d0 = sub(b, a), d1 = sub(c, b);
    f32 l0 = len(d0), l1 = len(d1);
    if (l0 <= 0.0f || l1 <= 0.0f) return 0.0f;
    f32 cs = dot(d0, d1) / (l0 * l1); // cos of the turn
    if (cs >= 1.0f) return 0.0f;
    // tan of half the turn: sqrt((1 - cos) / (1 + cos)).
    return cs > -0.999f ? hw * kv::sqrt((1.0f - cs) / (1.0f + cs)) : 1e30f;
}

// One open piece of a polyline: its outline (the points may be changed).
void open_piece(Out& out, V2* p, u32 n, f32 hw, const PathStroke& st, f32 tol) {
    // An end segment shorter than the inner join's reach at its far vertex (a dash cut a
    // chord near its end) would fold the outline back over the cap: that vertex goes, and
    // the piece runs straight to the next point, within the flattening's tolerance of the
    // curve.
    V2 tmp[3];
    if (n > 2 && len(sub(p[1], p[0])) < inner_reach(p[0], p[1], p[2], hw)) {
        tmp[0] = p[0];
        if (n == 3) { tmp[1] = p[2]; p = tmp; n = 2; }
        else { p = p + 1; n -= 1; p[0] = tmp[0]; }
    }
    if (n > 2 && len(sub(p[n - 1], p[n - 2])) < inner_reach(p[n - 3], p[n - 2], p[n - 1], hw)) {
        p[n - 2] = p[n - 1];
        n -= 1;
    }
    if (n == 1) {
        // A zero-length piece: a round cap is a dot, a square cap a square; butt is nothing.
        if (st.cap == LineCap::Butt) return;
        out.begin();
        if (st.cap == LineCap::Round) { out.pt(V2{p[0].x + hw, p[0].y}); arc(out, p[0], hw, 0.0f, 2.0f * kPi, tol); }
        else { out.pt(V2{p[0].x - hw, p[0].y - hw}); out.pt(V2{p[0].x + hw, p[0].y - hw}); out.pt(V2{p[0].x + hw, p[0].y + hw}); out.pt(V2{p[0].x - hw, p[0].y + hw}); }
        return;
    }
    out.begin();
    V2 d0 = mul(sub(p[1], p[0]), 1.0f / len(sub(p[1], p[0])));
    out.pt(add(p[0], mul(V2{-d0.y, d0.x}, hw)));
    // The left side forward.
    for (u32 i = 1; i + 1 < n; ++i) {
        f32 la = len(sub(p[i], p[i - 1])), lb = len(sub(p[i + 1], p[i]));
        V2 a = mul(sub(p[i], p[i - 1]), 1.0f / la), b = mul(sub(p[i + 1], p[i]), 1.0f / lb);
        out.pt(add(p[i], mul(V2{-a.y, a.x}, hw)));
        join(out, p[i], a, b, la, lb, hw, 1.0f, st, tol);
    }
    V2 de = mul(sub(p[n - 1], p[n - 2]), 1.0f / len(sub(p[n - 1], p[n - 2])));
    out.pt(add(p[n - 1], mul(V2{-de.y, de.x}, hw)));
    cap(out, p[n - 1], de, hw, 1.0f, st, tol);
    // The right side back.
    for (u32 i = n - 2; i >= 1; --i) {
        f32 la = len(sub(p[i], p[i + 1])), lb = len(sub(p[i - 1], p[i]));
        V2 a = mul(sub(p[i], p[i + 1]), 1.0f / la), b = mul(sub(p[i - 1], p[i]), 1.0f / lb);
        out.pt(add(p[i], mul(V2{-a.y, a.x}, hw)));
        join(out, p[i], a, b, la, lb, hw, 1.0f, st, tol);
    }
    out.pt(add(p[0], mul(V2{d0.y, -d0.x}, hw)));
    cap(out, p[0], mul(d0, -1.0f), hw, 1.0f, st, tol);
}

// A closed polyline (its last point not repeating its first): two outlines.
void closed(Out& out, const V2* p, u32 n, f32 hw, const PathStroke& st, f32 tol) {
    for (i32 side = 1; side >= -1; side -= 2) {
        out.begin();
        for (u32 k = 0; k < n; ++k) {
            // Going forward on the left, backward on the right: the right outline is the left
            // outline of the reversed polyline.
            u32 i = side > 0 ? k : n - 1 - k;
            u32 prev = side > 0 ? (i + n - 1) % n : (i + 1) % n, next = side > 0 ? (i + 1) % n : (i + n - 1) % n;
            f32 la = len(sub(p[i], p[prev])), lb = len(sub(p[next], p[i]));
            V2 a = mul(sub(p[i], p[prev]), 1.0f / la), b = mul(sub(p[next], p[i]), 1.0f / lb);
            out.pt(add(p[i], mul(V2{-a.y, a.x}, hw)));
            join(out, p[i], a, b, la, lb, hw, 1.0f, st, tol);
        }
    }
}

// Points with the zero-length steps taken out.
u32 clean(const f32* in, u32 n, V2* out) {
    u32 m = 0;
    for (u32 i = 0; i < n; ++i) {
        V2 q{in[i * 2], in[i * 2 + 1]};
        if (m && q.x == out[m - 1].x && q.y == out[m - 1].y) continue;
        out[m++] = q;
    }
    return m;
}

} // namespace

void stroke_polyline(StrokeOutlines& o, const f32* pts, u32 n, bool is_closed, const PathStroke& st, const f32* dashes, f32 tol) {
    if (!n || !(st.width > 0.0f)) return;
    f32 hw = st.width * 0.5f;
    Out out{o};
    Array<V2> p;
    p.alloc = o.points.alloc;
    V2* buf = p.push_n(n + 1);
    u32 m = clean(pts, n, buf);
    if (is_closed && m > 1 && buf[m - 1].x == buf[0].x && buf[m - 1].y == buf[0].y) --m;
    // Dashes: the pieces that are on, each open.
    f32 total = 0.0f;
    for (u32 i = 0; i < st.dash_count; ++i) total += dashes[i] > 0.0f ? dashes[i] : 0.0f;
    if (st.dash_count && total > 0.0f && m > 1) {
        if (is_closed) buf[m++] = buf[0];
        u32 di = 0;
        f32 left = 0.0f;
        bool on = true;
        // The offset into the pattern, as SVG's dashoffset: start that far in.
        f32 off = st.dash_offset - total * kv::floor(st.dash_offset / total);
        for (;;) {
            left = dashes[di] > 0.0f ? dashes[di] : 0.0f;
            if (off < left) { left -= off; break; }
            off -= left;
            di = (di + 1) % st.dash_count;
            on = !on;
        }
        // The dashes that are on, as pieces (points in `all`, each piece's start in `cut`),
        // then stroked. A closed subpath on at its start and at its end has one dash across
        // the seam, not two capped ones meeting there: the last piece is put before the first.
        Array<V2> all;
        Array<u32> cut;
        all.alloc = o.points.alloc;
        cut.alloc = o.points.alloc;
        bool on_at_start = on;
        if (on) { cut.push(0); all.push(buf[0]); }
        for (u32 i = 0; i + 1 < m; ++i) {
            V2 a = buf[i], b = buf[i + 1];
            f32 l = len(sub(b, a)), at = 0.0f;
            while (l - at > left) {
                at += left;
                V2 q = add(a, mul(sub(b, a), at / l));
                if (!on) cut.push(all.len);
                all.push(q);
                on = !on;
                di = (di + 1) % st.dash_count;
                left = dashes[di] > 0.0f ? dashes[di] : 0.0f;
            }
            left -= l - at;
            if (on) all.push(b);
        }
        u32 pieces = cut.len;
        bool seam = is_closed && on && on_at_start && pieces >= 2;
        Array<V2> joined;
        joined.alloc = o.points.alloc;
        for (u32 k = seam ? 1 : 0; k < pieces; ++k) {
            u32 a = cut[k], b = k + 1 < pieces ? cut[k + 1] : all.len;
            if (seam && k == pieces - 1) {
                // The last piece, then the first after its shared point.
                joined.clear();
                for (u32 i = a; i < b; ++i) joined.push(all[i]);
                for (u32 i = 1; i < cut[1]; ++i) joined.push(all[i]);
                open_piece(out, joined.data, joined.len, hw, st, tol);
            } else if (b > a) {
                open_piece(out, all.data + a, b - a, hw, st, tol);
            }
        }
        joined.free();
        all.free();
        cut.free();
        p.free();
        return;
    }
    if (is_closed && m > 2) closed(out, buf, m, hw, st, tol);
    else open_piece(out, buf, m, hw, st, tol);
    p.free();
}

} // namespace kv
