// Korvid lua: Lua 5.5.1's C API (vendor/lua's lua.h, lauxlib.h and lualib.h, unchanged),
// as this module builds it: C++ linkage throughout (the library is compiled as C++, so
// nothing here is extern "C"), errors by longjmp rather than exceptions, and the math
// library and the arithmetic (math.sin, %, ^, //) on kv's functions, so a script computes
// the same on every platform.
//
// longjmp is the rule to keep in mind: a Lua error leaves through every C++ frame between
// the raise and its pcall without running a destructor. Code called from Lua keeps
// nothing with one alive across a call that can raise (luaL_check*, lua_error, any
// allocation), and holds what must outlive an error in userdata.
#pragma once
#include "../../../vendor/lua/lua.h"
#include "../../../vendor/lua/lauxlib.h"
#include "../../../vendor/lua/lualib.h"
