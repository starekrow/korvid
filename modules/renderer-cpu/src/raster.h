// renderer-cpu's pixels (docs/PRDs/CPU-Renderer-PRD.md, "The CPU backend"): quads prepared
// in the window's pixels, drawn a 64 x 64 tile at a time into 16-bit linear light, encoded to
// 8-bit sRGB when the tile is written out. This header and raster.cc include nothing of the
// engine's but its types: they are the code that will be built fast (the PRD's "Fast code
// where it pays"), and must share no inline function with the sim.
#pragma once
#include "korvid/core/types.h"
#include "korvid/renderer-cpu/shader.h"

namespace kv { namespace rb_cpu {

constexpr u32 kTile = 64;

// A texture as the rasterizer reads it: RGBA8, its RGB sRGB-encoded or not. Plain data (the
// draws holding it are hashed as bytes).
struct CpuTexture {
    const u8* pixels;
    u32       width, height;
    u32       version; // moves when its pixels are written: a tile's hash sees new pixels
    bool      srgb;
};

enum CpuBlend : u8 { kCpuOpaque = 0, kCpuAlpha = 1, kCpuAdditive = 2 };

// The post pass's world image: the layers below the post layer, R, G, B, pad, 16-bit linear.
struct CpuWorld {
    const u16* pixels;
    u32        width, height;
};

// One quad, ready to rasterize, everything in the window's pixels.
struct CpuQuad {
    i32 vx[4], vy[4];   // its corners in 1/256 pixel, wound so the inside is positive
    i32 x0, y0, x1, y1; // the pixels it may touch, [x0, x1) x [y0, y1), inside the target
    // Its local coordinates (0 to 1 across, +y up) at pixel (x, y)'s centre:
    // l0 + x * ldx + y * ldy.
    f32 l0x, l0y, ldx_x, ldx_y, ldy_x, ldy_y;
    f32 uvx, uvy, uvw, uvh; // the row's uv rect, the material's offset added
    u32 tint[4];            // the sprite's tint times the material's, 16.16 fixed
    CpuTexture tex;
    u8  filter;             // 0 nearest, 1 linear
    u8  blend;              // CpuBlend
    // A game shader's quad: the shader, and what its varyings need besides the above. World
    // at local (0, 0) and its change across local x and y; the tint as floats; the
    // material's texture slot and row; the frame's time and tick (in the hash: a shader may
    // animate).
    CpuShaderFn shader;
    f32 w0x, w0y, wlx_x, wlx_y, wly_x, wly_y;
    f32 tintf[4];
    u32 tex_slot, material;
    f32 time;
    u32 tick;
    u64 hash;               // of everything above: a tile's hash is its items'
};

// One item of the UI primitive (ui.slang), its words decoded: colors linear and
// premultiplied, sizes the shader's. Its pixels are in the UI's own space (the shader's `px`,
// top-left origin), reached from a window pixel's centre through l0 + x * ldx + y * ldy.
struct CpuUi {
    u8  kind;                // 0 a box, 1 a glyph
    u8  gradient, textured, shadow;
    u8  gamma;               // a glyph's coverage curve, one of 64 (by its ink's luminance)
    i32 x0, y0, x1, y1;      // the window pixels it may touch, [x0, x1) x [y0, y1)
    f32 l0x, l0y, ldx_x, ldx_y, ldy_x, ldy_y;
    f32 lo[2], hi[2];        // its quad cut by its clip, UI pixels: a centre in [lo, hi) draws
    f32 core[4];             // a box's pixels that are exactly `fill` (x0, y0, x1, y1), or empty
    f32 rx, ry, rw, rh;      // its rect
    f32 cx, cy, hx, hy;      // its centre and half size
    f32 radii[4];            // tl, tr, br, bl
    f32 border;
    f32 dir[2], reach;       // a gradient's direction and reach
    f32 so[2], shx, shy, sradii[4], inv_sigma; // a shadow's offset, half size, radii, 1 / (sigma sqrt 2)
    f32 uv0[2], uvd[2];      // a glyph's or an image's uv at its rect's top left, and across it
    f32 fill[4], fill2[4], edge[4], shade[4]; // a glyph's color is `fill`
    u32 flat[4];             // `fill` as 16-bit premultiplied
    CpuTexture tex;
    u64 hash;
};

// sRGB byte to 16-bit linear, and 16-bit linear to the sRGB byte (correctly rounded).
struct CpuLuts {
    u16 decode[256];
    u8  encode[65536];
    f32 shade[1025];      // 0.5 (1 - erf(x)) for x from -4 to 4 in 128ths: ui.slang's approximation
    u16 cover[64][257];   // a glyph's coverage c / 256 raised to 0.75 + 0.7 g / 63, 16-bit
};
const CpuLuts& cpu_luts();

// A linear channel (0 to 1, clamped) as 16-bit fixed point.
u16 cpu_linear16(f32 c);

// A tile's draws name a quad, or with this bit a UI item.
constexpr u32 kCpuDrawUi = 1u << 31;

// What a pass's tiles share: its draws, its clear, and for game shaders the frame's inputs
// (`shaders`: push block, layers, materials, textures, the world image) and the post shader
// that makes a tile's ground instead of the clear.
struct CpuFrame {
    const CpuQuad*       quads = nullptr;
    const CpuUi*         ui = nullptr;
    u16                  clear[3] = {};
    bool                 quantize8 = false;
    const CpuShaderCall* shaders = nullptr;
    CpuShaderFn          post = nullptr;
};

// One tile: the draws `items` names, in order, over the clear (or the post shader's
// picture), into `pixels` (kTile x kTile of R, G, B, pad, 16 bits each); (tx, ty) is its
// top-left pixel, tw x th its size.
void cpu_raster_tile(const CpuFrame& frame, const u32* items, u32 count, u16* pixels, u32 tx, u32 ty, u32 tw, u32 th);

// A texture read for a game shader: linear RGBA. `sampler` 0 nearest and repeat, 1 linear
// and repeat, 2 linear and clamped.
void cpu_sample_texture(const CpuTexture& t, u32 sampler, f32 u, f32 v, f32 out[4]);
void cpu_sample_world(const CpuWorld& w, f32 u, f32 v, f32 out[4]); // linear, clamped
// The tile encoded into the target at its place: four bytes a pixel, B, G, R, 255 (R, G, B,
// 255 when `rgba`).
void cpu_encode_tile(const u16* pixels, u32 tw, u32 th, u8* target, u32 pitch, bool rgba);

} } // namespace kv::rb_cpu
