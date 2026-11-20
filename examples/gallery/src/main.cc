// The renderer's features in fixed scenes, each drawn and written as a screenshot
// (gallery_<scene>.ppm), then compared with another backend's by the picture tool.
// It drives the render interface directly: a window, a renderer, a stream
// of sprite deltas built here, and RenderViews with chosen interpolation points. No sim,
// no frame loop, no libm in any position, so two backends' pictures differ only where
// their GPUs do.
//
// The window is opened at whatever size in window units makes the framebuffer exactly
// 960x540 pixels on this display (a Retina Mac: 480x270; a 1x Linux desktop: 960x540), so
// every backend draws the same pixels. Textures are made here; the game shader and the post
// shader are this module's own (shaders/). Neither reads the clock.
//
//   gallery [--out DIR] [--scene NAME]   (DIR: where the PPMs go, "." by default)
#include "korvid/gallery/module.h"
#include "korvid/platform/io.h"
#include "korvid/core/format.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/platform/module.h"
#include "korvid/platform_hid/host_window.h"
#include "korvid/platform_hid/module.h"
#include "korvid/render/renderer.h"
#include "korvid/render/ui.h"
#include "korvid/patherizer/patherizer.h"
#include "korvid/core/memory.h"
#include <string.h>

namespace kv {

namespace {

constexpr u32 kWidth  = 960;
constexpr u32 kHeight = 540;
constexpr f32 kPi     = 3.14159265358979f;

struct Gallery {
    Engine*     e = nullptr;
    HostWindow* window = nullptr;
    Renderer*   r = nullptr;
    const char* out = ".";
    Handle checker, disc, glyph;         // textures
    Handle gear[3], badge;               // shapes, rasterized by patherizer (the ui scene)
    Handle opaque_checker, linear_checker, alpha_disc, add_disc, alpha_glyph, tinted_disc, rings; // materials
    Array<SpriteDelta> deltas;
    Array<Handle>      live;             // the entities in the render world now
    u64 next_entity = 1;
    u32 written = 0, failed = 0;
};

// ---- textures, made here --------------------------------------------------------------------

Handle make_texture(Gallery& g, const char* name, u32 w, u32 h, const u8* rgba, TextureFormat fmt = TextureFormat::RGBA8_SRGB) {
    TextureDesc d;
    d.name = name;
    d.format = fmt;
    Handle t = renderer_texture_create(g.r, d);
    renderer_texture_upload(g.r, t, w, h, rgba, (usize)w * h * 4);
    return t;
}

// Shapes as Krome draws them (docs/PRDs/Krome-Slice-5-PRD.md): rasterized by patherizer at the
// size they are drawn, into sRGB with straight alpha, and drawn as textured UI boxes. A
// one-colour shape (Material's settings icon, every paint currentColor) is rasterized white
// and the box tints it; a coloured one (gradients, a dashed stroke, a clip, group opacity) is
// drawn as it is.
Handle shape_texture(Gallery& g, const char* name, const PathShape& s, u32 w, u32 h) {
    Array<u8> px;
    px.alloc = mem_general();
    u8* p = px.push_n((usize)w * h * 4);
    memset(p, 0, (usize)w * h * 4);
    patherizer_draw(s, PathTarget{p, w, h, w * 4, false}, PathOptions{});
    Handle t = make_texture(g, name, w, h, p);
    px.free();
    return t;
}

void make_shapes(Gallery& g) {
    PathShape s;
    s.init(mem_general());
    // Material Symbols' settings, 20 px grid (modules/krome/assets/icons/settings.svg).
    s.view[0] = 0; s.view[1] = -960; s.view[2] = 960; s.view[3] = 960;
    {
        PathBuilder b(s);
        b.svg_path("m403-96-22-114q-23-9-44.5-21T296-259l-110 37-77-133 87-76q-2-12-3-24t-1-25q0-13 1-25t3-24l-87-76 77-133 110 37q19-16 40.5-28t44.5-21l22-114h154l22 114q23 9 44.5 21t40.5 28l110-37 77 133-87 76q2 12 3 24t1 25q0 13-1 25t-3 24l87 76-77 133-110-37q-19 16-40.5 28T579-210L557-96H403Zm59-72h36l19-99q38-7 71-26t57-48l96 32 18-30-76-67q6-17 9.5-35.5T696-480q0-20-3.5-38.5T683-554l76-67-18-30-96 32q-24-29-57-48t-71-26l-19-99h-36l-19 99q-38 7-71 26t-57 48l-96-32-18 30 76 67q-6 17-9.5 35.5T264-480q0 20 3.5 38.5T277-406l-76 67 18 30 96-32q24 29 57 48t71 26l19 99Zm18-168q60 0 102-42t42-102q0-60-42-102t-102-42q-60 0-102 42t-42 102q0 60 42 102t102 42Zm0-144Z");
        b.fill(s.add_current());
    }
    const u32 sizes[3] = {20, 40, 80};
    const char* names[3] = {"gallery.gear20", "gallery.gear40", "gallery.gear80"};
    for (u32 i = 0; i < 3; ++i) g.gear[i] = shape_texture(g, names[i], s, sizes[i], sizes[i]);

    // A badge: a radial gradient disc (focal, off centre) with a dashed round-capped ring, a
    // star stroked with mitred joins over a linear gradient, clipped to the disc, at 80% opacity.
    s.clear();
    s.view[0] = 0; s.view[1] = 0; s.view[2] = 100; s.view[3] = 100;
    {
        PathBuilder b(s);
        const PathStop warm[3] = {{0.0f, {255, 236, 160, 255}}, {0.6f, {245, 120, 60, 255}}, {1.0f, {150, 30, 80, 255}}};
        PathPaint radial;
        radial.kind = PathPaint::Radial;
        radial.g[0] = 50; radial.g[1] = 50; radial.g[2] = 46; radial.g[3] = 36; radial.g[4] = 34;
        auto circle = [&](f32 cx, f32 cy, f32 r) { // four cubics
            const f32 k = 0.5522847f * r;
            b.move_to(cx + r, cy);
            b.cubic_to(cx + r, cy + k, cx + k, cy + r, cx, cy + r);
            b.cubic_to(cx - k, cy + r, cx - r, cy + k, cx - r, cy);
            b.cubic_to(cx - r, cy - k, cx - k, cy - r, cx, cy - r);
            b.cubic_to(cx + k, cy - r, cx + r, cy - k, cx + r, cy);
            b.close();
        };
        circle(50, 50, 46);
        b.fill(s.add_gradient(radial, warm, 3));
        u32 clip = b.begin_group();
        circle(50, 50, 40);
        b.fill(s.add_solid(255, 255, 255, 255));
        b.end_group(clip);
        s.clips.push(clip);
        u32 group = b.begin_group(0.8f, 1 + clip);
        const PathStop cool[2] = {{0.0f, {120, 220, 255, 255}}, {1.0f, {40, 70, 200, 255}}};
        PathPaint linear;
        linear.kind = PathPaint::Linear;
        linear.g[0] = 20; linear.g[1] = 20; linear.g[2] = 80; linear.g[3] = 80;
        // A five-pointed star, its points precomputed (no libm in the picture).
        const f32 star[10][2] = {{50, 8}, {60.6f, 35.4f}, {90, 37}, {67, 55.6f}, {74.7f, 84}, {50, 68}, {25.3f, 84}, {33, 55.6f}, {10, 37}, {39.4f, 35.4f}};
        b.move_to(star[0][0], star[0][1]);
        for (u32 i = 1; i < 10; ++i) b.line_to(star[i][0], star[i][1]);
        b.close();
        PathStroke st;
        st.width = 4; st.join = LineJoin::Miter; st.miter_limit = 4;
        b.fill_and_stroke(s.add_gradient(linear, cool, 2), FillRule::NonZero, s.add_solid(255, 255, 255, 255), st);
        b.end_group(group);
        PathStroke ring;
        ring.width = 3; ring.cap = LineCap::Round;
        const f32 dash[2] = {6, 6};
        circle(50, 50, 46);
        b.stroke(s.add_solid(255, 250, 240, 255), ring, 1.0f, dash, 2);
    }
    g.badge = shape_texture(g, "gallery.badge", s, 120, 120);
    s.free();
}

void make_textures(Gallery& g) {
    // An 8x8 checker in two colors, for filtering and repeats.
    u8 checker[8 * 8 * 4];
    for (u32 y = 0; y < 8; ++y)
        for (u32 x = 0; x < 8; ++x) {
            u8* p = checker + (y * 8 + x) * 4;
            bool a = ((x + y) & 1) == 0;
            p[0] = a ? 230 : 40; p[1] = a ? 200 : 90; p[2] = a ? 60 : 200; p[3] = 255;
        }
    g.checker = make_texture(g, "gallery.checker", 8, 8, checker);
    // A 64x64 disc: white, alpha falling off toward the rim, for blending and linear filtering.
    static u8 disc[64 * 64 * 4];
    for (u32 y = 0; y < 64; ++y)
        for (u32 x = 0; x < 64; ++x) {
            u8* p = disc + (y * 64 + x) * 4;
            i32 dx = (i32)x * 2 - 63, dy = (i32)y * 2 - 63;
            i32 d2 = dx * dx + dy * dy; // integers: no libm in the picture
            i32 a = d2 >= 64 * 64 ? 0 : 255 - (255 * d2) / (64 * 64);
            p[0] = p[1] = p[2] = 255; p[3] = (u8)a;
        }
    g.disc = make_texture(g, "gallery.disc", 64, 64, disc);
    // A 16x16 "F": asymmetric every way, so a flip or a turn shows.
    static const char* glyph_rows[16] = {
        "................", ".##############.", ".##############.", ".###............", ".###............", ".###............",
        ".###############", ".###############", ".###............", ".###............", ".###............", ".###............",
        ".###............", ".###............", ".###............", "................"};
    u8 glyph[16 * 16 * 4];
    for (u32 y = 0; y < 16; ++y)
        for (u32 x = 0; x < 16; ++x) {
            u8* p = glyph + (y * 16 + x) * 4;
            bool on = glyph_rows[y][x] == '#';
            p[0] = on ? 255 : 0; p[1] = on ? 255 : 0; p[2] = on ? 255 : 0; p[3] = on ? 255 : 0;
        }
    g.glyph = make_texture(g, "gallery.glyph", 16, 16, glyph);
    make_shapes(g);
}

Handle make_material(Gallery& g, const char* name, Handle tex, BlendMode blend, TextureFilter filter, color tint,
                     const char* shader = nullptr, color p1 = color_white()) {
    MaterialDesc d;
    d.name    = name;
    d.texture = tex;
    d.blend   = blend;
    d.filter  = filter;
    d.tint    = tint;
    d.shader  = shader ? name_str(name_intern(shader)) : nullptr;
    d.params[4] = p1.r; d.params[5] = p1.g; d.params[6] = p1.b; d.params[7] = p1.a;
    return renderer_material_create(g.r, d);
}

void make_materials(Gallery& g) {
    g.opaque_checker = make_material(g, "gallery.opaque_checker", g.checker, BlendMode::Opaque, TextureFilter::Nearest, color_white());
    g.linear_checker = make_material(g, "gallery.linear_checker", g.checker, BlendMode::Opaque, TextureFilter::Linear, color_white());
    g.alpha_disc     = make_material(g, "gallery.alpha_disc", g.disc, BlendMode::Alpha, TextureFilter::Linear, color_white());
    g.add_disc       = make_material(g, "gallery.add_disc", g.disc, BlendMode::Additive, TextureFilter::Linear, color_white());
    g.alpha_glyph    = make_material(g, "gallery.alpha_glyph", g.glyph, BlendMode::Alpha, TextureFilter::Nearest, color_white());
    g.tinted_disc    = make_material(g, "gallery.tinted_disc", g.disc, BlendMode::Alpha, TextureFilter::Linear, color_rgba(1.0f, 0.4f, 0.4f, 1.0f));
    g.rings          = make_material(g, "gallery.rings", g.disc, BlendMode::Alpha, TextureFilter::Linear, color_white(), "gallery_rings",
                                     color_rgba(0.2f, 0.5f, 1.0f, 1.0f));
}

// ---- the render world -------------------------------------------------------------------------

struct Sprite {
    Placement2   placement;
    Handle       material;
    rect2        uv{0, 0, 1, 1};
    vec2         size{64, 64};
    color        tint = color_white();
    i32          layer = 0;
    SpriteSpace  space = SpriteSpace::World;
    ScreenAnchor anchor = ScreenAnchor::Center;
    SpriteSort   sort = SpriteSort::Row;
    f32          order = 0.0f;
};

Sprite at(f32 x, f32 y, Handle material, f32 size = 64.0f) {
    Sprite s;
    s.placement.position = point2{x, y};
    s.material = material;
    s.size = vec2{size, size};
    return s;
}

RenderSpriteState state_of(const Sprite& s, bool snap) {
    RenderSpriteState st{};
    st.placement = s.placement;
    st.material  = s.material;
    st.uv        = s.uv;
    st.size      = s.size;
    st.tint      = s.tint;
    st.layer     = s.layer;
    st.space     = s.space;
    st.anchor    = s.anchor;
    st.sort      = s.sort;
    st.order     = s.order;
    st.snap      = snap;
    return st;
}

// A new sprite, set in the current tick: where it is is where it was.
Handle add(Gallery& g, const Sprite& s) {
    Handle h = Handle::make(0, 1, (u32)g.next_entity++); // an entity the renderer keys by; no ktable behind it
    g.deltas.push(SpriteDelta{SpriteDeltaKind::Set, h, state_of(s, true)});
    g.live.push(h);
    return h;
}

// The same sprite moved in the current tick: the renderer interpolates from where it was.
void move(Gallery& g, Handle h, const Sprite& s) { g.deltas.push(SpriteDelta{SpriteDeltaKind::Set, h, state_of(s, false)}); }

void tick(Gallery& g) { g.deltas.push(SpriteDelta{SpriteDeltaKind::Tick, Handle{}, RenderSpriteState{}}); }

void clear(Gallery& g) {
    for (Handle h : g.live) g.deltas.push(SpriteDelta{SpriteDeltaKind::Remove, h, RenderSpriteState{}});
    g.live.clear();
}

void flush(Gallery& g) {
    renderer_apply(g.r, g.deltas.span());
    g.deltas.clear();
}

RenderView view_of(f32 alpha) {
    RenderView v;
    v.alpha = alpha;
    v.view  = rect2{-(f32)kWidth * 0.5f, -(f32)kHeight * 0.5f, (f32)kWidth, (f32)kHeight}; // one world unit a pixel, centered
    v.clear_color = color_rgba(0.09f, 0.09f, 0.11f, 1.0f);
    return v;
}

// A few frames so uploads land and nothing is half-made, then one kept as a screenshot.
void shoot(Gallery& g, const char* scene, const RenderView& view) {
    for (u32 i = 0; i < 4; ++i) { platform_pump(*g.e); renderer_render(g.r, view); }
    char path[512];
    format(path, sizeof path, "%s/gallery_%s.ppm", g.out, scene);
    io_remove(path);
    renderer_screenshot(g.r, path);
    for (u32 i = 0; i < 8; ++i) {
        platform_pump(*g.e);
        renderer_render(g.r, view);
        IoBytes shot; // the frame that carries it writes it before render returns
        if (io_read_now(path, &shot)) {
            bool any = shot.size > 0;
            shot.free();
            if (any) { ++g.written; KV_INFO("gallery", "%s", path); return; }
        }
    }
    ++g.failed;
    KV_ERROR("gallery", "no screenshot for '%s'", scene);
}

// ---- the scenes -------------------------------------------------------------------------------

// Every blend over an opaque background, layered: alpha discs, additive discs over them, an
// opaque glyph over both, tints.
void scene_blends(Gallery& g) {
    Sprite bg = at(0, 0, g.opaque_checker);
    // The uv repeats the checker: 64 units a repeat, 8 a texel, so every texel edge falls on a
    // pixel edge. (At 900x480, 7.5 a texel, edges fell on pixel centres, where each backend's
    // rounding picked a side and the references could not agree.)
    bg.size = vec2{960, 512}; bg.uv = rect2{0, 0, 15, 8}; bg.layer = -10;
    add(g, bg);
    for (i32 i = 0; i < 5; ++i) {
        Sprite a = at(-320.0f + 120.0f * (f32)i, 60, g.alpha_disc, 140);
        a.tint = color_rgba(0.2f * (f32)i, 0.6f, 1.0f - 0.2f * (f32)i, 0.4f + 0.15f * (f32)i);
        add(g, a);
        Sprite b = at(-320.0f + 120.0f * (f32)i, -40, g.add_disc, 140);
        b.layer = 1;
        b.tint = color_rgba(1.0f, 0.3f + 0.15f * (f32)i, 0.2f, 1.0f);
        add(g, b);
    }
    Sprite op = at(0, -160, g.alpha_glyph, 96);
    op.layer = 2;
    add(g, op);
    Sprite tinted = at(300, -160, g.tinted_disc, 120);
    tinted.layer = 2;
    add(g, tinted);
}

// Rotations, scales, mirrors and uv rects on the asymmetric glyph.
void scene_transform(Gallery& g) {
    for (i32 i = 0; i < 8; ++i) {
        Sprite s = at(-385.0f + 110.0f * (f32)i, 140, g.alpha_glyph, 80);
        s.placement.rotation = (f32)i * kPi * 0.25f;
        add(g, s);
    }
    const vec2 scales[] = {{1, 1}, {-1, 1}, {1, -1}, {-1, -1}, {2, 1}, {1, 0.5f}};
    for (i32 i = 0; i < 6; ++i) {
        Sprite s = at(-340.0f + 136.0f * (f32)i, 0, g.alpha_glyph, 80);
        s.placement.scale = scales[i];
        add(g, s);
    }
    // The glyph's top half, its left half, and the whole repeated 3x2.
    Sprite top = at(-200, -150, g.alpha_glyph, 96);  top.uv = rect2{0, 0, 1, 0.5f};
    Sprite left = at(0, -150, g.alpha_glyph, 96);    left.uv = rect2{0, 0, 0.5f, 1};
    Sprite rep = at(200, -150, g.alpha_glyph, 96);   rep.uv = rect2{0, 0, 3, 2}; rep.size = vec2{144, 96};
    add(g, top); add(g, left); add(g, rep);
}

// The checker magnified with each filter, and the disc magnified with linear.
void scene_filter(Gallery& g) {
    Sprite n = at(-240, 0, g.opaque_checker, 320);
    Sprite l = at(160, 0, g.linear_checker, 320);
    Sprite d = at(400, 150, g.alpha_disc, 150);
    d.layer = 1;
    add(g, n); add(g, l); add(g, d);
}

// Sorting within a layer: by key (row order reversed), by feet (lower on screen in front),
// and row order where keys tie.
void scene_sort(Gallery& g) {
    for (i32 i = 0; i < 6; ++i) { // keyed: the first added drawn last
        Sprite s = at(-380.0f + 40.0f * (f32)i, 120, g.alpha_disc, 120);
        s.tint = color_rgba(1.0f - 0.15f * (f32)i, 0.3f, 0.15f * (f32)i, 1.0f);
        s.sort = SpriteSort::Key;
        s.order = (f32)(10 - i);
        add(g, s);
    }
    for (i32 i = 0; i < 6; ++i) { // feet: a staircase going up and right, each in front of the next
        Sprite s = at(-60.0f + 40.0f * (f32)i, -60.0f + 30.0f * (f32)i, g.alpha_disc, 120);
        s.tint = color_rgba(0.3f, 1.0f - 0.15f * (f32)i, 0.2f + 0.15f * (f32)i, 1.0f);
        s.sort = SpriteSort::Feet;
        s.order = -60.0f;
        s.layer = 1;
        add(g, s);
    }
    for (i32 i = 0; i < 6; ++i) { // equal keys: row order
        Sprite s = at(220.0f + 30.0f * (f32)i, -150, g.alpha_disc, 120);
        s.tint = color_rgba(0.2f + 0.15f * (f32)i, 0.2f, 1.0f, 1.0f);
        s.sort = SpriteSort::Key;
        s.order = 5.0f;
        s.layer = 2;
        add(g, s);
    }
}

// Interpolation: glyphs moved, turned and scaled between the last two ticks, drawn at an
// interpolation point; a sprite not set in the last tick stays where it is.
Handle g_moving[4];
void scene_interpolate_setup(Gallery& g) {
    for (i32 i = 0; i < 4; ++i) {
        Sprite s = at(-300.0f, 120.0f - 80.0f * (f32)i, g.alpha_glyph, 64);
        g_moving[i] = add(g, s);
    }
    Sprite still = at(0, -200, g.tinted_disc, 80);
    add(g, still);
    flush(g);
    tick(g);
    for (i32 i = 0; i < 4; ++i) {
        Sprite s = at(300.0f, 120.0f - 80.0f * (f32)i, g.alpha_glyph, 64);
        if (i >= 1) s.placement.rotation = kPi;        // half a turn
        if (i >= 2) s.placement.scale = vec2{2, 2};      // doubled
        if (i == 3) s.placement.position.y += 40.0f;     // and up
        move(g, g_moving[i], s);
    }
}

// The camera zoomed in and turned; screen sprites at the window's corners and center,
// untouched by it.
RenderView scene_camera(Gallery& g) {
    Sprite bg = at(0, 0, g.opaque_checker);
    bg.size = vec2{600, 400}; bg.uv = rect2{0, 0, 6, 4};
    add(g, bg);
    for (i32 i = 0; i < 3; ++i) {
        Sprite s = at(-150.0f + 150.0f * (f32)i, 0, g.alpha_glyph, 80);
        s.layer = 1;
        add(g, s);
    }
    const ScreenAnchor anchors[] = {ScreenAnchor::TopLeft, ScreenAnchor::TopRight, ScreenAnchor::BottomLeft, ScreenAnchor::BottomRight, ScreenAnchor::Center};
    for (i32 i = 0; i < 5; ++i) {
        Sprite s = at(0, 0, g.alpha_glyph, 48);
        s.space = SpriteSpace::Screen;
        s.anchor = anchors[i];
        s.layer = 10;
        s.tint = color_rgba(1.0f, 0.8f, 0.2f, 1.0f);
        // Inset from the corner it is measured from (the anchor's direction, inward).
        i32 ax = 0, ay = 0;
        screen_anchor_dir(anchors[i], &ax, &ay);
        s.placement.position = point2{-40.0f * (f32)ax, -40.0f * (f32)ay};
        add(g, s);
    }
    RenderView v = view_of(0.0f);
    v.view = rect2{-240, -135, 480, 270}; // twice the zoom
    v.rotation = kPi / 6.0f;              // a sixth of a turn
    return v;
}

// A game's sprite shader, and a post shader over the layers below 5; a glyph above it.
void scene_shaders(Gallery& g) {
    Sprite bg = at(0, 0, g.opaque_checker);
    bg.size = vec2{960, 512}; bg.uv = rect2{0, 0, 15, 8}; bg.layer = -10; // 8 units a texel, as in blends
    add(g, bg);
    for (i32 i = 0; i < 3; ++i) add(g, at(-300.0f + 300.0f * (f32)i, 40, g.rings, 200));
    Sprite over = at(0, -180, g.alpha_glyph, 96);
    over.layer = 5; // at the post layer: drawn over the post pass, untouched by it
    add(g, over);
    renderer_post_layer(g.r, 5, "gallery_post");
    const f32 params[4] = {0.2f, 1.0f, 0.4f, 0.6f};
    renderer_post_params(g.r, params, 1);
}

// The UI primitive (korvid/render/ui.h): boxes with every corner, border, gradient and
// shadow it has, an image in a box, glyphs, and clips nested in clips, over a sprite
// background, with a sprite on the layer above the list drawn over it.
UiBox ui_box(f32 x, f32 y, f32 w, f32 h, color fill, f32 radius = 0.0f) {
    UiBox b;
    b.rect = rect2{x, y, w, h};
    b.fill = fill;
    for (f32& r : b.radius) r = radius;
    return b;
}

void scene_ui(Gallery& g, UiList& l) {
    Sprite bg = at(0, 0, g.opaque_checker);
    bg.size = vec2{960, 512}; bg.uv = rect2{0, 0, 15, 8}; bg.layer = -10;
    add(g, bg);
    Sprite over = at(380, -200, g.alpha_glyph, 64);
    over.layer = 1; // above the list's layer
    add(g, over);

    const color panel = color_rgba(0.13f, 0.15f, 0.19f, 1.0f), accent = color_rgba(0.35f, 0.63f, 1.0f, 1.0f);
    const color white = color_white();
    // A panel with a soft shadow, a border and rounded corners.
    UiBox card = ui_box(40, 40, 400, 220, panel, 12);
    card.border = 1; card.border_color = color_rgba(1, 1, 1, 0.18f);
    card.shadow_color = color_rgba(0, 0, 0, 0.6f); card.shadow_blur = 18; card.shadow_offset = vec2{0, 8};
    l.box(card);
    // Corners: none, small, pill, one each, and a circle.
    f32 x = 64;
    const f32 radii[] = {0, 4, 14};
    for (f32 r : radii) { l.box(ui_box(x, 64, 80, 28, accent, r)); x += 96; }
    UiBox odd = ui_box(x, 64, 64, 28, accent);
    odd.radius[0] = 14; odd.radius[1] = 0; odd.radius[2] = 14; odd.radius[3] = 0;
    l.box(odd);
    l.box(ui_box(64, 108, 40, 40, color_rgba(1.0f, 0.55f, 0.2f, 1.0f), 20));
    // Borders of one and two pixels, with and without a fill.
    UiBox b1 = ui_box(120, 108, 120, 40, color_rgba(0, 0, 0, 0), 6);
    b1.border = 1; b1.border_color = white;
    l.box(b1);
    UiBox b2 = ui_box(256, 108, 160, 40, color_rgba(0.2f, 0.22f, 0.28f, 1.0f), 6);
    b2.border = 2; b2.border_color = accent;
    l.box(b2);
    // Gradients: down, across, and on the diagonal.
    const f32 angles[] = {90, 0, 45};
    x = 64;
    for (f32 a : angles) {
        UiBox gr = ui_box(x, 168, 112, 72, color_rgba(0.95f, 0.3f, 0.45f, 1.0f), 8);
        gr.gradient = true; gr.fill2 = color_rgba(0.25f, 0.2f, 0.9f, 1.0f); gr.angle = a;
        l.box(gr);
        x += 124;
    }
    // An image in a rounded box, and the glyph as text would be drawn, in two colors.
    UiBox img = ui_box(480, 40, 160, 120, white, 16);
    img.textured = true; img.texture = renderer_texture_slot(g.r, g.checker); img.uv = rect2{0, 0, 2, 1.5f};
    l.box(img);
    for (u32 i = 0; i < 6; ++i) {
        UiGlyph gl;
        gl.rect = rect2{480.0f + 24.0f * (f32)i, 180, 16, 16};
        gl.uv = rect2{0, 0, 1, 1};
        gl.texture = renderer_texture_slot(g.r, g.glyph);
        gl.tint = i & 1 ? accent : white;
        l.glyph(gl);
    }
    // Clips: a box clipped by a rect that cuts its corner, and inside a clipped rect, glyphs
    // and a box that run past both edges of a second, smaller one.
    u32 outer = l.clip(rect2{680, 40, 240, 200});
    UiBox frame = ui_box(670, 30, 140, 100, accent, 20);
    frame.clip = outer;
    l.box(frame);
    u32 inner = l.clip(rect2{700, 150, 160, 60});
    UiBox under = ui_box(690, 140, 200, 40, color_rgba(0.9f, 0.9f, 0.3f, 1.0f), 10);
    under.clip = inner;
    l.box(under);
    for (u32 i = 0; i < 10; ++i) {
        UiGlyph gl;
        gl.rect = rect2{684.0f + 20.0f * (f32)i, 186, 24, 24};
        gl.uv = rect2{0, 0, 1, 1};
        gl.texture = renderer_texture_slot(g.r, g.glyph);
        gl.tint = color_rgba(0.1f, 0.1f, 0.1f, 1.0f);
        gl.clip = inner;
        l.glyph(gl);
    }
    // A translucent box over the card's edge, and a shadow spread wider than its box.
    UiBox glass = ui_box(360, 220, 200, 120, color_rgba(1, 1, 1, 0.25f), 18);
    glass.border = 1; glass.border_color = color_rgba(1, 1, 1, 0.5f);
    l.box(glass);
    UiBox lifted = ui_box(120, 340, 240, 120, panel, 10);
    lifted.shadow_color = color_rgba(0.35f, 0.63f, 1.0f, 0.8f); lifted.shadow_blur = 24; lifted.shadow_spread = 6;
    l.box(lifted);
    // Shapes (patherizer): a one-colour icon at 20, 40 and 80 px, white and tinted, and a
    // coloured badge, each drawn at the size it was rasterized.
    f32 gx = 600;
    for (u32 i = 0; i < 3; ++i) {
        f32 sz = (f32)(20u << i);
        for (u32 k = 0; k < 2; ++k) {
            UiBox ic = ui_box(gx, 280.0f + (f32)k * 100.0f, sz, sz, k ? accent : white);
            ic.textured = true; ic.texture = renderer_texture_slot(g.r, g.gear[i]); ic.uv = rect2{0, 0, 1, 1};
            l.box(ic);
        }
        gx += sz + 24.0f;
    }
    UiBox badge = ui_box(800, 280, 120, 120, white);
    badge.textured = true; badge.texture = renderer_texture_slot(g.r, g.badge); badge.uv = rect2{0, 0, 1, 1};
    l.box(badge);
    renderer_ui_set(g.r, 0, l);
}

// ---- the run ---------------------------------------------------------------------------------

// A window whose framebuffer is kWidth x kHeight pixels on this display.
HostWindow* open_window(Engine& e) {
    WindowDesc d;
    d.title   = "korvid gallery";
    d.width   = kWidth;
    d.height  = kHeight;
    d.surface = (WindowSurface)renderer_window_surface(); // what the bound backend draws into
    HostWindow* w = platform_open_window(e, d);
    if (!w || (w->width() == kWidth && w->height() == kHeight)) return w;
    f32 scale = w->pixel_scale();
    w->close();
    d.width  = (u32)((f32)kWidth / scale + 0.5f);
    d.height = (u32)((f32)kHeight / scale + 0.5f);
    w = platform_open_window(e, d);
    if (w && (w->width() != kWidth || w->height() != kHeight))
        KV_WARN("gallery", "the framebuffer is %ux%u, not %ux%u: these pictures compare only with ones of their size", w->width(), w->height(), kWidth, kHeight);
    return w;
}

int run(int argc, char** argv) {
    const char* only = nullptr;
    Gallery g;
    RendererBackend backend = RendererBackend::Auto;
    bool headless = false, cpu_shaders = true;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) g.out = argv[++i];
        else if (strcmp(argv[i], "--scene") == 0 && i + 1 < argc) only = argv[++i];
        else if (strcmp(argv[i], "--renderer") == 0 && i + 1 < argc && renderer_backend_parse(argv[i + 1], &backend)) ++i;
        else if (strcmp(argv[i], "--headless") == 0) { headless = true; backend = RendererBackend::Cpu; }
        else if (strcmp(argv[i], "--cpu-shaders") == 0 && i + 1 < argc && (!strcmp(argv[i + 1], "all") || !strcmp(argv[i + 1], "none"))) cpu_shaders = !strcmp(argv[++i], "all");
        else { KV_ERROR("gallery", "unknown option '%s' (--out DIR, --scene NAME, --renderer auto|gpu|cpu, --cpu-shaders all|none, --headless)", argv[i]); return 2; }
    }
    Engine e;
    if (!engine_init(e, kv_module_list())) return 1;
    renderer_choose(backend); // after the backends' modules start (their probes read them), before the window
    g.e = &e;
    g.deltas.alloc = g.live.alloc = mem_general();
    int rc = 0;
    // Headless: the CPU renderer draws the frame's kWidth x kHeight pixels with no window.
    g.window = headless ? nullptr : open_window(e);
    if (g.window || headless) {
        RendererDesc rd;
        rd.backend = backend;
        rd.native_window = g.window ? g.window->native_handle() : nullptr;
        rd.width  = g.window ? g.window->width() : kWidth;
        rd.height = g.window ? g.window->height() : kHeight;
        rd.vsync  = false;
        rd.cpu_shaders = cpu_shaders;
        g.r = renderer_create(e, rd);
    }
    if (!g.r) rc = 1;
    if (rc == 0) {
        make_textures(g);
        make_materials(g);
        auto want = [&](const char* name) { return !only || strcmp(only, name) == 0; };
        auto simple = [&](const char* name, void (*build)(Gallery&)) {
            if (!want(name)) return;
            clear(g); tick(g); build(g); flush(g);
            shoot(g, name, view_of(0.0f));
        };
        simple("blends", scene_blends);
        simple("transform", scene_transform);
        simple("filter", scene_filter);
        simple("sort", scene_sort);
        if (want("interpolate")) {
            clear(g); tick(g); scene_interpolate_setup(g); flush(g);
            shoot(g, "interpolate_0", view_of(0.0f));
            shoot(g, "interpolate_50", view_of(0.5f));
            shoot(g, "interpolate_90", view_of(0.9f));
        }
        if (want("camera")) {
            clear(g); tick(g); RenderView v = scene_camera(g); flush(g);
            shoot(g, "camera", v);
        }
        if (want("shaders")) {
            clear(g); tick(g); scene_shaders(g); flush(g);
            shoot(g, "shaders", view_of(0.0f));
            renderer_post_layer(g.r, 0, nullptr);
        }
        // The UI primitive, where the backend draws it (Metal does not yet).
        if (want("ui") && renderer_caps(g.r).ui) {
            UiList l;
            l.init(mem_general());
            clear(g); tick(g); scene_ui(g, l); flush(g);
            shoot(g, "ui", view_of(0.0f));
            renderer_ui_clear(g.r, 0);
            l.free();
        }
        KV_INFO("gallery", "%u pictures written to %s, %u failed", g.written, g.out, g.failed);
        if (g.failed) rc = 1;
    }
    g.deltas.free();
    g.live.free();
    engine_shutdown(e);
    names_shutdown();
    mem_shutdown();
    return rc;
}

} // namespace

KV_MODULE_DEFINE(gallery_module) = {
    .name = "gallery",
};

} // namespace kv

// Through platform_app_main, so it runs on iOS as well (inside the application there).
int main(int argc, char** argv) {
    return kv::platform_app_main(argc, argv, [](int c, char** v, void*) { return kv::run(c, v); }, nullptr);
}
