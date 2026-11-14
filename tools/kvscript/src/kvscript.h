// kvscript's pieces, for its own files: the libraries that are its own (kv.image,
// kv.font), each opened onto the stack as a table. The rest of kv is modules/script.
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

} // namespace kv
