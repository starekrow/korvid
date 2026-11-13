// The stb_truetype implementation, once for the engine, with its allocations on the
// engine's general allocator (attributed to whoever is running: the asset pump, a tool).
// The free macro has no size, so a header carries it.
#include "korvid/core/memory.h"
#include <string.h>

namespace {
void* stb_alloc(size_t n) {
    kv::Allocator* a = kv::mem_general();
    kv::usize* p = (kv::usize*)a->alloc(a, n + 16, 16);
    if (!p) return nullptr;
    *p = (kv::usize)n + 16;
    return (char*)p + 16;
}
void stb_free(void* p) {
    if (!p) return;
    kv::usize* h = (kv::usize*)((char*)p - 16);
    kv::Allocator* a = kv::mem_general();
    a->free(a, h, *h);
}
} // namespace

#define STBTT_malloc(x, u) ((void)(u), stb_alloc(x))
#define STBTT_free(x, u)   ((void)(u), stb_free(x))
#define STB_TRUETYPE_IMPLEMENTATION
// Its math is kv's, not the platform's libm, so a glyph rasterizes to the same pixels on
// every platform; with these defined it does not include <math.h>. Each is the double
// function it would have called.
#include "korvid/math/scalar.h"
#define STBTT_ifloor(x)   ((int)kv::floor((kv::f64)(x)))
#define STBTT_iceil(x)    ((int)kv::ceil((kv::f64)(x)))
#define STBTT_sqrt(x)     kv::sqrt((kv::f64)(x))
#define STBTT_pow(x, y)   kv::pow((kv::f64)(x), (kv::f64)(y))
#define STBTT_fmod(x, y)  kv::fmod((kv::f64)(x), (kv::f64)(y))
#define STBTT_cos(x)      kv::cos((kv::f64)(x))
#define STBTT_acos(x)     kv::acos((kv::f64)(x))
#define STBTT_fabs(x)     kv::fabs((kv::f64)(x))
// Vendored, not ours: its warnings are silenced for its own lines only.
#if KV_COMPILER_MSVC
#pragma warning(push, 0)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
#include "../vendor/stb_truetype.h"
#if KV_COMPILER_MSVC
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif
