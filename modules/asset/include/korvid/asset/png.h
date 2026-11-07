// Korvid asset: a small PNG decoder on zlib. 8-bit samples, every color type (gray,
// gray+alpha, RGB, RGBA, palette with or without tRNS), no interlace; always produces
// tightly packed RGBA8, top row first. CRCs are not verified. The PRD names libpng; this
// stands in until it is vendored, and the call does not change.
#pragma once
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

} // namespace kv
