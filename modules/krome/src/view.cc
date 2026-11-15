// Krome's view: text measured in the fonts it is drawn in, screens laid out against the
// window, painted into the renderer's UI lists (korvid/render/ui.h) only when something
// changed, and the pointer routed top first through overlays and screens
// (docs/PRDs/Krome-Slice-1-PRD.md, "Paint and the display list", "Input and overlays").
#include "internal.h"
#include "korvid/asset/font.h"
#include "korvid/asset/texture.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
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
    if (node == p.pressed) return KromeState::Pressed;
    for (u32 i = 0; i < p.hovered_n; ++i) if (p.hovered[i] == node) return KromeState::Hover;
    return KromeState::Base;
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
    f32 width = font_measure(*fa, str), height = fa->line_height;
    f32 x = in.x, top = in.y;
    if (n.align_x == KromeAlign::Center) x += (in.w - width) * 0.5f;
    else if (n.align_x == KromeAlign::End) x += in.w - width;
    if (n.align_y == KromeAlign::Center) top += (in.h - height) * 0.5f;
    else if (n.align_y == KromeAlign::End) top += in.h - height;
    f32 baseline = floor(top + fa->ascent + 0.5f);
    x = floor(x + 0.5f);
    color c = faded(l.text_color, l.opacity);
    u32 prev = 0;
    for (const char* q = str; *q;) {
        u32 cp = utf8_next(q);
        if (prev) x += font_kern(*fa, prev, cp);
        prev = cp;
        const Glyph* g = font_glyph_or_replacement(*fa, cp);
        if (!g) continue;
        if (g->size.x > 0 && g->size.y > 0) {
            UiGlyph gl;
            f32 cx = x + g->offset.x, cy = baseline - g->offset.y;
            gl.rect = rect2{floor(cx - g->size.x * 0.5f + 0.5f), floor(cy - g->size.y * 0.5f + 0.5f), g->size.x, g->size.y};
            gl.uv = g->uv;
            gl.texture = slot;
            gl.tint = c;
            gl.clip = clip;
            p.list.glyph(gl);
        }
        x += g->advance;
    }
}

void paint_node(Paint& p, u32 node, const KromeLook* parent, rect2 clip_rect, u32 clip) {
    const KromeNode& n = p.tree.nodes[node];
    if (n.flags & KROME_HIDDEN) return;
    KromeLook l = p.tree.resolve_look(node, state_of(p, node), parent);
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
    if (n.first == KROME_NONE) return;
    if (n.flags & (KROME_CLIP | KROME_SCROLL)) {
        clip_rect = intersect(clip_rect, r);
        clip = p.list.clip(clip_rect);
    }
    for (u32 ch = n.first; ch != KROME_NONE; ch = p.tree.nodes[ch].next) paint_node(p, ch, &l, clip_rect, clip);
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
        if (found == KROME_NONE && (!node.event.is_null() || node.type == KromeType::Hit)) found = n;
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
        if (order[i]->overlays.len) return order[i]->overlays.last()->modal ? nullptr : order[i];
    return nullptr;
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
    if (id) {
        at = KROME_NONE;
        for (u32 i = 0; i < sc.overlays.len; ++i) if (sc.overlays[i]->id == id) at = i;
        if (at == KROME_NONE) return;
    }
    KromeOverlay* o = sc.overlays[at];
    for (u32 i = at + 1; i < sc.overlays.len; ++i) sc.overlays[i - 1] = sc.overlays[i];
    --sc.overlays.len;
    // What the pointer was on in it is gone.
    if (s.under.screen == sc.id && s.under.overlay == o->id) s.under = KromeTarget{};
    if (s.captured.screen == sc.id && s.captured.overlay == o->id) s.captured = KromeTarget{};
    o->tree.free();
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
    KromeMeasure m{&s, measure_text, measure_image};
    for (KromeScreen* sc : s.screens) {
        if (!sc->ready) continue;
        if (landed) sc->tree.dirty = true;
        if (sc->tree.dirty && sc->tree.root != KROME_NONE) {
            krome_layout(sc->tree, sc->tree.root, rect2{0, 0, W, H}, m, true);
            ++s.layouts;
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
    }
}

void krome_pointer(KromeModule& s, f32 x, f32 y, bool down, f32 wheel, bool escape) {
    s.px = x;
    s.py = y;
    // Escape (a press) closes the topmost overlay, light or modal.
    if (escape) {
        KromeScreen* order[k_max_screens];
        u32 n = 0;
        screens_top_first(s, order, &n);
        for (u32 i = 0; i < n; ++i)
            if (order[i]->overlays.len) { krome_close_overlay(s, *order[i], 0); break; }
    }

    KromeTarget under = hit(s, x, y);
    if (!(under == s.under)) {
        repaint(s, s.under);
        repaint(s, under);
        s.under = under;
    }

    if (down && !s.down) {
        // A press outside the light overlays closes them, and goes no further.
        KromeScreen* light = light_overlays_screen(s);
        if (light && !(under.screen == light->id && under.overlay)) {
            while (light->overlays.len && !light->overlays.last()->modal) krome_close_overlay(s, *light, 0);
            s.captured = KromeTarget{};
            s.under = hit(s, x, y);
        } else {
            s.captured = pressable(s, under);
        }
        repaint(s, s.captured);
    } else if (!down && s.down) {
        KromeTarget was = s.captured;
        s.captured = KromeTarget{};
        repaint(s, was);
        if (was.node != KROME_NONE && pressable(s, under) == was) {
            KromeScreen* sc = nullptr;
            KromeTree* tree = krome_target_tree(s, was, &sc);
            if (tree && sc) krome_fire(s, *sc, *tree, was.node, 1.0); // may close what it was in: nothing after
        }
    } else if (down && s.captured.node != KROME_NONE) {
        // Pressed only while the pointer is over what took the press.
        repaint(s, s.captured);
    }
    s.down = down;

    if (wheel != 0.0f && under.node != KROME_NONE) {
        KromeScreen* sc = nullptr;
        KromeTree* tree = krome_target_tree(s, under, &sc);
        for (u32 n = under.node; tree && n != KROME_NONE; n = tree->nodes[n].parent) {
            KromeNode& node = tree->nodes[n];
            if (!(node.flags & KROME_SCROLL)) continue;
            if (node.dir == KromeDir::Row) node.scroll.x -= wheel * k_wheel_step;
            else node.scroll.y -= wheel * k_wheel_step;
            tree->dirty = true;
            if (KromeOverlay* o = under.overlay ? krome_overlay(*sc, under.overlay) : nullptr) o->placed = false;
            break;
        }
    }
}

void krome_free_view(KromeModule& s) {
    s.fonts.free();
    s.images.free();
    s.layers.free();
    s.list.free();
}

} // namespace kv
