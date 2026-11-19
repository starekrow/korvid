// Lua 5.5.1 (vendor/lua, unchanged; kormake's bootstrap compiles the same copy, as C)
// compiled as C++ for the engine, all of it in this one file as Lua's onelua.c does. Its
// errors are longjmp, not exceptions (the engine has none), so the bindings keep nothing
// with a destructor alive across a call that can raise. Its math library and its
// arithmetic (%, ^, //) are kv's (scalar.h), so a script computes the same on every
// platform, as the engine does. Its seed is a constant, so every state runs the same way
// every time.
#if defined(_MSC_VER) && !defined(__clang__)
// C4701 (a variable maybe used uninitialized: lvm.c's and lobject.c's n1 and n2, which Lua's
// number macros always set) comes from cl's optimizer, which does not honour the push below
// around the lines it is about. Off for the whole of this file, which is Lua's alone
// (unity = false); the engine's own files keep it.
#pragma warning(disable: 4701)
#endif
#include "korvid/math/scalar.h"
#define LUA_USE_LONGJMP
// luai_makeseed is lauxlib's "random" seed (the time and an address): math.random starts
// from it, as do math.randomseed() with no argument, table.sort's pivots and, through
// luaL_newstate and kv_lua_newstate, the strings' hash and so pairs' order. A constant
// makes all of them the same on every run.
#define luai_makeseed() 0u
#if !defined(_WIN32)
#define LUA_USE_POSIX
#endif
// iOS has no system(): Lua's own switch makes os.execute answer that no shell is there.
#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#define LUA_USE_IOS
#endif
#endif
// Vendored, not ours: its warnings are silenced for its own lines only.
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push, 0)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
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

// kv_lua_newstate: luaL_newstate (the seed, lauxlib's panic and warnings) on an engine
// allocator. Lua's allocator contract on the engine's: nsize 0 frees, anything else is a
// new block with what fits of the old one copied in.
#include "korvid/lua/lua.h"
namespace {
void* kv_lua_alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    kv::Allocator* a = (kv::Allocator*)ud;
    if (nsize == 0) {
        kv::free_in(a, ptr, osize);
        return nullptr;
    }
    void* n = kv::alloc_in(a, nsize);
    if (n && ptr) {
        memcpy(n, ptr, osize < nsize ? osize : nsize);
        kv::free_in(a, ptr, osize);
    }
    return n;
}
} // namespace

lua_State* kv_lua_newstate(kv::Allocator* a) {
    lua_State* L = lua_newstate(kv_lua_alloc, a, luaL_makeseed(nullptr));
    if (L) {
        lua_atpanic(L, &panic);
        lua_setwarnf(L, warnfon, L);
    }
    return L;
}
