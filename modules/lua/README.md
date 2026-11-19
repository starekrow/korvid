# lua

Lua 5.5.1, the official distribution (`vendor/lua`: its `src/` and `README`, unchanged),
built for the engine. It is the one copy in the tree: kormake's bootstrap compiles the
same files into kormake (as C, with the host's libm, for a build tool), and this module
compiles them for everything else, kvscript first, a game that embeds a scripting
language next.

## How the engine builds it

`src/lua.cc` is all of Lua in one translation unit, in the order Lua's own `onelua.c`
uses, compiled as C++. Four things differ from a stock build, none of them in Lua's
files:

- **Errors are `longjmp`** (`LUA_USE_LONGJMP`): the engine has no exceptions. A Lua error
  leaves through every C++ frame between the raise and its `pcall` without running a
  destructor, so code that Lua calls keeps nothing with one alive across a call that can
  raise (`luaL_check*`, `lua_error`, anything that allocates), and holds what must outlive
  an error in a userdata.
- **The math is kv's.** Lua names every math function through `l_mathop(name)`; with
  `<math.h>` read first, each name is defined to kv's function (`modules/math`) for Lua's
  lines. `math.sin`, `math.exp`, `x ^ y`, `x % y`, `x // y` are bit for bit the engine's,
  the same on every platform; the tests check it.
- **The seed is fixed.** Lua seeds `math.random` from the time and an address
  (`luai_makeseed`, lauxlib's hook); here it is a constant, so every state draws the same
  numbers on every run, and so do `math.randomseed()` with no argument, `table.sort`'s
  pivots and the strings' hash (and with it `pairs`' order over string keys). A script
  that wants a varying seed calls `math.randomseed` itself, with one of its own
  (`math.randomseed(os.time())`). The tests check the first draw.
- **C++ linkage.** The API has no `extern "C"`: everything that uses it is C++ too.

The module is not a unity build: Lua's sources define short macros (`cast`, `G`) that
must stay in their file.

## Using it

```cpp
#include "korvid/lua/lua.h"   // lua.h, lauxlib.h and lualib.h

lua_State* L = kv_lua_newstate(mem_general());   // or a module's own allocator
luaL_openlibs(L);
if (luaL_dostring(L, "return 6 * 7") == LUA_OK) KV_INFO("game", "%g", lua_tonumber(L, -1));
lua_close(L);
```

The API is Lua's own; the reference manual is at https://www.lua.org/manual/5.5/. Lua's
`io` and `os` libraries are the C library's: on Windows `io.open` does not take UTF-8
paths, so engine code hands a script the platform's files instead (kvscript's `kv.fs`).
`kv_lua_newstate` is `luaL_newstate` (its seed, panic and warning handlers) on an engine
allocator, so Lua's memory is in the engine's accounting; Krome, goldberg and kvscript
make their states with it. `luaL_newstate` is still there, on `realloc`.

## Upgrading

`vendor/UPGRADE.md`: fetch the tarball, check its hash, replace `vendor/lua/`, then run
kormake's own test (`tools/kormake/tests/run.sh`), this module's (`kormake test lua`) and
kvscript's ports.
