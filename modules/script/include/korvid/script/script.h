// Korvid script: the `kv` libraries for Lua that the engine's tools share.
//
//   lua_State* L = kv_lua_newstate(mem_general());
//   luaL_openlibs(L);
//   script_open_kv(L);            // pushes the kv table: fs, zip, json, sha256, random
//   lua_setglobal(L, "kv");
//
// A tool adds its own libraries to the table before naming it. What each library holds is
// in tools/kvscript/README.md, where a script's author looks.
#pragma once
#include "korvid/core/types.h"
#include "korvid/lua/lua.h"

namespace kv {

// Pushes a new table with every library below set on it.
void script_open_kv(lua_State* L);

// Each pushes its library's table.
void script_open_fs(lua_State* L);
void script_open_zip(lua_State* L);
void script_open_json(lua_State* L);
// Each pushes its one function.
void script_open_sha256(lua_State* L);
void script_open_random(lua_State* L);

// Pushes an engine ktable as a Lua ktable object, its rows read and written by field name
// from the ktable's own field tables. See ktable.cc for what the object answers.
// The caller answers for access: whoever runs the Lua (a system, usually) has declared
// what it writes, and the ktable must outlive the Lua state's use of the object.
struct KTable;
void script_push_ktable(lua_State* L, KTable* table);

// A message handler for lua_pcall: the error with a traceback.
int script_traceback(lua_State* L);

} // namespace kv
