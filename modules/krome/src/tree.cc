// The tree's storage and its properties by name.
#include "korvid/krome/tree.h"
#include "korvid/platform_hid/input.h"
#include "korvid/dejavu/dejavu.h"
#include <stdlib.h>
#include <string.h>

namespace kv {

const char* const KROME_DEFAULT_FONT = KV_DEJAVU_SANS;

void KromeTree::init(Allocator* a) {
    nodes.alloc = looks.alloc = strings.alloc = a;
    clear();
}

void KromeTree::free() {
    nodes.free();
    looks.free();
    strings.free();
    root = KROME_NONE;
}

void KromeTree::clear() {
    nodes.clear();
    looks.clear();
    strings.clear();
    looks.push(KromeLook{});
    strings.push(0); // offset 0 is ""
    root = KROME_NONE;
    dirty = true;
}

u32 KromeTree::string(const char* s, u32 len) {
    u32 at = strings.len;
    char* p = strings.push_n(len + 1);
    if (len) memcpy(p, s, len);
    p[len] = 0;
    return at;
}

u32 KromeTree::add(KromeType type, u32 parent) {
    u32 depth = 0;
    for (u32 p = parent; p != KROME_NONE; p = nodes[p].parent)
        if (++depth >= KROME_MAX_DEPTH) return KROME_NONE;
    KromeNode n;
    n.type = type;
    n.parent = parent;
    u32 i = nodes.len;
    nodes.push(n);
    if (parent == KROME_NONE) {
        if (root == KROME_NONE) root = i;
    } else {
        KromeNode& p = nodes[parent];
        if (p.last == KROME_NONE) p.first = i;
        else nodes[p.last].next = i;
        p.last = i;
    }
    dirty = true;
    return i;
}

void KromeTree::set_text(u32 node, const char* s, u32 len) {
    nodes[node].text = string(s, len);
    nodes[node].shown = KROME_NONE;
    dirty = true;
}
void KromeTree::set_text(u32 node, const char* s) { set_text(node, s, (u32)strlen(s)); }

void KromeTree::set_shown(u32 node, const char* s, u32 len) {
    // An unchanged display text keeps its offset, so filling in the same value twice
    // neither grows the strings nor marks the tree.
    KromeNode& n = nodes[node];
    if (n.shown != KROME_NONE && strlen(strings.data + n.shown) == len && memcmp(strings.data + n.shown, s, len) == 0) return;
    n.shown = string(s, len);
    dirty = true;
}

const char* KromeTree::text(u32 node) const {
    const KromeNode& n = nodes[node];
    if (n.shown != KROME_NONE) return strings.data + n.shown;
    return n.text != KROME_NONE ? strings.data + n.text : "";
}
const char* KromeTree::raw_text(u32 node) const {
    const KromeNode& n = nodes[node];
    return n.text != KROME_NONE ? strings.data + n.text : "";
}

namespace {

int hexd(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

bool size_value(KromeAxis& a, const KromeValue& v) {
    if (v.kind == KromeValue::Range) { a.lo = v.lo; a.hi = v.hi; return true; } // bounds, not a mode
    if (v.kind == KromeValue::Number) { a.mode = KromeSize::Fixed; a.value = v.n; }
    else if (v.kind == KromeValue::String && !strcmp(v.s, "hug")) a.mode = KromeSize::Hug;
    else if (v.kind == KromeValue::String && !strcmp(v.s, "fill")) a.mode = KromeSize::Fill;
    else if (v.kind == KromeValue::String && *v.s && v.s[strlen(v.s) - 1] == '%') {
        char* end = nullptr;
        f64 pct = strtod(v.s, &end);
        if (end != v.s + strlen(v.s) - 1 || pct < 0) return false;
        a.mode = KromeSize::Percent;
        a.value = (f32)pct;
    } else return false;
    a.set = true;
    return true;
}

bool numbers(const KromeValue& v, f32* out, u32 want_max, u32* n) {
    if (v.kind == KromeValue::Number) { out[0] = v.n; *n = 1; return true; }
    if (v.kind != KromeValue::List || v.count == 0 || v.count > want_max) return false;
    for (u32 i = 0; i < v.count; ++i) out[i] = v.list[i];
    *n = v.count;
    return true;
}

bool color_value(const KromeValue& v, color* out) { return v.kind == KromeValue::String && krome_parse_color(v.s, out); }

// Nine anchors: "top-left" ... "bottom-right", and "center".
bool anchor_value(const char* s, i8* x, i8* y) {
    static const char* const vert[] = {"top", "middle", "bottom"};
    static const char* const horz[] = {"left", "center", "right"};
    if (!strcmp(s, "center")) { *x = *y = (i8)KromeAlign::Center; return true; }
    for (i8 j = 0; j < 3; ++j) {
        usize n = strlen(vert[j]);
        if (strncmp(s, vert[j], n) != 0) continue;
        if (s[n] == 0) { *x = (i8)KromeAlign::Center; *y = j; return true; }
        if (s[n] != '-') return false;
        for (i8 i = 0; i < 3; ++i) if (!strcmp(s + n + 1, horz[i])) { *x = i; *y = j; return true; }
    }
    for (i8 i = 0; i < 3; ++i) if (!strcmp(s, horz[i])) { *x = i; *y = (i8)KromeAlign::Center; return true; }
    return false;
}

// A look property: true when `prop` is one, with *why set when its value was wrong.
bool look_prop(KromeLook& l, const char* prop, const KromeValue& v, const char** why) {
    f32 f[4];
    u32 n = 0;
    *why = nullptr;
    if (!strcmp(prop, "bg"))           { if (!color_value(v, &l.bg)) *why = "bg takes a color"; else l.set |= KROME_LOOK_BG; return true; }
    if (!strcmp(prop, "bg2"))          { if (!color_value(v, &l.bg2)) *why = "bg2 takes a color"; else l.set |= KROME_LOOK_BG2; return true; }
    if (!strcmp(prop, "border-color")) { if (!color_value(v, &l.border_color)) *why = "border-color takes a color"; else l.set |= KROME_LOOK_BORDER_COLOR; return true; }
    if (!strcmp(prop, "shadow-color")) { if (!color_value(v, &l.shadow_color)) *why = "shadow-color takes a color"; else l.set |= KROME_LOOK_SHADOW_COLOR; return true; }
    if (!strcmp(prop, "color"))        { if (!color_value(v, &l.text_color)) *why = "color takes a color"; else l.set |= KROME_LOOK_COLOR; return true; }
    if (!strcmp(prop, "tint"))         { if (!color_value(v, &l.tint)) *why = "tint takes a color"; else l.set |= KROME_LOOK_TINT; return true; }
    if (!strcmp(prop, "angle"))        { if (v.kind != KromeValue::Number) *why = "angle takes a number"; else { l.angle = v.n; l.set |= KROME_LOOK_ANGLE; } return true; }
    if (!strcmp(prop, "border"))       { if (v.kind != KromeValue::Number || v.n < 0) *why = "border takes a number, 0 or more"; else { l.border = v.n; l.set |= KROME_LOOK_BORDER; } return true; }
    if (!strcmp(prop, "opacity"))      { if (v.kind != KromeValue::Number || v.n < 0 || v.n > 1) *why = "opacity takes a number from 0 to 1"; else { l.opacity = v.n; l.set |= KROME_LOOK_OPACITY; } return true; }
    if (!strcmp(prop, "size"))         { if (v.kind != KromeValue::Number || v.n <= 0) *why = "size takes a number above 0"; else { l.size = v.n; l.set |= KROME_LOOK_SIZE; } return true; }
    if (!strcmp(prop, "transition")) { if (v.kind != KromeValue::Number || v.n < 0 || v.n > 10) *why = "transition takes seconds, 0 to 10"; else { l.transition = v.n; l.set |= KROME_LOOK_TRANSITION; } return true; }
    if (!strcmp(prop, "scrollbar-color")) { if (!color_value(v, &l.scrollbar_color)) *why = "scrollbar-color takes a color"; else l.set |= KROME_LOOK_SCROLLBAR_COLOR; return true; }
    if (!strcmp(prop, "scrollbar-track")) { if (!color_value(v, &l.scrollbar_track)) *why = "scrollbar-track takes a color"; else l.set |= KROME_LOOK_SCROLLBAR_TRACK; return true; }
    if (!strcmp(prop, "scrollbar-size")) { if (v.kind != KromeValue::Number || v.n <= 0) *why = "scrollbar-size takes a number above 0"; else { l.scrollbar_size = v.n; l.set |= KROME_LOOK_SCROLLBAR_SIZE; } return true; }
    if (!strcmp(prop, "caret-color"))  { if (!color_value(v, &l.caret_color)) *why = "caret-color takes a color"; else l.set |= KROME_LOOK_CARET_COLOR; return true; }
    if (!strcmp(prop, "selection-color")) { if (!color_value(v, &l.selection_color)) *why = "selection-color takes a color"; else l.set |= KROME_LOOK_SELECTION_COLOR; return true; }
    if (!strcmp(prop, "line-height"))  { if (v.kind != KromeValue::Number || v.n <= 0) *why = "line-height takes a number above 0, times the size"; else { l.line_height = v.n; l.set |= KROME_LOOK_LINE_HEIGHT; } return true; }
    if (!strcmp(prop, "font"))         { if (v.kind != KromeValue::String) *why = "font takes an asset path"; else { l.font = name_intern(v.s); l.set |= KROME_LOOK_FONT; } return true; }
    if (!strcmp(prop, "image"))        { if (v.kind != KromeValue::String) *why = "image takes an asset path"; else { l.image = name_intern(v.s); l.set |= KROME_LOOK_IMAGE; } return true; }
    if (!strcmp(prop, "region")) {
        // x, y, w, h in the file's pixels.
        if (!numbers(v, f, 4, &n) || n != 4 || f[0] < 0 || f[1] < 0 || !(f[2] > 0) || !(f[3] > 0)) { *why = "region takes x, y, w and h: a corner at 0 or more, a size above 0"; return true; }
        memcpy(l.region, f, sizeof l.region);
        l.set |= KROME_LOOK_REGION;
        return true;
    }
    if (!strcmp(prop, "frames")) {
        if (v.kind != KromeValue::Number || v.n < 1 || v.n > 4096 || v.n != (f32)(u32)v.n) { *why = "frames takes a whole number from 1 to 4096"; return true; }
        l.frames = (u32)v.n;
        l.set |= KROME_LOOK_FRAMES;
        return true;
    }
    if (!strcmp(prop, "frame") || !strcmp(prop, "frame-at")) {
        bool at = prop[5] == '-';
        if (v.kind != KromeValue::Number || !(v.n > -1e9f && v.n < 1e9f)) { *why = at ? "frame-at takes a number, 0 the first frame and 1 the last" : "frame takes a number, from 0"; return true; }
        l.frame = v.n;
        l.frame_at = at;
        l.set |= KROME_LOOK_FRAME;
        return true;
    }
    if (!strcmp(prop, "fps")) {
        if (v.kind != KromeValue::Number || v.n < 0 || v.n > 240) { *why = "fps takes frames a second, 0 (still) to 240"; return true; }
        l.fps = v.n;
        l.set |= KROME_LOOK_FPS;
        return true;
    }
    if (!strcmp(prop, "loops")) {
        if (v.kind != KromeValue::Number || v.n < 0 || v.n > 1e6f || v.n != (f32)(u32)v.n) { *why = "loops takes a whole number, 0 for ever"; return true; }
        l.loops = (u32)v.n;
        l.set |= KROME_LOOK_LOOPS;
        return true;
    }
    if (!strcmp(prop, "gain")) {
        // One number for every channel, or one each: red, green, blue.
        if (!numbers(v, f, 3, &n) || (n != 1 && n != 3)) { *why = "gain takes one number, or three"; return true; }
        if (n == 1) f[1] = f[2] = f[0];
        for (u32 i = 0; i < 3; ++i) if (!(f[i] >= 0.0f && f[i] <= 16.0f)) { *why = "gain takes numbers from 0 to 16"; return true; }
        memcpy(l.gain, f, sizeof l.gain);
        l.set |= KROME_LOOK_GAIN;
        return true;
    }
    if (!strcmp(prop, "hue")) {
        if (v.kind != KromeValue::Number || !(v.n > -1e6f && v.n < 1e6f)) { *why = "hue takes degrees"; return true; }
        l.hue = v.n;
        l.set |= KROME_LOOK_HUE;
        return true;
    }
    if (!strcmp(prop, "saturation")) {
        if (v.kind != KromeValue::Number || !(v.n >= 0.0f && v.n <= 16.0f)) { *why = "saturation takes a number from 0 (grey) to 16"; return true; }
        l.saturation = v.n;
        l.set |= KROME_LOOK_SATURATION;
        return true;
    }
    if (!strcmp(prop, "fit")) {
        static const char* const names[] = {"stretch", "natural", "tile", "slice"};
        for (u8 i = 0; i < 4; ++i)
            if (v.kind == KromeValue::String && !strcmp(v.s, names[i])) { l.fit = (KromeFit)i; l.set |= KROME_LOOK_FIT; return true; }
        *why = "fit takes stretch, natural, tile or slice";
        return true;
    }
    if (!strcmp(prop, "slice")) {
        // One border all round, or left, top, right, bottom: the file's pixels.
        if (!numbers(v, f, 4, &n) || (n != 1 && n != 4)) { *why = "slice takes one border, or left, top, right and bottom"; return true; }
        if (n == 1) f[1] = f[2] = f[3] = f[0];
        if (f[0] < 0 || f[1] < 0 || f[2] < 0 || f[3] < 0) { *why = "slice takes borders of 0 or more"; return true; }
        memcpy(l.slice, f, sizeof l.slice);
        l.set |= KROME_LOOK_SLICE;
        return true;
    }
    if (!strcmp(prop, "radius")) {
        if (!numbers(v, f, 4, &n) || n == 3) { *why = "radius takes one, two or four numbers"; return true; }
        if (n == 1) f[1] = f[2] = f[3] = f[0];
        if (n == 2) { f[2] = f[0]; f[3] = f[1]; } // top-left and bottom-right, then the other pair
        memcpy(l.radius, f, sizeof l.radius);
        l.set |= KROME_LOOK_RADIUS;
        return true;
    }
    if (!strcmp(prop, "shadow")) {
        // x y blur [spread]
        if (!numbers(v, f, 4, &n) || n < 3) { *why = "shadow takes x, y, blur and an optional spread"; return true; }
        l.shadow_offset = vec2{f[0], f[1]};
        l.shadow_blur = f[2];
        l.shadow_spread = n == 4 ? f[3] : 0.0f;
        l.set |= KROME_LOOK_SHADOW;
        return true;
    }
    return false;
}

void overlay(KromeLook& to, const KromeLook& from) {
    u32 s = from.set;
    if (s & KROME_LOOK_BG) to.bg = from.bg;
    if (s & KROME_LOOK_BG2) to.bg2 = from.bg2;
    if (s & KROME_LOOK_ANGLE) to.angle = from.angle;
    if (s & KROME_LOOK_RADIUS) memcpy(to.radius, from.radius, sizeof to.radius);
    if (s & KROME_LOOK_BORDER) to.border = from.border;
    if (s & KROME_LOOK_BORDER_COLOR) to.border_color = from.border_color;
    if (s & KROME_LOOK_SHADOW) { to.shadow_offset = from.shadow_offset; to.shadow_blur = from.shadow_blur; to.shadow_spread = from.shadow_spread; }
    if (s & KROME_LOOK_SHADOW_COLOR) to.shadow_color = from.shadow_color;
    if (s & KROME_LOOK_COLOR) to.text_color = from.text_color;
    if (s & KROME_LOOK_OPACITY) to.opacity = from.opacity;
    if (s & KROME_LOOK_FONT) to.font = from.font;
    if (s & KROME_LOOK_SIZE) to.size = from.size;
    if (s & KROME_LOOK_IMAGE) to.image = from.image;
    if (s & KROME_LOOK_REGION) memcpy(to.region, from.region, sizeof to.region);
    if (s & KROME_LOOK_FIT) to.fit = from.fit;
    if (s & KROME_LOOK_SLICE) memcpy(to.slice, from.slice, sizeof to.slice);
    if (s & KROME_LOOK_FRAMES) to.frames = from.frames;
    if (s & KROME_LOOK_FRAME) { to.frame = from.frame; to.frame_at = from.frame_at; }
    if (s & KROME_LOOK_FPS) to.fps = from.fps;
    if (s & KROME_LOOK_LOOPS) to.loops = from.loops;
    if (s & KROME_LOOK_GAIN) memcpy(to.gain, from.gain, sizeof to.gain);
    if (s & KROME_LOOK_HUE) to.hue = from.hue;
    if (s & KROME_LOOK_SATURATION) to.saturation = from.saturation;
    if (s & KROME_LOOK_TINT) to.tint = from.tint;
    if (s & KROME_LOOK_LINE_HEIGHT) to.line_height = from.line_height;
    if (s & KROME_LOOK_CARET_COLOR) to.caret_color = from.caret_color;
    if (s & KROME_LOOK_SCROLLBAR_COLOR) to.scrollbar_color = from.scrollbar_color;
    if (s & KROME_LOOK_SCROLLBAR_TRACK) to.scrollbar_track = from.scrollbar_track;
    if (s & KROME_LOOK_SCROLLBAR_SIZE) to.scrollbar_size = from.scrollbar_size;
    if (s & KROME_LOOK_TRANSITION) to.transition = from.transition;
    if (s & KROME_LOOK_SELECTION_COLOR) to.selection_color = from.selection_color;
    to.set |= s;
}

} // namespace

bool krome_parse_edge(const char* s, KromeEdge* out) {
    if (!s || !*s) return false;
    *out = KromeEdge{};
    char* end = nullptr;
    f64 a = strtod(s, &end);
    if (end == s || !(a > -1e9 && a < 1e9)) return false;
    if (*end == 0) { out->dist = (f32)a; return true; }
    if (*end != '%') return false;
    out->frac = (f32)(a * 0.01);
    const char* p = end + 1;
    if (*p == 0) return true;
    if (*p != '+' && *p != '-') return false;
    f64 d = strtod(p, &end);
    if (end == p + 1 || *end != 0 || !(d > -1e9 && d < 1e9)) return false;
    out->dist = (f32)d;
    return true;
}

bool krome_parse_shortcut(const char* text, u32* key, u32* mods) {
    *key = 0;
    *mods = 0;
    char word[32];
    for (const char* p = text; *p;) {
        u32 n = 0;
        while (*p && *p != '+' && n + 1 < sizeof word) word[n++] = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p), ++p;
        word[n] = 0;
        if (*p == '+') ++p;
        if (!n || *key) return false; // the key is last, and there is one
        if (!strcmp(word, "mod")) {
            *mods |= KV_PLATFORM_MOD_KEY == Key::LGui ? KROME_MOD_CMD : KROME_MOD_CTRL;
        } else if (!strcmp(word, "ctrl")) *mods |= KROME_MOD_CTRL;
        else if (!strcmp(word, "shift")) *mods |= KROME_MOD_SHIFT;
        else if (!strcmp(word, "alt") || !strcmp(word, "option")) *mods |= KROME_MOD_ALT;
        else if (!strcmp(word, "cmd") || !strcmp(word, "command")) *mods |= KROME_MOD_CMD;
        else {
            for (u32 k = 1; k < (u32)Key::Count; ++k) {
                const char* name = key_name((Key)k);
                u32 i = 0;
                while (name[i] && word[i] && (name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]) == word[i]) ++i;
                if (!name[i] && !word[i]) { *key = k; break; }
            }
            if (!*key) return false;
        }
    }
    return *key != 0;
}

const char* KromeTree::set(u32 node, const char* prop, const KromeValue& v, KromeState state) {
    if (node >= nodes.len) return "no such node";
    KromeNode& n = nodes[node];
    if (state == KromeState::Base) {
        // Layout's properties.
        if (!strcmp(prop, "w")) { dirty = true; return size_value(n.w, v) ? nullptr : "w takes a number, a percentage, hug, fill or a range"; }
        if (!strcmp(prop, "h")) { dirty = true; return size_value(n.h, v) ? nullptr : "h takes a number, a percentage, hug, fill or a range"; }
        // A stack child's edges (KR30): l, t, r, b; "none" unties one.
        static const char* const edges[] = {"l", "t", "r", "b"};
        for (u32 e = 0; e < 4; ++e) {
            if (strcmp(prop, edges[e]) != 0) continue;
            KromeEdge edge;
            u8 bit = (u8)(1u << e);
            if (v.kind == KromeValue::Number && v.n > -1e9f && v.n < 1e9f) edge.dist = v.n;
            else if (v.kind == KromeValue::String && !strcmp(v.s, "none")) { n.tied &= (u8)~bit; dirty = true; return nullptr; }
            else if (v.kind != KromeValue::String || !krome_parse_edge(v.s, &edge)) return "an edge takes a distance (10), a fraction (33.3%), both (100%-40), or none";
            n.edge[e] = edge;
            n.tied |= bit;
            dirty = true;
            return nullptr;
        }
        if (!strcmp(prop, "x") || !strcmp(prop, "y")) {
            if (v.kind != KromeValue::Number || !(v.n > -1e9f && v.n < 1e9f)) return "an offset takes logical pixels";
            (prop[0] == 'x' ? n.offset.x : n.offset.y) = v.n;
            n.tied |= prop[0] == 'x' ? KROME_TIE_X : KROME_TIE_Y;
            dirty = true;
            return nullptr;
        }
        if (!strcmp(prop, "scroll")) {
            if (v.kind != KromeValue::String) return "scroll takes x, y or xy";
            u32 f = !strcmp(v.s, "x") ? KROME_SCROLL_X : !strcmp(v.s, "y") ? KROME_SCROLL_Y : !strcmp(v.s, "xy") ? (KROME_SCROLL_X | KROME_SCROLL_Y) : 0;
            if (!f) return "scroll takes x, y or xy";
            n.flags |= f | KROME_CLIP;
            dirty = true;
            return nullptr;
        }
        if (!strcmp(prop, "tooltip")) {
            if (v.kind != KromeValue::String) return "tooltip takes a string";
            n.tooltip = string(v.s, (u32)strlen(v.s));
            return nullptr;
        }
        if (!strcmp(prop, "part")) {
            if (v.kind != KromeValue::String) return "part takes a name";
            n.part = name_intern(v.s);
            return nullptr;
        }
        if (!strcmp(prop, "behave")) {
            if (v.kind != KromeValue::String) return "behave takes a behaviour's name";
            n.behave = name_intern(v.s);
            return nullptr;
        }
        if (!strcmp(prop, "taborder")) {
            if (v.kind != KromeValue::Number || v.n < -1000 || v.n > 1000) return "taborder takes a number from -1000 to 1000";
            n.taborder = (i16)v.n;
            return nullptr;
        }
        if (!strcmp(prop, "step")) {
            if (v.kind != KromeValue::Number || v.n < 0) return "step takes a number, 0 or more";
            n.step = v.n;
            return nullptr;
        }
        if (!strcmp(prop, "range")) {
            if (v.kind != KromeValue::Range) return "range takes lo..hi";
            n.lo = v.lo;
            n.hi = v.hi;
            return nullptr;
        }
        if (!strcmp(prop, "key")) {
            u32 key = 0, mods = 0;
            if (v.kind != KromeValue::String || !krome_parse_shortcut(v.s, &key, &mods)) return "key takes a shortcut: mod+s, ctrl+shift+z, f5";
            n.shortcut = (u16)key;
            n.shortcut_mods = (u8)mods;
            return nullptr;
        }
        if (!strcmp(prop, "arrived")) {
            if (v.kind != KromeValue::String) return "an event takes a name";
            n.arrived = name_intern(v.s);
            return nullptr;
        }
        if (!strcmp(prop, "context")) {
            if (v.kind != KromeValue::String) return "an event takes a name";
            n.context = name_intern(v.s);
            return nullptr;
        }
        if (!strcmp(prop, "gap")) {
            if (v.kind != KromeValue::Number) return "gap takes a number";
            n.gap = v.n; dirty = true; return nullptr;
        }
        if (!strcmp(prop, "pad")) {
            f32 f[4];
            u32 c = 0;
            if (!numbers(v, f, 4, &c) || c == 3) return "pad takes one, two or four numbers";
            if (c == 1) { n.pad[0] = n.pad[1] = n.pad[2] = n.pad[3] = f[0]; }
            else if (c == 2) { n.pad[0] = n.pad[2] = f[0]; n.pad[1] = n.pad[3] = f[1]; } // across, then along
            else { n.pad[0] = f[0]; n.pad[1] = f[1]; n.pad[2] = f[2]; n.pad[3] = f[3]; }
            dirty = true;
            return nullptr;
        }
        if (!strcmp(prop, "dir")) {
            if (v.kind != KromeValue::String) return "dir takes row, col or stack";
            if (!strcmp(v.s, "row")) n.dir = KromeDir::Row;
            else if (!strcmp(v.s, "col")) n.dir = KromeDir::Col;
            else if (!strcmp(v.s, "stack")) n.dir = KromeDir::Stack;
            else return "dir takes row, col or stack";
            dirty = true;
            return nullptr;
        }
        if (!strcmp(prop, "anchor")) {
            if (v.kind != KromeValue::String || !anchor_value(v.s, &n.anchor_x, &n.anchor_y)) return "anchor takes top-left ... bottom-right, or center";
            dirty = true;
            return nullptr;
        }
        if (!strcmp(prop, "id")) {
            if (v.kind != KromeValue::String) return "id takes a name";
            n.id = name_intern(v.s);
            return nullptr;
        }
        if (!strcmp(prop, "event")) {
            if (v.kind != KromeValue::String) return "event takes a name";
            n.event = name_intern(v.s);
            return nullptr;
        }
        if (!strcmp(prop, "change") || !strcmp(prop, "blur")) {
            if (v.kind != KromeValue::String) return "an event takes a name";
            (prop[0] == 'c' ? n.change : n.blur) = name_intern(v.s);
            return nullptr;
        }
        if (!strcmp(prop, "placeholder")) {
            if (v.kind != KromeValue::String) return "placeholder takes a string";
            n.placeholder = string(v.s, (u32)strlen(v.s));
            dirty = true;
            return nullptr;
        }
        if (!strcmp(prop, "cursor")) {
            static const char* const names[] = {"", "arrow", "ibeam", "hand", "move", "resize-ew", "resize-ns", "forbidden"};
            for (u8 i = 1; i < 8; ++i)
                if (v.kind == KromeValue::String && !strcmp(v.s, names[i])) { n.cursor = (KromeCursor)i; return nullptr; }
            return "cursor takes arrow, ibeam, hand, move, resize-ew, resize-ns or forbidden";
        }
        if (!strcmp(prop, "grip")) {
            static const char* const names[] = {"none", "move", "n", "ne", "e", "se", "s", "sw", "w", "nw", "drag"};
            for (u8 i = 0; i < 11; ++i)
                if (v.kind == KromeValue::String && !strcmp(v.s, names[i])) { n.grip = (KromeGrip)i; return nullptr; }
            return "grip takes move, drag, n, ne, e, se, s, sw, w, nw or none";
        }
    }
    // A look's property, in the state's look (made on first use).
    u32* slot = state == KromeState::Base ? &n.look : &n.states[(u32)state];
    u32 at = state == KromeState::Base ? *slot : *slot ? *slot - 1 : 0;
    KromeLook l = at ? looks[at] : KromeLook{};
    const char* why = nullptr;
    if (!look_prop(l, prop, v, &why)) return state == KromeState::Base ? "unknown property" : "not a property a state can set";
    if (why) return why;
    if (!at) { at = looks.len; looks.push(l); *slot = state == KromeState::Base ? at : at + 1; }
    else looks[at] = l;
    if (!strcmp(prop, "font") || !strcmp(prop, "size") || !strcmp(prop, "image") || !strcmp(prop, "line-height") || !strcmp(prop, "region") || !strcmp(prop, "frames")) dirty = true;
    return nullptr;
}

const char* KromeTree::set_flag(u32 node, const char* w, bool on) {
    if (node >= nodes.len) return "no such node";
    if (on) return flag(node, w);
    u32 was = nodes[node].flags;
    // Cleared: the bit the word sets, found by setting it on a copy.
    nodes[node].flags = 0;
    const char* bad = flag(node, w);
    u32 bit = nodes[node].flags;
    nodes[node].flags = was & ~bit;
    dirty = true;
    return bad;
}

const char* KromeTree::flag(u32 node, const char* w) {
    if (node >= nodes.len) return "no such node";
    KromeNode& n = nodes[node];
    struct Bit { const char* word; u32 bit; };
    static const Bit bits[] = {{"fill", KROME_FILL}, {"clip", KROME_CLIP}, {"scroll", KROME_SCROLL},
                               {"disabled", KROME_DISABLED}, {"selected", KROME_SELECTED}, {"hidden", KROME_HIDDEN},
                               {"subpixel", KROME_SUBPIXEL}, {"nowrap", KROME_NOWRAP},
                               {"multiline", KROME_MULTILINE}, {"readonly", KROME_READONLY},
                               {"focusable", KROME_FOCUSABLE}, {"nofocus", KROME_NOFOCUS}, {"secret", KROME_SECRET}, {"tabs", KROME_TABS},
                               {"pixel", KROME_PIXEL}, {"across", KROME_ACROSS}};
    for (const Bit& b : bits) if (!strcmp(w, b.word)) { n.flags |= b.bit; dirty = true; return nullptr; }
    if (!strcmp(w, "left"))   { n.align_x = KromeAlign::Start; dirty = true; return nullptr; }
    if (!strcmp(w, "center")) { n.align_x = KromeAlign::Center; dirty = true; return nullptr; }
    if (!strcmp(w, "right"))  { n.align_x = KromeAlign::End; dirty = true; return nullptr; }
    if (!strcmp(w, "top"))    { n.align_y = KromeAlign::Start; dirty = true; return nullptr; }
    if (!strcmp(w, "middle")) { n.align_y = KromeAlign::Center; dirty = true; return nullptr; }
    if (!strcmp(w, "bottom")) { n.align_y = KromeAlign::End; dirty = true; return nullptr; }
    return "unknown flag";
}

KromeLook KromeTree::resolve_look(u32 node, KromeState state, const KromeLook* parent) const {
    const KromeNode& n = nodes[node];
    KromeLook l = looks[n.look];
    if (state != KromeState::Base && n.states[(u32)state]) overlay(l, looks[n.states[(u32)state] - 1]);
    // Disabled and selected are also a node's data: their looks apply on top of the state
    // the pointer gives it, the node's own or an ancestor's (a checkbox's box is selected
    // when the hit around it is).
    u32 flags = n.flags;
    for (u32 p = n.parent; p != KROME_NONE && p < nodes.len; p = nodes[p].parent) flags |= nodes[p].flags & (KROME_SELECTED | KROME_DISABLED);
    if ((flags & KROME_SELECTED) && state != KromeState::Selected && n.states[(u32)KromeState::Selected]) overlay(l, looks[n.states[(u32)KromeState::Selected] - 1]);
    if ((flags & KROME_DISABLED) && state != KromeState::Disabled && n.states[(u32)KromeState::Disabled]) overlay(l, looks[n.states[(u32)KromeState::Disabled] - 1]);
    if (!(l.set & KROME_LOOK_COLOR)) l.text_color = parent ? parent->text_color : color{1, 1, 1, 1};
    if (!(l.set & KROME_LOOK_FONT)) l.font = parent ? parent->font : name_intern(KROME_DEFAULT_FONT);
    if (!(l.set & KROME_LOOK_SIZE)) l.size = parent ? parent->size : KROME_DEFAULT_SIZE;
    if (!(l.set & KROME_LOOK_LINE_HEIGHT)) l.line_height = parent ? parent->line_height : 0.0f;
    if (parent) l.opacity *= parent->opacity;
    // `pixel` is a flag that inherits: a skin says it once, at its root.
    l.pixel = (n.flags & KROME_PIXEL) || (parent && parent->pixel);
    // What was inherited counts as said from here down.
    l.set |= KROME_LOOK_INHERITED;
    return l;
}

bool krome_parse_color(const char* s, color* out) {
    if (!s) return false;
    if (!strcmp(s, "none") || !strcmp(s, "transparent")) { *out = color{0, 0, 0, 0}; return true; }
    if (s[0] != '#') return false;
    usize n = strlen(s + 1);
    u32 v[8];
    if (n != 3 && n != 4 && n != 6 && n != 8) return false;
    for (usize i = 0; i < n; ++i) { int d = hexd(s[1 + i]); if (d < 0) return false; v[i] = (u32)d; }
    f32 c[4] = {1, 1, 1, 1};
    if (n <= 4) for (usize i = 0; i < n; ++i) c[i] = (f32)(v[i] * 17) / 255.0f;
    else for (usize i = 0; i < n / 2; ++i) c[i] = (f32)(v[2 * i] * 16 + v[2 * i + 1]) / 255.0f;
    *out = color{c[0], c[1], c[2], c[3]};
    return true;
}

const char* krome_type_name(KromeType t) {
    switch (t) {
    case KromeType::Box: return "box";
    case KromeType::Text: return "text";
    case KromeType::Image: return "image";
    case KromeType::Hit: return "hit";
    case KromeType::Edit: return "edit";
    }
    return "?";
}

static const char* const k_states[] = {"base", "hover", "pressed", "focused", "disabled", "selected"};
const char* krome_state_name(KromeState s) { return (u32)s < (u32)KromeState::Count ? k_states[(u32)s] : "?"; }
bool krome_state_of(const char* name, KromeState* out) {
    for (u32 i = 1; i < (u32)KromeState::Count; ++i) if (!strcmp(name, k_states[i])) { *out = (KromeState)i; return true; }
    return false;
}

} // namespace kv
