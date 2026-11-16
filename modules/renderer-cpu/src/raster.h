// renderer-cpu's pixels (docs/PRDs/CPU-Renderer-PRD.md, "The CPU backend"): quads prepared
// in the window's pixels, drawn a 64 x 64 tile at a time into 16-bit linear light, encoded to
// 8-bit sRGB when the tile is written out. This header and raster.cc include nothing of the
// engine's but its types: they are the code that will be built fast (the PRD's "Fast code
// where it pays"), and must share no inline function with the sim.
#pragma once
#include "korvid/core/types.h"

namespace kv { namespace rb_cpu {

constexpr u32 kTile = 64;

// A texture as the rasterizer reads it: RGBA8, its RGB sRGB-encoded or not.
struct CpuTexture {
    const u8* pixels = nullptr;
    u32       width = 0, height = 0;
    bool      srgb = true;
};

enum CpuBlend : u8 { kCpuOpaque = 0, kCpuAlpha = 1, kCpuAdditive = 2 };

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
};

// sRGB byte to 16-bit linear, and 16-bit linear to the sRGB byte (correctly rounded).
struct CpuLuts {
    u16 decode[256];
    u8  encode[65536];
};
const CpuLuts& cpu_luts();

// A linear channel (0 to 1, clamped) as 16-bit fixed point.
u16 cpu_linear16(f32 c);

// One tile: the quads `items` names, in order, over the clear, into `pixels` (kTile x kTile
// of R, G, B, pad, 16 bits each); (tx, ty) is its top-left pixel, tw x th its size.
void cpu_raster_tile(const CpuQuad* quads, const u32* items, u32 count, u16* pixels,
                     u32 tx, u32 ty, u32 tw, u32 th, const u16 clear[3], bool quantize8);
// The tile encoded into the target at its place: four bytes a pixel, B, G, R, 255 (R, G, B,
// 255 when `rgba`).
void cpu_encode_tile(const u16* pixels, u32 tw, u32 th, u8* target, u32 pitch, bool rgba);

} } // namespace kv::rb_cpu
