-- The `kv` libraries a Lua script gets from the engine's tools: files and directories
-- (kv.fs), zips (kv.zip), JSON read and written (kv.json), SHA-256, the engine's Mt19937.
-- A library, not an engine module: kvscript and goldberg each open it onto their own Lua
-- state and add what is theirs (kvscript: kv.image and kv.font).
module "script" {
  deps = { "core", "platform", "pack", "inflate", "lua" },
}
