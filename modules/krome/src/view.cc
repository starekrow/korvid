// Krome's view: text measured in the fonts it is drawn in, screens laid out against the
// window, painted into the renderer's UI lists (korvid/render/ui.h) only when something
// changed, and the pointer routed top first through overlays and screens
// (docs/PRDs/Krome-Slice-1-PRD.md, "Paint and the display list", "Input and overlays").
#include "internal.h"
#include "korvid/typeset/typeset.h"
#include "korvid/core/arena.h"
#include "korvid/asset/font.h"
#include "korvid/asset/texture.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/time.h"
#include "korvid/core/format.h"
#include "korvid/core/utf8.h"
#include "korvid/math/scalar.h"
#include "korvid/platform_hid/host_window.h"
#include "korvid/platform_hid/module.h"
#include "korvid/render/renderer.h"
#include <string.h>

namespace kv {

namespace {

constexpr f32 k_wheel_step = 40.0f; // logical pixels a notch of the wheel scrolls

// ---- fonts and images --------------------------------------------------------------------------

KromeFont* font_for(KromeModule& s, Name path, f32 size) {
    i32 px = (i32)(size * s.scale + 0.5f);
    if (px < 4) px = 4;
    if (px > 256) px = 256;
    for (KromeFont& f : s.fonts) if (f.path == path && f.px == (u32)px) return &f;
    KromeFont f{path, (u32)px, asset_load_font(*s.e, name_str(path), (f32)px), false};
    s.fonts.push(f);
    return &s.fonts.last();
}

const FontAsset* ready_font(KromeModule& s, KromeFont* f) {
    if (!f || f->asset.is_null()) return nullptr;
    const FontAsset* fa = asset_font(*s.e, f->asset);
    return fa && fa->state == AssetState::Ready ? fa : nullptr;
}

KromeImage* image_for(KromeModule& s, Name path) {
    for (KromeImage& i : s.images) if (i.path == path) return &i;
    KromeImage i{path, asset_load_texture(*s.e, name_str(path)), false};
    s.images.push(i);
    return &s.images.last();
}

const TextureAsset* ready_image(KromeModule& s, KromeImage* i) {
    if (!i || i->asset.is_null()) return nullptr;
    const TextureAsset* t = asset_texture(*s.e, i->asset);
    return t && t->state == AssetState::Ready ? t : nullptr;
}

// Layout's measures, in logical pixels: the font at the size it is drawn, a guess until it
// is ready (the screen is laid out again when it is).
vec2 measure_text(void* user, const char* str, u32 len, Name font, f32 size) {
    KromeModule& s = *(KromeModule*)user;
    const FontAsset* fa = ready_font(s, font_for(s, font, size));
    if (!fa) return vec2{(f32)utf8_count(str) * size * 0.55f, size * 1.25f};
    (void)len;
    return vec2{font_measure(*fa, str) / s.scale, fa->line_height / s.scale};
}

// A paragraph's measure in logical pixels: its widest line and height broken at `width`
// (logical; <= 0 unbroken), and the narrowest width it breaks to. A guess until the font is in.
vec2 measure_paragraph(void* user, const char* str, u32 len, Name font, f32 size, f32 lh, f32 width, f32* narrowest) {
    KromeModule& s = *(KromeModule*)user;
    const FontAsset* fa = ready_font(s, font_for(s, font, size));
    f32 line = lh > 0.0f ? floor(lh * size * s.scale + 0.5f) : 0.0f;
    if (!fa) {
        *narrowest = 0.0f;
        return vec2{(f32)utf8_count(str) * size * 0.55f, (line > 0.0f ? line / s.scale : size * 1.25f)};
    }
    TypesetMeasure m = typeset_measure(*fa, str, len, width > 0.0f ? width * s.scale : 0.0f, line);
    *narrowest = m.narrowest / s.scale;
    // One line unbroken is as wide as the font says it is: what painting measures too.
    return vec2{(width > 0.0f ? m.widest : m.widest) / s.scale, m.height / s.scale};
}

vec2 measure_image(void* user, Name path) {
    KromeModule& s = *(KromeModule*)user;
    const TextureAsset* t = ready_image(s, image_for(s, path));
    return t ? vec2{(f32)t->width, (f32)t->height} : vec2{0, 0};
}

// ---- rects -------------------------------------------------------------------------------------------

bool inside(rect2 r, f32 x, f32 y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

rect2 intersect(rect2 a, rect2 b) {
    f32 x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    f32 x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w, y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    return rect2{x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0};
}

const rect2 k_everywhere{-1e9f, -1e9f, 2e9f, 2e9f};

// ---- placing overlays ----------------------------------------------------------------------------------

bool anchor_rect(KromeScreen& sc, const KromeOverlay& o, rect2* out) {
    if (o.anchor.is_null()) return false;
    u32 n = krome_find(sc.tree, o.anchor);
    if (n != KROME_NONE) { *out = sc.tree.nodes[n].rect; return true; }
    for (KromeOverlay* below : sc.overlays) {
        if (below == &o) break;
        n = krome_find(below->tree, o.anchor);
        if (n != KROME_NONE) { *out = below->tree.nodes[n].rect; return true; }
    }
    return false;
}

void place_overlay(KromeScreen& sc, KromeOverlay& o, const KromeMeasure& m, f32 W, f32 H) {
    if (o.tree.root == KROME_NONE) { o.placed = true; return; }
    krome_layout(o.tree, o.tree.root, rect2{0, 0, W, H}, m, false);
    const KromeNode& root = o.tree.nodes[o.tree.root];
    f32 w = root.rect.w, h = root.rect.h;
    rect2 a;
    f32 x, y;
    if (o.side == KromeSide::Pointer || !anchor_rect(sc, o, &a)) {
        if (o.side == KromeSide::Pointer) { x = o.at.x; y = o.at.y; }
        else { x = (W - w) * 0.5f; y = (H - h) * 0.5f; } // no anchor: the window's center
    } else {
        switch (o.side) {
        case KromeSide::Above: x = a.x; y = a.y - h; if (y < 0 && a.y + a.h + h <= H) y = a.y + a.h; break;
        case KromeSide::Right: x = a.x + a.w; y = a.y; if (x + w > W && a.x - w >= 0) x = a.x - w; break;
        case KromeSide::Left:  x = a.x - w; y = a.y; if (x < 0 && a.x + a.w + w <= W) x = a.x + a.w; break;
        default:               x = a.x; y = a.y + a.h; if (y + h > H && a.y - h >= 0) y = a.y - h; break; // below, flipped up near the bottom
        }
    }
    // Kept inside the window.
    if (x + w > W) x = W - w;
    if (y + h > H) y = H - h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    krome_layout(o.tree, o.tree.root, rect2{x, y, w, h}, m, true);
    o.placed = true;
    sc.repaint = true;
}

// ---- scrollbars ----------------------------------------------------------------------------------------

constexpr f32 k_min_thumb = 20.0f; // logical pixels, or the track's length when that is less

// What a scroll box shows one way and how much there is (logical pixels); false when it
// does not scroll that way or it all fits.
bool overflows(const KromeNode& n, bool horizontal, f32* view, f32* extent) {
    if (n.dir == KromeDir::Stack || n.first == KROME_NONE) return false;
    if (horizontal ? !krome_scrolls_x(n) : !krome_scrolls_y(n)) return false;
    *view = horizontal ? n.rect.w - n.pad[0] - n.pad[2] : n.rect.h - n.pad[1] - n.pad[3];
    *extent = horizontal ? n.content.x : n.content.y;
    return *view > 0.0f && *extent > *view + 0.5f;
}

// A scroll box's bar one way, logical pixels: the track along the box's far edge (short of
// the corner when there is a bar the other way too) and the thumb in it.
struct BarShape { rect2 track, thumb; f32 view, extent, room; };

bool bar_of(const KromeTree& t, u32 node, bool horizontal, BarShape* b) {
    const KromeNode& n = t.nodes[node];
    if (!overflows(n, horizontal, &b->view, &b->extent)) return false;
    f32 v2, e2;
    bool other = overflows(n, !horizontal, &v2, &e2);
    f32 size = t.looks[n.look].scrollbar_size;
    rect2 r = n.rect;
    f32 corner = other ? size : 0.0f;
    b->track = horizontal ? rect2{r.x, r.y + r.h - size, r.w - corner, size} : rect2{r.x + r.w - size, r.y, size, r.h - corner};
    f32 len = horizontal ? b->track.w : b->track.h;
    f32 thumb = len * b->view / b->extent;
    f32 least = k_min_thumb < len ? k_min_thumb : len;
    if (thumb < least) thumb = least;
    b->room = len - thumb;
    f32 most = b->extent - b->view;
    f32 at = (horizontal ? n.scroll.x : n.scroll.y) / most * b->room;
    b->thumb = horizontal ? rect2{b->track.x + at, b->track.y, thumb, size} : rect2{b->track.x, b->track.y + at, size, thumb};
    return true;
}

// ---- paint ---------------------------------------------------------------------------------------------

struct Paint {
    KromeModule&      s;
    KromeScreen&      sc;
    const KromeTree&  tree;
    UiList&           list;
    Renderer*         r;
    u32               overlay;
    u32               hovered[KROME_MAX_DEPTH + 1] = {};
    u32               hovered_n = 0;
    u32               pressed = KROME_NONE;
};

color faded(color c, f32 o) { c.a *= o; return c; }

KromeState state_of(const Paint& p, u32 node) {
    // An edit shows the focus however it came; anything else only when Tab brought it (a
    // pressed button grows no ring).
    if (krome_is_focused(p.s, p.sc.id, p.overlay, node) && (p.s.focus.keyboard || p.tree.nodes[node].type == KromeType::Edit)) return KromeState::Focused;
    if (node == p.pressed) return KromeState::Pressed;
    for (u32 i = 0; i < p.hovered_n; ++i) if (p.hovered[i] == node) return KromeState::Hover;
    return KromeState::Base;
}

// ---- transitions ----------------------------------------------------------------------------------------

KromeModule::Colors colors_of(const KromeLook& l) { return KromeModule::Colors{l.bg, l.bg2, l.border_color, l.shadow_color, l.text_color, l.opacity}; }

bool same(const KromeModule::Colors& a, const KromeModule::Colors& b) { return !memcmp(&a, &b, sizeof a); }

color mix(color a, color b, f32 k) { return color{a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k, a.a + (b.a - a.a) * k}; }

KromeModule::Colors mix(const KromeModule::Colors& a, const KromeModule::Colors& b, f32 k) {
    return KromeModule::Colors{mix(a.bg, b.bg, k), mix(a.bg2, b.bg2, k), mix(a.border, b.border, k), mix(a.shadow, b.shadow, k), mix(a.text, b.text, k), a.opacity + (b.opacity - a.opacity) * k};
}

// Where a transition is at a time: eased in and out.
KromeModule::Colors at_time(const KromeModule::Anim& a, f64 now) {
    f32 k = a.dur > 0.0f ? (f32)((now - a.start) / a.dur) : 1.0f;
    if (k >= 1.0f) return a.to;
    if (k < 0.0f) k = 0.0f;
    return mix(a.from, a.to, k * k * (3.0f - 2.0f * k));
}

// A node with a transition: when the colors its look resolves to change, it eases there from
// where it was (mid-way, if a transition was running), and is painted where it is now.
void ease(Paint& p, u32 node, const KromeNode& n, KromeLook& l) {
    KromeModule& s = p.s;
    KromeModule::Colors target = colors_of(l);
    KromeModule::Anim* a = nullptr;
    for (KromeModule::Anim& x : s.anims) if (x.screen == p.sc.id && x.overlay == p.overlay && x.node == node) { a = &x; break; }
    if (!a || a->id != n.id) {
        // First seen (or another node at its index since a rebuild): where it is, at once.
        if (!a) { s.anims.alloc = s.general; s.anims.push(KromeModule::Anim{}); a = &s.anims.last(); }
        *a = KromeModule::Anim{p.sc.id, p.overlay, node, n.id, target, target, s.clock, l.transition};
        return;
    }
    if (!same(a->to, target)) {
        a->from = at_time(*a, s.clock);
        a->to = target;
        a->start = s.clock;
        a->dur = l.transition;
    }
    if (s.clock - a->start >= a->dur) return;
    KromeModule::Colors c = at_time(*a, s.clock);
    l.bg = c.bg;
    l.bg2 = c.bg2;
    l.border_color = c.border;
    l.shadow_color = c.shadow;
    l.text_color = c.text;
    l.opacity = c.opacity;
    s.animating = true;
}

void paint_text(Paint& p, const KromeNode& n, u32 node, const KromeLook& l, rect2 r, u32 clip) {
    const char* str = p.tree.text(node);
    if (!*str) return;
    KromeModule& s = p.s;
    const FontAsset* fa = ready_font(s, font_for(s, l.font, l.size));
    if (!fa) return;
    u32 slot = p.r ? renderer_texture_slot(p.r, fa->texture) : 0;
    f32 scale = s.scale;
    rect2 in{r.x + n.pad[0] * scale, r.y + n.pad[1] * scale, r.w - (n.pad[0] + n.pad[2]) * scale, r.h - (n.pad[1] + n.pad[3]) * scale};
    color c = faded(l.text_color, l.opacity);
    u32 len = (u32)strlen(str);
    // Broken into lines when one line is wider than the width it got (and it may wrap), or it
    // has a `\n`; then the paragraph's lines are aligned in that width. One line is placed as
    // it always was, whole.
    f32 one = font_measure(*fa, str);
    // Whether it fits is layout's width, not the pixel rect's: rounding each edge to a whole
    // pixel can take most of a pixel off a text that was given exactly its one line.
    f32 room = (n.rect.w - n.pad[0] - n.pad[2]) * scale;
    bool multi = strchr(str, '\n') || (!(n.flags & KROME_NOWRAP) && one > room + 0.5f) || l.line_height > 0.0f;
    if (multi) {
        TypesetParams tp;
        tp.width = (n.flags & KROME_NOWRAP) ? 0.0f : room + 0.5f;
        tp.align = n.align_x == KromeAlign::Center ? TypesetAlign::Center : n.align_x == KromeAlign::End ? TypesetAlign::Right : TypesetAlign::Left;
        tp.line_height = l.line_height > 0.0f ? floor(l.line_height * l.size * scale + 0.5f) : 0.0f;
        tp.subpixel = (n.flags & KROME_SUBPIXEL) != 0;
        Typeset t;
        t.glyphs.alloc = t.lines.alloc = mem_scratch();
        MemScope scope;
        typeset_paragraph(*fa, str, len, tp, t);
        f32 top = in.y;
        if (n.align_y == KromeAlign::Center) top += (in.h - t.height) * 0.5f;
        else if (n.align_y == KromeAlign::End) top += in.h - t.height;
        f32 x0 = floor(in.x + 0.5f), y0 = floor(top + 0.5f);
        for (const TypesetGlyph& tg : t.glyphs) {
            const Glyph* g = tg.glyph;
            if (!g || g->size.x <= 0 || g->size.y <= 0) continue;
            f32 pen = x0 + (tp.subpixel ? floor(tg.x) : tg.x);
            UiGlyph gl;
            f32 cx = pen + g->offset.x, cy = y0 + t.lines[tg.line].baseline - g->offset.y;
            gl.rect = rect2{floor(cx - g->size.x * 0.5f + 0.5f), floor(cy - g->size.y * 0.5f + 0.5f), g->size.x, g->size.y};
            gl.uv = g->uv;
            gl.texture = g->texture.is_null() ? slot : renderer_texture_slot(p.r, g->texture);
            gl.tint = c;
            gl.clip = clip;
            p.list.glyph(gl);
        }
        return;
    }
    f32 width = one, height = fa->line_height;
    f32 x = in.x, top = in.y;
    if (n.align_x == KromeAlign::Center) x += (in.w - width) * 0.5f;
    else if (n.align_x == KromeAlign::End) x += in.w - width;
    if (n.align_y == KromeAlign::Center) top += (in.h - height) * 0.5f;
    else if (n.align_y == KromeAlign::End) top += in.h - height;
    f32 baseline = floor(top + fa->ascent + 0.5f);
    x = floor(x + 0.5f);
    u32 prev = 0;
    for (const char* q = str; *q;) {
        u32 cp = utf8_next(q);
        if (prev) x += font_kern(*fa, prev, cp);
        prev = cp;
        // Whole pixels: each glyph's box snapped from the pen. Subpixel: the copy made at the
        // pen's quarter, drawn from the whole pixel under it.
        f32 pen = x;
        u32 quarter = 0;
        if (n.flags & KROME_SUBPIXEL) {
            pen = floor(x);
            quarter = (u32)((x - pen) * 4.0f + 0.5f);
            if (quarter == 4) { quarter = 0; pen += 1.0f; }
        }
        const Glyph* g = quarter ? font_glyph_at(*fa, cp, quarter) : font_glyph_or_replacement(*fa, cp);
        if (!g) continue;
        if (g->size.x > 0 && g->size.y > 0) {
            UiGlyph gl;
            f32 cx = pen + g->offset.x, cy = baseline - g->offset.y;
            gl.rect = rect2{floor(cx - g->size.x * 0.5f + 0.5f), floor(cy - g->size.y * 0.5f + 0.5f), g->size.x, g->size.y};
            gl.uv = g->uv;
            gl.texture = g->texture.is_null() ? slot : renderer_texture_slot(p.r, g->texture);
            gl.tint = c;
            gl.clip = clip;
            p.list.glyph(gl);
        }
        x += g->advance;
    }
}

void paint_edit(Paint& p, const KromeNode& n, u32 node, const KromeLook& l, rect2 r, rect2 clip_rect);

void paint_node(Paint& p, u32 node, const KromeLook* parent, rect2 clip_rect, u32 clip) {
    const KromeNode& n = p.tree.nodes[node];
    if (n.flags & KROME_HIDDEN) return;
    KromeLook l = p.tree.resolve_look(node, state_of(p, node), parent);
    if (l.transition > 0.0f) ease(p, node, n, l);
    f32 scale = p.s.scale;
    rect2 r = krome_pixel_rect(n.rect, scale);
    const TextureAsset* img = n.type == KromeType::Image && (l.set & KROME_LOOK_IMAGE) ? ready_image(p.s, image_for(p.s, l.image)) : nullptr;
    bool fill = l.bg.a > 0.0f || ((l.set & KROME_LOOK_BG2) && l.bg2.a > 0.0f);
    bool edge = l.border > 0.0f && l.border_color.a > 0.0f;
    bool shade = l.shadow_color.a > 0.0f;
    if (fill || edge || shade || img) {
        UiBox b;
        b.rect = r;
        for (u32 i = 0; i < 4; ++i) b.radius[i] = l.radius[i] * scale;
        b.fill = faded(l.bg, l.opacity);
        if (l.set & KROME_LOOK_BG2) { b.gradient = true; b.fill2 = faded(l.bg2, l.opacity); b.angle = l.angle; }
        if (edge) {
            f32 w = floor(l.border * scale + 0.5f);
            b.border = w < 1.0f ? 1.0f : w;
            b.border_color = faded(l.border_color, l.opacity);
        }
        if (shade) {
            b.shadow_offset = vec2{l.shadow_offset.x * scale, l.shadow_offset.y * scale};
            b.shadow_blur = l.shadow_blur * scale;
            b.shadow_spread = l.shadow_spread * scale;
            b.shadow_color = faded(l.shadow_color, l.opacity);
        }
        if (img) {
            b.textured = true;
            b.texture = p.r ? renderer_texture_slot(p.r, img->texture) : 0;
            if (!fill) b.fill = faded(l.tint, l.opacity);
        }
        b.clip = clip;
        p.list.box(b);
    }
    if (n.type == KromeType::Text) paint_text(p, n, node, l, r, clip);
    if (n.type == KromeType::Edit) paint_edit(p, n, node, l, r, clip_rect);
    if (n.first == KROME_NONE) return;
    if (n.flags & (KROME_CLIP | KROME_SCROLL)) {
        clip_rect = intersect(clip_rect, r);
        clip = p.list.clip(clip_rect);
    }
    for (u32 ch = n.first; ch != KROME_NONE; ch = p.tree.nodes[ch].next) paint_node(p, ch, &l, clip_rect, clip);
    // A scroll box's bars, over its content.
    if (!(n.flags & (KROME_SCROLL | KROME_SCROLL_X | KROME_SCROLL_Y))) return;
    const KromeLook& bl = p.tree.looks[n.look];
    for (u32 h = 0; h < 2; ++h) {
        BarShape bs;
        if (!bar_of(p.tree, node, h == 1, &bs)) continue;
        if (bl.scrollbar_track.a > 0.0f) {
            UiBox b;
            b.rect = krome_pixel_rect(bs.track, scale);
            b.fill = faded(bl.scrollbar_track, l.opacity);
            b.clip = clip;
            p.list.box(b);
        }
        // Inset from the edges by a pixel, rounded; brighter while it is being dragged.
        const KromeModule::Bar& drag = p.s.bar;
        bool held = drag.active && drag.box.screen == p.sc.id && drag.box.overlay == p.overlay && drag.box.node == node && drag.horizontal == (h == 1);
        color c = bl.scrollbar_color;
        if (held) { c.a *= 1.6f; if (c.a > 1.0f) c.a = 1.0f; }
        rect2 th = bs.thumb;
        if (h == 1) { th.y += 1.0f; th.h -= 2.0f; } else { th.x += 1.0f; th.w -= 2.0f; }
        UiBox b;
        b.rect = krome_pixel_rect(th, scale);
        f32 rr = (h == 1 ? b.rect.h : b.rect.w) * 0.5f;
        for (u32 i = 0; i < 4; ++i) b.radius[i] = rr;
        b.fill = faded(c, l.opacity);
        b.clip = clip;
        p.list.box(b);
    }
}

// An edit: its text (the buffer while it has the focus, an input method's composition at
// the caret, the placeholder faded while it is empty), the selection under it, the caret
// over it, all clipped to its inside and moved to keep the caret in view. A press or a drag
// waiting on its layout is resolved here.
void paint_edit(Paint& p, const KromeNode& n, u32 node, const KromeLook& l, rect2 r, rect2 clip_rect) {
    KromeModule& s = p.s;
    const FontAsset* fa = ready_font(s, font_for(s, l.font, l.size));
    if (!fa) return;
    f32 scale = s.scale;
    rect2 in{r.x + n.pad[0] * scale, r.y + n.pad[1] * scale, r.w - (n.pad[0] + n.pad[2]) * scale, r.h - (n.pad[1] + n.pad[3]) * scale};
    if (in.w <= 0 || in.h <= 0) return;
    u32 clip = p.list.clip(intersect(clip_rect, in));
    bool focused = krome_is_focused(s, p.sc.id, p.overlay, node);
    KromeFocus& f = s.focus;
    bool multi = (n.flags & KROME_MULTILINE) != 0;
    const char* text = focused ? f.buf.c_str() : p.tree.text(node);
    u32 len = focused ? f.buf.len : (u32)strlen(text);
    // The composition goes into what is drawn, at the caret, and nowhere else.
    MemScope scope;
    u32 comp = focused ? (u32)strlen(f.composing) : 0;
    u32 caret = focused ? f.buf.caret : 0;
    if (comp) {
        char* shown = (char*)alloc_in(mem_scratch(), len + comp + 1, 1);
        memcpy(shown, text, caret);
        memcpy(shown + caret, f.composing, comp);
        memcpy(shown + caret + comp, text + caret, len - caret);
        len += comp;
        shown[len] = 0;
        text = shown;
    }
    bool placeholder = len == 0 && n.placeholder != KROME_NONE;
    const char* drawn = placeholder ? p.tree.strings.data + n.placeholder : text;
    u32 drawn_len = placeholder ? (u32)strlen(drawn) : len;

    TypesetParams tp;
    tp.width = multi ? in.w : 0.0f;
    tp.align = n.align_x == KromeAlign::Center ? TypesetAlign::Center : n.align_x == KromeAlign::End ? TypesetAlign::Right : TypesetAlign::Left;
    tp.line_height = l.line_height > 0.0f ? floor(l.line_height * l.size * scale + 0.5f) : 0.0f;
    tp.subpixel = (n.flags & KROME_SUBPIXEL) != 0;
    if ((n.flags & KROME_SECRET) && !placeholder) tp.mask = 0x2022; // a bullet for each character
    Typeset t;
    t.lines.alloc = t.glyphs.alloc = mem_scratch();
    typeset_paragraph(*fa, drawn, drawn_len, tp, t);
    // One line sits in the middle of the inside; many start at its top.
    f32 top = multi ? in.y : in.y + floor((in.h - t.line_height) * 0.5f + 0.5f);
    vec2 scroll = focused ? f.scroll : vec2{0, 0};

    if (focused) {
        f.font = fa;
        f.params = tp;
        f.view_h = in.h;
        // What the pointer did, now that the text is laid out (the buffer's own: no composition).
        if (f.pending) {
            f.pending = false;
            Typeset bt;
            bt.lines.alloc = bt.glyphs.alloc = mem_scratch();
            typeset_paragraph(*fa, f.buf.c_str(), f.buf.len, tp, bt);
            f32 lx = f.pending_at.x - in.x + f.scroll.x, ly = f.pending_at.y - top + f.scroll.y;
            u32 at = typeset_hit(bt, lx, ly);
            // A secret's words are not told apart: a double press takes it all.
            if (f.pending_clicks == 2 && (n.flags & KROME_SECRET)) f.pending_clicks = 3;
            if (f.pending_clicks == 2) {
                typeset_word_at(f.buf.c_str(), f.buf.len, at, &f.word_begin, &f.word_end);
                f.buf.anchor = f.word_begin;
                f.buf.move(f.word_end, true);
            } else if (f.pending_clicks == 3) {
                const TypesetLine& ln = bt.lines[typeset_line_of(bt, at)];
                f.word_begin = ln.begin;
                f.word_end = ln.next;
                f.buf.anchor = ln.begin;
                f.buf.move(ln.next, true);
            } else if (f.pending_clicks == 0 && f.clicks >= 2 && f.word_end > f.word_begin) {
                // A drag after a double or triple press keeps whole words (lines).
                if (at < f.word_begin) { f.buf.anchor = f.word_end; u32 b0 = 0, b1 = 0; typeset_word_at(f.buf.c_str(), f.buf.len, at, &b0, &b1); f.buf.move(b0, true); }
                else if (at > f.word_end) { f.buf.anchor = f.word_begin; u32 b0 = 0, b1 = 0; typeset_word_at(f.buf.c_str(), f.buf.len, at, &b0, &b1); f.buf.move(b1, true); }
            } else {
                f.buf.move(at, f.pending_extend);
            }
            f.want_x = -1.0f;
            caret = f.buf.caret;
            if (comp == 0) { t.free(); typeset_paragraph(*fa, f.buf.c_str(), f.buf.len, tp, t); }
        }
        // Keep the caret in view: across in one line, down in many.
        rect2 cr = typeset_caret(t, caret + f.compose_caret * (comp ? 1 : 0));
        if (!multi) {
            if (cr.x - f.scroll.x > in.w - 2.0f) f.scroll.x = cr.x - in.w + 2.0f;
            if (cr.x - f.scroll.x < 0.0f) f.scroll.x = cr.x;
            f32 most = t.width + 2.0f - in.w;
            if (f.scroll.x > most) f.scroll.x = most > 0.0f ? most : 0.0f;
            if (f.scroll.x < 0.0f) f.scroll.x = 0.0f;
        } else {
            if (cr.y + cr.h - f.scroll.y > in.h) f.scroll.y = cr.y + cr.h - in.h;
            if (cr.y - f.scroll.y < 0.0f) f.scroll.y = cr.y;
            f32 most = t.height - in.h;
            if (f.scroll.y > most) f.scroll.y = most > 0.0f ? most : 0.0f;
            if (f.scroll.y < 0.0f) f.scroll.y = 0.0f;
        }
        scroll = f.scroll;
    }
    f32 ox = floor(in.x + 0.5f) - floor(scroll.x + 0.5f), oy = top - floor(scroll.y + 0.5f);
    if (focused) f.origin = vec2{ox, oy};

    // The selection, under the text.
    if (focused && f.buf.selected() && !comp) {
        rect2 sel[64];
        u32 ns = typeset_selection(t, f.buf.sel_begin(), f.buf.sel_end(), sel, 64);
        color sc = faded((l.set & KROME_LOOK_SELECTION_COLOR) ? l.selection_color : KromeLook{}.selection_color, l.opacity);
        for (u32 i = 0; i < ns; ++i) {
            UiBox b;
            b.rect = rect2{floor(ox + sel[i].x), oy + sel[i].y, floor(sel[i].w + 0.5f), sel[i].h};
            b.fill = sc;
            b.clip = clip;
            p.list.box(b);
        }
    }
    // The text.
    u32 slot = p.r ? renderer_texture_slot(p.r, fa->texture) : 0;
    color c = faded(l.text_color, l.opacity * (placeholder ? 0.45f : 1.0f));
    for (const TypesetGlyph& tg : t.glyphs) {
        const Glyph* g = tg.glyph;
        if (!g || g->size.x <= 0 || g->size.y <= 0) continue;
        f32 pen = ox + (tp.subpixel ? floor(tg.x) : tg.x);
        UiGlyph gl;
        f32 cx = pen + g->offset.x, cy = oy + t.lines[tg.line].baseline - g->offset.y;
        gl.rect = rect2{floor(cx - g->size.x * 0.5f + 0.5f), floor(cy - g->size.y * 0.5f + 0.5f), g->size.x, g->size.y};
        gl.uv = g->uv;
        gl.texture = g->texture.is_null() ? slot : renderer_texture_slot(p.r, g->texture);
        gl.tint = c;
        gl.clip = clip;
        p.list.glyph(gl);
    }
    if (!focused) return;
    f32 px1 = scale >= 1.0f ? floor(scale + 0.5f) : 1.0f;
    // The composition, underlined.
    if (comp) {
        rect2 a = typeset_caret(t, caret), z = typeset_caret(t, caret + comp);
        UiBox u;
        u.rect = rect2{floor(ox + a.x), oy + a.y + a.h - px1 - 1.0f, floor(z.x - a.x + 0.5f), px1};
        u.fill = c;
        u.clip = clip;
        p.list.box(u);
    }
    // The caret, while the blink has it on.
    rect2 cr = typeset_caret(t, caret + (comp ? f.compose_caret : 0));
    if (f.blink_on && !(n.flags & KROME_READONLY)) {
        UiBox k;
        k.rect = rect2{floor(ox + cr.x), oy + cr.y, px1, cr.h};
        k.fill = faded((l.set & KROME_LOOK_CARET_COLOR) ? l.caret_color : l.text_color, l.opacity);
        k.clip = clip;
        p.list.box(k);
    }
    // The input method opens beside the caret, where a composition began (not its own caret,
    // which it moves itself): told when that moves, not every frame, since an input method
    // answers each telling with its state.
    rect2 at = typeset_caret(t, caret);
    rect2 area{floor(ox + at.x), oy + at.y, in.w, at.h};
    if (area.x != f.told.x || area.y != f.told.y || area.h != f.told.h) { f.told = area; platform_text_area(*s.e, area, 0.0f); }
}

// The nodes the pointer is over (the deepest and its ancestors), and the one being pressed,
// in this tree.
void pointer_states(Paint& p) {
    const KromeTarget& u = p.s.under;
    if (u.screen == p.sc.id && u.overlay == p.overlay && u.node != KROME_NONE)
        for (u32 n = u.node; n != KROME_NONE && p.hovered_n <= KROME_MAX_DEPTH; n = p.tree.nodes[n].parent) p.hovered[p.hovered_n++] = n;
    const KromeTarget& c = p.s.captured;
    if (p.s.down && c.screen == p.sc.id && c.overlay == p.overlay && c.node != KROME_NONE)
        for (u32 i = 0; i < p.hovered_n; ++i) if (p.hovered[i] == c.node) { p.pressed = c.node; break; }
}

void paint_tree(KromeModule& s, KromeScreen& sc, const KromeTree& t, u32 overlay, UiList& list) {
    if (t.root == KROME_NONE) return;
    Paint p{s, sc, t, list, renderer_get(*s.e), overlay, {}, 0, KROME_NONE};
    pointer_states(p);
    paint_node(p, t.root, nullptr, k_everywhere, 0);
}

// ---- the pointer ------------------------------------------------------------------------------------------

// The deepest node under (x, y), physical pixels; later children are over earlier ones.
u32 hit_node(const KromeTree& t, u32 node, f32 x, f32 y, f32 scale, rect2 clip) {
    const KromeNode& n = t.nodes[node];
    if ((n.flags & KROME_HIDDEN) || !inside(clip, x, y)) return KROME_NONE;
    rect2 r = krome_pixel_rect(n.rect, scale);
    rect2 inner = (n.flags & (KROME_CLIP | KROME_SCROLL)) ? intersect(clip, r) : clip;
    u32 best = KROME_NONE;
    for (u32 ch = n.first; ch != KROME_NONE; ch = t.nodes[ch].next) {
        u32 h = hit_node(t, ch, x, y, scale, inner);
        if (h != KROME_NONE) best = h;
    }
    if (best != KROME_NONE) return best;
    return inside(r, x, y) ? node : KROME_NONE;
}

// Screens top first: a higher layer, then the later opened.
void screens_top_first(KromeModule& s, KromeScreen** out, u32* n) {
    *n = 0;
    for (KromeScreen* sc : s.screens) if (sc->ready) out[(*n)++] = sc;
    for (u32 i = 1; i < *n; ++i)
        for (u32 j = i; j > 0; --j) {
            KromeScreen* a = out[j - 1];
            KromeScreen* b = out[j];
            bool b_first = b->layer > a->layer || (b->layer == a->layer && b->id > a->id);
            if (!b_first) break;
            out[j - 1] = b;
            out[j] = a;
        }
}

constexpr u32 k_max_screens = 64;

} // namespace

void krome_drop_anims(KromeModule& s, u32 screen, u32 overlay) {
    u32 w = 0;
    for (u32 i = 0; i < s.anims.len; ++i)
        if (!(s.anims[i].screen == screen && s.anims[i].overlay == overlay)) s.anims[w++] = s.anims[i];
    s.anims.len = w;
}

KromeScreen* krome_top_screen(KromeModule& s) {
    KromeScreen* order[k_max_screens];
    u32 n = 0;
    screens_top_first(s, order, &n);
    return n ? order[0] : nullptr;
}

namespace {

// What the pointer is over, top first. A modal overlay hides everything under it: over
// anything else the answer is the modal itself with no node.
KromeTarget hit(KromeModule& s, f32 x, f32 y) {
    KromeScreen* order[k_max_screens];
    u32 n = 0;
    screens_top_first(s, order, &n);
    for (u32 i = 0; i < n && i < k_max_screens; ++i) {
        KromeScreen& sc = *order[i];
        for (u32 k = sc.overlays.len; k > 0; --k) {
            KromeOverlay& o = *sc.overlays[k - 1];
            if (o.passive) continue; // a tooltip: the pointer goes through it
            u32 h = o.tree.root != KROME_NONE ? hit_node(o.tree, o.tree.root, x, y, s.scale, k_everywhere) : KROME_NONE;
            if (h != KROME_NONE) return KromeTarget{sc.id, o.id, h};
            if (o.modal) return KromeTarget{sc.id, o.id, KROME_NONE};
        }
        if (sc.tree.root == KROME_NONE) continue;
        u32 h = hit_node(sc.tree, sc.tree.root, x, y, s.scale, k_everywhere);
        if (h != KROME_NONE) return KromeTarget{sc.id, 0, h};
    }
    return KromeTarget{};
}

// The node a press on `t` belongs to: the nearest with an event, or a hit; none when it or
// an ancestor is disabled.
KromeTarget pressable(KromeModule& s, KromeTarget t) {
    KromeScreen* sc = nullptr;
    KromeTree* tree = krome_target_tree(s, t, &sc);
    if (!tree || t.node == KROME_NONE) return KromeTarget{};
    u32 found = KROME_NONE;
    for (u32 n = t.node; n != KROME_NONE; n = tree->nodes[n].parent) {
        const KromeNode& node = tree->nodes[n];
        if (node.flags & KROME_DISABLED) return KromeTarget{};
        if (found == KROME_NONE && (!node.event.is_null() || !node.binding.is_null() || !node.behave.is_null() || node.type == KromeType::Hit || node.type == KromeType::Edit)) found = n;
    }
    t.node = found;
    return found == KROME_NONE ? KromeTarget{} : t;
}

void repaint(KromeModule& s, const KromeTarget& t) {
    if (!t.screen) return;
    if (KromeScreen* sc = krome_screen_by_id(s, t.screen)) sc->repaint = true;
}

// The screen whose top overlay is the topmost light one, or null.
KromeScreen* light_overlays_screen(KromeModule& s) {
    KromeScreen* order[k_max_screens];
    u32 n = 0;
    screens_top_first(s, order, &n);
    for (u32 i = 0; i < n; ++i)
        for (u32 k = order[i]->overlays.len; k > 0; --k) {
            const KromeOverlay& o = *order[i]->overlays[k - 1];
            if (o.passive) continue;
            return o.modal ? nullptr : order[i];
        }
    return nullptr;
}

// ---- the pointer on scrollbars, the wheel, tooltips -------------------------------------------------

// A press on a scrollbar under the pointer: on the thumb it takes it for a drag, on the
// track it pages toward the press. True when it was on one.
bool bar_press(KromeModule& s, const KromeTarget& under, f32 x, f32 y) {
    KromeScreen* sc = nullptr;
    KromeTree* t = under.node != KROME_NONE ? krome_target_tree(s, under, &sc) : nullptr;
    f32 lx = x / s.scale, ly = y / s.scale;
    for (u32 c = t ? under.node : KROME_NONE; c != KROME_NONE; c = t->nodes[c].parent) {
        for (u32 h = 0; h < 2; ++h) {
            BarShape bs;
            if (!bar_of(*t, c, h == 1, &bs) || !inside(bs.track, lx, ly)) continue;
            KromeNode& n = t->nodes[c];
            s.bar = KromeModule::Bar{KromeTarget{under.screen, under.overlay, c}, h == 1, -1.0f, true};
            if (inside(bs.thumb, lx, ly)) {
                s.bar.grab = h == 1 ? lx - bs.thumb.x : ly - bs.thumb.y;
            } else {
                f32 along = h == 1 ? lx : ly, start = h == 1 ? bs.thumb.x : bs.thumb.y;
                f32& sv = h == 1 ? n.scroll.x : n.scroll.y;
                sv += along < start ? -bs.view : bs.view;
            }
            t->dirty = true;
            if (under.overlay) if (KromeOverlay* o = krome_overlay(*sc, under.overlay)) o->placed = false;
            sc->repaint = true;
            return true;
        }
    }
    return false;
}

// A held thumb follows the pointer.
void bar_drag(KromeModule& s, f32 x, f32 y) {
    KromeScreen* sc = nullptr;
    KromeTree* t = krome_target_tree(s, s.bar.box, &sc);
    if (!t || s.bar.box.node >= t->nodes.len || s.bar.grab < 0.0f) return;
    BarShape bs;
    bool h = s.bar.horizontal;
    if (!bar_of(*t, s.bar.box.node, h, &bs) || bs.room <= 0.0f) return;
    f32 along = (h ? x / s.scale - bs.track.x : y / s.scale - bs.track.y) - s.bar.grab;
    f32 k = along / bs.room;
    k = k < 0.0f ? 0.0f : k > 1.0f ? 1.0f : k;
    KromeNode& n = t->nodes[s.bar.box.node];
    f32 v = k * (bs.extent - bs.view);
    f32& sv = h ? n.scroll.x : n.scroll.y;
    if (sv == v) return;
    sv = v;
    t->dirty = true;
    if (s.bar.box.overlay) if (KromeOverlay* o = krome_overlay(*sc, s.bar.box.overlay)) o->placed = false;
    sc->repaint = true;
}

// The node with a behaviour the pointer is over hears it come and go.
void follow_hover(KromeModule& s, const KromeTarget& under) {
    KromeTarget h;
    KromeTree* t = under.node != KROME_NONE ? krome_target_tree(s, under, nullptr) : nullptr;
    for (u32 c = t ? under.node : KROME_NONE; c != KROME_NONE; c = t->nodes[c].parent)
        if (!t->nodes[c].behave.is_null()) { h = KromeTarget{under.screen, under.overlay, c}; break; }
    if (h == s.hover_behave) return;
    KromeTarget was = s.hover_behave;
    s.hover_behave = h;
    KromeCallArgs a;
    a.on = false;
    if (was.node != KROME_NONE) krome_behave(s, was, KromeCall::Hover, a);
    a.on = true;
    if (h.node != KROME_NONE) krome_behave(s, h, KromeCall::Hover, a);
}

void close_tip(KromeModule& s) {
    if (!s.tip_overlay) return;
    u32 id = s.tip_overlay;
    s.tip_overlay = 0;
    if (KromeScreen* sc = krome_screen_by_id(s, s.tip.screen)) krome_close_overlay(s, *sc, id);
}

// The node with a tooltip the pointer rests on: shown once it has rested long enough, shut
// when it leaves or presses.
void follow_tip(KromeModule& s, const KromeTarget& under, bool press) {
    KromeTarget tip;
    KromeScreen* sc = nullptr;
    KromeTree* t = under.node != KROME_NONE ? krome_target_tree(s, under, &sc) : nullptr;
    for (u32 c = t ? under.node : KROME_NONE; c != KROME_NONE; c = t->nodes[c].parent)
        if (t->nodes[c].tooltip != KROME_NONE) { tip = KromeTarget{under.screen, under.overlay, c}; break; }
    f64 now = time_now();
    if (!(tip == s.tip)) {
        close_tip(s);
        s.tip = tip;
        s.tip_since = now;
        s.tip_shut = false;
        if (tip.node != KROME_NONE) engine_want_frame_at(*s.e, now + s.tip_delay);
    }
    if (press) { close_tip(s); s.tip_shut = true; }
    if (tip.node == KROME_NONE || s.tip_overlay || s.tip_shut || now - s.tip_since < s.tip_delay || !sc) return;
    char text[512];
    format(text, sizeof text, "%s", t->strings.data + t->nodes[tip.node].tooltip);
    u32 id = krome_open_overlay(s, *sc);
    KromeOverlay* o = krome_overlay(*sc, id);
    o->passive = true;
    o->side = KromeSide::Pointer;
    o->at = vec2{s.px / s.scale + 12.0f, s.py / s.scale + 18.0f}; // below and right of the arrow
    if (!krome_tooltip_overlay(s, *sc, *o, text)) { krome_close_overlay(s, *sc, id); s.tip_shut = true; return; }
    s.tip_overlay = id;
}

} // namespace

// ---- overlays -----------------------------------------------------------------------------------------------

KromeOverlay* krome_overlay(KromeScreen& sc, u32 id) {
    for (KromeOverlay* o : sc.overlays) if (o->id == id) return o;
    return nullptr;
}

u32 krome_open_overlay(KromeModule& s, KromeScreen& sc) {
    KromeOverlay* o = alloc_items<KromeOverlay>(s.general, 1);
    *o = KromeOverlay{};
    o->id = sc.next_overlay++;
    o->tree.init(s.general);
    o->at = vec2{s.px / s.scale, s.py / s.scale};
    sc.overlays.alloc = s.general;
    sc.overlays.push(o);
    sc.repaint = true;
    return o->id;
}

void krome_close_overlay(KromeModule& s, KromeScreen& sc, u32 id) {
    if (!sc.overlays.len) return;
    u32 at = sc.overlays.len - 1;
    while (!id && at > 0 && sc.overlays[at]->passive) --at; // "the top one" is never a tooltip
    if (!id && sc.overlays[at]->passive) return;
    if (id) {
        at = KROME_NONE;
        for (u32 i = 0; i < sc.overlays.len; ++i) if (sc.overlays[i]->id == id) at = i;
        if (at == KROME_NONE) return;
    }
    KromeOverlay* o = sc.overlays[at];
    if (s.tip_overlay == o->id && s.tip.screen == sc.id) s.tip_overlay = 0;
    if (s.bar.box.screen == sc.id && s.bar.box.overlay == o->id) s.bar = KromeModule::Bar{};
    if (s.hover_behave.screen == sc.id && s.hover_behave.overlay == o->id) s.hover_behave = KromeTarget{};
    krome_drop_anims(s, sc.id, o->id);
    for (u32 i = at + 1; i < sc.overlays.len; ++i) sc.overlays[i - 1] = sc.overlays[i];
    --sc.overlays.len;
    // What the pointer was on in it is gone, and the keyboard if an edit in it had it.
    if (s.focus.node != KROME_NONE && s.focus.screen == sc.id && s.focus.overlay == o->id) krome_edit_blur(s);
    if (s.under.screen == sc.id && s.under.overlay == o->id) s.under = KromeTarget{};
    if (s.captured.screen == sc.id && s.captured.overlay == o->id) s.captured = KromeTarget{};
    o->tree.free();
    if (o->arena.base) arena_destroy(o->arena);
    free_items(s.general, o, 1);
    sc.repaint = true;
}

void krome_free_overlays(KromeModule& s, KromeScreen& sc) {
    while (sc.overlays.len) krome_close_overlay(s, sc, 0);
    sc.overlays.free();
}

KromeTree* krome_target_tree(KromeModule& s, const KromeTarget& t, KromeScreen** screen) {
    KromeScreen* sc = t.screen ? krome_screen_by_id(s, t.screen) : nullptr;
    if (screen) *screen = sc;
    if (!sc) return nullptr;
    if (!t.overlay) return &sc->tree;
    KromeOverlay* o = krome_overlay(*sc, t.overlay);
    return o ? &o->tree : nullptr;
}

// ---- the frame ------------------------------------------------------------------------------------------------

void krome_update_viewport(KromeModule& s) {
    if (s.viewport_fixed) return;
    HostWindow* w = platform_window(*s.e);
    if (!w || !w->width() || !w->height()) return;
    f32 W = (f32)w->width(), H = (f32)w->height(), k = w->pixel_scale();
    if (!(k > 0.0f)) k = 1.0f;
    if (W == s.win_w && H == s.win_h && k == s.scale) return;
    s.win_w = W;
    s.win_h = H;
    s.scale = k;
    for (KromeScreen* sc : s.screens) {
        sc->tree.dirty = true;
        for (KromeOverlay* o : sc->overlays) o->placed = false;
    }
}

void krome_paint_screen(KromeModule& s, KromeScreen& sc, UiList& list) {
    paint_tree(s, sc, sc.tree, 0, list);
    for (KromeOverlay* o : sc.overlays) {
        if (o->dim.a > 0.0f) {
            UiBox d;
            d.rect = rect2{0, 0, s.win_w, s.win_h};
            d.fill = o->dim;
            list.box(d);
        }
        paint_tree(s, sc, o->tree, o->id, list);
    }
}

void krome_view_frame(KromeModule& s) {
    krome_update_viewport(s);
    if (!(s.win_w > 0.0f && s.win_h > 0.0f)) return;
    // A transition mid-way paints again, and asks for the frame to.
    if (s.animating) {
        s.animating = false;
        for (KromeScreen* sc : s.screens) sc->repaint = true;
        engine_want_frame(*s.e);
    }
    // A font or an image that has landed changes what text and images measure.
    bool landed = false;
    for (KromeFont& f : s.fonts)
        if (!f.ready && ready_font(s, &f)) { f.ready = true; landed = true; }
    for (KromeImage& i : s.images)
        if (!i.ready && ready_image(s, &i)) { i.ready = true; landed = true; }
    // Something still on its way keeps a loop that paces on demand ticking until it lands.
    bool waiting = false;
    for (KromeFont& f : s.fonts) if (!f.ready && !f.asset.is_null()) waiting = true;
    for (KromeImage& i : s.images) if (!i.ready && !i.asset.is_null()) waiting = true;
    for (KromeScreen* sc : s.screens) if (sc->loading) waiting = true;
    if (waiting) engine_want_frame(*s.e);
    f32 W = s.win_w / s.scale, H = s.win_h / s.scale;
    KromeMeasure m{&s, measure_text, measure_image, measure_paragraph};
    for (KromeScreen* sc : s.screens) {
        if (!sc->ready) continue;
        if (landed) sc->tree.dirty = true;
        if (sc->tree.dirty && sc->tree.root != KROME_NONE) {
            krome_layout(sc->tree, sc->tree.root, rect2{0, 0, W, H}, m, true);
            ++s.layouts;
            // A virtualized each whose view left what it built is built again, and laid out.
            if (krome_virtual_regions(s, *sc) && sc->tree.root != KROME_NONE) {
                krome_layout(sc->tree, sc->tree.root, rect2{0, 0, W, H}, m, true);
                ++s.layouts;
            }
            sc->repaint = true;
            for (KromeOverlay* o : sc->overlays) o->placed = false;
        }
        for (KromeOverlay* o : sc->overlays)
            if (!o->placed || o->tree.dirty || landed) place_overlay(*sc, *o, m, W, H);
    }

    // The lists, a layer at a time: rebuilt where a screen on it changed, cleared where no
    // screen is left.
    Renderer* r = renderer_get(*s.e);
    if (r && !s.warned_no_ui && !renderer_caps(r).ui) {
        KV_WARN("krome", "the %s renderer does not draw the UI primitive yet: Krome's screens run but show nothing", renderer_caps(r).backend);
        s.warned_no_ui = true;
    }
    i32 layers[k_max_screens];
    u32 nl = 0;
    for (KromeScreen* sc : s.screens) {
        if (!sc->ready) continue;
        bool seen = false;
        for (u32 i = 0; i < nl; ++i) if (layers[i] == sc->layer) seen = true;
        if (!seen && nl < k_max_screens) layers[nl++] = sc->layer;
    }
    for (u32 i = 0; i < nl; ++i) {
        i32 layer = layers[i];
        bool known = false, changed = false;
        for (i32 l : s.layers) if (l == layer) known = true;
        for (KromeScreen* sc : s.screens) if (sc->ready && sc->layer == layer && sc->repaint) changed = true;
        if (known && !changed) continue;
        s.list.clear();
        for (KromeScreen* sc : s.screens) {
            if (!sc->ready || sc->layer != layer) continue;
            krome_paint_screen(s, *sc, s.list);
            sc->repaint = false;
        }
        ++s.paints;
        if (r) { renderer_ui_set(r, layer, s.list); ++s.submits; engine_want_frame(*s.e); }
        if (!known) s.layers.push(layer);
    }
    for (u32 i = 0; i < s.layers.len;) {
        bool live = false;
        for (u32 k = 0; k < nl; ++k) if (layers[k] == s.layers[i]) live = true;
        if (live) { ++i; continue; }
        if (r) renderer_ui_clear(r, s.layers[i]);
        s.layers[i] = s.layers.last();
        --s.layers.len;
    }    if (s.animating) engine_want_frame(*s.e);
}

void krome_pointer(KromeModule& s, f32 x, f32 y, bool down, f32 wheel, bool escape) {
    s.px = x;
    s.py = y;
    // Escape (a press) closes the topmost overlay, light or modal.
    if (escape) {
        KromeScreen* order[k_max_screens];
        u32 n = 0;
        screens_top_first(s, order, &n);
        for (u32 i = 0; i < n; ++i) {
            bool any = false;
            for (KromeOverlay* o : order[i]->overlays) if (!o->passive) any = true;
            if (any) { krome_close_overlay(s, *order[i], 0); break; }
        }
    }

    KromeTarget under = hit(s, x, y);
    if (!(under == s.under)) {
        repaint(s, s.under);
        repaint(s, under);
        s.under = under;
    }
    follow_tip(s, under, down && !s.down);
    follow_hover(s, under);

    if (down && !s.down) {
        // A press outside the light overlays closes them, and goes no further.
        KromeScreen* light = light_overlays_screen(s);
        bool on_bar = false;
        if (light && !(under.screen == light->id && under.overlay)) {
            while (light->overlays.len && !light->overlays.last()->modal) krome_close_overlay(s, *light, 0);
            s.captured = KromeTarget{};
            s.under = hit(s, x, y);
        } else if ((on_bar = bar_press(s, under, x, y))) {
            s.captured = KromeTarget{}; // a scrollbar's: the keyboard stays where it was
        } else {
            s.captured = pressable(s, under);
        }
        // A press on an edit gives it the keyboard, on anything else focusable the focus; a
        // press anywhere else takes it away.
        KromeScreen* psc = nullptr;
        KromeTree* pt = s.captured.node != KROME_NONE ? krome_target_tree(s, s.captured, &psc) : nullptr;
        if (pt && psc && pt->nodes[s.captured.node].type == KromeType::Edit) {
            Input* in = platform_input(*s.e);
            bool shift = in && (in->raw.down(Key::LShift) || in->raw.down(Key::RShift));
            krome_edit_press(s, *psc, s.captured.overlay, s.captured.node, x, y, shift);
        } else if (pt && psc && krome_focusable(*pt, s.captured.node)) {
            krome_focus_take(s, *psc, s.captured.overlay, s.captured.node, false);
        } else if (!on_bar) {
            krome_edit_blur(s);
        }
        // A behaviour hears the press, in its own pixels.
        if (s.captured.node != KROME_NONE) {
            KromeCallArgs a;
            a.x = x;
            a.y = y;
            krome_behave(s, s.captured, KromeCall::Press, a);
            s.drag_at = vec2{x, y};
        }
        repaint(s, s.captured);
    } else if (!down && s.down && s.bar.active) {
        repaint(s, s.bar.box);
        s.bar = KromeModule::Bar{};
    } else if (down && s.bar.active) {
        bar_drag(s, x, y);
    } else if (!down && s.down) {
        KromeTarget was = s.captured;
        s.captured = KromeTarget{};
        repaint(s, was);
        KromeScreen* wsc = nullptr;
        KromeTree* wt = was.node != KROME_NONE ? krome_target_tree(s, was, &wsc) : nullptr;
        bool was_edit = wt && wt->nodes[was.node].type == KromeType::Edit;
        // A behaviour's node: the release is its, and it says what a click is.
        if (wt && !wt->nodes[was.node].behave.is_null()) {
            KromeCallArgs a;
            a.x = x;
            a.y = y;
            krome_behave(s, was, KromeCall::Release, a);
        } else if (was.node != KROME_NONE && !was_edit && pressable(s, under) == was) {
            KromeScreen* sc = nullptr;
            KromeTree* tree = krome_target_tree(s, was, &sc);
            if (tree && sc) krome_fire(s, *sc, *tree, was.node, 1.0); // may close what it was in: nothing after
        }
    } else if (down && s.captured.node != KROME_NONE) {
        // Pressed only while the pointer is over what took the press; an edit's drag selects.
        repaint(s, s.captured);
        KromeTree* ct = krome_target_tree(s, s.captured, nullptr);
        bool edit = ct && ct->nodes[s.captured.node].type == KromeType::Edit;
        if (edit && krome_is_focused(s, s.captured.screen, s.captured.overlay, s.captured.node)) krome_edit_drag(s, x, y);
        else if (ct && !ct->nodes[s.captured.node].behave.is_null() && (x != s.drag_at.x || y != s.drag_at.y)) {
            s.drag_at = vec2{x, y};
            KromeCallArgs a;
            a.x = x;
            a.y = y;
            krome_behave(s, s.captured, KromeCall::Drag, a);
        }
    }
    s.down = down;

    // The pointer's shape: the nearest node under it that says one, an edit's I-beam.
    {
        CursorShape shape = CursorShape::Arrow;
        KromeScreen* usc = nullptr;
        KromeTree* ut = under.node != KROME_NONE ? krome_target_tree(s, under, &usc) : nullptr;
        for (u32 c = ut ? under.node : KROME_NONE; c != KROME_NONE; c = ut->nodes[c].parent) {
            const KromeNode& cn = ut->nodes[c];
            if (cn.cursor != KromeCursor::Unset) { shape = (CursorShape)((u8)cn.cursor - 1); break; }
            if (cn.type == KromeType::Edit) { shape = CursorShape::IBeam; break; }
        }
        KromeTree* ct = s.down ? krome_target_tree(s, s.captured, nullptr) : nullptr;
        if (ct && s.captured.node < ct->nodes.len && ct->nodes[s.captured.node].type == KromeType::Edit && krome_is_focused(s, s.captured.screen, s.captured.overlay, s.captured.node)) shape = CursorShape::IBeam;
        if ((u8)shape + 1 != s.cursor) { s.cursor = (u8)shape + 1; platform_cursor(*s.e, shape); }
    }

    if (wheel != 0.0f) krome_wheel(s, 0.0f, wheel);
}

void krome_wheel(KromeModule& s, f32 dx, f32 dy) {
    const KromeTarget under = s.under;
    if (under.node == KROME_NONE || (dx == 0.0f && dy == 0.0f)) return;
    KromeScreen* sc = nullptr;
    KromeTree* tree = krome_target_tree(s, under, &sc);
    for (u32 n = under.node; tree && n != KROME_NONE; n = tree->nodes[n].parent) {
        if (!tree->nodes[n].behave.is_null() && dy != 0.0f) {
            KromeCallArgs a;
            a.dy = dy;
            if (krome_behave(s, KromeTarget{under.screen, under.overlay, n}, KromeCall::Wheel, a)) break;
            tree = krome_target_tree(s, under, &sc);
            if (!tree || n >= tree->nodes.len) break;
        }
        KromeNode& node = tree->nodes[n];
        bool sx = krome_scrolls_x(node), sy = krome_scrolls_y(node);
        bool moved = false;
        if (dy != 0.0f && sy) { node.scroll.y -= dy * k_wheel_step; moved = true; }
        else if (dy != 0.0f && sx) { node.scroll.x -= dy * k_wheel_step; moved = true; } // a row: the wheel goes across
        if (dx != 0.0f && sx) { node.scroll.x += dx * k_wheel_step; moved = true; }
        if (!moved) continue;
        tree->dirty = true;
        if (KromeOverlay* o = under.overlay ? krome_overlay(*sc, under.overlay) : nullptr) o->placed = false;
        break;
    }
}

void krome_context_press(KromeModule& s, f32 x, f32 y) {
    s.px = x;
    s.py = y;
    KromeTarget under = hit(s, x, y);
    follow_tip(s, under, true);
    KromeScreen* light = light_overlays_screen(s);
    if (light && !(under.screen == light->id && under.overlay)) {
        while (light->overlays.len && !light->overlays.last()->modal) krome_close_overlay(s, *light, 0);
        under = hit(s, x, y);
        s.under = under;
    }
    KromeScreen* sc = nullptr;
    KromeTree* t = under.node != KROME_NONE ? krome_target_tree(s, under, &sc) : nullptr;
    for (u32 c = t ? under.node : KROME_NONE; c != KROME_NONE; c = t->nodes[c].parent) {
        const KromeNode& n = t->nodes[c];
        if (n.flags & KROME_DISABLED) return;
        if (n.context.is_null()) continue;
        krome_fire_at(s, *sc, *t, c, n.context, x / s.scale, y / s.scale);
        return;
    }
}

void krome_free_view(KromeModule& s) {
    s.fonts.free();
    s.images.free();
    s.layers.free();
    s.list.free();
}

} // namespace kv
