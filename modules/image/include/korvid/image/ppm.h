// Korvid image: binary PPM (P6, 8-bit), what --screenshot and gallery write.
// Header comments ('#' to the end of the line) are allowed between the header's fields;
// maxval must be 255.
#pragma once
#include "korvid/core/array.h"
#include "korvid/core/memory.h"

namespace kv {

struct PpmImage {
    u32 width  = 0;
    u32 height = 0;
    u8* rgb    = nullptr; // width * height * 3 bytes, from the allocator given; the caller frees it
};

// Decodes [data, data + size) into `out`, whose pixels come from `pixels` (null: the
// general allocator). On failure returns false, allocates nothing, and sets *error.
bool ppm_decode(const void* data, usize size, Allocator* pixels, PpmImage& out, const char** error);

// Appends a P6 file of `width` x `height` tightly packed RGB pixels to `out`.
void ppm_encode(const u8* rgb, u32 width, u32 height, Array<u8>& out);

} // namespace kv
