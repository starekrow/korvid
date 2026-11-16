// svg's tests (docs/PRDs/Krome-Slice-5-PRD.md, milestone 3):
//   - the test files (files/), converted and drawn, against resvg's pictures of them
//     (references/, made by tools/patherizer-refs.sh), within the picture tool's tolerance;
//   - std's icons converted from their files draw the bytes their path data drew through the
//     builder (patherizer's tests hold those against resvg);
//   - every refusal names its line, and lenient makes it a warning;
//   - a shape through .kshape and back draws the same bytes.
#include "korvid/svg/svg.h"
#include "korvid/core/format.h"
#include "korvid/core/memory.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include "korvid/image/png.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

using namespace kv;

void here(char* out, usize cap, const char* rel) {
    const char* f = __FILE__;
    const char* slash = strrchr(f, '/');
    format(out, cap, "%.*s/%s", slash ? (int)(slash - f) : 1, slash ? f : ".", rel);
}

bool read_file(const char* path, Array<u8>& out) {
    out.alloc = mem_general();
    out.clear();
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    u8 buf[65536];
    for (usize n; (n = fread(buf, 1, sizeof buf, f)) > 0;) memcpy(out.push_n((u32)n), buf, n);
    fclose(f);
    return true;
}

void draw(const PathShape& s, u32 size, Array<u8>& px, const u8* current = nullptr) {
    px.alloc = mem_general();
    px.clear();
    u8* p = px.push_n(size * size * 4);
    memset(p, 0, (usize)size * size * 4);
    PathTarget t{p, size, size, size * 4, false};
    PathOptions o;
    if (current) memcpy(o.current, current, 4);
    patherizer_draw(s, t, o);
}

bool convert(const char* path, PathShape& s, bool lenient = false) {
    Array<u8> text;
    if (!read_file(path, text)) { KV_ERROR("test", "cannot read %s", path); return false; }
    SvgMessages m;
    SvgOptions o;
    o.lenient = lenient;
    bool ok = svg_to_shape((const char*)text.data, text.len, o, s, m);
    for (u32 i = 0; i < m.items.len; ++i) {
        char line[300];
        m.format_one(i, path, line, sizeof line);
        KV_INFO("test", "%s", line);
    }
    text.free();
    m.free();
    return ok;
}

// Premultiplied channels within 8 levels on all but 0.5% of the pixels.
bool against(const char* name, u32 size, const Array<u8>& got) {
    char path[600], rel[128];
    format(rel, sizeof rel, "references/%s_%u.png", name, size);
    here(path, sizeof path, rel);
    Array<u8> bytes;
    if (!read_file(path, bytes)) { KV_ERROR("test", "no reference %s", path); return false; }
    PngImage ref;
    const char* why = "";
    bool ok = png_decode(bytes.data, bytes.len, mem_general(), ref, &why) && ref.width == size && ref.height == size;
    bytes.free();
    if (!ok) return false;
    u32 off = 0, worst = 0;
    for (u32 i = 0; i < size * size; ++i) {
        const u8* a = got.data + i * 4;
        const u8* b = ref.rgba + i * 4;
        bool bad = false;
        for (u32 k = 0; k < 4; ++k) {
            u32 pa = k < 3 ? (a[k] * a[3] + 127) / 255 : a[3], pb = k < 3 ? (b[k] * b[3] + 127) / 255 : b[3];
            u32 d = pa > pb ? pa - pb : pb - pa;
            worst = d > worst ? d : worst;
            bad |= d > 8;
        }
        off += bad;
    }
    free_in(mem_general(), ref.rgba, (usize)size * size * 4);
    f64 frac = (f64)off / (size * size);
    // KV_SVG_DUMP=DIR: a picture that differs is written there.
    if (const char* dump = getenv("KV_SVG_DUMP"); dump && off) {
        Array<u8> png;
        png.alloc = mem_general();
        const char* e = "";
        if (png_encode(got.data, size, size, 4, png, &e)) {
            char out[700];
            format(out, sizeof out, "%s/%s_%u.ours.png", dump, name, size);
            if (FILE* w = fopen(out, "wb")) { fwrite(png.data, 1, png.len, w); fclose(w); }
        }
        png.free();
    }
    if (frac > 0.005) KV_ERROR("test", "%s at %u: %u pixels past 8 (%.2f%%), largest %u", name, size, off, frac * 100.0, worst);
    return frac <= 0.005;
}

// At 200 px: what is under test here is what SVG means, and at small sizes the pictures also
// differ by what patherizer's coverage is (conflation where a fill's edge and a thin stroke
// share pixels, coincident edges under a clip, gradients that change inside a pixel), which
// patherizer's own tests hold at every size.
void test_files(TestCtx& c) {
    c.begin("the test files against resvg's pictures");
    const char* const files[] = {"shapes", "structure", "paint"};
    const u32 sizes[] = {200};
    PathShape s;
    s.init(mem_general());
    Array<u8> px;
    for (const char* f : files) {
        char path[600], rel[128];
        format(rel, sizeof rel, "files/%s.svg", f);
        here(path, sizeof path, rel);
        KV_CHECK(c, convert(path, s, true)); // structure.svg and paint.svg have nothing refused; shapes.svg's title is skipped
        for (u32 size : sizes) {
            draw(s, size, px);
            KV_CHECK(c, against(f, size, px));
        }
    }
    px.free();
    s.free();
}

void test_icons(TestCtx& c) {
    c.begin("the icons, converted, draw as their path data did");
    char names_path[600];
    here(names_path, sizeof names_path, "../../krome/icons/NAMES");
    Array<u8> names;
    KV_CHECK(c, read_file(names_path, names));
    names.push(0);
    PathShape a, b;
    a.init(mem_general());
    b.init(mem_general());
    Array<u8> pa, pb;
    const u8 black[4] = {0, 0, 0, 255};
    u32 count = 0;
    for (char* line = (char*)names.data; line && *line;) {
        char* end = strchr(line, '\n');
        if (end) *end = 0;
        if (line[0] && line[0] != '#') {
            char path[600], rel[200];
            format(rel, sizeof rel, "../../krome/assets/icons/%s.svg", line);
            here(path, sizeof path, rel);
            KV_CHECK(c, convert(path, a));
            KV_CHECK(c, a.one_colour());
            // The same path data through the builder, as patherizer's tests draw it.
            Array<u8> text;
            read_file(path, text);
            text.push(0);
            const char* d = strstr((const char*)text.data, " d=\"");
            b.clear();
            memcpy(b.view, a.view, sizeof b.view);
            if (d) {
                char data[4096];
                const char* e = strchr(d + 4, '"');
                format(data, sizeof data, "%.*s", e ? (int)(e - (d + 4)) : 0, d + 4);
                PathBuilder pbld(b);
                pbld.svg_path(data);
                pbld.fill(b.add_current());
            }
            text.free();
            const u32 sizes[2] = {16, 48};
            for (u32 size : sizes) {
                draw(a, size, pa, black);
                draw(b, size, pb, black);
                KV_CHECK(c, pa.len == pb.len && !memcmp(pa.data, pb.data, pa.len));
            }
            ++count;
        }
        line = end ? end + 1 : nullptr;
    }
    KV_CHECK(c, count >= 20);
    names.free(); pa.free(); pb.free();
    a.free(); b.free();
}

void test_refusals(TestCtx& c) {
    c.begin("refusals with their lines");
    const char* text =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 10 10\">\n"  // 1
        "  <rect width=\"4\" height=\"4\"/>\n"                                    // 2
        "  <text x=\"1\" y=\"8\">hi</text>\n"                                    // 3
        "  <circle r=\"2\" cx=\"7\" cy=\"7\" filter=\"url(#f)\"/>\n"           // 4
        "  <path d=\"M1 1 L 2 x\"/>\n"                                           // 5
        "</svg>\n";
    PathShape s;
    s.init(mem_general());
    SvgMessages m;
    KV_CHECK(c, !svg_to_shape(text, strlen(text), SvgOptions{}, s, m));
    bool text3 = false, filter4 = false, path5 = false;
    for (const SvgMessage& msg : m.items) {
        text3 |= msg.line == 3 && msg.error && strstr(msg.text, "<text>");
        filter4 |= msg.line == 4 && msg.error && strstr(msg.text, "filter");
        path5 |= msg.line == 5 && msg.error && strstr(msg.text, "path data");
    }
    KV_CHECK(c, text3 && filter4 && path5 && m.errors == 3);
    char line[300];
    m.format_one(0, "x.svg", line, sizeof line);
    KV_CHECK(c, !strncmp(line, "x.svg:3: ", 9));
    // Lenient: the refusals are warnings; the path data error is still an error.
    SvgOptions lenient;
    lenient.lenient = true;
    KV_CHECK(c, !svg_to_shape(text, strlen(text), lenient, s, m) && m.errors == 1);
    // No viewBox and no size: refused on line 1.
    const char* bare = "<svg xmlns=\"http://www.w3.org/2000/svg\"><rect width=\"1\" height=\"1\"/></svg>";
    KV_CHECK(c, !svg_to_shape(bare, strlen(bare), SvgOptions{}, s, m) && m.items.len && m.items[0].line == 1);
    m.free();
    s.free();
}

void test_kshape(TestCtx& c) {
    c.begin(".kshape out and back");
    char path[600];
    here(path, sizeof path, "files/paint.svg");
    PathShape a, b;
    a.init(mem_general());
    b.init(mem_general());
    KV_CHECK(c, convert(path, a, true));
    Array<u8> bytes;
    bytes.alloc = mem_general();
    KV_CHECK(c, patherizer_write_kshape(a, bytes));
    const char* why = "";
    KV_CHECK(c, patherizer_read_kshape(bytes.data, bytes.len, b, &why));
    Array<u8> pa, pb;
    draw(a, 64, pa);
    draw(b, 64, pb);
    KV_CHECK(c, pa.len == pb.len && !memcmp(pa.data, pb.data, pa.len));
    // The lines came through; a truncated file is refused.
    bool lines = false;
    for (const PathNode& n : b.nodes) lines |= n.line > 1;
    KV_CHECK(c, lines);
    KV_CHECK(c, !patherizer_read_kshape(bytes.data, bytes.len / 2, b, &why));
    bytes.free(); pa.free(); pb.free();
    a.free(); b.free();
}

} // namespace

// --bench: the slice's numbers (docs/PRDs/Krome-Slice-5-PRD.md, KP17), on this machine, in
// this build. Each time is the best of five runs of a batch, divided by the batch.
f64 best_of(const PathShape& s, u32 size, u32 batch) {
    Array<u8> px;
    px.alloc = mem_general();
    u8* p = px.push_n(size * size * 4);
    f64 best = 1e9;
    for (u32 run = 0; run < 5; ++run) {
        f64 t0 = time_now();
        for (u32 i = 0; i < batch; ++i) {
            memset(p, 0, (usize)size * size * 4);
            patherizer_draw(s, PathTarget{p, size, size, size * 4, false}, PathOptions{});
        }
        f64 t = (time_now() - t0) / batch;
        if (t < best) best = t;
    }
    px.free();
    return best;
}

int bench() {
    const char* names[] = {"check", "remove", "expand_more", "expand_less", "chevron_left", "chevron_right",
                           "arrow_drop_down", "arrow_right", "close", "add", "search", "menu", "arrow_upward",
                           "arrow_downward", "info", "warning", "error", "folder", "draft", "settings", "refresh"};
    const u32 n = sizeof names / sizeof names[0];
    PathShape icons[n];
    for (u32 i = 0; i < n; ++i) {
        icons[i].init(mem_general());
        char path[600], rel[128];
        format(rel, sizeof rel, "../../krome/assets/icons/%s.svg", names[i]);
        here(path, sizeof path, rel);
        if (!convert(path, icons[i])) return 1;
    }
    // An icon at 16, 32 and 64: the mean over the 21 icons, and the slowest.
    const u32 sizes[3] = {16, 32, 64};
    for (u32 size : sizes) {
        f64 sum = 0, worst = 0;
        const char* slow = "";
        for (u32 i = 0; i < n; ++i) {
            f64 t = best_of(icons[i], size, 200);
            sum += t;
            if (t > worst) { worst = t; slow = names[i]; }
        }
        printf("icon %2u px: mean %6.1f us, slowest %6.1f us (%s)\n", size, sum / n * 1e6, worst * 1e6, slow);
    }
    // The first frame of a screen of 50 icons at 20 logical pixels: 50 rasterizations (the 21
    // icons, then again at 21 and 22 px so each is its own entry), at a scale of 1 and of 2.
    const u32 scales[2] = {1, 2};
    for (u32 scale : scales) {
        f64 total = 0;
        for (u32 k = 0; k < 50; ++k) total += best_of(icons[k % n], (20 + k / n) * scale, 20);
        printf("50 icons at 20 px, scale %u: %.2f ms\n", scale, total * 1e3);
    }
    // An illustration at 1000 px: the test files.
    const char* files[3] = {"shapes", "structure", "paint"};
    for (const char* f : files) {
        char path[600], rel[128];
        format(rel, sizeof rel, "files/%s.svg", f);
        here(path, sizeof path, rel);
        PathShape s;
        s.init(mem_general());
        if (!convert(path, s)) return 1;
        printf("%s.svg at 1000 px: %.2f ms (%u nodes)\n", f, best_of(s, 1000, 2) * 1e3, s.nodes.len);
        s.free();
    }
    for (PathShape& s : icons) s.free();
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "--bench")) return bench();
    TestCtx c;
    test_files(c);
    test_icons(c);
    test_refusals(c);
    test_kshape(c);
    kv::mem_shutdown();
    return test_report(c, "svg_tests");
}
