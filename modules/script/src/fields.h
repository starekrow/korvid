// Korvid script: one field of a field table (component.h) to Lua and back, as the ktable
// binding, the event binding and the resource binding all map them. Values: a bool is a
// boolean; integers are integers (a u64 past 2^63 wraps, as Lua's integers do); floats are
// numbers; a handle is its 64 bits as an integer, 0 for none; a char[N] is a string; a
// u8[N] is a string of exactly N bytes (a digest); any other array is a list.
#pragma once
#include "korvid/core/component.h"
#include "korvid/lua/lua.h"

namespace kv {

// Pushes the field's value in `row`.
void script_push_field(lua_State* L, const FieldDesc& f, const u8* row);
// Writes the value at `index` into the field in `row`. Raises a Lua error, naming the
// field, for a value of the wrong type, an integer that does not fit, a string too long
// or a list of the wrong length: a row that silently holds something else is the worse
// outcome.
void script_set_field(lua_State* L, const FieldDesc& f, u8* row, int index);
// The field named `name` in the table, or null.
const FieldDesc* script_find_field(Span<const FieldDesc> fields, const char* name);
// Pushes a table of every field in `row`, by name.
void script_push_fields(lua_State* L, Span<const FieldDesc> fields, const u8* row);
// Writes each key of the table at `index` into its field in `row`. Raises for a key
// that names no field (`what` names the type in the message) and as script_set_field does.
void script_set_fields(lua_State* L, Span<const FieldDesc> fields, u8* row, int index, const char* what);

} // namespace kv
