// svg: an SVG file into a patherizer shape (svg.h). The document is walked once, element by
// element, a style inherited down the tree as SVG inherits it; each drawing element's geometry
// goes through the shape builder, its paints made as it needs them (an objectBoundingBox
// gradient with the element's own bounds), its transform on its node, a group for each `g`
// and for anything with opacity or a clip.
#include "korvid/svg/svg.h"
#include "korvid/core/arena.h"
#include "korvid/core/format.h"
#include "korvid/core/memory.h"
#include "korvid/math/scalar.h"
#include "korvid/xml/xml.h"
#include <stdarg.h>
#include <string.h>

namespace kv {

void SvgMessages::format_one(u32 i, const char* name, char* out, usize cap) const {
    if (i >= items.len) { if (cap) out[0] = 0; return; }
    const SvgMessage& m = items[i];
    const char* kind = m.error ? "" : "warning: ";
    if (m.line) format(out, cap, "%s:%u: %s%s", name, m.line, kind, m.text);
    else format(out, cap, "%s: %s%s", name, kind, m.text);
}

namespace {

const f32 kPi = 3.14159265358979f;

// ---- small readers -------------------------------------------------------------------------------

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
const char* skip(const char* p) { while (is_space(*p) || *p == ',') ++p; return p; }

bool same(const char* a, const char* b) { return a && b && !strcmp(a, b); }
bool same_n(const char* a, u32 n, const char* b) { return strlen(b) == n && !memcmp(a, b, n); }

// A length: a number and a unit (px, or none; pt, pc, mm, cm, in in CSS's ratios to px), or a
// percentage of `whole` (when whole > 0). False when there is none.
bool length(const char* v, f32 whole, f32* out) {
    if (!v) return false;
    const char* p = v;
    f32 n;
    if (!patherizer_number(&p, &n)) return false;
    p = skip(p);
    if (*p == '%') { if (!(whole > 0.0f)) return false; *out = n * whole / 100.0f; return true; }
    f32 k = 1.0f;
    if (p[0] == 'p' && p[1] == 't') k = 96.0f / 72.0f;
    else if (p[0] == 'p' && p[1] == 'c') k = 16.0f;
    else if (p[0] == 'm' && p[1] == 'm') k = 96.0f / 25.4f;
    else if (p[0] == 'c' && p[1] == 'm') k = 96.0f / 2.54f;
    else if (p[0] == 'i' && p[1] == 'n') k = 96.0f;
    *out = n * k;
    return true;
}

// The CSS colour keywords.
struct Named { const char* name; u32 rgb; };
const Named kNames[] = {
    {"aliceblue", 0xf0f8ff}, {"antiquewhite", 0xfaebd7}, {"aqua", 0x00ffff}, {"aquamarine", 0x7fffd4}, {"azure", 0xf0ffff},
    {"beige", 0xf5f5dc}, {"bisque", 0xffe4c4}, {"black", 0x000000}, {"blanchedalmond", 0xffebcd}, {"blue", 0x0000ff},
    {"blueviolet", 0x8a2be2}, {"brown", 0xa52a2a}, {"burlywood", 0xdeb887}, {"cadetblue", 0x5f9ea0}, {"chartreuse", 0x7fff00},
    {"chocolate", 0xd2691e}, {"coral", 0xff7f50}, {"cornflowerblue", 0x6495ed}, {"cornsilk", 0xfff8dc}, {"crimson", 0xdc143c},
    {"cyan", 0x00ffff}, {"darkblue", 0x00008b}, {"darkcyan", 0x008b8b}, {"darkgoldenrod", 0xb8860b}, {"darkgray", 0xa9a9a9},
    {"darkgreen", 0x006400}, {"darkgrey", 0xa9a9a9}, {"darkkhaki", 0xbdb76b}, {"darkmagenta", 0x8b008b}, {"darkolivegreen", 0x556b2f},
    {"darkorange", 0xff8c00}, {"darkorchid", 0x9932cc}, {"darkred", 0x8b0000}, {"darksalmon", 0xe9967a}, {"darkseagreen", 0x8fbc8f},
    {"darkslateblue", 0x483d8b}, {"darkslategray", 0x2f4f4f}, {"darkslategrey", 0x2f4f4f}, {"darkturquoise", 0x00ced1}, {"darkviolet", 0x9400d3},
    {"deeppink", 0xff1493}, {"deepskyblue", 0x00bfff}, {"dimgray", 0x696969}, {"dimgrey", 0x696969}, {"dodgerblue", 0x1e90ff},
    {"firebrick", 0xb22222}, {"floralwhite", 0xfffaf0}, {"forestgreen", 0x228b22}, {"fuchsia", 0xff00ff}, {"gainsboro", 0xdcdcdc},
    {"ghostwhite", 0xf8f8ff}, {"gold", 0xffd700}, {"goldenrod", 0xdaa520}, {"gray", 0x808080}, {"grey", 0x808080},
    {"green", 0x008000}, {"greenyellow", 0xadff2f}, {"honeydew", 0xf0fff0}, {"hotpink", 0xff69b4}, {"indianred", 0xcd5c5c},
    {"indigo", 0x4b0082}, {"ivory", 0xfffff0}, {"khaki", 0xf0e68c}, {"lavender", 0xe6e6fa}, {"lavenderblush", 0xfff0f5},
    {"lawngreen", 0x7cfc00}, {"lemonchiffon", 0xfffacd}, {"lightblue", 0xadd8e6}, {"lightcoral", 0xf08080}, {"lightcyan", 0xe0ffff},
    {"lightgoldenrodyellow", 0xfafad2}, {"lightgray", 0xd3d3d3}, {"lightgreen", 0x90ee90}, {"lightgrey", 0xd3d3d3}, {"lightpink", 0xffb6c1},
    {"lightsalmon", 0xffa07a}, {"lightseagreen", 0x20b2aa}, {"lightskyblue", 0x87cefa}, {"lightslategray", 0x778899}, {"lightslategrey", 0x778899},
    {"lightsteelblue", 0xb0c4de}, {"lightyellow", 0xffffe0}, {"lime", 0x00ff00}, {"limegreen", 0x32cd32}, {"linen", 0xfaf0e6},
    {"magenta", 0xff00ff}, {"maroon", 0x800000}, {"mediumaquamarine", 0x66cdaa}, {"mediumblue", 0x0000cd}, {"mediumorchid", 0xba55d3},
    {"mediumpurple", 0x9370db}, {"mediumseagreen", 0x3cb371}, {"mediumslateblue", 0x7b68ee}, {"mediumspringgreen", 0x00fa9a}, {"mediumturquoise", 0x48d1cc},
    {"mediumvioletred", 0xc71585}, {"midnightblue", 0x191970}, {"mintcream", 0xf5fffa}, {"mistyrose", 0xffe4e1}, {"moccasin", 0xffe4b5},
    {"navajowhite", 0xffdead}, {"navy", 0x000080}, {"oldlace", 0xfdf5e6}, {"olive", 0x808000}, {"olivedrab", 0x6b8e23},
    {"orange", 0xffa500}, {"orangered", 0xff4500}, {"orchid", 0xda70d6}, {"palegoldenrod", 0xeee8aa}, {"palegreen", 0x98fb98},
    {"paleturquoise", 0xafeeee}, {"palevioletred", 0xdb7093}, {"papayawhip", 0xffefd5}, {"peachpuff", 0xffdab9}, {"peru", 0xcd853f},
    {"pink", 0xffc0cb}, {"plum", 0xdda0dd}, {"powderblue", 0xb0e0e6}, {"purple", 0x800080}, {"rebeccapurple", 0x663399},
    {"red", 0xff0000}, {"rosybrown", 0xbc8f8f}, {"royalblue", 0x4169e1}, {"saddlebrown", 0x8b4513}, {"salmon", 0xfa8072},
    {"sandybrown", 0xf4a460}, {"seagreen", 0x2e8b57}, {"seashell", 0xfff5ee}, {"sienna", 0xa0522d}, {"silver", 0xc0c0c0},
    {"skyblue", 0x87ceeb}, {"slateblue", 0x6a5acd}, {"slategray", 0x708090}, {"slategrey", 0x708090}, {"snow", 0xfffafa},
    {"springgreen", 0x00ff7f}, {"steelblue", 0x4682b4}, {"tan", 0xd2b48c}, {"teal", 0x008080}, {"thistle", 0xd8bfd8},
    {"tomato", 0xff6347}, {"turquoise", 0x40e0d0}, {"violet", 0xee82ee}, {"wheat", 0xf5deb3}, {"white", 0xffffff},
    {"whitesmoke", 0xf5f5f5}, {"yellow", 0xffff00}, {"yellowgreen", 0x9acd32},
};

u8 hexv(char c) { return (u8)(c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 255); }

// A colour: #rgb, #rgba, #rrggbb, #rrggbbaa, rgb(), rgba(), or a keyword. False when it is
// none of them.
bool colour(const char* v, u8 out[4]) {
    v = skip(v);
    out[3] = 255;
    if (*v == '#') {
        u32 n = 0;
        while (hexv(v[1 + n]) != 255) ++n;
        if (n == 3 || n == 4) { for (u32 k = 0; k < n; ++k) out[k] = (u8)(hexv(v[1 + k]) * 17); return true; }
        if (n == 6 || n == 8) { for (u32 k = 0; k < n / 2; ++k) out[k] = (u8)(hexv(v[1 + k * 2]) * 16 + hexv(v[2 + k * 2])); return true; }
        return false;
    }
    if (!strncmp(v, "rgb(", 4) || !strncmp(v, "rgba(", 5)) {
        const char* p = strchr(v, '(') + 1;
        for (u32 k = 0; k < 4; ++k) {
            p = skip(p);
            if (*p == ')') { if (k == 3) break; return false; }
            if (*p == '/') p = skip(p + 1);
            f32 n;
            if (!patherizer_number(&p, &n)) return false;
            p = skip(p);
            if (*p == '%') { n = k < 3 ? n * 2.55f : n / 100.0f; ++p; }
            else if (k == 3) n = n; // alpha 0 to 1
            f32 c = k < 3 ? n : n * 255.0f;
            out[k] = (u8)(c <= 0.0f ? 0 : c >= 255.0f ? 255 : (u32)(c + 0.5f));
        }
        return true;
    }
    char word[32];
    u32 n = 0;
    while (v[n] && !is_space(v[n]) && n < sizeof word - 1) { word[n] = (char)(v[n] >= 'A' && v[n] <= 'Z' ? v[n] + 32 : v[n]); ++n; }
    word[n] = 0;
    if (same(word, "transparent")) { out[0] = out[1] = out[2] = out[3] = 0; return true; }
    for (const Named& c : kNames)
        if (same(word, c.name)) { out[0] = (u8)(c.rgb >> 16); out[1] = (u8)(c.rgb >> 8); out[2] = (u8)c.rgb; return true; }
    return false;
}

// A transform list, applied right to left as SVG does. False when it cannot be read.
bool transform_list(const char* v, PathTransform* out) {
    PathTransform total;
    const char* p = skip(v);
    while (*p) {
        const char* name = p;
        while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) ++p;
        u32 nl = (u32)(p - name);
        p = skip(p);
        if (*p != '(') return false;
        ++p;
        f32 a[6];
        u32 n = 0;
        for (;;) {
            p = skip(p);
            if (*p == ')') { ++p; break; }
            if (n == 6 || !patherizer_number(&p, &a[n])) return false;
            ++n;
        }
        PathTransform t;
        if (same_n(name, nl, "matrix") && n == 6) for (u32 k = 0; k < 6; ++k) t.m[k] = a[k];
        else if (same_n(name, nl, "translate") && (n == 1 || n == 2)) { t.m[4] = a[0]; t.m[5] = n == 2 ? a[1] : 0.0f; }
        else if (same_n(name, nl, "scale") && (n == 1 || n == 2)) { t.m[0] = a[0]; t.m[3] = n == 2 ? a[1] : a[0]; }
        else if (same_n(name, nl, "rotate") && (n == 1 || n == 3)) {
            f32 r = a[0] * kPi / 180.0f, c = kv::cos(r), s = kv::sin(r);
            t.m[0] = c; t.m[1] = s; t.m[2] = -s; t.m[3] = c;
            if (n == 3) { t.m[4] = a[1] - c * a[1] + s * a[2]; t.m[5] = a[2] - s * a[1] - c * a[2]; }
        }
        else if (same_n(name, nl, "skewX") && n == 1) t.m[2] = kv::tan(a[0] * kPi / 180.0f);
        else if (same_n(name, nl, "skewY") && n == 1) t.m[1] = kv::tan(a[0] * kPi / 180.0f);
        else return false;
        total = t.then(total);
        p = skip(p);
    }
    *out = total;
    return true;
}

// ---- style ------------------------------------------------------------------------------------------

struct Paint {
    enum Kind : u8 { None, Current, Solid, Url };
    Kind kind = Solid;
    u8   rgba[4] = {0, 0, 0, 255};
    char id[64] = {};
    bool fallback = false; // a url's fallback colour, when the id is not found
    Kind fallback_kind = None;
};

struct Style {
    Paint fill, stroke;
    f32   fill_opacity = 1.0f, stroke_opacity = 1.0f;
    f32   width = 1.0f;
    LineJoin join = LineJoin::Miter;
    LineCap  cap = LineCap::Butt;
    f32   miter = 4.0f;
    f32   dashes[32] = {};
    u32   dash_count = 0;
    f32   dash_offset = 0.0f;
    FillRule fill_rule = FillRule::NonZero, clip_rule = FillRule::NonZero;
    bool  has_color = false;
    u8    color[4] = {0, 0, 0, 255};
    bool  visible = true;
    Style() { stroke.kind = Paint::None; }
};

// What an element has that is not inherited.
struct Own {
    f32  opacity = 1.0f;
    bool display = true;
    char clip[64] = {};
};

// ---- the converter ----------------------------------------------------------------------------------

struct Conv {
    const XmlDoc& doc;
    const SvgOptions& o;
    PathShape& s;
    SvgMessages& m;
    PathBuilder b;
    f32 view[4] = {0, 0, 0, 0};
    u32 use_depth = 0;
    Conv(const XmlDoc& d, const SvgOptions& opt, PathShape& shape, SvgMessages& msgs) : doc(d), o(opt), s(shape), m(msgs), b(shape) {}

    void say(u32 line, bool error, const char* fmt, ...) KV_PRINTF(4, 5);
    // A refusal: an error, or with `lenient` a warning (and the caller leaves the thing out).
    void refuse(u32 line, const char* what) { say(line, !o.lenient, "%s", what); }

    const XmlNode* by_id(const char* id) const {
        for (u32 i = 0; i < doc.count; ++i) {
            const XmlNode& n = doc.nodes[i];
            if (n.type != XmlType::Element) continue;
            const char* v = xml_attr(doc, n, "id");
            if (v && !strcmp(v, id)) return &n;
        }
        return nullptr;
    }
    const char* href(const XmlNode& n) const {
        const char* h = xml_attr(doc, n, "href");
        if (!h) h = xml_attr(doc, n, "xlink:href");
        return h && h[0] == '#' ? h + 1 : nullptr;
    }

    bool paint(const char* v, const Style& parent, Paint& out, u32 line, const char* prop);
    void property(const char* name, const char* value, const Style& parent, Style& st, Own& own, u32 line);
    void style_of(const XmlNode& n, const Style& parent, Style& st, Own& own);
    void element(const XmlNode& n, const Style& parent, bool in_clip);
    void children(const XmlNode& n, const Style& st, bool in_clip);
    bool geometry(const XmlNode& n, const char* name);
    u32  make_paint(const Paint& p, const Style& st, u32 line);
    void end_shape(const XmlNode& n, const Style& st, const PathTransform& t, bool in_clip);
    u32  clip_group(const char* id, u32 line);
    bool bounds(f32 out[4]) const;
};

void Conv::say(u32 line, bool error, const char* fmt, ...) {
    SvgMessage msg;
    msg.line = line;
    msg.error = error;
    va_list args;
    va_start(args, fmt);
    vformat(msg.text, sizeof msg.text, fmt, args);
    va_end(args);
    m.items.push(msg);
    if (error) ++m.errors;
}

bool Conv::paint(const char* v, const Style& parent, Paint& out, u32 line, const char* prop) {
    v = skip(v);
    if (!*v || same(v, "inherit")) return true; // the parent's, already in `out`
    if (same(v, "none")) { out.kind = Paint::None; return true; }
    if (!strncmp(v, "currentColor", 12) || !strncmp(v, "currentcolor", 12)) {
        out.kind = parent.has_color ? Paint::Solid : Paint::Current;
        if (parent.has_color) memcpy(out.rgba, parent.color, 4);
        return true;
    }
    if (!strncmp(v, "url(", 4)) {
        const char* p = skip(v + 4);
        if (*p == '\'' || *p == '"') ++p;
        if (*p != '#') { say(line, true, "%s: only a url(#id) in this file is read", prop); return false; }
        ++p;
        u32 n = 0;
        while (p[n] && p[n] != ')' && p[n] != '\'' && p[n] != '"' && n < sizeof out.id - 1) { out.id[n] = p[n]; ++n; }
        out.id[n] = 0;
        out.kind = Paint::Url;
        const char* rest = strchr(p, ')');
        out.fallback = false;
        if (rest) {
            rest = skip(rest + 1);
            if (*rest) {
                Paint fb;
                if (same(rest, "none")) { out.fallback = true; out.fallback_kind = Paint::None; }
                else if (colour(rest, fb.rgba)) { out.fallback = true; out.fallback_kind = Paint::Solid; memcpy(out.rgba, fb.rgba, 4); }
            }
        }
        return true;
    }
    if (!colour(v, out.rgba)) { say(line, true, "%s: '%s' is not a colour", prop, v); return false; }
    out.kind = Paint::Solid;
    return true;
}

// One property, from an attribute or from style="".
void Conv::property(const char* name, const char* value, const Style& parent, Style& st, Own& own, u32 line) {
    const char* v = skip(value);
    if (same(v, "inherit")) return;
    if (same(name, "fill")) paint(v, st, st.fill, line, name);
    else if (same(name, "stroke")) paint(v, st, st.stroke, line, name);
    else if (same(name, "color")) { if (colour(v, st.color)) st.has_color = true; else if (!same(v, "currentColor")) say(line, true, "color: '%s' is not a colour", v); }
    else if (same(name, "fill-opacity") || same(name, "stroke-opacity") || same(name, "opacity")) {
        const char* p = v;
        f32 n;
        if (!patherizer_number(&p, &n)) { say(line, true, "%s: '%s' is not a number", name, v); return; }
        if (*skip(p) == '%') n /= 100.0f;
        n = n < 0.0f ? 0.0f : n > 1.0f ? 1.0f : n;
        if (same(name, "fill-opacity")) st.fill_opacity = n;
        else if (same(name, "stroke-opacity")) st.stroke_opacity = n;
        else own.opacity = n;
    }
    else if (same(name, "stroke-width")) { f32 w; if (length(v, kv::sqrt((view[2] * view[2] + view[3] * view[3]) * 0.5f), &w) && w >= 0.0f) st.width = w; else say(line, true, "stroke-width: '%s' is not a length", v); }
    else if (same(name, "stroke-linejoin")) {
        if (same(v, "miter") || same(v, "miter-clip") || same(v, "arcs")) st.join = LineJoin::Miter;
        else if (same(v, "round")) st.join = LineJoin::Round;
        else if (same(v, "bevel")) st.join = LineJoin::Bevel;
        else say(line, true, "stroke-linejoin: '%s' is not one", v);
    }
    else if (same(name, "stroke-linecap")) {
        if (same(v, "butt")) st.cap = LineCap::Butt;
        else if (same(v, "round")) st.cap = LineCap::Round;
        else if (same(v, "square")) st.cap = LineCap::Square;
        else say(line, true, "stroke-linecap: '%s' is not one", v);
    }
    else if (same(name, "stroke-miterlimit")) { const char* p = v; f32 n; if (patherizer_number(&p, &n) && n >= 1.0f) st.miter = n; else say(line, true, "stroke-miterlimit: '%s' is not a number of at least 1", v); }
    else if (same(name, "stroke-dasharray")) {
        st.dash_count = 0;
        if (same(v, "none")) return;
        const char* p = v;
        f32 n, total = 0.0f;
        while (st.dash_count < 32 && patherizer_number(&p, &n)) {
            p = skip(p);
            if (*p == '%') ++p; // as user units: rare, and close enough
            if (n < 0.0f) { say(line, true, "stroke-dasharray: a negative length"); st.dash_count = 0; return; }
            st.dashes[st.dash_count++] = n;
            total += n;
        }
        if (st.dash_count & 1) {
            // An odd list repeats: SVG's rule.
            u32 c = st.dash_count;
            for (u32 k = 0; k < c && st.dash_count < 32; ++k) st.dashes[st.dash_count++] = st.dashes[k];
        }
        if (!(total > 0.0f)) st.dash_count = 0;
    }
    else if (same(name, "stroke-dashoffset")) { f32 n; if (length(v, 0.0f, &n)) st.dash_offset = n; }
    else if (same(name, "fill-rule") || same(name, "clip-rule")) {
        FillRule r = same(v, "evenodd") ? FillRule::EvenOdd : FillRule::NonZero;
        if (!same(v, "evenodd") && !same(v, "nonzero")) say(line, true, "%s: '%s' is not nonzero or evenodd", name, v);
        if (same(name, "fill-rule")) st.fill_rule = r; else st.clip_rule = r;
    }
    else if (same(name, "visibility")) st.visible = !(same(v, "hidden") || same(v, "collapse"));
    else if (same(name, "display")) own.display = !same(v, "none");
    else if (same(name, "clip-path")) {
        if (same(v, "none")) { own.clip[0] = 0; return; }
        const char* p = strchr(v, '#');
        if (strncmp(v, "url(", 4) || !p) { say(line, true, "clip-path: only a url(#id) in this file is read"); return; }
        u32 n = 0;
        ++p;
        while (p[n] && p[n] != ')' && p[n] != '\'' && p[n] != '"' && n < sizeof own.clip - 1) { own.clip[n] = p[n]; ++n; }
        own.clip[n] = 0;
    }
    else if (same(name, "filter") || same(name, "mask") || same(name, "marker") || same(name, "marker-start") || same(name, "marker-mid") || same(name, "marker-end")) {
        if (!same(v, "none")) {
            char what[160];
            format(what, sizeof what, "%s is not drawn (shapes have no %s)", name, same(name, "filter") ? "filters" : same(name, "mask") ? "masks" : "markers");
            refuse(line, what);
        }
    }
    (void)parent;
}

// Presentation attributes, then style="" declarations over them.
void Conv::style_of(const XmlNode& n, const Style& parent, Style& st, Own& own) {
    st = parent;
    static const char* const props[] = {"fill", "stroke", "color", "fill-opacity", "stroke-opacity", "opacity", "stroke-width",
                                        "stroke-linejoin", "stroke-linecap", "stroke-miterlimit", "stroke-dasharray", "stroke-dashoffset",
                                        "fill-rule", "clip-rule", "visibility", "display", "clip-path", "filter", "mask", "marker",
                                        "marker-start", "marker-mid", "marker-end"};
    // color first: currentColor in fill or stroke reads it.
    if (const char* c = xml_attr(doc, n, "color")) property("color", c, parent, st, own, n.line);
    for (const char* p : props) {
        if (same(p, "color")) continue;
        if (const char* v = xml_attr(doc, n, p)) property(p, v, parent, st, own, n.line);
    }
    const char* css = xml_attr(doc, n, "style");
    if (!css) return;
    // name: value; name: value
    char name[64], value[512];
    const char* p = css;
    for (u32 pass = 0; pass < 2; ++pass) {
        p = css;
        while (*p) {
            while (is_space(*p) || *p == ';') ++p;
            if (!*p) break;
            u32 nl = 0;
            while (*p && *p != ':' && *p != ';' && nl < sizeof name - 1) name[nl++] = *p++;
            while (nl && is_space(name[nl - 1])) --nl;
            name[nl] = 0;
            if (*p != ':') { while (*p && *p != ';') ++p; continue; }
            ++p;
            while (is_space(*p)) ++p;
            u32 vl = 0;
            while (*p && *p != ';' && vl < sizeof value - 1) value[vl++] = *p++;
            while (vl && is_space(value[vl - 1])) --vl;
            value[vl] = 0;
            // color on the first pass, everything else on the second.
            if ((pass == 0) == same(name, "color")) {
                bool known = false;
                for (const char* q : props) known |= same(q, name);
                if (known) property(name, value, parent, st, own, n.line);
            }
        }
    }
}

// The path being built (the builder's verbs since verb0), bounded: its curves' extremes, not
// their control points.
bool Conv::bounds(f32 out[4]) const {
    f32 x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    auto take = [&](f32 x, f32 y) { x0 = x < x0 ? x : x0; y0 = y < y0 ? y : y0; x1 = x > x1 ? x : x1; y1 = y > y1 ? y : y1; };
    const f32* p = s.points.data + b.point0;
    f32 cx = 0, cy = 0;
    for (u32 i = b.verb0; i < s.verbs.len; ++i) {
        PathVerb v = s.verbs[i];
        if (v == PathVerb::Move || v == PathVerb::Line) { take(p[0], p[1]); cx = p[0]; cy = p[1]; p += 2; }
        else if (v == PathVerb::Quad) {
            take(p[2], p[3]);
            for (u32 k = 0; k < 2; ++k) {
                f32 a = k ? cy : cx, c1 = p[k], e = p[2 + k];
                f32 den = a - 2.0f * c1 + e;
                if (kv::fabs(den) > 1e-12f) {
                    f32 t = (a - c1) / den;
                    if (t > 0.0f && t < 1.0f) {
                        f32 u = 1.0f - t;
                        f32 px = u * u * cx + 2.0f * u * t * p[0] + t * t * p[2], py = u * u * cy + 2.0f * u * t * p[1] + t * t * p[3];
                        take(px, py);
                    }
                }
            }
            cx = p[2]; cy = p[3]; p += 4;
        } else if (v == PathVerb::Cubic) {
            take(p[4], p[5]);
            for (u32 k = 0; k < 2; ++k) {
                f32 a = k ? cy : cx, c1 = p[k], c2 = p[2 + k], e = p[4 + k];
                // The derivative's roots: 3 (-a + 3c1 - 3c2 + e) t^2 + 6 (a - 2c1 + c2) t + 3 (c1 - a).
                f32 A = -a + 3.0f * c1 - 3.0f * c2 + e, B = 2.0f * (a - 2.0f * c1 + c2), C = c1 - a;
                f32 ts[2];
                u32 n = 0;
                if (kv::fabs(A) < 1e-12f) { if (kv::fabs(B) > 1e-12f) ts[n++] = -C / B; }
                else {
                    f32 d = B * B - 4.0f * A * C;
                    if (d >= 0.0f) { f32 sq = kv::sqrt(d); ts[n++] = (-B + sq) / (2.0f * A); ts[n++] = (-B - sq) / (2.0f * A); }
                }
                for (u32 j = 0; j < n; ++j) {
                    f32 t = ts[j];
                    if (!(t > 0.0f && t < 1.0f)) continue;
                    f32 u = 1.0f - t, c0 = u * u * u, k1 = 3.0f * u * u * t, k2 = 3.0f * u * t * t, k3 = t * t * t;
                    take(c0 * cx + k1 * p[0] + k2 * p[2] + k3 * p[4], c0 * cy + k1 * p[1] + k2 * p[3] + k3 * p[5]);
                }
            }
            cx = p[4]; cy = p[5]; p += 6;
        }
    }
    if (x0 > x1) return false;
    out[0] = x0; out[1] = y0; out[2] = x1 - x0; out[3] = y1 - y0;
    return true;
}

// A gradient's attribute, from it or from what it names with href (up to a depth).
const char* grad_attr(Conv& c, const XmlNode& g, const char* name) {
    const XmlNode* n = &g;
    for (u32 d = 0; d < 8 && n; ++d) {
        if (const char* v = xml_attr(c.doc, *n, name)) return v;
        const char* h = c.href(*n);
        n = h ? c.by_id(h) : nullptr;
    }
    return nullptr;
}

// The gradient whose stops this one uses: the first in its href chain that has any.
const XmlNode* grad_stops(Conv& c, const XmlNode& g) {
    const XmlNode* n = &g;
    for (u32 d = 0; d < 8 && n; ++d) {
        if (xml_count(c.doc, *n, "stop")) return n;
        const char* h = c.href(*n);
        n = h ? c.by_id(h) : nullptr;
    }
    return nullptr;
}

// A paint index for the shape: None, Current, a solid, or the gradient the url names (with
// the path being built's bounds, for objectBoundingBox).
u32 Conv::make_paint(const Paint& p, const Style& st, u32 line) {
    if (p.kind == Paint::None) return 0;
    if (p.kind == Paint::Current) return s.add_current();
    if (p.kind == Paint::Solid) return s.add_solid(p.rgba[0], p.rgba[1], p.rgba[2], p.rgba[3]);
    const XmlNode* g = by_id(p.id);
    bool linear = g && same(g->name, "linearGradient"), radial = g && same(g->name, "radialGradient");
    if (!linear && !radial) {
        if (p.fallback) return p.fallback_kind == Paint::None ? 0 : s.add_solid(p.rgba[0], p.rgba[1], p.rgba[2], p.rgba[3]);
        if (g && same(g->name, "pattern")) refuse(line, "a pattern is not drawn (shapes have no patterns)");
        else say(line, true, "no gradient with the id '%s'", p.id);
        return 0;
    }
    bool obb = !same(grad_attr(*this, *g, "gradientUnits"), "userSpaceOnUse");
    f32 box[4] = {0, 0, 1, 1};
    if (obb && (!bounds(box) || !(box[2] > 0.0f) || !(box[3] > 0.0f))) return 0; // no area: SVG paints nothing
    // Coordinates: fractions (or percentages) of the box, or user units (percentages of the view).
    auto coord = [&](const char* name, f32 dflt, u32 axis) {
        const char* v = grad_attr(*this, *g, name);
        if (!v) return dflt;
        f32 n;
        const char* q = v;
        if (!patherizer_number(&q, &n)) return dflt;
        if (*skip(q) == '%') {
            n /= 100.0f;
            if (!obb) n *= axis == 0 ? view[2] : axis == 1 ? view[3] : kv::sqrt((view[2] * view[2] + view[3] * view[3]) * 0.5f);
        }
        return n;
    };
    PathPaint pp;
    pp.kind = linear ? PathPaint::Linear : PathPaint::Radial;
    if (linear) {
        pp.g[0] = coord("x1", 0.0f, 0); pp.g[1] = coord("y1", 0.0f, 1);
        pp.g[2] = coord("x2", obb ? 1.0f : view[2], 0); pp.g[3] = coord("y2", 0.0f, 1);
    } else {
        f32 half = obb ? 0.5f : 0.0f;
        pp.g[0] = coord("cx", obb ? 0.5f : view[2] * 0.5f, 0); pp.g[1] = coord("cy", obb ? 0.5f : view[3] * 0.5f, 1);
        pp.g[2] = coord("r", obb ? 0.5f : kv::sqrt((view[2] * view[2] + view[3] * view[3]) * 0.5f) * 0.5f, 2);
        pp.g[3] = coord("fx", pp.g[0], 0); pp.g[4] = coord("fy", pp.g[1], 1);
        (void)half;
    }
    const char* sp = grad_attr(*this, *g, "spreadMethod");
    pp.spread = same(sp, "reflect") ? GradientSpread::Reflect : same(sp, "repeat") ? GradientSpread::Repeat : GradientSpread::Pad;
    pp.linear_rgb = same(grad_attr(*this, *g, "color-interpolation"), "linearRGB");
    PathTransform gt;
    if (const char* tv = grad_attr(*this, *g, "gradientTransform")) {
        if (!transform_list(tv, &gt)) say(g->line, true, "gradientTransform: '%s' cannot be read", tv);
    }
    if (obb) {
        PathTransform box_t;
        box_t.m[0] = box[2]; box_t.m[3] = box[3]; box_t.m[4] = box[0]; box_t.m[5] = box[1];
        gt = gt.then(box_t);
    }
    pp.transform = gt;
    // The stops: offsets clamped and kept ascending.
    PathStop stops[64];
    u32 n = 0;
    const XmlNode* holder = grad_stops(*this, *g);
    f32 last = 0.0f;
    for (const XmlNode* stop = holder ? xml_child(doc, *holder, "stop") : nullptr; stop && n < 64; stop = xml_next_named(doc, *stop, "stop")) {
        // A stop's colour and opacity: its attributes, then its style.
        f32 off = 0.0f;
        if (const char* ov = xml_attr(doc, *stop, "offset")) {
            const char* q = ov;
            if (patherizer_number(&q, &off) && *skip(q) == '%') off /= 100.0f;
        }
        off = off < last ? last : off > 1.0f ? 1.0f : off;
        last = off;
        u8 rgba[4] = {0, 0, 0, 255};
        f32 opacity = 1.0f;
        auto apply = [&](const char* name, const char* value) {
            if (same(name, "stop-color")) {
                // currentColor in a stop: the painted element's colour if it has one, else black.
                if (same(skip(value), "currentColor") || same(skip(value), "currentcolor")) { if (st.has_color) memcpy(rgba, st.color, 4); }
                else if (!colour(value, rgba)) say(stop->line, true, "stop-color: '%s' is not a colour", value);
            } else if (same(name, "stop-opacity")) {
                const char* q = value;
                f32 o2;
                if (patherizer_number(&q, &o2)) opacity = o2 < 0.0f ? 0.0f : o2 > 1.0f ? 1.0f : o2;
            }
        };
        if (const char* v = xml_attr(doc, *stop, "stop-color")) apply("stop-color", v);
        if (const char* v = xml_attr(doc, *stop, "stop-opacity")) apply("stop-opacity", v);
        if (const char* css = xml_attr(doc, *stop, "style")) {
            char name[32], value[128];
            const char* q = css;
            while (*q) {
                while (is_space(*q) || *q == ';') ++q;
                u32 nl = 0, vl = 0;
                while (*q && *q != ':' && nl < sizeof name - 1) name[nl++] = *q++;
                while (nl && is_space(name[nl - 1])) --nl;
                name[nl] = 0;
                if (*q == ':') ++q;
                while (is_space(*q)) ++q;
                while (*q && *q != ';' && vl < sizeof value - 1) value[vl++] = *q++;
                while (vl && is_space(value[vl - 1])) --vl;
                value[vl] = 0;
                if (nl) apply(name, value);
            }
        }
        rgba[3] = (u8)(rgba[3] * opacity + 0.5f);
        stops[n].offset = off;
        memcpy(stops[n].rgba, rgba, 4);
        ++n;
    }
    if (n == 0) return 0;                                   // SVG: no stops paints nothing
    if (n == 1) return s.add_solid(stops[0].rgba[0], stops[0].rgba[1], stops[0].rgba[2], stops[0].rgba[3]);
    return s.add_gradient(pp, stops, n);
}

// The path just built, ended as a node with the style's fill and stroke.
void Conv::end_shape(const XmlNode& n, const Style& st, const PathTransform& t, bool in_clip) {
    if (s.verbs.len == b.verb0) return;
    b.transform = t;
    if (in_clip) {
        // A clip's shapes count by their outlines: filled, by clip-rule.
        u32 i = b.fill(s.add_solid(0, 0, 0, 255), st.clip_rule);
        s.nodes[i].line = n.line;
        return;
    }
    if (!st.visible) { s.verbs.len = b.verb0; s.points.len = b.point0; return; }
    u32 fill = make_paint(st.fill, st, n.line);
    u32 stroke = st.width > 0.0f ? make_paint(st.stroke, st, n.line) : 0;
    u32 i = b.fill(fill, st.fill_rule, st.fill_opacity);
    s.nodes[i].line = n.line;
    if (stroke) {
        PathStroke ps;
        ps.width = st.width;
        ps.join = st.join;
        ps.cap = st.cap;
        ps.miter_limit = st.miter;
        ps.first_dash = s.dashes.len;
        ps.dash_count = st.dash_count;
        ps.dash_offset = st.dash_offset;
        for (u32 k = 0; k < st.dash_count; ++k) s.dashes.push(st.dashes[k]);
        s.strokes.push(ps);
        s.nodes[i].stroke = stroke;
        s.nodes[i].stroke_style = s.strokes.len - 1;
        s.nodes[i].stroke_opacity = st.stroke_opacity;
    }
}

// A clip group for clipPath `id`, made once and listed; 0 when there is none.
u32 Conv::clip_group(const char* id, u32 line) {
    const XmlNode* cp = by_id(id);
    if (!cp || !same(cp->name, "clipPath")) { say(line, true, "no clipPath with the id '%s'", id); return 0; }
    if (same(xml_attr(doc, *cp, "clipPathUnits"), "objectBoundingBox")) {
        refuse(cp->line, "clipPathUnits=\"objectBoundingBox\" is not drawn");
        return 0;
    }
    u32 g = b.begin_group();
    s.nodes[g].line = cp->line;
    if (const char* tv = xml_attr(doc, *cp, "transform")) {
        if (!transform_list(tv, &s.nodes[g].transform)) say(cp->line, true, "transform: '%s' cannot be read", tv);
    }
    Style st;
    Own own;
    Style base;
    style_of(*cp, base, st, own);
    children(*cp, st, true);
    b.end_group(g);
    s.clips.push(g);
    return g + 1;
}

// A drawing element's geometry, through the builder. False when it has none to draw.
bool Conv::geometry(const XmlNode& n, const char* name) {
    auto num = [&](const char* attr, f32 whole, f32 dflt) { f32 v; return length(xml_attr(doc, n, attr), whole, &v) ? v : dflt; };
    f32 vw = view[2], vh = view[3], vd = kv::sqrt((vw * vw + vh * vh) * 0.5f);
    if (same(name, "path")) {
        const char* d = xml_attr(doc, n, "d");
        if (!d || !*skip(d)) return false;
        u32 at = 0;
        if (!b.svg_path(d, &at)) {
            // SVG draws the path up to the error.
            say(n.line, true, "path data cannot be read at byte %u of d", at);
            return s.verbs.len > b.verb0;
        }
        return true;
    }
    if (same(name, "rect")) {
        f32 x = num("x", vw, 0), y = num("y", vh, 0), w = num("width", vw, 0), h = num("height", vh, 0);
        if (!(w > 0.0f) || !(h > 0.0f)) return false;
        f32 rx = num("rx", vw, -1), ry = num("ry", vh, -1);
        if (rx < 0.0f && ry < 0.0f) rx = ry = 0.0f;
        else if (rx < 0.0f) rx = ry;
        else if (ry < 0.0f) ry = rx;
        rx = rx > w * 0.5f ? w * 0.5f : rx;
        ry = ry > h * 0.5f ? h * 0.5f : ry;
        if (rx <= 0.0f || ry <= 0.0f) {
            b.move_to(x, y); b.line_to(x + w, y); b.line_to(x + w, y + h); b.line_to(x, y + h); b.close();
        } else {
            const f32 k = 0.5522847498f;
            f32 kx = rx * k, ky = ry * k;
            b.move_to(x + rx, y);
            b.line_to(x + w - rx, y);
            b.cubic_to(x + w - rx + kx, y, x + w, y + ry - ky, x + w, y + ry);
            b.line_to(x + w, y + h - ry);
            b.cubic_to(x + w, y + h - ry + ky, x + w - rx + kx, y + h, x + w - rx, y + h);
            b.line_to(x + rx, y + h);
            b.cubic_to(x + rx - kx, y + h, x, y + h - ry + ky, x, y + h - ry);
            b.line_to(x, y + ry);
            b.cubic_to(x, y + ry - ky, x + rx - kx, y, x + rx, y);
            b.close();
        }
        return true;
    }
    if (same(name, "circle") || same(name, "ellipse")) {
        f32 cx = num("cx", vw, 0), cy = num("cy", vh, 0), rx, ry;
        if (same(name, "circle")) rx = ry = num("r", vd, 0);
        else { rx = num("rx", vw, -1); ry = num("ry", vh, -1); if (rx < 0.0f) rx = ry; if (ry < 0.0f) ry = rx; }
        if (!(rx > 0.0f) || !(ry > 0.0f)) return false;
        const f32 k = 0.5522847498f;
        f32 kx = rx * k, ky = ry * k;
        b.move_to(cx + rx, cy);
        b.cubic_to(cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry);
        b.cubic_to(cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy);
        b.cubic_to(cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry);
        b.cubic_to(cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy);
        b.close();
        return true;
    }
    if (same(name, "line")) {
        b.move_to(num("x1", vw, 0), num("y1", vh, 0));
        b.line_to(num("x2", vw, 0), num("y2", vh, 0));
        return true;
    }
    if (same(name, "polyline") || same(name, "polygon")) {
        const char* p = xml_attr(doc, n, "points");
        if (!p) return false;
        f32 x, y;
        u32 k = 0;
        while (patherizer_number(&p, &x) && patherizer_number(&p, &y)) { if (k++) b.line_to(x, y); else b.move_to(x, y); }
        if (k < 2) return false; // the caller takes back what was built
        if (same(name, "polygon")) b.close();
        return true;
    }
    return false;
}

void Conv::children(const XmlNode& n, const Style& st, bool in_clip) {
    for (const XmlNode* c = xml_child(doc, n); c; c = xml_next_named(doc, *c)) element(*c, st, in_clip);
}

void Conv::element(const XmlNode& n, const Style& parent, bool in_clip) {
    const char* name = n.name;
    // Editors' own (sodipodi:namedview, inkscape:...), and what describes rather than draws.
    if (strchr(name, ':') || same(name, "title") || same(name, "desc") || same(name, "metadata")) return;
    // Defined, and drawn where something names them.
    if (same(name, "defs") || same(name, "linearGradient") || same(name, "radialGradient") || same(name, "clipPath") || same(name, "stop")) return;
    static const char* const refused[] = {"text", "tspan", "textPath", "image", "filter", "mask", "pattern", "marker", "foreignObject",
                                          "style", "script", "switch", "animate", "animateTransform", "animateMotion", "set", "symbol", "svg"};
    for (const char* r : refused)
        if (same(name, r)) {
            char what[160];
            if (same(name, "style")) format(what, sizeof what, "a <style> element is not read (style=\"...\" on elements is)");
            else if (same(name, "svg")) format(what, sizeof what, "a nested <svg> is not drawn");
            else format(what, sizeof what, "<%s> is not drawn", name);
            refuse(n.line, what);
            return;
        }
    Style st;
    Own own;
    style_of(n, parent, st, own);
    if (!own.display) return;
    PathTransform t;
    if (const char* tv = xml_attr(doc, n, "transform")) {
        if (!transform_list(tv, &t)) say(n.line, true, "transform: '%s' cannot be read", tv);
    }
    bool group = same(name, "g") || same(name, "a") || same(name, "use");
    bool drawing = same(name, "path") || same(name, "rect") || same(name, "circle") || same(name, "ellipse") || same(name, "line") ||
                   same(name, "polyline") || same(name, "polygon");
    if (!group && !drawing) { say(n.line, false, "<%s> is not an element shapes know: left out", name); return; }
    if (in_clip && group && !same(name, "use")) { refuse(n.line, "a group inside a clipPath is not drawn"); return; }
    // Opacity or a clip: a group of its own around it.
    u32 clip = !in_clip && own.clip[0] ? clip_group(own.clip, n.line) : 0;
    bool wrap = !in_clip && (group || own.opacity < 1.0f || clip);
    u32 g = 0;
    if (wrap) {
        g = b.begin_group(own.opacity, clip);
        s.nodes[g].line = n.line;
        s.nodes[g].transform = t;
        t = PathTransform{};
    }
    if (same(name, "use")) {
        const char* id = href(n);
        const XmlNode* target = id ? by_id(id) : nullptr;
        f32 x = 0, y = 0;
        length(xml_attr(doc, n, "x"), view[2], &x);
        length(xml_attr(doc, n, "y"), view[3], &y);
        if (!target) say(n.line, true, "use: no element with the id '%s'", id ? id : "");
        else if (use_depth >= 8) say(n.line, true, "use: nested too deep (a loop?)");
        else {
            ++use_depth;
            u32 inner = b.begin_group();
            s.nodes[inner].transform = PathTransform::translate(x, y);
            if (in_clip) s.nodes[inner].transform = s.nodes[inner].transform.then(t);
            element(*target, st, in_clip);
            b.end_group(inner);
            --use_depth;
        }
    } else if (group) {
        children(n, st, in_clip);
    } else {
        b.verb0 = s.verbs.len;
        b.point0 = s.points.len;
        if (geometry(n, name)) end_shape(n, st, t, in_clip);
        else { s.verbs.len = b.verb0; s.points.len = b.point0; }
    }
    if (wrap) b.end_group(g);
}

} // namespace

bool svg_to_shape(const char* text, usize len, const SvgOptions& options, PathShape& out, SvgMessages& msgs) {
    out.clear();
    msgs.items.clear();
    msgs.errors = 0;
    if (!msgs.items.alloc) msgs.items.alloc = mem_general();
    MemScope scope;
    XmlDoc doc;
    if (!xml_parse(text, len, &scope.arena->a, doc)) {
        SvgMessage m;
        m.line = doc.error_line;
        format(m.text, sizeof m.text, "%s", doc.error);
        msgs.items.push(m);
        msgs.errors = 1;
        return false;
    }
    const XmlNode& root = doc.root();
    Conv c(doc, options, out, msgs);
    if (!same(root.name, "svg")) { c.say(root.line, true, "the root element is <%s>, not <svg>", root.name); return false; }
    const char* vb = xml_attr(doc, root, "viewBox");
    bool have = false;
    if (vb) {
        const char* p = vb;
        have = true;
        for (u32 k = 0; k < 4; ++k) have = have && patherizer_number(&p, &c.view[k]);
        if (!have || !(c.view[2] > 0.0f) || !(c.view[3] > 0.0f)) { c.say(root.line, true, "viewBox: '%s' is not four numbers with a width and height", vb); return false; }
    } else {
        f32 w = 0, h = 0;
        if (length(xml_attr(doc, root, "width"), 0.0f, &w) && length(xml_attr(doc, root, "height"), 0.0f, &h) && w > 0.0f && h > 0.0f) {
            c.view[2] = w; c.view[3] = h;
        } else {
            c.say(root.line, true, "no viewBox, and no width and height to make one");
            return false;
        }
    }
    if (const char* par = xml_attr(doc, root, "preserveAspectRatio"); par && same(par, "none"))
        c.say(root.line, false, "preserveAspectRatio=\"none\": shapes keep their aspect (a caller may stretch them)");
    memcpy(out.view, c.view, sizeof out.view);
    Style base;
    Style st;
    Own own;
    c.style_of(root, base, st, own);
    PathTransform t;
    if (const char* tv = xml_attr(doc, root, "transform")) transform_list(tv, &t);
    u32 g = c.b.begin_group(own.opacity);
    out.nodes[g].transform = t;
    out.nodes[g].line = root.line;
    c.children(root, st, false);
    c.b.end_group(g);
    return msgs.errors == 0;
}

} // namespace kv
