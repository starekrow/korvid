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
    Handle h = Handle::make(0, 1, (u32)g.next_entity++); // an entity the renderer keys by; no table behind it
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
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) g.out = argv[++i];
        else if (strcmp(argv[i], "--scene") == 0 && i + 1 < argc) only = argv[++i];
        else { KV_ERROR("gallery", "unknown option '%s' (--out DIR, --scene NAME)", argv[i]); return 2; }
    }
    Engine e;
    if (!engine_init(e, kv_module_list())) return 1;
    g.e = &e;
    g.deltas.alloc = g.live.alloc = mem_general();
    int rc = 0;
    g.window = open_window(e);
    if (g.window) {
        RendererDesc rd;
        rd.native_window = g.window->native_handle();
        rd.width  = g.window->width();
        rd.height = g.window->height();
        rd.vsync  = false;
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
