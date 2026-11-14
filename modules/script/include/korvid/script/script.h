// Korvid script: the `kv` libraries for Lua that the engine's tools share.
//
//   lua_State* L = luaL_newstate();
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

// Pushes an engine kind (a table of rows) as a Lua object, its rows read and written by
// field name from the kind's own field tables. See kind.cc for what the object answers.
// The caller answers for access: whoever runs the Lua (a system, usually) has declared
// what it writes, and the table must outlive the Lua state's use of the object.
struct Table;
void script_push_kind(lua_State* L, Table* table);

// A message handler for lua_pcall: the error with a traceback.
int script_traceback(lua_State* L);

} // namespace kv
