// Korvid render: the UI primitive (docs/PRDs/Krome-Slice-1-PRD.md, "The UI primitive"). A
// path beside the sprites for drawing they do badly: thousands of small rectangles, each
// with a look of its own, in painter's order, clipped, in the window's pixels. A list is
// given for a layer and drawn every frame until it is replaced; an unchanged list costs a
// draw call and no upload.
//
//   UiList list;
//   list.init(mem_general());
//   u32 clip = list.clip(rect2{0, 0, 300, 200});
//   UiBox b;  b.rect = {10, 10, 120, 32};  b.fill = {0.2f, 0.3f, 0.5f, 1};  b.radius[0..3] = 6;  b.clip = clip;
//   list.box(b);
//   UiGlyph g;  g.rect = ...;  g.uv = ...;  g.texture = renderer_texture_slot(r, atlas);  g.clip = clip;
//   list.glyph(g);
//   renderer_ui_set(r, 100, list);  // drawn after the sprites of layer 100
//
// Coordinates are physical pixels, the origin at the window's top left, y down. Colors are
// sRGB with straight alpha, as a color picker gives them; the shader blends in linear light.
#pragma once
#include "korvid/core/array.h"
#include "korvid/math/color.h"
#include "korvid/math/rect2.h"

namespace kv {

struct Renderer;

// A rounded rectangle: a fill (solid, or a gradient from `fill` to `fill2` along `angle`),
// a border inside its edge, a soft shadow under it, an image over the fill when textured.
struct UiBox {
    rect2 rect;
    f32   radius[4] = {};       // top-left, top-right, bottom-right, bottom-left
    color fill{0, 0, 0, 0};
    color fill2{0, 0, 0, 0};
    bool  gradient = false;
    f32   angle = 90.0f;        // degrees; 90: fill at the top, fill2 at the bottom
    f32   border = 0.0f;        // width in pixels, inside the edge
    color border_color{0, 0, 0, 0};
    vec2  shadow_offset;
    f32   shadow_blur = 0.0f;   // pixels
    f32   shadow_spread = 0.0f;
    color shadow_color{0, 0, 0, 0};
    bool  textured = false;     // the fill is multiplied by the texture
    bool  nearest = false;      // sampled nearest, not linear (a pixel-art skin drawn larger)
    u32   texture = 0;          // a bindless slot (renderer_texture_slot)
    // The part of the texture drawn over the rect, 0 to 1. Every sample is held half a texel
    // inside it, so a region of a sheet never reads its neighbour.
    rect2 uv{0, 0, 1, 1};
    // Above 0: the uv rect is drawn this many pixels across and down, repeated from the rect's
    // top left, the last row and column cut (a tiled background, one box however many tiles).
    vec2  tile;
    u32   clip = 0;             // UiList::clip's index; 0 is no clip
};

// A glyph, or any small textured quad: the texel times the color.
struct UiGlyph {
    rect2 rect;
    rect2 uv;
    u32   texture = 0;          // a bindless slot
    color tint{1, 1, 1, 1};
    u32   clip = 0;
};

constexpr u32 KV_UI_MAX_CLIPS = 65535;

// A list in the form the GPU reads: items in draw order, each with its own look, and the
// clip rects they refer to. Built on the CPU, copied by renderer_ui_set.
struct UiList {
    Array<u32> words;   // the items
    Array<u32> offsets; // each item's first word
    Array<f32> clips;   // x, y, w, h each; clip 0 covers everything

    void init(Allocator* a);
    void free();
    void clear();        // empty, clip 0 kept
    u32  clip(rect2 r);  // a clip rect's index; past KV_UI_MAX_CLIPS, 0
    void box(const UiBox& b);
    void glyph(const UiGlyph& g);
    u32  count() const { return offsets.len; }
};

// Replaces what is drawn at `layer` (the sprites' layer space, clamped to -128..127): drawn
// after that layer's sprites and before the next layer's. The list is copied. On the main
// window's surface; the forms with a `surface` draw on another window's
// (renderer_surface_add), where there are no sprites, only the lists, in layer order.
void renderer_ui_set(Renderer* r, i32 layer, const UiList& list);
void renderer_ui_clear(Renderer* r, i32 layer);
void renderer_ui_set(Renderer* r, u32 surface, i32 layer, const UiList& list);
void renderer_ui_clear(Renderer* r, u32 surface, i32 layer);

// The most surfaces a renderer draws: the main window's, 0, and one for each other window,
// by the window's id (platform_hid's KV_WINDOWS).
constexpr u32 KV_RENDER_SURFACES = 8;
// Lists set over the run, and the bytes the GPU holds for them now.
struct RendererUiStats { u64 sets = 0, stages = 0; u32 layers = 0; u64 bytes = 0; };
RendererUiStats renderer_ui_stats(const Renderer* r);

} // namespace kv
