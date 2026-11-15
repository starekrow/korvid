// The tree's storage and its properties by name.
#include "korvid/krome/tree.h"
#include <string.h>

namespace kv {

const char* const KROME_DEFAULT_FONT = "text/DejaVuSans.ttf";

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
    else return false;
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
    if (!strcmp(prop, "font"))         { if (v.kind != KromeValue::String) *why = "font takes an asset path"; else { l.font = name_intern(v.s); l.set |= KROME_LOOK_FONT; } return true; }
    if (!strcmp(prop, "image"))        { if (v.kind != KromeValue::String) *why = "image takes an asset path"; else { l.image = name_intern(v.s); l.set |= KROME_LOOK_IMAGE; } return true; }
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
    if (s & KROME_LOOK_TINT) to.tint = from.tint;
    to.set |= s;
}

} // namespace

const char* KromeTree::set(u32 node, const char* prop, const KromeValue& v, KromeState state) {
    if (node >= nodes.len) return "no such node";
    KromeNode& n = nodes[node];
    if (state == KromeState::Base) {
        // Layout's properties.
        if (!strcmp(prop, "w")) { dirty = true; return size_value(n.w, v) ? nullptr : "w takes a number, hug, fill or a range"; }
        if (!strcmp(prop, "h")) { dirty = true; return size_value(n.h, v) ? nullptr : "h takes a number, hug, fill or a range"; }
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
    if (!strcmp(prop, "font") || !strcmp(prop, "size") || !strcmp(prop, "image")) dirty = true;
    return nullptr;
}

const char* KromeTree::flag(u32 node, const char* w) {
    if (node >= nodes.len) return "no such node";
    KromeNode& n = nodes[node];
    struct Bit { const char* word; u16 bit; };
    static const Bit bits[] = {{"fill", KROME_FILL}, {"clip", KROME_CLIP}, {"scroll", KROME_SCROLL},
                               {"disabled", KROME_DISABLED}, {"selected", KROME_SELECTED}, {"hidden", KROME_HIDDEN}};
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
    // the pointer gives it.
    if ((n.flags & KROME_SELECTED) && state != KromeState::Selected && n.states[(u32)KromeState::Selected]) overlay(l, looks[n.states[(u32)KromeState::Selected] - 1]);
    if ((n.flags & KROME_DISABLED) && state != KromeState::Disabled && n.states[(u32)KromeState::Disabled]) overlay(l, looks[n.states[(u32)KromeState::Disabled] - 1]);
    if (!(l.set & KROME_LOOK_COLOR)) l.text_color = parent ? parent->text_color : color{1, 1, 1, 1};
    if (!(l.set & KROME_LOOK_FONT)) l.font = parent ? parent->font : name_intern(KROME_DEFAULT_FONT);
    if (!(l.set & KROME_LOOK_SIZE)) l.size = parent ? parent->size : KROME_DEFAULT_SIZE;
    if (parent) l.opacity *= parent->opacity;
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
