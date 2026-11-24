// Korvid image: JPEG decoding (KR25), written for the engine as png.h is: a plain function
// over bytes, memory from an allocator, no engine state, nothing shared between calls, so
// any task may call it.
//
// - Baseline, extended sequential and progressive Huffman JPEG, 8-bit: greyscale, YCbCr at
//   any sampling (4:4:4, 4:2:2, 4:2:0, 4:4:0, 4:1:1, ...), RGB (Adobe's transform 0), CMYK
//   and Adobe's YCCK (as RGB: recognisable, not colour-managed); restart markers.
// - Decoded at a fraction of its size by doing less: `scale` 2, 4 or 8 runs a 4x4, 2x2 or
//   1x1 inverse DCT on each block instead of the 8x8, so an eighth costs the entropy
//   decode and little else.
// - Chroma upsampled by interpolation (libjpeg's "fancy" upsampling: a triangle filter for
//   2:1 in either direction), as libjpeg-turbo does by default; replicated where it does
//   (other ratios, and at an eighth). The inverse DCT is libjpeg's integer one, and the
//   colour conversion its fixed-point one, so the pixels are libjpeg-turbo's to within
//   rounding.
// - The EXIF orientation applied (JpegOptions::orient) or only reported.
// - Hostile input: the size is checked against `max_pixels` before anything is allocated;
//   every read is bounded; a truncated or spoiled file gives the rows it had (`partial`,
//   and for a sequential file only the rows decoded) or fails with a reason.
// Not taken: arithmetic coding, 12-bit samples, lossless and hierarchical JPEG.
#pragma once
#include "korvid/core/memory.h"

namespace kv {

// What the header says, without decoding a pixel.
struct JpegInfo {
    u32  width = 0, height = 0;   // as stored, before orientation
    u8   components = 0;          // 1, 3 or 4
    bool progressive = false;
    u8   orientation = 1;         // EXIF's 1 to 8; 1 without one
};

struct JpegOptions {
    u32  scale      = 1;           // 1, 2, 4 or 8: the output is the image over this, rounded up
    bool orient     = true;        // apply the EXIF orientation; false: report it in the image
    u64  max_pixels = 64ull << 20; // a larger image is refused before anything is allocated
};

struct JpegImage {
    u32  width = 0, height = 0;   // of `rgba`, after scale and orientation
    u8*  rgba = nullptr;          // width * height * 4 bytes, top row first, from the allocator given; the caller frees it
    u8   orientation = 1;         // still to apply: 1 when it was applied (or there was none)
    bool partial = false;         // the data ended or was spoiled before the image did
};

// The header's facts. False, with *error, when it is not a JPEG this decodes.
bool jpeg_info(const void* data, usize size, JpegInfo* out, const char** error);

// Decodes [data, data + size) into `out`, its pixels from `pixels` (null: the general
// allocator), its temporaries from the same and freed before return. False, nothing
// allocated, and *error set to a static string ("not a JPEG", "arithmetic coding is not
// supported", "too large", ...) when there is nothing to give.
bool jpeg_decode(const void* data, usize size, const JpegOptions& opt, Allocator* pixels, JpegImage& out, const char** error);

} // namespace kv
