// kvscript: kv.font, TrueType text drawn into a kv.image with the truetype module's
// stb_truetype, whose math is kv's, so a glyph comes out the same on every platform. Drawing follows Pillow's
// ImageDraw.text where a port from it should match: the size is the em in pixels, the
// anchor is two letters (l m r, then a m s d), and the ink is blended in by the coverage
// as Pillow blends it into an RGBA picture.
#include "kvscript.h"
#include "korvid/core/memory.h"
#include "korvid/core/utf8.h"
#include "korvid/math/scalar.h"
#include "korvid/platform/io.h"
#include <string.h>

#include "korvid/truetype/truetype.h"

namespace kv {

namespace {

constexpr const char* kFontMeta = "kv.font";

// The font's file follows it in the userdata; stb_truetype points into it.
struct Font {
    stbtt_fontinfo info;
    f32 scale;
    f32 ascent, descent; // in pixels: above the baseline, and below it (negative)
    usize size;
};
u8* font_bytes(Font* f) { return (u8*)(f + 1); }

Font* check_font(lua_State* L, int i) { return (Font*)luaL_checkudata(L, i, kFontMeta); }

int font_load(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    f64 px = luaL_checknumber(L, 2);
    IoBytes file;
    i32 err = 0;
    if (!io_read_file(path, &file, nullptr, &err)) return luaL_error(L, "font: cannot read '%s' (errno %d)", path, err);
    Font* f = (Font*)lua_newuserdatauv(L, sizeof(Font) + file.size, 0);
    memcpy(font_bytes(f), file.data, file.size);
    f->size = file.size;
    file.free();
    luaL_setmetatable(L, kFontMeta);
    if (!stbtt_InitFont(&f->info, font_bytes(f), stbtt_GetFontOffsetForIndex(font_bytes(f), 0)))
        return luaL_error(L, "font: '%s' is not a TrueType font", path);
    // The em in pixels, as Pillow sizes a font: asset's fonts size by height
    // (ScaleForPixelHeight), which is why this is a path to stb_truetype of its own.
    f->scale = stbtt_ScaleForMappingEmToPixels(&f->info, (f32)px);
    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
    f->ascent = (f32)asc * f->scale;
    f->descent = (f32)desc * f->scale;
    return 1;
}

// The text's advance in pixels, kerning included.
f32 advance(Font* f, const char* text) {
    f32 x = 0;
    const char* p = text;
    for (u32 cp = utf8_next(p), next = 0; cp; cp = next) {
        next = utf8_next(p);
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&f->info, (int)cp, &adv, &lsb);
        x += (f32)adv * f->scale;
        if (next) x += (f32)stbtt_GetCodepointKernAdvance(&f->info, (int)cp, (int)next) * f->scale;
    }
    return x;
}

int l_measure(lua_State* L) {
    Font* f = check_font(L, 1);
    lua_pushnumber(L, advance(f, luaL_checkstring(L, 2)));
    lua_pushnumber(L, f->ascent - f->descent);
    return 2;
}

// font:draw(image, text, x, y, r, g, b, a, anchor): one line of UTF-8.
int l_draw(lua_State* L) {
    Font* f = check_font(L, 1);
    Image* m = kvscript_check_image(L, 2);
    const char* text = luaL_checkstring(L, 3);
    f64 x = luaL_checknumber(L, 4), y = luaL_checknumber(L, 5);
    u8 ink[4];
    for (int i = 0; i < 4; ++i) {
        lua_Integer v = luaL_optinteger(L, 6 + i, i == 3 ? 255 : 0);
        ink[i] = (u8)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
    const char* anchor = luaL_optstring(L, 10, "la");
    if (strlen(anchor) != 2 || !strchr("lmr", anchor[0]) || !strchr("amsd", anchor[1]))
        return luaL_error(L, "font: anchor \"%s\": l, m or r, then a, m, s or d", anchor);
    f32 w = advance(f, text);
    f64 left = anchor[0] == 'l' ? x : (anchor[0] == 'm' ? x - w / 2.0 : x - w);
    f64 base = anchor[1] == 'a' ? y + f->ascent : (anchor[1] == 'm' ? y + (f->ascent + f->descent) / 2.0 : (anchor[1] == 's' ? y : y + f->descent));
    f64 pen = left;
    i32 by = (i32)kv::floor(base);
    f32 shift_y = (f32)(base - by);
    const char* p = text;
    for (u32 cp = utf8_next(p), next = 0; cp; cp = next) {
        next = utf8_next(p);
        i32 px = (i32)kv::floor(pen);
        f32 shift_x = (f32)(pen - px);
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBoxSubpixel(&f->info, (int)cp, f->scale, f->scale, shift_x, shift_y, &x0, &y0, &x1, &y1);
        i32 gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0 && gw * gh <= 1 << 20) {
            u8* cov = (u8*)mem_alloc((usize)gw * gh);
            if (!cov) return luaL_error(L, "font: no memory for a %dx%d glyph", gw, gh);
            stbtt_MakeCodepointBitmapSubpixel(&f->info, cov, gw, gh, gw, f->scale, f->scale, shift_x, shift_y, (int)cp);
            for (i32 j = 0; j < gh; ++j)
                for (i32 i = 0; i < gw; ++i) {
                    i32 dx = px + x0 + i, dy = by + y0 + j;
                    u32 c = cov[j * gw + i];
                    if (!c || dx < 0 || dy < 0 || dx >= m->w || dy >= m->h) continue;
                    u8* o = image_px(m) + ((usize)dy * m->w + dx) * 4;
                    // Pillow's fill through a mask into RGBA: the color channels' coverage
                    // made full where the picture is clear, then each channel blended.
                    for (u32 b = 0; b < 4; ++b) {
                        u32 k = c;
                        if (b != 3) k = 255 - (255 - k) * (1 - (255 - o[3]) / 255);
                        u32 t = o[b] * (255 - k) + ink[b] * k + 128;
                        o[b] = (u8)(((t >> 8) + t) >> 8);
                    }
                }
            mem_free(cov);
        }
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&f->info, (int)cp, &adv, &lsb);
        pen += (f64)adv * f->scale;
        if (next) pen += (f64)stbtt_GetCodepointKernAdvance(&f->info, (int)cp, (int)next) * f->scale;
    }
    return 0;
}

constexpr luaL_Reg k_font_methods[] = {{"draw", l_draw}, {"measure", l_measure}, {nullptr, nullptr}};
constexpr luaL_Reg k_font_functions[] = {{"load", font_load}, {nullptr, nullptr}};

} // namespace

void kvscript_open_font(lua_State* L) {
    luaL_newmetatable(L, kFontMeta);
    lua_newtable(L);
    luaL_setfuncs(L, k_font_methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
    lua_newtable(L);
    luaL_setfuncs(L, k_font_functions, 0);
}

} // namespace kv
