# script

The `kv` libraries a Lua script gets from the engine's tools, as a library those tools
link: kvscript (which adds `kv.image` and `kv.font`) and goldberg. Not an engine module:
it has no systems and nothing to register; a tool opens it onto its own Lua state.

```cpp
#include "korvid/script/script.h"

lua_State* L = luaL_newstate();
luaL_openlibs(L);
script_open_kv(L);                 // pushes kv: fs, zip, json, sha256, random
lua_setglobal(L, "kv");
script_push_kind(L, engine_table(e, "goldberg.file"));
lua_setglobal(L, "files");
```

| | |
|---|---|
| `kv.fs` | files and directories through the platform: UTF-8 paths on every OS |
| `kv.zip` | a zip read whole, entries stored or deflated, CRCs checked |
| `kv.json` | `encode` as Python's `json.dump` writes; `decode` into the same values with key order and line numbers kept, `//` comments and dropped members on request |
| `kv.sha256` | a digest as 64 hex digits (`korvid/core/sha256.h`) |
| `kv.random` | the engine's `Mt19937`, seeded as Python's |

What each function takes and returns is in `tools/kvscript/README.md`, where a script's
author looks.

## Kinds in Lua

`script_push_kind` pushes an engine kind as an object whose rows are read and written by
field name, from the kind's own field tables. The layout stays in C++ (the component
structs); Lua holds whatever a row cannot.

```lua
local h = files:spawn{ path = "ships/big.png", size = 3 }   -- a handle, an integer; nil if the kind is full
files:set(h, "size", 4)        files:get(h, "size")          -- get: nil for a stale handle
local r = files:row(h)         r.size = 5   print(r.path, r.handle, r.alive)
for h in files:each() do ... end                             -- live rows, slot order
files:destroy(h)  files:valid(h)  files:live()  files:capacity()  files:name()  files:fields()
```

- A field is `"field"` when one component of the kind has it, and always `"Component.field"`.
- bool is a boolean; integers are integers; floats are numbers; a handle is its 64 bits,
  0 for none; `char[N]` is a string; `u8[N]` is a string of exactly N bytes; any other
  array is a list.
- A value of the wrong type, an integer that does not fit, a string too long, an unknown
  field, a write through a stale handle: each is a Lua error, never a quiet change.
- The caller answers for access. The Lua runs inside a system that declared what it
  writes; the binding checks nothing of that.

Lua errors are `longjmp` (see `modules/lua`): nothing in this module keeps an object with
a destructor alive across a call that can raise.
