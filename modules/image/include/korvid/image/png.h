// Korvid image: PNG on zlib (the inflate module's), both ways.
//
// Decoding: 8-bit samples, every color type (gray, gray+alpha, RGB, RGBA, palette with or
// without tRNS), and 1, 2 and 4-bit gray and palette; no 16-bit samples, no interlace;
// always produces tightly packed RGBA8, top row first. CRCs are not verified. The PRD
// names libpng; this stands in until it is vendored, and the call does not change.
//
// Encoding: 8-bit RGB or RGBA, one IDAT, every row filtered by whichever of the five
// filters leaves the smallest sum of absolute bytes (the usual heuristic), deflated at
// zlib's best. What it writes, png_decode reads back exactly.
#pragma once
#include "korvid/core/array.h"
#include "korvid/core/memory.h"

namespace kv {

struct PngImage {
    u32 width  = 0;
    u32 height = 0;
    u8* rgba   = nullptr; // width * height * 4 bytes, from the allocator given; the caller frees it
};

// Decodes [data, data + size) into `out`, whose pixels come from `pixels` (null: the
// general allocator). Temporaries use the calling thread's scratch and are released
// before return. On failure returns false, allocates nothing, and sets *error to a
// static string naming the reason ("not a PNG", "corrupt image data", ...).
bool png_decode(const void* data, usize size, Allocator* pixels, PngImage& out, const char** error);

// Encodes `width` x `height` pixels of `channels` bytes each (3: RGB, 4: RGBA), tightly
// packed, top row first, appending the file to `out` (whose allocator it uses). The
// compressor's state and the filtered rows come from the calling thread's scratch and are
// released before return. False, with `out` as it was and *error set, for a size or
// channel count it cannot write.
bool png_encode(const u8* pixels, u32 width, u32 height, u32 channels, Array<u8>& out, const char** error);

} // namespace kv
