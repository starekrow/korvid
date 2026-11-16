// patherizer: drawing a shape (patherizer.h). Each path is flattened to lines in the target's
// pixels, its coverage accumulated in integer cells, and its paint composited over a 16-bit
// premultiplied canvas, which is the target's pixels going in and coming out.
//
// Coverage is FreeType's grey rasterizer's: positions in 1/256 pixel; each line adds to the
// cells it crosses its signed height (cover) and that height times the sum of its x offsets
// in the cell at entry and exit (area); a sweep along a row turns the cover of the cells to
// the left plus the cell's own into the signed area of the pixel inside the outline. The
// fill rule then reads that winding area: nonzero clamps it, even-odd folds it. It is exact
// for edges that do not overlap within a pixel. All of it is integer: the bytes are the
// same on every platform. Float is the flattening, the transforms and the gradients, on the
// engine's math with contraction off.
#include "korvid/patherizer/patherizer.h"
#include "stroke.h"
#include "korvid/core/memory.h"
#include "korvid/math/scalar.h"
#include <string.h>

namespace kv {

namespace {

constexpr i32 kSub = 256;             // subpixels a pixel
constexpr i32 kBand = 64;             // rows rasterized at once
// A flattened curve's furthest distance from the curve, pixels. A chord inside a curve takes
// coverage from the pixels it passes: at 0.02 pixel, at most about five levels in 255.
constexpr f32 kTolerance = 0.02f;

struct Segment { i32 x0, y0, x1, y1; }; // 24.8, y0 != y1

struct Draw {
    const PathShape& s;
    const PathOptions& o;
    Allocator* a;
    u32 w, h;
    u16* canvas;          // w x h x 4, premultiplied, sRGB-encoded values times 257
    Array<f32> poly;      // the subpaths' points, flattened
    Array<u32> starts;    // where each subpath starts in poly (floats)
    Array<u8>  closed;    // whether each was closed
    Array<Segment> segs;
    StrokeOutlines outlines;
    Array<u16> mask;      // a clip's coverage, w x h, while one is being made
    bool       to_mask = false;
    Array<i32> cover, area;
    Array<u16> cov;       // a band's coverage, 0 to 65535 (bw x kBand)
    Draw(const PathShape& shape, const PathOptions& options, Allocator* al, u32 width, u32 height)
        : s(shape), o(options), a(al), w(width), h(height), canvas(nullptr) {
        poly.alloc = starts.alloc = segs.alloc = al;
        closed.alloc = al;
        outlines.points.alloc = outlines.starts.alloc = al;
        mask.alloc = al;
        cover.alloc = area.alloc = al;
        cov.alloc = al;
    }
};

i32 fixed(f32 v) { return (i32)kv::floor(v * (f32)kSub + 0.5f); }

i64 floor_div(i64 a, i64 b) { i64 q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }

// ---- flattening -------------------------------------------------------------------------------------

void add_line(Draw& d, f32 x0, f32 y0, f32 x1, f32 y1) {
    Segment sg{fixed(x0), fixed(y0), fixed(x1), fixed(y1)};
    if (sg.y0 != sg.y1) d.segs.push(sg);
}

void point(Draw& d, f32 x, f32 y) { d.poly.push(x); d.poly.push(y); }

void begin_sub(Draw& d) {
    // An empty or one-point subpath is replaced by the next.
    if (d.starts.len && d.poly.len - d.starts.last() <= 2) {
        d.poly.len = d.starts.last();
        d.starts.pop();
        d.closed.pop();
    }
    d.starts.push(d.poly.len);
    d.closed.push(0);
}

// Segments a curve is cut into for the tolerance (Wang's bound on the second differences).
u32 pieces(f32 dd, f32 tol) {
    if (!(dd > 0.0f)) return 1;
    f32 n = kv::ceil(kv::sqrt(dd / tol));
    if (!(n < 4096.0f)) return 4096;
    return n < 1.0f ? 1 : (u32)n;
}

void flatten_quad(Draw& d, f32 tol, f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2) {
    f32 ddx = x0 - 2.0f * x1 + x2, ddy = y0 - 2.0f * y1 + y2;
    u32 n = pieces(0.25f * kv::sqrt(ddx * ddx + ddy * ddy), tol);
    for (u32 i = 1; i <= n; ++i) {
        f32 t = (f32)i / (f32)n, u = 1.0f - t;
        if (i == n) { point(d, x2, y2); break; }
        point(d, u * u * x0 + 2.0f * u * t * x1 + t * t * x2, u * u * y0 + 2.0f * u * t * y1 + t * t * y2);
    }
}

void flatten_cubic(Draw& d, f32 tol, f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2, f32 x3, f32 y3) {
    f32 ax = x0 - 2.0f * x1 + x2, ay = y0 - 2.0f * y1 + y2;
    f32 bx = x1 - 2.0f * x2 + x3, by = y1 - 2.0f * y2 + y3;
    f32 la = kv::sqrt(ax * ax + ay * ay), lb = kv::sqrt(bx * bx + by * by);
    u32 n = pieces(0.75f * (la > lb ? la : lb), tol);
    for (u32 i = 1; i <= n; ++i) {
        f32 t = (f32)i / (f32)n, u = 1.0f - t;
        if (i == n) { point(d, x3, y3); break; }
        f32 c0 = u * u * u, c1 = 3.0f * u * u * t, c2 = 3.0f * u * t * t, c3 = t * t * t;
        point(d, c0 * x0 + c1 * x1 + c2 * x2 + c3 * x3, c0 * y0 + c1 * y1 + c2 * y2 + c3 * y3);
    }
}

// A path's subpaths as polylines (poly, starts, closed), through `t`, to `tol` in the
// units `t` gives.
void flatten_path(Draw& d, const PathNode& n, const PathTransform& t, f32 tol) {
    d.poly.clear();
    d.starts.clear();
    d.closed.clear();
    const f32* p = d.s.points.data + n.first_point;
    f32 cx = 0, cy = 0, sx = 0, sy = 0;
    bool open = false;
    for (u32 i = 0; i < n.verb_count; ++i) {
        PathVerb v = d.s.verbs[n.first_verb + i];
        f32 q[6];
        u32 k = v == PathVerb::Move || v == PathVerb::Line ? 1 : v == PathVerb::Quad ? 2 : v == PathVerb::Cubic ? 3 : 0;
        for (u32 j = 0; j < k; ++j) t.apply(p[j * 2], p[j * 2 + 1], &q[j * 2], &q[j * 2 + 1]);
        p += k * 2;
        if (v != PathVerb::Move && v != PathVerb::Close && !open) {
            // A drawing command with no subpath open starts one where the last ended.
            begin_sub(d);
            point(d, cx, cy);
            open = true;
        }
        switch (v) {
        case PathVerb::Move: begin_sub(d); point(d, q[0], q[1]); cx = sx = q[0]; cy = sy = q[1]; open = true; break;
        case PathVerb::Line: point(d, q[0], q[1]); cx = q[0]; cy = q[1]; break;
        case PathVerb::Quad: flatten_quad(d, tol, cx, cy, q[0], q[1], q[2], q[3]); cx = q[2]; cy = q[3]; break;
        case PathVerb::Cubic: flatten_cubic(d, tol, cx, cy, q[0], q[1], q[2], q[3], q[4], q[5]); cx = q[4]; cy = q[5]; break;
        case PathVerb::Close:
            if (open) d.closed.last() = 1;
            open = false;
            cx = sx; cy = sy; // a command after a close starts from the subpath's start
            break;
        }
    }
    if (d.starts.len && d.poly.len - d.starts.last() < 2) { d.poly.len = d.starts.last(); d.starts.pop(); d.closed.pop(); }
}

// Every subpath as a closed outline's lines: a fill closes what is open.
void fill_lines(Draw& d) {
    d.segs.clear();
    for (u32 k = 0; k < d.starts.len; ++k) {
        u32 a = d.starts[k], b = k + 1 < d.starts.len ? d.starts[k + 1] : d.poly.len;
        u32 n = (b - a) / 2;
        const f32* q = d.poly.data + a;
        for (u32 i = 0; i + 1 < n; ++i) add_line(d, q[i * 2], q[i * 2 + 1], q[i * 2 + 2], q[i * 2 + 3]);
        if (n >= 2) add_line(d, q[(n - 1) * 2], q[(n - 1) * 2 + 1], q[0], q[1]);
    }
}

// How far `t` stretches a length at most: the tolerance in the path's own units is the
// target's divided by it.
f32 stretch(const PathTransform& t) {
    f32 a = kv::sqrt(t.m[0] * t.m[0] + t.m[1] * t.m[1]), b = kv::sqrt(t.m[2] * t.m[2] + t.m[3] * t.m[3]);
    f32 k = a > b ? a : b;
    return k > 1e-12f ? k : 1e-12f;
}

// A path's stroke, widened in its own space, as lines in the target's pixels.
void stroke_lines(Draw& d, const PathNode& n, const PathTransform& t) {
    f32 tol = kTolerance / stretch(t);
    flatten_path(d, n, PathTransform{}, tol);
    const PathStroke& st = d.s.strokes[n.stroke_style < d.s.strokes.len ? n.stroke_style : 0];
    d.outlines.points.clear();
    d.outlines.starts.clear();
    for (u32 k = 0; k < d.starts.len; ++k) {
        u32 a = d.starts[k], b = k + 1 < d.starts.len ? d.starts[k + 1] : d.poly.len;
        stroke_polyline(d.outlines, d.poly.data + a, (b - a) / 2, d.closed[k] != 0, st, d.s.dashes.data + st.first_dash, tol);
    }
    d.segs.clear();
    const StrokeOutlines& o = d.outlines;
    for (u32 k = 0; k < o.starts.len; ++k) {
        u32 a = o.starts[k], b = k + 1 < o.starts.len ? o.starts[k + 1] : o.points.len;
        u32 m = (b - a) / 2;
        if (m < 2) continue;
        f32 fx, fy, px, py;
        t.apply(o.points[a], o.points[a + 1], &fx, &fy);
        px = fx; py = fy;
        for (u32 i = 1; i < m; ++i) {
            f32 x, y;
            t.apply(o.points[a + i * 2], o.points[a + i * 2 + 1], &x, &y);
            add_line(d, px, py, x, y);
            px = x; py = y;
        }
        add_line(d, px, py, fx, fy);
    }
}

// ---- coverage -------------------------------------------------------------------------------------

struct Band { i32 x0, y0, bw, rows; }; // the pixels: [x0, x0 + bw) x [y0, y0 + rows)

inline void cell(Draw& d, const Band& b, i32 cx, i32 row, i32 dy, i32 fx0, i32 fx1) {
    // A cell left of the band is the band's first: its cover still counts for the row.
    i32 c = cx - b.x0;
    if (c < 0) { c = 0; fx0 = fx1 = 0; }
    if (c > b.bw) c = b.bw;
    usize at = (usize)row * (usize)(b.bw + 1) + (usize)c;
    d.cover[at] += dy;
    d.area[at] += dy * (fx0 + fx1);
}

// One segment's part in the band, its rows from top to bottom.
void accumulate(Draw& d, const Band& b, Segment s) {
    i32 sign = 1;
    if (s.y0 > s.y1) { sign = -1; Segment t{s.x1, s.y1, s.x0, s.y0}; s = t; }
    i32 top = b.y0 * kSub, bottom = (b.y0 + b.rows) * kSub;
    if (s.y1 <= top || s.y0 >= bottom) return;
    i64 dx = (i64)s.x1 - s.x0, dy = (i64)s.y1 - s.y0;
    auto x_at = [&](i32 y) -> i32 { return (i32)((i64)s.x0 + floor_div(dx * ((i64)y - s.y0), dy)); };
    i32 ya = s.y0 > top ? s.y0 : top, yb = s.y1 < bottom ? s.y1 : bottom;
    for (i32 ry = floor_div(ya, kSub); ry * kSub < yb; ++ry) {
        i32 r0 = ry * kSub, r1 = r0 + kSub;
        i32 py0 = ya > r0 ? ya : r0, py1 = yb < r1 ? yb : r1;
        if (py0 >= py1) continue;
        i32 px0 = py0 == s.y0 ? s.x0 : x_at(py0), px1 = py1 == s.y1 ? s.x1 : x_at(py1);
        i32 row = ry - b.y0;
        i32 c0 = (i32)floor_div(px0, kSub), c1 = (i32)floor_div(px1, kSub);
        if (c0 == c1) {
            cell(d, b, c0, row, sign * (py1 - py0), px0 - c0 * kSub, px1 - c0 * kSub);
            continue;
        }
        // Across cells: the y where the row's piece crosses each cell boundary.
        i64 pdx = (i64)px1 - px0, pdy = (i64)py1 - py0;
        i32 step = c1 > c0 ? 1 : -1;
        i32 cx = c0, x = px0, y = py0;
        while (cx != c1) {
            i32 bx = step > 0 ? (cx + 1) * kSub : cx * kSub;
            i32 by = (i32)((i64)py0 + floor_div(pdy * ((i64)bx - px0), pdx));
            if (by < y) by = y;
            if (by > py1) by = py1;
            cell(d, b, cx, row, sign * (by - y), x - cx * kSub, bx - cx * kSub);
            x = bx;
            y = by;
            cx += step;
        }
        cell(d, b, cx, row, sign * (py1 - y), x - cx * kSub, px1 - cx * kSub);
    }
}

// The band's coverage for a rule, 0 to 65535 a pixel, from its cells.
void sweep(Draw& d, const Band& b, FillRule rule) {
    const i64 full = 2 * kSub * kSub; // a pixel's whole area in cell units
    for (i32 row = 0; row < b.rows; ++row) {
        i64 acc = 0;
        const i32* cv = d.cover.data + (usize)row * (b.bw + 1);
        const i32* ar = d.area.data + (usize)row * (b.bw + 1);
        u16* out = d.cov.data + (usize)row * b.bw;
        for (i32 x = 0; x < b.bw; ++x) {
            i64 v = 2 * kSub * (acc + cv[x]) - ar[x];
            acc += cv[x];
            if (v < 0) v = -v;
            if (rule == FillRule::EvenOdd) {
                v %= 2 * full;
                if (v > full) v = 2 * full - v;
            } else if (v > full) {
                v = full;
            }
            out[x] = (u16)((v * 65535 + full / 2) / full);
        }
    }
}

// ---- paint --------------------------------------------------------------------------------------------

f32 srgb_to_linear(f32 c) { return c <= 0.04045f ? c / 12.92f : kv::pow((c + 0.055f) / 1.055f, 2.4f); }
f32 linear_to_srgb(f32 l) { return l <= 0.0031308f ? l * 12.92f : 1.055f * kv::pow(l, 1.0f / 2.4f) - 0.055f; }

// A gradient's colour at t, straight, sRGB 0 to 1.
void gradient_at(const PathShape& s, const PathPaint& p, f32 t, f32 out[4]) {
    if (p.spread == GradientSpread::Repeat) t = t - kv::floor(t);
    else if (p.spread == GradientSpread::Reflect) { t = kv::fabs(t); t = t - 2.0f * kv::floor(t * 0.5f); if (t > 1.0f) t = 2.0f - t; }
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const PathStop* st = s.stops.data + p.first_stop;
    u32 n = p.stop_count;
    if (!n) { out[0] = out[1] = out[2] = out[3] = 0.0f; return; }
    const PathStop* a = &st[0];
    const PathStop* b = &st[0];
    if (t <= st[0].offset) a = b = &st[0];
    else if (t >= st[n - 1].offset) a = b = &st[n - 1];
    else for (u32 i = 0; i + 1 < n; ++i) if (t >= st[i].offset && t <= st[i + 1].offset) { a = &st[i]; b = &st[i + 1]; break; }
    f32 span = b->offset - a->offset;
    f32 f = span > 0.0f ? (t - a->offset) / span : 0.0f;
    for (u32 k = 0; k < 4; ++k) {
        f32 ca = (f32)a->rgba[k] / 255.0f, cb = (f32)b->rgba[k] / 255.0f;
        if (p.linear_rgb && k < 3) { ca = srgb_to_linear(ca); cb = srgb_to_linear(cb); }
        f32 c = ca + (cb - ca) * f;
        out[k] = p.linear_rgb && k < 3 ? linear_to_srgb(c) : c;
    }
}

// t of a radial gradient at a point (in its space): the circle through the point, of the
// family from the focus (radius 0) to the end circle (t = 1).
f32 radial_t(const PathPaint& p, f32 x, f32 y) {
    f32 cx = p.g[0], cy = p.g[1], r = p.g[2], fx = p.g[3], fy = p.g[4];
    // A focus outside the circle is brought just inside it, as SVG 1.1 does.
    f32 ex = fx - cx, ey = fy - cy, dist = kv::sqrt(ex * ex + ey * ey);
    if (dist > r * 0.999f && dist > 0.0f) { f32 k = r * 0.999f / dist; fx = cx + ex * k; fy = cy + ey * k; }
    f32 pdx = x - fx, pdy = y - fy, cdx = cx - fx, cdy = cy - fy;
    f32 A = cdx * cdx + cdy * cdy - r * r, B = pdx * cdx + pdy * cdy, C = pdx * pdx + pdy * pdy;
    if (kv::fabs(A) < 1e-12f) return B != 0.0f ? C / (2.0f * B) : 0.0f;
    f32 disc = B * B - A * C;
    if (disc < 0.0f) disc = 0.0f;
    f32 sq = kv::sqrt(disc);
    // A < 0 with the focus inside: the larger root is (B - sq) / A.
    f32 t = (B - sq) / A;
    if (t < 0.0f) t = (B + sq) / A;
    return t;
}

// The paint's colour at a pixel's centre, straight 16-bit, alpha with `opacity` folded in.
struct Painter {
    const PathShape& s;
    const PathPaint& p;
    PathTransform to_paint; // the target's pixels to the gradient's space
    u32 solid[4];
    bool flat;
};

bool make_painter(const Draw& d, u32 index, const PathTransform& node, f32 opacity, Painter* out) {
    const PathPaint& p = d.s.paints[index];
    out->flat = p.kind == PathPaint::Solid || p.kind == PathPaint::Current;
    u32 op = (u32)(opacity * 65535.0f + 0.5f);
    if (opacity >= 1.0f) op = 65535;
    if (!(opacity > 0.0f)) op = 0;
    if (out->flat) {
        const u8* c = p.kind == PathPaint::Current ? d.o.current : p.rgba;
        for (u32 k = 0; k < 3; ++k) out->solid[k] = c[k] * 257u;
        out->solid[3] = (c[3] * 257u * op + 32767) / 65535;
        return true;
    }
    if (p.kind != PathPaint::Linear && p.kind != PathPaint::Radial) return false;
    PathTransform total = p.transform.then(node);
    if (!total.invert(&out->to_paint)) return false;
    out->solid[3] = op; // the opacity, for the gradient's alpha
    return true;
}

inline void paint_at(const Painter& pt, f32 x, f32 y, u32 out[4]) {
    if (pt.flat) { out[0] = pt.solid[0]; out[1] = pt.solid[1]; out[2] = pt.solid[2]; out[3] = pt.solid[3]; return; }
    f32 gx, gy;
    pt.to_paint.apply(x, y, &gx, &gy);
    f32 t;
    if (pt.p.kind == PathPaint::Linear) {
        f32 dx = pt.p.g[2] - pt.p.g[0], dy = pt.p.g[3] - pt.p.g[1], len = dx * dx + dy * dy;
        t = len > 0.0f ? ((gx - pt.p.g[0]) * dx + (gy - pt.p.g[1]) * dy) / len : 0.0f;
    } else {
        t = radial_t(pt.p, gx, gy);
    }
    f32 c[4];
    gradient_at(pt.s, pt.p, t, c);
    for (u32 k = 0; k < 3; ++k) out[k] = (u32)(c[k] * 65535.0f + 0.5f);
    u32 a = (u32)(c[3] * 65535.0f + 0.5f);
    out[3] = (u32)(((u64)a * pt.solid[3] + 32767) / 65535);
}

// Coverage times the paint, over the canvas; or, making a clip, coverage into the mask
// (their union: each path's coverage over what is there).
void composite(Draw& d, const Band& b, const Painter& pt) {
    if (d.to_mask) {
        for (i32 row = 0; row < b.rows; ++row) {
            const u16* cv = d.cov.data + (usize)row * b.bw;
            u16* m = d.mask.data + (usize)(b.y0 + row) * d.w + (usize)b.x0;
            for (i32 x = 0; x < b.bw; ++x) {
                u32 c = cv[x], v = m[x];
                m[x] = (u16)(v + c - (v * c + 32767) / 65535);
            }
        }
        return;
    }
    for (i32 row = 0; row < b.rows; ++row) {
        const u16* cv = d.cov.data + (usize)row * b.bw;
        i32 y = b.y0 + row;
        u16* px = d.canvas + ((usize)y * d.w + (usize)b.x0) * 4;
        for (i32 x = 0; x < b.bw; ++x, px += 4) {
            u32 c = cv[x];
            if (!c) continue;
            u32 col[4];
            paint_at(pt, (f32)(b.x0 + x) + 0.5f, (f32)y + 0.5f, col);
            u32 sa = (u32)(((u64)col[3] * c + 32767) / 65535);
            if (!sa) continue;
            u32 ia = 65535 - sa;
            for (u32 k = 0; k < 3; ++k) {
                u32 s = (u32)(((u64)col[k] * sa + 32767) / 65535);
                px[k] = (u16)(s + (px[k] * ia + 32767) / 65535);
            }
            px[3] = (u16)(sa + (px[3] * ia + 32767) / 65535);
        }
    }
}

// A path's fill: its segments rasterized band by band over their bounds, each band
// composited.
void fill_segments(Draw& d, FillRule rule, const Painter& pt) {
    if (!d.segs.len) return;
    i32 minx = d.segs[0].x0, maxx = minx, miny = d.segs[0].y0, maxy = miny;
    for (const Segment& s : d.segs) {
        i32 lx = s.x0 < s.x1 ? s.x0 : s.x1, hx = s.x0 < s.x1 ? s.x1 : s.x0;
        i32 ly = s.y0 < s.y1 ? s.y0 : s.y1, hy = s.y0 < s.y1 ? s.y1 : s.y0;
        minx = lx < minx ? lx : minx; maxx = hx > maxx ? hx : maxx;
        miny = ly < miny ? ly : miny; maxy = hy > maxy ? hy : maxy;
    }
    i32 px0 = (i32)floor_div(minx, kSub), px1 = (i32)floor_div(maxx, kSub) + 1;
    i32 py0 = (i32)floor_div(miny, kSub), py1 = (i32)floor_div(maxy, kSub) + 1;
    if (px0 < 0) px0 = 0;
    if (py0 < 0) py0 = 0;
    if (px1 > (i32)d.w) px1 = (i32)d.w;
    if (py1 > (i32)d.h) py1 = (i32)d.h;
    // A closed outline wholly outside the target winds to nothing inside it. One that
    // crosses its left edge keeps its windings: cells left of the target are counted in its
    // first column (cell()).
    if (px0 >= px1 || py0 >= py1) return;
    Band b;
    b.x0 = px0;
    b.bw = px1 - px0;
    if (b.bw <= 0) return;
    for (i32 y = py0; y < py1; y += kBand) {
        b.y0 = y;
        b.rows = py1 - y < kBand ? py1 - y : kBand;
        usize cells = (usize)b.rows * (b.bw + 1);
        d.cover.clear(); d.area.clear(); d.cov.clear();
        for (usize i = 0; i < cells; ++i) { d.cover.push(0); d.area.push(0); }
        for (usize i = 0; i < (usize)b.rows * b.bw; ++i) d.cov.push(0);
        for (const Segment& s : d.segs) accumulate(d, b, s);
        sweep(d, b, rule);
        composite(d, b, pt);
    }
}

// The view box to the target's pixels.
PathTransform fit(const PathShape& s, const PathOptions& o, u32 w, u32 h) {
    f32 vw = s.view[2] > 0.0f ? s.view[2] : (f32)w, vh = s.view[3] > 0.0f ? s.view[3] : (f32)h;
    f32 sx = (f32)w / vw, sy = (f32)h / vh;
    if (!o.stretch) { f32 k = sx < sy ? sx : sy; sx = sy = k; }
    f32 ox = ((f32)w - vw * sx) * 0.5f, oy = ((f32)h - vh * sy) * 0.5f;
    PathTransform t;
    t.m[0] = sx; t.m[3] = sy;
    t.m[4] = ox - s.view[0] * sx;
    t.m[5] = oy - s.view[1] * sy;
    return t;
}

bool is_clip(const PathShape& s, u32 node) {
    for (u32 c : s.clips) if (c == node) return true;
    return false;
}

void draw_nodes(Draw& d, u32 first, u32 end, const PathTransform& parent);

// The clip group `clip`'s outlines, in the user space `t` of what it clips, as the mask.
bool make_mask(Draw& d, u32 clip, const PathTransform& t) {
    usize px = (usize)d.w * d.h;
    d.mask.clear();
    u16* m = d.mask.push_n((u32)px);
    if (!m) return false;
    memset(m, 0, px * sizeof(u16));
    const PathNode& g = d.s.nodes[clip];
    bool was = d.to_mask;
    d.to_mask = true;
    draw_nodes(d, clip + 1, clip + 1 + g.count, g.transform.then(t));
    d.to_mask = was;
    return true;
}

// A layer (a group with opacity or a clip) over the canvas under it: each pixel times the
// opacity, and the clip's coverage.
void composite_layer(Draw& d, const u16* layer, u16* under, f32 opacity, const u16* mask) {
    u32 op = opacity >= 1.0f ? 65535u : !(opacity > 0.0f) ? 0u : (u32)(opacity * 65535.0f + 0.5f);
    usize n = (usize)d.w * d.h;
    for (usize i = 0; i < n; ++i) {
        const u16* l = layer + i * 4;
        if (!l[3]) continue;
        u32 k = mask ? (u32)(((u64)op * mask[i] + 32767) / 65535) : op;
        if (!k) continue;
        u16* u = under + i * 4;
        u32 sa = (l[3] * k + 32767) / 65535, ia = 65535 - sa;
        for (u32 c = 0; c < 3; ++c) {
            u32 s = (l[c] * k + 32767) / 65535;
            u32 v = s + (u[c] * ia + 32767) / 65535;
            u[c] = (u16)(v > 65535 ? 65535 : v);
        }
        u[3] = (u16)(sa + (u[3] * ia + 32767) / 65535);
    }
}

// The nodes [first, end) under `parent`, in order.
void draw_nodes(Draw& d, u32 first, u32 end, const PathTransform& parent) {
    for (u32 i = first; i < end; ++i) {
        const PathNode& n = d.s.nodes[i];
        PathTransform t = n.transform.then(parent);
        if (n.kind == PathNode::Group) {
            if (is_clip(d.s, i)) { i += n.count; continue; }
            u32 clip = n.clip && n.clip - 1 < d.s.nodes.len && d.s.nodes[n.clip - 1].kind == PathNode::Group ? n.clip - 1 : ~0u;
            bool layer = !d.to_mask && (n.opacity < 1.0f || clip != ~0u);
            if (!layer) {
                draw_nodes(d, i + 1, i + 1 + n.count, t);
                i += n.count;
                continue;
            }
            if (!(n.opacity > 0.0f)) { i += n.count; continue; }
            // Its own canvas, clear; the children into it; then over the one under it.
            usize bytes = (usize)d.w * d.h * 4 * sizeof(u16);
            u16* under = d.canvas;
            u16* mine = (u16*)alloc_in(d.a, bytes, 16);
            if (!mine) { i += n.count; continue; }
            memset(mine, 0, bytes);
            d.canvas = mine;
            draw_nodes(d, i + 1, i + 1 + n.count, t);
            d.canvas = under;
            const u16* mask = nullptr;
            Array<u16> keep;
            keep.alloc = d.a;
            if (clip != ~0u && make_mask(d, clip, t)) {
                // The mask is the Draw's, and a clip inside the clip's own content would
                // write it again: kept aside while it is used.
                keep.push_n(d.mask.len);
                memcpy(keep.data, d.mask.data, (usize)d.mask.len * sizeof(u16));
                mask = keep.data;
            }
            composite_layer(d, mine, under, n.opacity, mask);
            keep.free();
            free_in(d.a, mine, bytes);
            i += n.count;
            continue;
        }
        if (n.fill && n.fill < d.s.paints.len && (d.to_mask || d.s.paints[n.fill].kind != PathPaint::None)) {
            Painter pt{d.s, d.s.paints[n.fill], {}, {}, false};
            if (d.to_mask || make_painter(d, n.fill, t, n.fill_opacity, &pt)) {
                flatten_path(d, n, t, kTolerance);
                fill_lines(d);
                fill_segments(d, n.rule, pt);
            }
        } else if (d.to_mask && !n.stroke) {
            // A clip's path counts by its outline, whatever it is painted with.
            Painter pt{d.s, d.s.paints[0], {}, {}, false};
            flatten_path(d, n, t, kTolerance);
            fill_lines(d);
            fill_segments(d, n.rule, pt);
        }
        if (!d.to_mask && n.stroke && n.stroke < d.s.paints.len && d.s.paints[n.stroke].kind != PathPaint::None) {
            Painter pt{d.s, d.s.paints[n.stroke], {}, {}, false};
            if (make_painter(d, n.stroke, t, n.stroke_opacity, &pt)) {
                stroke_lines(d, n, t);
                fill_segments(d, FillRule::NonZero, pt);
            }
        }
    }
}

} // namespace

bool patherizer_draw(const PathShape& shape, const PathTarget& target, const PathOptions& options) {
    if (!target.pixels || !target.width || !target.height) return true;
    Allocator* a = options.scratch ? options.scratch : mem_general();
    Draw d(shape, options, a, target.width, target.height);
    usize px = (usize)d.w * d.h;
    d.canvas = (u16*)alloc_in(a, px * 4 * sizeof(u16), 16);
    if (!d.canvas) return false;
    // The target's pixels, premultiplied, in.
    for (u32 y = 0; y < d.h; ++y) {
        const u8* in = target.pixels + (usize)y * target.pitch;
        u16* c = d.canvas + (usize)y * d.w * 4;
        for (u32 x = 0; x < d.w; ++x, in += 4, c += 4) {
            u32 al = in[3] * 257u;
            for (u32 k = 0; k < 3; ++k) c[k] = (u16)(target.premultiplied ? in[k] * 257u : (in[k] * 257u * al + 32767) / 65535);
            c[3] = (u16)al;
        }
    }
    draw_nodes(d, 0, shape.nodes.len, fit(shape, options, d.w, d.h));
    // And out.
    for (u32 y = 0; y < d.h; ++y) {
        u8* out = target.pixels + (usize)y * target.pitch;
        const u16* c = d.canvas + (usize)y * d.w * 4;
        for (u32 x = 0; x < d.w; ++x, out += 4, c += 4) {
            u32 al = c[3];
            out[3] = (u8)((al + 128) / 257);
            for (u32 k = 0; k < 3; ++k) {
                u32 v = target.premultiplied ? c[k] : (al ? (u32)(((u64)c[k] * 65535 + al / 2) / al) : 0);
                if (v > 65535) v = 65535;
                out[k] = (u8)((v + 128) / 257);
            }
        }
    }
    free_in(a, d.canvas, px * 4 * sizeof(u16));
    d.poly.free(); d.segs.free(); d.cover.free(); d.area.free(); d.cov.free();
    d.starts.free(); d.closed.free(); d.outlines.points.free(); d.outlines.starts.free(); d.mask.free();
    return true;
}

} // namespace kv
