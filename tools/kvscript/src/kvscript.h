// kvscript's pieces, for its own files: the Lua state's libraries (kv.image, kv.font,
// kv.zip, kv.fs), each opened onto the stack as a table.
#pragma once
#include "korvid/core/types.h"
#include "korvid/lua/lua.h"

namespace kv {

// An RGBA8 picture, its pixels right after it in the same userdata.
struct Image {
    i32 w, h;
};
inline u8* image_px(Image* m) { return (u8*)(m + 1); }

Image* kvscript_check_image(lua_State* L, int index);

// Each pushes its library's table.
void kvscript_open_image(lua_State* L);
void kvscript_open_font(lua_State* L);
void kvscript_open_zip(lua_State* L);
void kvscript_open_fs(lua_State* L);
// Sets kv.sha256 and kv.json on the table at the top of the stack.
void kvscript_open_json_sha(lua_State* L);

} // namespace kv
