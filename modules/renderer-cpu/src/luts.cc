// The sRGB tables the pixel kernels read (raster.h), made once with the engine's math: this
// file is built as the rest of the engine is, so raster.cc needs no math of its own.
#include "raster.h"
#include "korvid/math/scalar.h"

namespace kv { namespace rb_cpu {

namespace {

f32 srgb_to_linear(f32 c) { return c <= 0.04045f ? c / 12.92f : kv::pow((c + 0.055f) / 1.055f, 2.4f); }
f32 linear_to_srgb(f32 l) { return l <= 0.0031308f ? l * 12.92f : 1.055f * kv::pow(l, 1.0f / 2.4f) - 0.055f; }

void make(CpuLuts& t) {
    for (u32 i = 0; i < 256; ++i) t.decode[i] = (u16)(srgb_to_linear((f32)i / 255.0f) * 65535.0f + 0.5f);
    for (u32 i = 0; i < 65536; ++i) {
        f32 s = linear_to_srgb((f32)i / 65535.0f) * 255.0f + 0.5f;
        t.encode[i] = (u8)(s >= 255.0f ? 255 : s <= 0.0f ? 0 : (u32)s);
    }
}

} // namespace

const CpuLuts& cpu_luts() {
    static CpuLuts t;
    static bool made = false; // made by the first frame, on the engine's thread, before any tile job
    if (!made) { make(t); made = true; }
    return t;
}

u16 cpu_linear16(f32 c) {
    if (!(c > 0.0f)) return 0;
    if (c >= 1.0f) return 65535;
    return (u16)(c * 65535.0f + 0.5f);
}

} } // namespace kv::rb_cpu
