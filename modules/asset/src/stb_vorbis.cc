// The stb_vorbis implementation, once. Decoding goes through stb_vorbis_open_memory with
// a caller-supplied work buffer, so the decoder never calls malloc; the output buffer is
// the engine's.
#include "korvid/core/types.h"
// Vendored, not ours: its warnings are silenced for its own lines only.
#if KV_COMPILER_MSVC
#pragma warning(push, 0)
#pragma warning(disable: 4701 4703) // the optimizer's: push, 0 sets a level, which these ignore
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wcast-function-type"
#if !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized" // gcc's alone; clang errors on a group it does not know
#endif
#pragma GCC diagnostic ignored "-Wuninitialized"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wtautological-compare"
#endif
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_INTEGER_CONVERSION
// Its math is kv's, not the platform's libm, so a sound decodes to the same samples on
// every platform. It includes these itself; here first, they are already in when it
// does, so the seven names below are macros in its own lines alone. Each is the double
// function it would have called.
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <limits.h>
#if defined(_MSC_VER) || defined(__MINGW32__)
#include <malloc.h>
#endif
#if defined(__linux__)
#include <alloca.h>
#endif
#include "korvid/math/scalar.h"
#define sin(x)      kv::sin((kv::f64)(x))
#define cos(x)      kv::cos((kv::f64)(x))
#define exp(x)      kv::exp((kv::f64)(x))
#define log(x)      kv::log((kv::f64)(x))
#define pow(x, y)   kv::pow((kv::f64)(x), (kv::f64)(y))
#define floor(x)    kv::floor((kv::f64)(x))
#define ldexp(x, e) kv::ldexp((kv::f64)(x), (kv::i32)(e))
#include "vendor/stb_vorbis.c"
#undef sin
#undef cos
#undef exp
#undef log
#undef pow
#undef floor
#undef ldexp
#if KV_COMPILER_MSVC
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif
