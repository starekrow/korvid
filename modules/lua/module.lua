-- Lua 5.5.1 for the engine: the official distribution (vendor/lua, unchanged; kormake's
-- bootstrap compiles the same copy into kormake) built as C++, its errors by longjmp and
-- its math kv's (src/lua.cc). kvscript runs on it; a game may embed it as well.
-- Not a unity build: Lua's sources define short macros that must stay in their file.
module "lua" {
  deps             = { "core", "math" },
  unity            = false,
  private_includes = { "vendor/lua" },
}
