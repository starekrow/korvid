// Korvid asset: a small PNG decoder on zlib. 8-bit samples, every color type, no
// interlace; always produces tightly packed RGBA8. The PRD names libpng; this stands
// in until it is vendored, and the call does not change.
#pragma once
#include "korvid/core/memory.h"

namespace kv {

struct PngImage {
    u32 width  = 0;
    u32 height = 0;
    u8* rgba   = nullptr; // width * height * 4 bytes, from the allocator given
};

// Decodes into `out`, whose pixels come from `pixels` (null: general). Temporaries use
// the calling thread's scratch. On failure returns false and sets *error to a reason.
bool png_decode(const void* data, usize size, Allocator* pixels, PngImage& out, const char** error);

} // namespace kv
