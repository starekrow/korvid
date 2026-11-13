// Lua 5.5.1 (vendor/lua, unchanged; kormake's bootstrap compiles the same copy, as C)
// compiled as C++ for the engine, all of it in this one file as Lua's onelua.c does. Its
// errors are longjmp, not exceptions (the engine has none), so the bindings keep nothing
// with a destructor alive across a call that can raise. Its math library and its
// arithmetic (%, ^, //) are kv's (scalar.h), so a script computes the same on every
// platform, as the engine does.
#include "korvid/math/scalar.h"
#define LUA_USE_LONGJMP
#if !defined(_WIN32)
#define LUA_USE_POSIX
#endif
// Vendored, not ours: its warnings are silenced for its own lines only.
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push, 0)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
#include "lprefix.h"
// Lua names its math through l_mathop(name)(...), which here is name(...): with <math.h> in
// first, each name becomes kv's function for the lines below (as asset's stb_vorbis.cc
// does), and math.h's own declarations, already read, are not touched.
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <float.h>
#include <locale.h>
#include <ctype.h>
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <time.h>
#define floor(x) kv::floor((kv::f64)(x))
#define ceil(x) kv::ceil((kv::f64)(x))
#define fabs(x) kv::fabs((kv::f64)(x))
#define sqrt(x) kv::sqrt((kv::f64)(x))
#define fmod(x, y) kv::fmod((kv::f64)(x), (kv::f64)(y))
#define pow(x, y) kv::pow((kv::f64)(x), (kv::f64)(y))
#define exp(x) kv::exp((kv::f64)(x))
#define log(x) kv::log((kv::f64)(x))
#define log2(x) kv::log2((kv::f64)(x))
#define log10(x) kv::log10((kv::f64)(x))
#define sin(x) kv::sin((kv::f64)(x))
#define cos(x) kv::cos((kv::f64)(x))
#define tan(x) kv::tan((kv::f64)(x))
#define asin(x) kv::asin((kv::f64)(x))
#define acos(x) kv::acos((kv::f64)(x))
#define atan2(x, y) kv::atan2((kv::f64)(x), (kv::f64)(y))
#define frexp(x, y) kv::frexp((kv::f64)(x), (kv::i32*)(y))
#define ldexp(x, y) kv::ldexp((kv::f64)(x), (kv::i32)(y))
// The core, then the libraries, in onelua.c's order.
#include "lzio.c"
#include "lctype.c"
#include "lopcodes.c"
#include "lmem.c"
#include "lundump.c"
#include "ldump.c"
#include "lstate.c"
#include "lgc.c"
#include "llex.c"
#include "lcode.c"
#include "lparser.c"
#include "ldebug.c"
#include "lfunc.c"
#include "lobject.c"
#include "ltm.c"
#include "lstring.c"
#include "ltable.c"
#include "ldo.c"
#include "lvm.c"
#include "lapi.c"
#include "lauxlib.c"
#include "lbaselib.c"
#include "lcorolib.c"
#include "ldblib.c"
#include "liolib.c"
#include "lmathlib.c"
#include "loadlib.c"
#include "loslib.c"
#include "lstrlib.c"
#include "ltablib.c"
#include "lutf8lib.c"
#include "linit.c"
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif
