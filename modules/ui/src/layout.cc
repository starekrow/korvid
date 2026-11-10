// UI layout: sizes up the tree, places down it, and turns each node into quads: frames as
// nine slices, images, meters, and rich text glyph by glyph with its effects.
#include "ui_internal.h"
#include "korvid/asset/font.h"
#include "korvid/asset/material.h"
#include "korvid/core/engine.h"
#include "korvid/core/hash.h"
#include "korvid/core/memory.h"
#include "korvid/core/utf8.h"
#include "korvid/render/renderer.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace kv {

namespace {

// ---- rich text -----------------------------------------------------------------------------------

enum : u8 { FX_WAVE = 1, FX_SHAKE = 2 };

struct RChar {
    u32   cp;
    color c;
    u8    fx;
    f32   pause;  // seconds the reveal waits after this character
};

// Parses a label's text into characters with their look. Model values are spliced in.
void parse_rich(UiState_& s, const UiDoc* d, const char* text, color base, Array<RChar>& out) {
    color stack_c[8];
    u32 depth = 0;
    color cur = base;
    u8 fx = 0;
    const char* p = text ? text : "";
    while (*p) {
        if (p[0] == '{' && p[1] == '{') { out.push(RChar{'{', cur, fx, 0.0f}); p += 2; continue; }
        if (p[0] == '{') {
            const char* end = strchr(p, '}');
            if (!end) break;
            char tag[64];
            usize n = (usize)(end - p - 1) < sizeof tag - 1 ? (usize)(end - p - 1) : sizeof tag - 1;
            memcpy(tag, p + 1, n);
            tag[n] = 0;
            p = end + 1;
            if (!strncmp(tag, "c=", 2)) {
                if (depth < KV_COUNTOF(stack_c)) stack_c[depth++] = cur;
                ui_color_parse(d, tag + 2, &cur);
            } else if (!strcmp(tag, "/c")) { if (depth) cur = stack_c[--depth]; }
            else if (!strcmp(tag, "wave")) fx |= FX_WAVE;
            else if (!strcmp(tag, "/wave")) fx &= (u8)~FX_WAVE;
            else if (!strcmp(tag, "shake")) fx |= FX_SHAKE;
            else if (!strcmp(tag, "/shake")) fx &= (u8)~FX_SHAKE;
            else if (!strncmp(tag, "pause=", 6)) { if (out.len) out.last().pause += (f32)atof(tag + 6); }
            else if (tag[0] == '=') {
                char v[128];
                ui_model_text(s, tag + 1, v, sizeof v);
                const char* q = v;
                while (u32 cp = utf8_next(q)) out.push(RChar{cp, cur, fx, 0.0f});
            }
            continue;
        }
        u32 cp = utf8_next(p);
        if (!cp) break;
        out.push(RChar{cp, cur, fx, 0.0f});
    }
}

struct Laid {
    const Glyph* g;
    f32 x;       // pen position, px from the line's start
    u32 line;
    u32 index;   // into the parsed characters
};

struct Block {
    Array<Laid> chars;
    Array<f32>  line_w;  // px
    f32 w = 0, h = 0, line_h = 0, ascent = 0; // px
};

// Lines of words: a word goes to the next line when it would cross `wrap` (px; <= 0: never).
void lay_text(const FontAsset& f, Span<const RChar> cs, f32 wrap, f32 line_h, f32 space, Block& b) {
    b.line_h = line_h;
    b.ascent = f.ascent + (line_h - (f.ascent - f.descent)) * 0.5f;
    u32 line = 0;
    f32 x = 0.0f;
    auto advance_of = [&](u32 i) {
        const Glyph* g = font_glyph_or_replacement(f, cs[i].cp);
        f32 a = g ? g->advance : 0.0f;
        if (cs[i].cp == ' ' && space > 0.0f) a = space; // the style's word gap
        if (i + 1 < cs.len) a += font_kern(f, cs[i].cp, cs[i + 1].cp);
        return a;
    };
    auto end_line = [&]() { b.line_w.push(x); ++line; x = 0.0f; };
    u32 i = 0;
    while (i < cs.len) {
        if (cs[i].cp == '\n') { end_line(); ++i; continue; }
        // The word from here: up to a space or a newline, with its trailing spaces.
        u32 j = i;
        f32 ww = 0.0f;
        while (j < cs.len && cs[j].cp != ' ' && cs[j].cp != '\n') ww += advance_of(j++);
        if (wrap > 0.0f && x > 0.0f && x + ww > wrap) end_line();
        for (u32 k = i; k < j; ++k) { b.chars.push(Laid{font_glyph_or_replacement(f, cs[k].cp), x, line, k}); x += advance_of(k); }
        while (j < cs.len && cs[j].cp == ' ') {
            if (wrap <= 0.0f || x + advance_of(j) <= wrap || x == 0.0f) { b.chars.push(Laid{font_glyph_or_replacement(f, ' '), x, line, j}); x += advance_of(j); }
            ++j;
        }
        i = j;
    }
    b.line_w.push(x);
    b.w = 0.0f;
    for (f32 lw : b.line_w) if (lw > b.w) b.w = lw;
    b.h = cs.len ? (f32)b.line_w.len * line_h : 0.0f; // nothing to show takes no room
}

// ---- helpers -------------------------------------------------------------------------------------

void point_frac(UiPoint p, f32* fx, f32* fy) {
    static const i8 k_x[] = {1, 0, 1, 2, 0, 2, 0, 1, 2};
    static const i8 k_y[] = {1, 0, 0, 0, 1, 1, 2, 2, 2};
    u32 i = (u32)p < 9 ? (u32)p : 0;
    *fx = (f32)k_x[i] * 0.5f;
    *fy = (f32)k_y[i] * 0.5f;
}

struct Ctx {
    Engine&   e;
    UiState_& s;
    UiScreen& sc;
    UiDoc*    doc;
    f32       k;     // px per unit
};

color node_color(const Ctx& c, Name n, color dflt) {
    if (n.is_null() || !c.doc) return dflt;
    color out = dflt;
    if (!ui_doc_color(*c.doc, n, &out)) ui_color_parse(c.doc, name_str(n), &out);
    return out;
}

const FontAsset* font_for(Ctx& c, f32 font_scale) {
    if (!c.doc) return nullptr;
    u32 px = (u32)(c.doc->font_px * font_scale * c.k + 0.5f);
    const FontAsset* f = ui_doc_font(c.e, c.s, *c.doc, px);
    return f && f->state == AssetState::Ready ? f : nullptr;
}

f32 line_px(Ctx& c, const UiNode& n, const FontAsset& f) {
    f32 l = c.doc && c.doc->line > 0.0f ? c.doc->line * n.font_scale * c.k : f.line_height;
    return l > 0.0f ? l : f.ascent - f.descent;
}

// A label's text laid out at its width (units; <= 0: unwrapped).
bool text_block(Ctx& c, const UiNode& n, f32 wrap_units, Array<RChar>& rich, Block& b) {
    const FontAsset* f = font_for(c, n.font_scale);
    if (!f) return false;
    color base = node_color(c, n.color, c.doc ? node_color(c, name_lookup("text"), color_white()) : color_white());
    if (n.type == UiType::Field) { // typed text is itself: no tags
        const char* p = n.text ? n.text : "";
        while (u32 cp = utf8_next(p)) rich.push(RChar{cp, base, 0, 0.0f});
    } else parse_rich(c.s, c.doc, n.text, base, rich);
    f32 space = c.doc && c.doc->space > 0.0f ? c.doc->space * n.font_scale * c.k : 0.0f;
    lay_text(*f, rich.span(), n.wrap && wrap_units > 0.0f ? wrap_units * c.k : 0.0f, line_px(c, n, *f), space, b);
    return true;
}

// ---- measuring ----------------------------------------------------------------------------------

void measure(Ctx& c, u32 i, f32 avail_w, f32 avail_h);

void natural(Ctx& c, UiNode& n, f32 inner_w, f32 inner_h, f32* cw, f32* ch) {
    *cw = *ch = 0.0f;
    switch (n.type) {
    case UiType::Label: case UiType::Field: {
        MemScope scope;
        Array<RChar> rich = array_in<RChar>(mem_scratch());
        Block b;
        b.chars = array_in<Laid>(mem_scratch());
        b.line_w = array_in<f32>(mem_scratch());
        if (text_block(c, n, n.w_mode == UiSize::Fit ? 0.0f : inner_w, rich, b)) { *cw = b.w / c.k; *ch = b.h / c.k; }
        if (n.type == UiType::Field && *ch == 0.0f && c.doc) *ch = (c.doc->line > 0.0f ? c.doc->line : c.doc->font_px) * n.font_scale;
        return;
    }
    case UiType::Image: {
        if (!n.texture.is_null()) { *cw = n.tex_rect.w; *ch = n.tex_rect.h; }
        else if (const UiImage* im = c.doc ? ui_doc_image(*c.doc, n.image) : nullptr) { *cw = im->rect.w * im->scale; *ch = im->rect.h * im->scale; }
        return;
    }
    case UiType::Meter: {
        f32 mx = ui_number(c.s, n.max, 1.0f);
        if (n.bar) { *cw = 60.0f; *ch = 6.0f; return; }
        const UiImage* im = c.doc ? ui_doc_image(*c.doc, n.image) : nullptr;
        u32 count = mx > 0.0f ? (u32)ceilf(mx) : 0;
        if (im && count) { *cw = im->rect.w * im->scale * (f32)count + n.gap * (f32)(count - 1); *ch = im->rect.h * im->scale; }
        return;
    }
    case UiType::Stack: case UiType::List: {
        bool row = n.type == UiType::Stack && n.dir == UiDir::Row;
        u32 count = 0;
        for (u32 k = n.first; k != KV_UI_NONE; k = c.sc.nodes[k].next) {
            measure(c, k, inner_w, inner_h);
            const UiNode& ch_ = c.sc.nodes[k];
            if (!ch_.visible) continue;
            if (row) { *cw += ch_.w; *ch = ch_.h > *ch ? ch_.h : *ch; }
            else { *ch += ch_.h; *cw = ch_.w > *cw ? ch_.w : *cw; }
            ++count;
        }
        if (count > 1) { if (row) *cw += n.gap * (f32)(count - 1); else *ch += n.gap * (f32)(count - 1); }
        if (n.type == UiType::List && c.doc) if (const UiImage* cur = ui_doc_image(*c.doc, name_lookup("cursor"))) *cw += cur->rect.w * cur->scale + 2.0f;
        return;
    }
    case UiType::Scroll: {
        for (u32 k = n.first; k != KV_UI_NONE; k = c.sc.nodes[k].next) {
            measure(c, k, inner_w, 1e6f);
            const UiNode& ch_ = c.sc.nodes[k];
            if (ch_.w > *cw) *cw = ch_.w;
            if (ch_.h > *ch) *ch = ch_.h;
        }
        return;
    }
    default: {
        for (u32 k = n.first; k != KV_UI_NONE; k = c.sc.nodes[k].next) {
            measure(c, k, inner_w, inner_h);
            const UiNode& ch_ = c.sc.nodes[k];
            if (!ch_.visible) continue;
            f32 w = ch_.w + fabsf(ch_.offset.x), h = ch_.h + fabsf(ch_.offset.y);
            if (w > *cw) *cw = w;
            if (h > *ch) *ch = h;
        }
        return;
    }
    }
}

void measure(Ctx& c, u32 i, f32 avail_w, f32 avail_h) {
    UiNode& n = c.sc.nodes[i];
    f32 padw = n.padding[0] + n.padding[2], padh = n.padding[1] + n.padding[3];
    n.w = n.w_mode == UiSize::Fixed ? n.size.x : n.w_mode == UiSize::Fill ? avail_w : 0.0f;
    n.h = n.h_mode == UiSize::Fixed ? n.size.y : n.h_mode == UiSize::Fill ? avail_h : 0.0f;
    f32 inner_w = (n.w_mode == UiSize::Fit ? avail_w : n.w) - padw;
    f32 inner_h = (n.h_mode == UiSize::Fit ? avail_h : n.h) - padh;
    f32 cw, ch;
    natural(c, n, inner_w, inner_h, &cw, &ch);
    if (n.w_mode == UiSize::Fit) n.w = cw + padw;
    if (n.h_mode == UiSize::Fit) n.h = ch + padh;
}

// ---- placing ------------------------------------------------------------------------------------

rect2 intersect(rect2 a, rect2 b) {
    f32 x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    f32 x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w, y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    return rect2{x0, y0, x1 > x0 ? x1 - x0 : 0.0f, y1 > y0 ? y1 - y0 : 0.0f};
}

void place(Ctx& c, u32 i, f32 x, f32 y, rect2 clip) {
    UiNode& n = c.sc.nodes[i];
    n.x = x; n.y = y; n.clip = clip;
    f32 ix = x + n.padding[0], iy = y + n.padding[1];
    f32 iw = n.w - n.padding[0] - n.padding[2], ih = n.h - n.padding[1] - n.padding[3];
    if (n.type == UiType::Stack || n.type == UiType::List) {
        bool row = n.type == UiType::Stack && n.dir == UiDir::Row;
        f32 lead = 0.0f;
        if (n.type == UiType::List && c.doc) if (const UiImage* cur = ui_doc_image(*c.doc, name_lookup("cursor"))) lead = cur->rect.w * cur->scale + 2.0f;
        f32 at = row ? ix : iy;
        for (u32 k = n.first; k != KV_UI_NONE; k = c.sc.nodes[k].next) {
            UiNode& ch_ = c.sc.nodes[k];
            if (!ch_.visible) continue;
            f32 cross = row ? ih - ch_.h : iw - lead - ch_.w;
            f32 off = n.align == UiAlign::Center ? cross * 0.5f : n.align == UiAlign::End ? cross : 0.0f;
            if (row) place(c, k, at + ch_.offset.x, iy + off + ch_.offset.y, clip);
            else place(c, k, ix + lead + off + ch_.offset.x, at + ch_.offset.y, clip);
            at += (row ? ch_.w : ch_.h) + n.gap;
        }
        return;
    }
    rect2 inner_clip = clip;
    f32 scroll = 0.0f;
    if (n.type == UiType::Scroll) {
        inner_clip = intersect(clip, rect2{ix, iy, iw, ih});
        f32 content = 0.0f;
        for (u32 k = n.first; k != KV_UI_NONE; k = c.sc.nodes[k].next) content = c.sc.nodes[k].h > content ? c.sc.nodes[k].h : content;
        f32 most = content - ih > 0.0f ? content - ih : 0.0f;
        if (n.scroll > most) n.scroll = most;
        if (n.scroll < 0.0f) n.scroll = 0.0f;
        scroll = n.scroll;
    }
    for (u32 k = n.first; k != KV_UI_NONE; k = c.sc.nodes[k].next) {
        UiNode& ch_ = c.sc.nodes[k];
        f32 ax, ay, px, py;
        point_frac(ch_.anchor, &ax, &ay);
        point_frac(ch_.pivot, &px, &py);
        // A scroll's content hangs from its top, whatever its vertical anchor.
        f32 cy = n.type == UiType::Scroll ? iy + ch_.offset.y - scroll : iy + ay * ih - py * ch_.h + ch_.offset.y;
        place(c, k, ix + ax * iw - px * ch_.w + ch_.offset.x, cy, inner_clip);
    }
}

// ---- quads --------------------------------------------------------------------------------------

// A quad in units, cropped to the clip, out in pixels.
void quad(Ctx& c, Handle material, rect2 uv, f32 x, f32 y, f32 w, f32 h, color tint, rect2 clip, bool snap = true) {
    if (w <= 0.0f || h <= 0.0f || material.is_null()) return;
    rect2 r = intersect(rect2{x, y, w, h}, clip);
    if (r.w <= 0.0f || r.h <= 0.0f) return;
    // The uv follows the crop.
    f32 u0 = (r.x - x) / w, v0 = (r.y - y) / h, u1 = (r.x + r.w - x) / w, v1 = (r.y + r.h - y) / h;
    UiQuad q;
    q.material = material;
    q.uv = rect2{uv.x + uv.w * u0, uv.y + uv.h * v0, uv.w * (u1 - u0), uv.h * (v1 - v0)};
    q.x = r.x * c.k; q.y = r.y * c.k; q.w = r.w * c.k; q.h = r.h * c.k;
    if (snap) { q.x = floorf(q.x + 0.5f); q.y = floorf(q.y + 0.5f); q.w = floorf(q.w + 0.5f); q.h = floorf(q.h + 0.5f); }
    q.tint = tint;
    q.layer = c.sc.open.layer;
    c.s.quads.push(q);
}

// A quad already in pixels (glyphs), cropped to the clip (units).
void quad_px(Ctx& c, Handle material, rect2 uv, f32 x, f32 y, f32 w, f32 h, color tint, rect2 clip) {
    quad(c, material, uv, x / c.k, y / c.k, w / c.k, h / c.k, tint, clip, false);
    if (!c.s.quads.len) return;
    UiQuad& q = c.s.quads.last();
    q.x = floorf(q.x + 0.5f); q.y = floorf(q.y + 0.5f);
}

void nine(Ctx& c, const UiFrame& f, const UiNode& n, color tint) {
    const TextureAsset* t = asset_texture(c.e, f.texture);
    const MaterialAsset* m = asset_material(c.e, f.material);
    if (!t || !m || !t->width || !t->height) return;
    f32 tw = (f32)t->width, th = (f32)t->height;
    f32 l = f.insets[0], tp = f.insets[1], r = f.insets[2], b = f.insets[3];
    f32 xs[4] = {n.x, n.x + l, n.x + n.w - r, n.x + n.w};
    f32 ys[4] = {n.y, n.y + tp, n.y + n.h - b, n.y + n.h};
    f32 us[4] = {0, l / tw, (tw - r) / tw, 1};
    f32 vs[4] = {0, tp / th, (th - b) / th, 1};
    for (u32 yi = 0; yi < 3; ++yi)
        for (u32 xi = 0; xi < 3; ++xi)
            quad(c, m->material, rect2{us[xi], vs[yi], us[xi + 1] - us[xi], vs[yi + 1] - vs[yi]}, xs[xi], ys[yi], xs[xi + 1] - xs[xi], ys[yi + 1] - ys[yi], tint, n.clip);
}

void image(Ctx& c, const UiImage& im, u32 frame, f32 x, f32 y, f32 w, f32 h, color tint, rect2 clip) {
    const TextureAsset* t = asset_texture(c.e, im.texture);
    const MaterialAsset* m = asset_material(c.e, im.material);
    if (!t || !m || !t->width) return;
    f32 tw = (f32)t->width, th = (f32)t->height;
    rect2 r = im.rect;
    r.x += r.w * (f32)(frame % (im.frames ? im.frames : 1));
    quad(c, m->material, rect2{r.x / tw, r.y / th, r.w / tw, r.h / th}, x, y, w, h, tint, clip);
}

void text(Ctx& c, const UiNode& n, bool caret) {
    MemScope scope;
    Array<RChar> rich = array_in<RChar>(mem_scratch());
    Block b;
    b.chars = array_in<Laid>(mem_scratch());
    b.line_w = array_in<f32>(mem_scratch());
    f32 iw = n.w - n.padding[0] - n.padding[2];
    if (!text_block(c, n, iw, rich, b)) return;
    const FontAsset* f = font_for(c, n.font_scale);
    color shadow = c.doc ? c.doc->shadow : color{0, 0, 0, 0};
    if (!n.shadow.is_null()) shadow = node_color(c, n.shadow, shadow);
    f32 x0 = (n.x + n.padding[0]) * c.k, y0 = (n.y + n.padding[1]) * c.k;
    u32 shown = n.revealed || n.reveal_rate <= 0.0f ? 0xFFFFFFFFu : n.reveal_shown;
    f32 t = c.sc.time;
    f32 last_x = 0.0f;
    u32 last_line = 0;
    // The shadow at every unit from the offset in, then the text over it.
    // In the font's pixels: a label drawn three times up drops three times as far.
    u32 drop = (u32)((f32)(c.doc ? c.doc->shadow_offset : 1) * (n.font_scale > 1.0f ? n.font_scale : 1.0f) + 0.5f);
    u32 passes = shadow.a > 0.0f ? drop + 1 : 1;
    for (u32 p = 0; p < passes; ++p)
        for (const Laid& l : b.chars) {
            if (l.index >= shown) break;
            last_x = l.x + (l.g ? l.g->advance : 0.0f); last_line = l.line;
            if (!l.g || l.g->size.x <= 0.0f) continue;
            const RChar& rc = rich[l.index];
            f32 lw = b.line_w[l.line];
            f32 ax = n.align == UiAlign::Center ? (iw * c.k - lw) * 0.5f : n.align == UiAlign::End ? iw * c.k - lw : 0.0f;
            f32 dx = 0.0f, dy = 0.0f;
            if (rc.fx & FX_WAVE) dy = sinf(t * 6.0f - (f32)l.index * 0.6f) * 1.5f * c.k;
            if (rc.fx & FX_SHAKE) {
                u64 hsh = hash_mix(((u64)l.index << 32) ^ (u64)(t * 20.0f));
                dx = ((f32)(hsh % 3) - 1.0f) * 0.75f * c.k;
                dy = ((f32)((hsh >> 8) % 3) - 1.0f) * 0.75f * c.k;
            }
            bool text_pass = p + 1 == passes;
            f32 off = text_pass ? 0.0f : (f32)(drop - p) * c.k; // the shadow, down and right
            f32 gx = x0 + ax + l.x + l.g->offset.x - l.g->size.x * 0.5f + dx + off;
            f32 gy = y0 + (f32)l.line * b.line_h + b.ascent - l.g->offset.y - l.g->size.y * 0.5f + dy + off;
            color tint = text_pass ? rc.c : color{shadow.r, shadow.g, shadow.b, shadow.a * rc.c.a};
            quad_px(c, f->material, l.g->uv, gx, gy, l.g->size.x, l.g->size.y, tint, n.clip);
        }
    if (caret && (u32)(c.sc.time * 2.0f) % 2 == 0) {
        f32 cx = x0 + last_x;
        f32 cy = y0 + (f32)last_line * b.line_h;
        if (!b.chars.len) cx = x0;
        if (!c.s.white_material.is_null()) quad(c, c.s.white_material, rect2_unit(), cx / c.k, cy / c.k, 1.0f, b.line_h / c.k, node_color(c, n.color, color_white()), n.clip);
    }
}

void emit_node(Ctx& c, u32 i) {
    UiNode& n = c.sc.nodes[i];
    if (!n.visible) return;
    color white = color_white();
    switch (n.type) {
    case UiType::Panel: {
        const UiFrame* f = c.doc ? ui_doc_frame(*c.doc, n.frame) : nullptr;
        if (f) nine(c, *f, n, node_color(c, n.color, white));
        else if (!n.fill.is_null() && !c.s.white_material.is_null()) quad(c, c.s.white_material, rect2_unit(), n.x, n.y, n.w, n.h, node_color(c, n.fill, white), n.clip);
        break;
    }
    case UiType::Image: {
        if (!n.texture.is_null()) {
            const TextureAsset* t = asset_texture(c.e, n.texture);
            const MaterialAsset* m = asset_material(c.e, n.material);
            if (t && m && t->width) quad(c, m->material, rect2{n.tex_rect.x / (f32)t->width, n.tex_rect.y / (f32)t->height, n.tex_rect.w / (f32)t->width, n.tex_rect.h / (f32)t->height}, n.x, n.y, n.w, n.h, node_color(c, n.color, white), n.clip);
        } else if (const UiImage* im = c.doc ? ui_doc_image(*c.doc, n.image) : nullptr) {
            image(c, *im, n.frame_index, n.x, n.y, n.w, n.h, node_color(c, n.color, white), n.clip);
        }
        break;
    }
    case UiType::Label: text(c, n, false); break;
    case UiType::Field: text(c, n, true); break;
    case UiType::Meter: {
        f32 v = ui_number(c.s, n.value, 0.0f), mx = ui_number(c.s, n.max, 1.0f);
        if (n.bar) {
            if (!c.s.white_material.is_null()) {
                quad(c, c.s.white_material, rect2_unit(), n.x, n.y, n.w, n.h, color{0.02f, 0.02f, 0.03f, 0.8f}, n.clip);
                f32 frac = mx > 0.0f ? v / mx : 0.0f;
                frac = frac < 0.0f ? 0.0f : frac > 1.0f ? 1.0f : frac;
                quad(c, c.s.white_material, rect2_unit(), n.x + 1, n.y + 1, (n.w - 2) * frac, n.h - 2, node_color(c, n.fill, color_rgba(0.9f, 0.6f, 0.1f)), n.clip);
            }
        } else if (const UiImage* im = c.doc ? ui_doc_image(*c.doc, n.image) : nullptr) {
            u32 count = mx > 0.0f ? (u32)ceilf(mx) : 0;
            for (u32 k = 0; k < count; ++k) {
                f32 part = v - (f32)k;
                part = part < 0.0f ? 0.0f : part > 1.0f ? 1.0f : part;
                u32 frame = (u32)(part * (f32)(im->frames - 1) + 0.5f);
                image(c, *im, frame, n.x + (im->rect.w * im->scale + n.gap) * (f32)k, n.y, im->rect.w * im->scale, im->rect.h * im->scale, white, n.clip);
            }
        }
        break;
    }
    case UiType::List: {
        // The focused item: the cursor beside it and the focus color on it.
        u32 idx = 0;
        for (u32 k = n.first; k != KV_UI_NONE; k = c.sc.nodes[k].next, ++idx) {
            UiNode& item = c.sc.nodes[k];
            bool focused = idx == n.focus;
            Name want = focused ? name_lookup("focus") : item.color;
            if (focused && c.doc) if (const UiImage* cur = ui_doc_image(*c.doc, name_lookup("cursor"))) {
                f32 bob = floorf(sinf(c.sc.time * 6.0f) * 1.5f + 0.5f);
                f32 cw_ = cur->rect.w * cur->scale, ch_ = cur->rect.h * cur->scale;
                image(c, *cur, 0, item.x - cw_ - 2.0f + bob, item.y + (item.h - ch_) * 0.5f, cw_, ch_, white, n.clip);
            }
            Name saved = item.color;
            item.color = focused && !want.is_null() ? want : saved;
            emit_node(c, k);
            item.color = saved;
        }
        return; // the items are drawn
    }
    default: break;
    }
    for (u32 k = n.first; k != KV_UI_NONE; k = c.sc.nodes[k].next) emit_node(c, k);
}

} // namespace

void ui_model_text(UiState_& s, const char* key, char* out, usize cap) {
    const UiModelValue* v = s.model.find(name_lookup(key).hash);
    if (!v) { out[0] = 0; return; }
    if (v->type == 0) snprintf(out, cap, "%lld", (long long)v->i);
    else if (v->type == 1) snprintf(out, cap, "%g", v->f);
    else utf8_copy(out, cap, name_str(v->s));
}

f32 ui_number(UiState_& s, const char* value, f32 dflt) {
    if (!value || !value[0]) return dflt;
    if (value[0] == '=') {
        const UiModelValue* v = s.model.find(name_lookup(value + 1).hash);
        if (!v) return dflt;
        return v->type == 0 ? (f32)v->i : v->type == 1 ? (f32)v->f : dflt;
    }
    return (f32)atof(value);
}

u32 ui_text_chars(UiState_& s, const char* t, f32 rate, f32* seconds) {
    MemScope scope;
    Array<RChar> rich = array_in<RChar>(mem_scratch());
    parse_rich(s, nullptr, t, color_white(), rich);
    f32 total = 0.0f;
    for (const RChar& r : rich) total += (rate > 0.0f ? 1.0f / rate : 0.0f) + r.pause;
    if (seconds) *seconds = total;
    return rich.len;
}

u32 ui_text_shown(UiState_& s, const char* t, f32 rate, f32 at) {
    MemScope scope;
    Array<RChar> rich = array_in<RChar>(mem_scratch());
    parse_rich(s, nullptr, t, color_white(), rich);
    f32 when = 0.0f;
    for (u32 i = 0; i < rich.len; ++i) {
        when += rate > 0.0f ? 1.0f / rate : 0.0f;
        if (when > at) return i;
        when += rich[i].pause;
    }
    return rich.len;
}

void ui_layout(Engine& e, UiState_& s, UiScreen& sc) {
    if (!sc.nodes.len) return;
    Ctx c{e, s, sc, s.docs.get(sc.doc), s.scale};
    f32 W = s.win_w / s.scale, H = s.win_h / s.scale;
    measure(c, 0, W, H);
    UiNode& root = sc.nodes[0];
    f32 ax, ay, px, py;
    point_frac(root.anchor, &ax, &ay);
    point_frac(root.pivot, &px, &py);
    place(c, 0, ax * W - px * root.w + root.offset.x, ay * H - py * root.h + root.offset.y, rect2{0, 0, W, H});
}

void ui_emit(Engine& e, UiState_& s, UiScreen& sc) {
    if (!sc.nodes.len) return;
    Ctx c{e, s, sc, s.docs.get(sc.doc), s.scale};
    emit_node(c, 0);
}

} // namespace kv
