#include "korvid/lua/lua.h"
#include "korvid/core/test.h"
#include "korvid/math/scalar.h"
#include <string.h>

// The engine's Lua: a chunk runs, an error comes back through pcall (by longjmp, past the
// C++ that called it), the standard libraries are there, the math is kv's to the bit, and
// math.random starts from the same seed every time.

namespace {
using namespace kv;

int fails(lua_State* L) { return luaL_error(L, "from C: %d", 42); }
bool same(f64 a, f64 b) { return math_bits::of(a) == math_bits::of(b); } // NaN too

f64 run_number(lua_State* L, const char* chunk) {
    if (luaL_dostring(L, chunk) != LUA_OK) return -1.0;
    f64 v = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return v;
}
} // namespace

void test_lua(TestCtx& c) {
    c.begin("lua");
    lua_State* L = kv_lua_newstate(mem_general());
    KV_CHECK(c, L != nullptr);
    luaL_openlibs(L);
    KV_CHECK(c, run_number(L, "return 6 * 7") == 42.0);
    KV_CHECK(c, run_number(L, "local t = {} for i = 1, 10 do t[i] = i * i end return #t + t[10]") == 110.0);
    KV_CHECK(c, run_number(L, "return #string.format('%5.2f|%d', 3.14159, 12)") == 8.0);
    KV_CHECK(c, run_number(L, "return utf8.len('h\\u{e9}llo')") == 5.0);

    // An error from C, caught by Lua's pcall; then one left for the host's.
    lua_pushcfunction(L, fails);
    lua_setglobal(L, "fails");
    KV_CHECK(c, luaL_dostring(L, "local ok, e = pcall(fails) return (not ok) and e:find('from C: 42') ~= nil") == LUA_OK && lua_toboolean(L, -1));
    lua_pop(L, 1);
    KV_CHECK(c, luaL_dostring(L, "error('out')") != LUA_OK && strstr(lua_tostring(L, -1), "out") != nullptr);
    lua_pop(L, 1);

    // kv's math, bit for bit: the library's functions and the operators.
    const f64 xs[] = {0.1, 1.0, 2.5, -3.75, 100.0, 1e-7};
    for (f64 x : xs) {
        lua_pushnumber(L, x);
        lua_setglobal(L, "x");
        KV_CHECK(c, same(run_number(L, "return math.sin(x)"), kv::sin(x)) && same(run_number(L, "return math.exp(x)"), kv::exp(x)));
        KV_CHECK(c, same(run_number(L, "return x ^ 1.7"), kv::pow(x, 1.7)) && same(run_number(L, "return math.atan(x, 2)"), kv::atan2(x, 2.0)));
        // Lua's float %: fmod, then the divisor added when the signs differ (luai_nummod).
        f64 m = kv::fmod(x, 0.7);
        if (m < 0.0) m += 0.7;
        KV_CHECK(c, same(run_number(L, "return x % 0.7"), m) && same(run_number(L, "return x // 0.3"), kv::floor(x / 0.3)));
    }
    KV_CHECK(c, run_number(L, "return math.log(8, 2)") == 3.0 && run_number(L, "return math.floor(-2.5)") == -3.0);
    lua_close(L);

    // The seed is fixed: a new state's first draw is the same on every run and platform.
    L = kv_lua_newstate(mem_general());
    luaL_openlibs(L);
    KV_CHECK(c, luaL_dostring(L, "return math.random(0)") == LUA_OK && lua_tointeger(L, -1) == 4554719557422691265);
    lua_close(L);
}

int main() {
    TestCtx c;
    test_lua(c);
    return test_report(c, "lua_tests");
}
