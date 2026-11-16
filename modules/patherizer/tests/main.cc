// patherizer's tests (docs/PRDs/Krome-Slice-5-PRD.md, milestone 1):
//   - coverage against what geometry says (rectangles, a circle's area, a half-plane);
//   - the fill rules on self-crossing and nested outlines;
//   - gradients against their formulas;
//   - the corpus and std's icons against resvg's pictures (references/, made by
//     tools/patherizer-refs.sh), within the picture tool's tolerance;
//   - the corpus's bytes hashed: the same on every platform.
//
// `patherizer_tests --write-svg DIR` writes the corpus as SVG files, for resvg.
#include "korvid/patherizer/patherizer.h"
#include "korvid/core/format.h"
#include "korvid/core/memory.h"
#include "korvid/core/test.h"
#include "korvid/image/png.h"
#include "korvid/math/scalar.h"
#include <stdio.h>
#include <string.h>

namespace {

using namespace kv;

// ---- the corpus: each shape in a 100 x 100 view box --------------------------------------------

const f32 kK = 0.5522847498f; // a quarter circle's cubic handle

void circle(PathBuilder& b, f32 cx, f32 cy, f32 r, bool reverse = false) {
    f32 k = r * kK;
    b.move_to(cx + r, cy);
    if (!reverse) {
        b.cubic_to(cx + r, cy + k, cx + k, cy + r, cx, cy + r);
        b.cubic_to(cx - k, cy + r, cx - r, cy + k, cx - r, cy);
        b.cubic_to(cx - r, cy - k, cx - k, cy - r, cx, cy - r);
        b.cubic_to(cx + k, cy - r, cx + r, cy - k, cx + r, cy);
    } else {
        b.cubic_to(cx + r, cy - k, cx + k, cy - r, cx, cy - r);
        b.cubic_to(cx - k, cy - r, cx - r, cy - k, cx - r, cy);
        b.cubic_to(cx - r, cy + k, cx - k, cy + r, cx, cy + r);
        b.cubic_to(cx + k, cy + r, cx + r, cy + k, cx + r, cy);
    }
    b.close();
}

void rect(PathBuilder& b, f32 x, f32 y, f32 w, f32 h) {
    b.move_to(x, y); b.line_to(x + w, y); b.line_to(x + w, y + h); b.line_to(x, y + h); b.close();
}

void star(PathBuilder& b) {
    // A pentagram drawn as one self-crossing outline.
    const f32 pi = 3.14159265358979f;
    for (u32 i = 0; i < 5; ++i) {
        f32 a = -pi * 0.5f + (f32)(i * 2 % 5) * 2.0f * pi / 5.0f;
        f32 x = 50.0f + 44.0f * kv::cos(a), y = 52.0f + 44.0f * kv::sin(a);
        if (i == 0) b.move_to(x, y); else b.line_to(x, y);
    }
    b.close();
}

// min_size: the smallest size compared with resvg's picture, which is supersampled (16 x 16
// samples a pixel, tools/patherizer-refs.sh). Below it the two differ by what each is, not by
// a mistake, and only the hash holds the picture:
//   - where several edges cross inside one pixel (a pentagram's points at 16 px), the
//     accumulated winding is the cell method's approximation (FreeType's too);
//   - where a gradient changes much inside one pixel (a repeat every few pixels), a pixel's
//     centre is not its average.
struct Case { const char* name; void (*make)(PathShape&); u32 min_size = 16; };

PathPaint linear(f32 x1, f32 y1, f32 x2, f32 y2, GradientSpread sp = GradientSpread::Pad) {
    PathPaint p; p.kind = PathPaint::Linear; p.g[0] = x1; p.g[1] = y1; p.g[2] = x2; p.g[3] = y2; p.spread = sp; return p;
}
PathPaint radial(f32 cx, f32 cy, f32 r, f32 fx, f32 fy, GradientSpread sp = GradientSpread::Pad) {
    PathPaint p; p.kind = PathPaint::Radial; p.g[0] = cx; p.g[1] = cy; p.g[2] = r; p.g[3] = fx; p.g[4] = fy; p.spread = sp; return p;
}

const Case kCases[] = {
    {"rect", [](PathShape& s) { PathBuilder b(s); rect(b, 20, 20, 60, 40); b.fill(s.add_solid(30, 120, 220, 255)); }},
    {"triangle", [](PathShape& s) { PathBuilder b(s); b.move_to(50, 8); b.line_to(92, 90); b.line_to(8, 76); b.close(); b.fill(s.add_solid(220, 60, 40, 255)); }},
    {"circle", [](PathShape& s) { PathBuilder b(s); circle(b, 50, 50, 40); b.fill(s.add_solid(20, 160, 90, 255)); }},
    {"star_nonzero", [](PathShape& s) { PathBuilder b(s); star(b); b.fill(s.add_solid(240, 180, 20, 255), FillRule::NonZero); }, 48},
    {"star_evenodd", [](PathShape& s) { PathBuilder b(s); star(b); b.fill(s.add_solid(240, 180, 20, 255), FillRule::EvenOdd); }, 48},
    {"ring_evenodd", [](PathShape& s) { PathBuilder b(s); circle(b, 50, 50, 42); circle(b, 50, 50, 24); b.fill(s.add_solid(120, 40, 200, 255), FillRule::EvenOdd); }},
    {"ring_nonzero", [](PathShape& s) { PathBuilder b(s); circle(b, 50, 50, 42); circle(b, 50, 50, 24, true); b.fill(s.add_solid(120, 40, 200, 255), FillRule::NonZero); }},
    {"quads", [](PathShape& s) {
        PathBuilder b(s);
        b.move_to(10, 80); b.quad_to(30, 0, 50, 60); b.quad_to(70, 0, 90, 80); b.quad_to(50, 100, 10, 80); b.close();
        b.fill(s.add_solid(200, 90, 150, 255));
    }},
    {"blob", [](PathShape& s) {
        PathBuilder b(s);
        b.move_to(50, 6); b.cubic_to(95, 6, 98, 50, 80, 70); b.cubic_to(62, 92, 20, 98, 12, 62); b.cubic_to(4, 30, 20, 6, 50, 6); b.close();
        b.fill(s.add_solid(60, 70, 80, 255));
    }},
    {"translucent", [](PathShape& s) {
        PathBuilder b(s);
        circle(b, 38, 40, 28); b.fill(s.add_solid(255, 0, 0, 160));
        circle(b, 62, 40, 28); b.fill(s.add_solid(0, 255, 0, 160));
        circle(b, 50, 62, 28); b.fill(s.add_solid(0, 0, 255, 160));
    }},
    {"linear", [](PathShape& s) {
        PathBuilder b(s); rect(b, 10, 10, 80, 80);
        PathStop st[2] = {{0.0f, {255, 200, 0, 255}}, {1.0f, {20, 40, 200, 255}}};
        b.fill(s.add_gradient(linear(10, 10, 90, 90), st, 2));
    }},
    {"linear3", [](PathShape& s) {
        PathBuilder b(s); rect(b, 10, 30, 80, 40);
        PathStop st[3] = {{0.0f, {255, 0, 0, 255}}, {0.5f, {255, 255, 255, 64}}, {1.0f, {0, 0, 255, 255}}};
        b.fill(s.add_gradient(linear(20, 0, 80, 0), st, 3));
    }},
    {"radial", [](PathShape& s) {
        PathBuilder b(s); circle(b, 50, 50, 45);
        PathStop st[2] = {{0.0f, {255, 255, 255, 255}}, {1.0f, {0, 80, 160, 255}}};
        b.fill(s.add_gradient(radial(50, 50, 45, 50, 50), st, 2));
    }},
    {"radial_focus", [](PathShape& s) {
        PathBuilder b(s); rect(b, 5, 5, 90, 90);
        PathStop st[3] = {{0.0f, {255, 240, 200, 255}}, {0.6f, {220, 80, 30, 255}}, {1.0f, {40, 10, 10, 255}}};
        b.fill(s.add_gradient(radial(50, 50, 40, 35, 35), st, 3));
    }, 48},
    {"reflect", [](PathShape& s) {
        PathBuilder b(s); rect(b, 0, 0, 100, 100);
        PathStop st[2] = {{0.0f, {0, 0, 0, 255}}, {1.0f, {255, 255, 255, 255}}};
        b.fill(s.add_gradient(linear(40, 0, 60, 0, GradientSpread::Reflect), st, 2));
    }, 48},
    {"repeat", [](PathShape& s) {
        PathBuilder b(s); rect(b, 0, 0, 100, 100);
        // The same colour at both ends: a repeat with no jump at each wrap, which a
        // supersampled reference would blur and a pixel's centre would not.
        PathStop st[3] = {{0.0f, {0, 120, 0, 255}}, {0.5f, {200, 255, 200, 255}}, {1.0f, {0, 120, 0, 255}}};
        b.fill(s.add_gradient(radial(50, 50, 15, 50, 50, GradientSpread::Repeat), st, 3));
    }, 200},
    {"turned", [](PathShape& s) {
        PathBuilder b(s);
        u32 g = b.begin_group();
        PathTransform t;
        f32 a = 0.5f, c = kv::cos(a), sn = kv::sin(a);
        t.m[0] = c; t.m[1] = sn; t.m[2] = -sn; t.m[3] = c; t.m[4] = 50; t.m[5] = 50;
        s.nodes[g].transform = t;
        rect(b, -30, -12, 60, 24); b.fill(s.add_solid(10, 10, 10, 255));
        b.end_group(g);
    }},
    {"needle", [](PathShape& s) { PathBuilder b(s); b.move_to(2, 50); b.line_to(98, 47); b.line_to(98, 53.5f); b.close(); b.fill(s.add_solid(0, 0, 0, 255)); }},
    {"arcs", [](PathShape& s) {
        PathBuilder b(s);
        b.svg_path("M 20 50 A 30 20 0 0 1 80 50 A 30 30 0 1 1 20 50 Z m 30 -10 a 10 10 0 1 0 0.01 0 z");
        b.fill(s.add_solid(90, 30, 60, 255), FillRule::EvenOdd);
    }},
};

// ---- the corpus as SVG, for resvg ------------------------------------------------------------------

void svg_color(char* out, usize cap, const u8 c[4]) { format(out, cap, "rgb(%u,%u,%u)\" fill-opacity=\"%g", c[0], c[1], c[2], c[3] / 255.0); }

bool write_svg(const PathShape& s, const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"%g %g %g %g\" width=\"100\" height=\"100\">\n", s.view[0], s.view[1], s.view[2], s.view[3]);
    fprintf(f, "<defs>\n");
    for (u32 i = 1; i < s.paints.len; ++i) {
        const PathPaint& p = s.paints[i];
        if (p.kind != PathPaint::Linear && p.kind != PathPaint::Radial) continue;
        const char* spread = p.spread == GradientSpread::Reflect ? "reflect" : p.spread == GradientSpread::Repeat ? "repeat" : "pad";
        const f32* m = p.transform.m;
        if (p.kind == PathPaint::Linear)
            fprintf(f, "<linearGradient id=\"p%u\" gradientUnits=\"userSpaceOnUse\" spreadMethod=\"%s\" gradientTransform=\"matrix(%g %g %g %g %g %g)\" x1=\"%g\" y1=\"%g\" x2=\"%g\" y2=\"%g\">\n",
                    i, spread, m[0], m[1], m[2], m[3], m[4], m[5], p.g[0], p.g[1], p.g[2], p.g[3]);
        else
            fprintf(f, "<radialGradient id=\"p%u\" gradientUnits=\"userSpaceOnUse\" spreadMethod=\"%s\" gradientTransform=\"matrix(%g %g %g %g %g %g)\" cx=\"%g\" cy=\"%g\" r=\"%g\" fx=\"%g\" fy=\"%g\">\n",
                    i, spread, m[0], m[1], m[2], m[3], m[4], m[5], p.g[0], p.g[1], p.g[2], p.g[3], p.g[4]);
        for (u32 k = 0; k < p.stop_count; ++k) {
            const PathStop& st = s.stops[p.first_stop + k];
            fprintf(f, "<stop offset=\"%g\" stop-color=\"rgb(%u,%u,%u)\" stop-opacity=\"%g\"/>\n", st.offset, st.rgba[0], st.rgba[1], st.rgba[2], st.rgba[3] / 255.0);
        }
        fprintf(f, p.kind == PathPaint::Linear ? "</linearGradient>\n" : "</radialGradient>\n");
    }
    fprintf(f, "</defs>\n");
    Array<u32> open;
    open.alloc = mem_general();
    for (u32 i = 0; i < s.nodes.len; ++i) {
        while (open.len && i > open.last()) { fprintf(f, "</g>\n"); open.pop(); }
        const PathNode& n = s.nodes[i];
        const f32* m = n.transform.m;
        if (n.kind == PathNode::Group) {
            fprintf(f, "<g transform=\"matrix(%g %g %g %g %g %g)\">\n", m[0], m[1], m[2], m[3], m[4], m[5]);
            open.push(i + n.count);
            continue;
        }
        fprintf(f, "<path transform=\"matrix(%g %g %g %g %g %g)\" fill-rule=\"%s\" d=\"", m[0], m[1], m[2], m[3], m[4], m[5], n.rule == FillRule::EvenOdd ? "evenodd" : "nonzero");
        const f32* p = s.points.data + n.first_point;
        for (u32 k = 0; k < n.verb_count; ++k) {
            switch (s.verbs[n.first_verb + k]) {
            case PathVerb::Move: fprintf(f, "M%.9g %.9g ", p[0], p[1]); p += 2; break;
            case PathVerb::Line: fprintf(f, "L%.9g %.9g ", p[0], p[1]); p += 2; break;
            case PathVerb::Quad: fprintf(f, "Q%.9g %.9g %.9g %.9g ", p[0], p[1], p[2], p[3]); p += 4; break;
            case PathVerb::Cubic: fprintf(f, "C%.9g %.9g %.9g %.9g %.9g %.9g ", p[0], p[1], p[2], p[3], p[4], p[5]); p += 6; break;
            case PathVerb::Close: fprintf(f, "Z "); break;
            }
        }
        const PathPaint& fp = s.paints[n.fill];
        char col[96];
        if (fp.kind == PathPaint::Solid) svg_color(col, sizeof col, fp.rgba);
        else format(col, sizeof col, "url(#p%u)", n.fill);
        fprintf(f, "\" fill=\"%s\" opacity=\"%g\"/>\n", col, n.fill_opacity);
    }
    while (open.len) { fprintf(f, "</g>\n"); open.pop(); }
    open.free();
    fprintf(f, "</svg>\n");
    fclose(f);
    return true;
}

// ---- std's icons: the path data of the vendored files -----------------------------------------------

struct Icon { char name[64]; char d[4096]; f32 view[4]; };

// The view box and every path's d of an SVG file as Material writes them (no more is read
// here: the svg module is milestone 3's).
bool read_icon(const char* path, Icon& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char buf[8192];
    usize n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    const char* vb = strstr(buf, "viewBox=\"");
    if (!vb || sscanf(vb + 9, "%f %f %f %f", &out.view[0], &out.view[1], &out.view[2], &out.view[3]) != 4) return false;
    out.d[0] = 0;
    for (const char* at = strstr(buf, " d=\""); at; at = strstr(at + 4, " d=\"")) {
        const char* end = strchr(at + 4, '"');
        if (!end) return false;
        usize len = strlen(out.d), add = (usize)(end - (at + 4));
        if (len + add + 2 >= sizeof out.d) return false;
        memcpy(out.d + len, at + 4, add);
        out.d[len + add] = ' ';
        out.d[len + add + 1] = 0;
    }
    return out.d[0] != 0;
}

void icons_dir(char* out, usize cap) {
    const char* here = __FILE__;
    const char* slash = strrchr(here, '/');
    format(out, cap, "%.*s/../../krome/assets/icons", slash ? (int)(slash - here) : 1, slash ? here : ".");
}
void refs_dir(char* out, usize cap) {
    const char* here = __FILE__;
    const char* slash = strrchr(here, '/');
    format(out, cap, "%.*s/references", slash ? (int)(slash - here) : 1, slash ? here : ".");
}

u32 icon_names(char names[][64], u32 cap) {
    char dir[512], path[600];
    icons_dir(dir, sizeof dir);
    format(path, sizeof path, "%s/../../icons/NAMES", dir);
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    u32 n = 0;
    char line[128];
    while (n < cap && fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        line[strcspn(line, "\r\n")] = 0;
        format(names[n++], 64, "%s", line);
    }
    fclose(f);
    return n;
}

// ---- drawing and comparing ---------------------------------------------------------------------------

struct Picture {
    Array<u8> px;
    u32 size = 0;
};

void draw(const PathShape& s, u32 size, Picture& out, const u8* current = nullptr) {
    out.px.alloc = mem_general();
    out.px.clear();
    out.size = size;
    u8* p = out.px.push_n(size * size * 4);
    memset(p, 0, (usize)size * size * 4);
    PathTarget t{p, size, size, size * 4, false};
    PathOptions o;
    if (current) memcpy(o.current, current, 4);
    patherizer_draw(s, t, o);
}

u64 hash(const u8* p, usize n, u64 h = 0xcbf29ce484222325ull) {
    for (usize i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

// Against resvg's picture: premultiplied channels within 8 levels, all but 0.5% of the
// pixels (the picture tool's defaults); the largest difference.
bool against_reference(TestCtx& c, const char* name, u32 size, const Picture& got, u32* worst_out) {
    char dir[512], path[700];
    refs_dir(dir, sizeof dir);
    format(path, sizeof path, "%s/%s_%u.png", dir, name, size);
    FILE* f = fopen(path, "rb");
    if (!f) { KV_ERROR("test", "no reference %s (tools/patherizer-refs.sh makes them)", path); return false; }
    Array<u8> bytes;
    bytes.alloc = mem_general();
    u8 buf[65536];
    for (usize n; (n = fread(buf, 1, sizeof buf, f)) > 0;) memcpy(bytes.push_n((u32)n), buf, n);
    fclose(f);
    PngImage ref;
    const char* why = "";
    bool ok = png_decode(bytes.data, bytes.len, mem_general(), ref, &why) && ref.width == size && ref.height == size;
    bytes.free();
    if (!ok) { KV_ERROR("test", "%s: %s", path, *why ? why : "the wrong size"); return false; }
    u32 off = 0, worst = 0;
    for (u32 i = 0; i < size * size; ++i) {
        const u8* a = got.px.data + i * 4;
        const u8* b = ref.rgba + i * 4;
        bool bad = false;
        for (u32 k = 0; k < 4; ++k) {
            u32 pa = k < 3 ? (a[k] * a[3] + 127) / 255 : a[3], pb = k < 3 ? (b[k] * b[3] + 127) / 255 : b[3];
            u32 d = pa > pb ? pa - pb : pb - pa;
            if (d > worst) worst = d;
            if (d > 8) bad = true;
        }
        off += bad;
    }
    free_in(mem_general(), ref.rgba, (usize)size * size * 4);
    *worst_out = worst;
    f64 frac = (f64)off / (size * size);
    if (frac > 0.005) KV_ERROR("test", "%s at %u: %u pixels past 8 (%.2f%%), largest %u", name, size, off, frac * 100.0, worst);
    (void)c;
    return frac <= 0.005;
}

const u32 kSizes[] = {16, 24, 48, 200};

// ---- the tests -------------------------------------------------------------------------------------------

void test_coverage(TestCtx& c) {
    c.begin("coverage against geometry");
    PathShape s;
    s.init(mem_general());
    s.view[2] = s.view[3] = 100;
    PathBuilder b(s);
    // A rectangle on pixel edges: full inside, empty outside, nothing between.
    rect(b, 20, 30, 40, 20);
    b.fill(s.add_solid(255, 255, 255, 255));
    Picture p;
    draw(s, 100, p);
    u32 partial = 0, inside = 0;
    for (u32 y = 0; y < 100; ++y)
        for (u32 x = 0; x < 100; ++x) {
            u8 a = p.px[(y * 100 + x) * 4 + 3];
            bool in = x >= 20 && x < 60 && y >= 30 && y < 50;
            if (in && a == 255) ++inside;
            if (a != 0 && a != 255) ++partial;
            if (!in && a != 0) ++partial;
        }
    KV_CHECK(c, inside == 40 * 20 && partial == 0);
    // Half a pixel in: half covered, exactly.
    s.clear();
    PathBuilder b2(s);
    rect(b2, 10.5f, 10, 20, 10);
    b2.fill(s.add_solid(255, 255, 255, 255));
    draw(s, 100, p);
    KV_CHECK(c, p.px[(15 * 100 + 10) * 4 + 3] == 128 && p.px[(15 * 100 + 30) * 4 + 3] == 128 && p.px[(15 * 100 + 20) * 4 + 3] == 255);
    // A circle's area: the sum of its coverage, within 0.5% of pi r^2.
    s.clear();
    PathBuilder b3(s);
    circle(b3, 50, 50, 40);
    b3.fill(s.add_solid(255, 255, 255, 255));
    draw(s, 100, p);
    f64 sum = 0;
    for (u32 i = 0; i < 100 * 100; ++i) sum += p.px[i * 4 + 3] / 255.0;
    f64 want = 3.14159265358979 * 40.0 * 40.0;
    KV_CHECK(c, sum > want * 0.995 && sum < want * 1.005);
    // A half-plane at an angle: a pixel's coverage is the area of it under the line, to a
    // 255th. The line y = 0.25 x + 20.3 crosses column 40 (x 40..41) between y 30.3 and 30.55:
    // pixel row 30 covers 1 - (0.3 + 0.55) / 2 = 0.575 of it below.
    s.clear();
    PathBuilder b4(s);
    b4.move_to(0, 20.3f); b4.line_to(100, 45.3f); b4.line_to(100, 100); b4.line_to(0, 100); b4.close();
    b4.fill(s.add_solid(255, 255, 255, 255));
    draw(s, 100, p);
    u8 a = p.px[(30 * 100 + 40) * 4 + 3];
    KV_CHECK(c, a >= 145 && a <= 148); // 0.575 * 255 = 146.6
    p.px.free();
    s.free();
}

void test_rules(TestCtx& c) {
    c.begin("fill rules");
    PathShape s;
    s.init(mem_general());
    s.view[2] = s.view[3] = 100;
    Picture p;
    // The pentagram's centre: wound twice. Nonzero fills it, even-odd leaves it.
    const FillRule rules[2] = {FillRule::NonZero, FillRule::EvenOdd};
    for (FillRule r : rules) {
        s.clear();
        PathBuilder b(s);
        star(b);
        b.fill(s.add_solid(255, 255, 255, 255), r);
        draw(s, 100, p);
        u8 centre = p.px[(52 * 100 + 50) * 4 + 3], arm = p.px[(20 * 100 + 50) * 4 + 3];
        if (r == FillRule::NonZero) KV_CHECK(c, centre == 255 && arm == 255);
        else KV_CHECK(c, centre == 0 && arm == 255);
    }
    // Two circles wound the same way: nonzero fills the hole, even-odd does not; wound
    // opposite ways, nonzero leaves the hole too.
    for (u32 k = 0; k < 3; ++k) {
        s.clear();
        PathBuilder b(s);
        circle(b, 50, 50, 42);
        circle(b, 50, 50, 24, k == 2);
        b.fill(s.add_solid(255, 255, 255, 255), k == 1 ? FillRule::EvenOdd : FillRule::NonZero);
        draw(s, 100, p);
        u8 hole = p.px[(50 * 100 + 50) * 4 + 3], band = p.px[(50 * 100 + 15) * 4 + 3];
        KV_CHECK(c, band == 255 && hole == (k == 0 ? 255 : 0));
    }
    p.px.free();
    s.free();
}

void test_gradients(TestCtx& c) {
    c.begin("gradients");
    PathShape s;
    s.init(mem_general());
    s.view[2] = 256; s.view[3] = 4;
    PathBuilder b(s);
    rect(b, 0, 0, 256, 4);
    PathStop st[2] = {{0.0f, {0, 0, 0, 255}}, {1.0f, {255, 255, 255, 255}}};
    b.fill(s.add_gradient(linear(0, 0, 256, 0), st, 2));
    Picture p;
    p.px.alloc = mem_general();
    u8* px = p.px.push_n(256 * 4 * 4);
    memset(px, 0, 256 * 4 * 4);
    PathTarget t{px, 256, 4, 256 * 4, false};
    patherizer_draw(s, t, PathOptions{});
    // Pixel x's centre is t = (x + 0.5) / 256: the value is 255 t, sRGB, within one.
    bool ok = true;
    for (u32 x = 0; x < 256; ++x) {
        i32 want = (i32)((x + 0.5) / 256.0 * 255.0 + 0.5), got = px[(256 + x) * 4];
        if (got - want > 1 || want - got > 1) ok = false;
    }
    KV_CHECK(c, ok);
    // In linear light the middle is brighter: 0.5 linear is sRGB 188.
    s.clear();
    PathBuilder b2(s);
    rect(b2, 0, 0, 256, 4);
    PathPaint lp = linear(0, 0, 256, 0);
    lp.linear_rgb = true;
    b2.fill(s.add_gradient(lp, st, 2));
    memset(px, 0, 256 * 4 * 4);
    patherizer_draw(s, t, PathOptions{});
    u8 mid = px[(256 + 128) * 4];
    KV_CHECK(c, mid >= 186 && mid <= 190);
    // currentColor is the option's.
    s.clear();
    PathBuilder b3(s);
    rect(b3, 0, 0, 256, 4);
    b3.fill(s.add_current());
    KV_CHECK(c, s.one_colour());
    memset(px, 0, 256 * 4 * 4);
    PathOptions o;
    o.current[0] = 10; o.current[1] = 200; o.current[2] = 30; o.current[3] = 255;
    patherizer_draw(s, t, o);
    KV_CHECK(c, px[0] == 10 && px[1] == 200 && px[2] == 30 && px[3] == 255);
    p.px.free();
    s.free();
}

// The corpus and the icons at each size: against resvg's pictures, and hashed.
void test_corpus(TestCtx& c) {
    c.begin("the corpus against resvg's pictures, and its hash");
    PathShape s;
    s.init(mem_general());
    Picture p;
    u64 h = 0xcbf29ce484222325ull;
    u32 worst = 0, compared = 0;
    char worst_at[96] = "";
    for (const Case& k : kCases) {
        s.clear();
        s.view[0] = s.view[1] = 0; s.view[2] = s.view[3] = 100;
        k.make(s);
        for (u32 size : kSizes) {
            draw(s, size, p);
            h = hash(p.px.data, p.px.len, h);
            if (size < k.min_size) continue;
            u32 w = 0;
            char name[96];
            format(name, sizeof name, "%s", k.name);
            KV_CHECK(c, against_reference(c, name, size, p, &w));
            if (w > worst) { worst = w; format(worst_at, sizeof worst_at, "%s at %u", name, size); }
            ++compared;
        }
    }
    char names[64][64];
    u32 n = icon_names(names, 64);
    KV_CHECK(c, n >= 20);
    char dir[512], path[700];
    icons_dir(dir, sizeof dir);
    static Icon icon;
    const u8 black[4] = {0, 0, 0, 255};
    for (u32 i = 0; i < n; ++i) {
        format(path, sizeof path, "%s/%s.svg", dir, names[i]);
        KV_CHECK(c, read_icon(path, icon));
        s.clear();
        memcpy(s.view, icon.view, sizeof s.view);
        PathBuilder b(s);
        u32 at = 0;
        KV_CHECK(c, b.svg_path(icon.d, &at));
        b.fill(s.add_current());
        for (u32 size : kSizes) {
            draw(s, size, p, black);
            h = hash(p.px.data, p.px.len, h);
            char name[96];
            format(name, sizeof name, "icon_%s", names[i]);
            u32 w = 0;
            KV_CHECK(c, against_reference(c, name, size, p, &w));
            if (w > worst) { worst = w; format(worst_at, sizeof worst_at, "%s at %u", name, size); }
            ++compared;
        }
    }
    KV_INFO("test", "%u pictures against resvg's, largest difference %u (%s); corpus hash %016llx", compared, worst, worst_at, (unsigned long long)h);
    // The same bytes everywhere: this is Linux's hash (dev and debug alike). Another
    // platform that gets another has drawn a pixel differently.
    const u64 kHash = 0x75ec614d43dcf830ull;
    if (kHash) KV_CHECK(c, h == kHash);
    p.px.free();
    s.free();
}

int write_corpus(const char* dir) {
    PathShape s;
    s.init(mem_general());
    char path[700];
    for (const Case& k : kCases) {
        s.clear();
        s.view[0] = s.view[1] = 0; s.view[2] = s.view[3] = 100;
        k.make(s);
        format(path, sizeof path, "%s/%s.svg", dir, k.name);
        if (!write_svg(s, path)) { fprintf(stderr, "cannot write %s\n", path); return 1; }
    }
    s.free();
    printf("%u corpus shapes written to %s\n", (u32)KV_COUNTOF(kCases), dir);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && !strcmp(argv[1], "--write-svg")) return write_corpus(argv[2]);
    TestCtx c;
    test_coverage(c);
    test_rules(c);
    test_gradients(c);
    test_corpus(c);
    kv::mem_shutdown();
    return test_report(c, "patherizer_tests");
}
