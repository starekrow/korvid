// kvscript: kv.image, RGBA8 pictures as Lua values. The pixels live in the userdata, so
// the collector owns them and a Lua error (a longjmp) leaks nothing. The operations are
// the ones asset tools reach for, done as Pillow does them where a tool ported from it
// should come out the same: alpha_composite's integer blend, resampling with Pillow's
// filters and its fixed-point passes on premultiplied pixels, rotation by its affine
// transform with bicubic sampling, crops that run off the edge into transparency.
#include "kvscript.h"
#include "korvid/core/format.h"
#include "korvid/core/memory.h"
#include "korvid/image/png.h"
#include "korvid/image/ppm.h"
#include "korvid/math/scalar.h"
#include "korvid/platform/io.h"
#include <stdlib.h>
#include <string.h>

namespace kv {

namespace {

constexpr const char* kImageMeta = "kv.image";
constexpr i64 kMaxPixels = 1ll << 28; // 1 GiB of RGBA: past that, a mistake

u8 clamp8(i64 v) { return (u8)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

Image* push_image(lua_State* L, i32 w, i32 h) {
    if (w < 0 || h < 0 || (i64)w * h > kMaxPixels) luaL_error(L, "image: %dx%d is not a size", w, h);
    usize bytes = (usize)w * (usize)h * 4;
    Image* m = (Image*)lua_newuserdatauv(L, sizeof(Image) + bytes, 0);
    m->w = w;
    m->h = h;
    memset(image_px(m), 0, bytes);
    luaL_setmetatable(L, kImageMeta);
    return m;
}

u8* at(Image* m, i32 x, i32 y) { return image_px(m) + ((usize)y * (usize)m->w + (usize)x) * 4; }

// ---- premultiplied, as Pillow's RGBa ---------------------------------------------------
// v * a / 255 rounded (Pillow's MULDIV255), and back by truncating division.
void premultiply(u8* p, usize n) {
    for (usize i = 0; i < n; ++i, p += 4) {
        u32 a = p[3];
        for (u32 c = 0; c < 3; ++c) {
            u32 t = p[c] * a + 128;
            p[c] = (u8)(((t >> 8) + t) >> 8);
        }
    }
}
void unpremultiply(u8* p, usize n) {
    for (usize i = 0; i < n; ++i, p += 4) {
        u32 a = p[3];
        if (a == 0 || a == 255) continue;
        for (u32 c = 0; c < 3; ++c) p[c] = clamp8((i64)(255u * p[c]) / a);
    }
}

// ---- resampling: Pillow's Resample.c ---------------------------------------------------
struct Filter {
    f64 support;
    f64 (*fn)(f64);
};
f64 sinc(f64 x) {
    if (x == 0.0) return 1.0;
    x *= 3.14159265358979323846;
    return kv::sin(x) / x;
}
f64 lanczos(f64 x) { return (-3.0 <= x && x < 3.0) ? sinc(x) * sinc(x / 3.0) : 0.0; }
f64 bicubic(f64 x) { // a = -0.5
    const f64 a = -0.5;
    if (x < 0.0) x = -x;
    if (x < 1.0) return ((a + 2.0) * x - (a + 3.0)) * x * x + 1.0;
    if (x < 2.0) return (((x - 5.0) * x + 8.0) * x - 4.0) * a;
    return 0.0;
}
f64 bilinear(f64 x) {
    if (x < 0.0) x = -x;
    return x < 1.0 ? 1.0 - x : 0.0;
}
f64 box(f64 x) { return (x > -0.5 && x <= 0.5) ? 1.0 : 0.0; }

constexpr i32 kPrecisionBits = 32 - 8 - 2;

// For each output pixel, the first input pixel and the integer weights of those after it.
struct Coeffs {
    i32  ksize = 0;
    i32* bounds = nullptr; // 2 per output: first, count
    i32* k = nullptr;      // ksize per output
};
void coeffs(i32 in_size, i32 out_size, const Filter& f, Coeffs* c, Allocator* a) {
    f64 scale = (f64)in_size / out_size, filterscale = scale < 1.0 ? 1.0 : scale;
    f64 support = f.support * filterscale;
    c->ksize = (i32)kv::ceil(support) * 2 + 1;
    c->bounds = (i32*)a->alloc(a, sizeof(i32) * 2 * (usize)out_size, 8);
    c->k = (i32*)a->alloc(a, sizeof(i32) * (usize)c->ksize * (usize)out_size, 8);
    f64* kd = (f64*)a->alloc(a, sizeof(f64) * (usize)c->ksize, 8);
    for (i32 xx = 0; xx < out_size; ++xx) {
        f64 center = (xx + 0.5) * scale, ww = 0.0, ss = 1.0 / filterscale;
        i32 xmin = (i32)(center - support + 0.5);
        if (xmin < 0) xmin = 0;
        i32 xmax = (i32)(center + support + 0.5);
        if (xmax > in_size) xmax = in_size;
        xmax -= xmin;
        for (i32 x = 0; x < xmax; ++x) {
            f64 w = f.fn((x + xmin - center + 0.5) * ss);
            kd[x] = w;
            ww += w;
        }
        for (i32 x = 0; x < c->ksize; ++x) {
            f64 w = x < xmax && ww != 0.0 ? kd[x] / ww : 0.0;
            f64 s = w * (f64)(1 << kPrecisionBits);
            c->k[(usize)xx * (usize)c->ksize + (usize)x] = (i32)(w < 0 ? kv::trunc(-0.5 + s) : kv::trunc(0.5 + s));
        }
        c->bounds[xx * 2] = xmin;
        c->bounds[xx * 2 + 1] = xmax;
    }
    a->free(a, kd, sizeof(f64) * (usize)c->ksize);
}
u8 clip8(i64 v) {
    if (v >= (1ll << kPrecisionBits << 8)) return 255;
    if (v <= 0) return 0;
    return (u8)(v >> kPrecisionBits);
}
// One pass along x (or along y, with the strides swapped): in is iw x ih, out ow x ih.
void pass_x(const u8* in, i32 iw, i32 ih, u8* out, i32 ow, const Coeffs& c) {
    for (i32 y = 0; y < ih; ++y)
        for (i32 xx = 0; xx < ow; ++xx) {
            i32 xmin = c.bounds[xx * 2], xmax = c.bounds[xx * 2 + 1];
            const i32* k = c.k + (usize)xx * (usize)c.ksize;
            for (u32 b = 0; b < 4; ++b) {
                i64 ss = 1ll << (kPrecisionBits - 1);
                for (i32 x = 0; x < xmax; ++x) ss += (i64)in[((usize)y * (usize)iw + (usize)(x + xmin)) * 4 + b] * k[x];
                out[((usize)y * (usize)ow + (usize)xx) * 4 + b] = clip8(ss);
            }
        }
}
void pass_y(const u8* in, i32 iw, u8* out, i32 oh, const Coeffs& c) {
    for (i32 yy = 0; yy < oh; ++yy) {
        i32 ymin = c.bounds[yy * 2], ymax = c.bounds[yy * 2 + 1];
        const i32* k = c.k + (usize)yy * (usize)c.ksize;
        for (i32 x = 0; x < iw; ++x)
            for (u32 b = 0; b < 4; ++b) {
                i64 ss = 1ll << (kPrecisionBits - 1);
                for (i32 y = 0; y < ymax; ++y) ss += (i64)in[((usize)(y + ymin) * (usize)iw + (usize)x) * 4 + b] * k[y];
                out[((usize)yy * (usize)iw + (usize)x) * 4 + b] = clip8(ss);
            }
    }
}
void free_coeffs(Coeffs& c, i32 out_size, Allocator* a) {
    a->free(a, c.bounds, sizeof(i32) * 2 * (usize)out_size);
    a->free(a, c.k, sizeof(i32) * (usize)c.ksize * (usize)out_size);
}

bool filter_named(const char* name, Filter* f) {
    if (!strcmp(name, "lanczos")) *f = Filter{3.0, lanczos};
    else if (!strcmp(name, "bicubic")) *f = Filter{2.0, bicubic};
    else if (!strcmp(name, "bilinear")) *f = Filter{1.0, bilinear};
    else if (!strcmp(name, "box")) *f = Filter{0.5, box};
    else return false;
    return true;
}

// ---- the functions -----------------------------------------------------------------------

Image* check(lua_State* L, int i) { return (Image*)luaL_checkudata(L, i, kImageMeta); }
u8 channel(lua_State* L, int i, lua_Integer dflt) { return clamp8(luaL_optinteger(L, i, dflt)); }

int l_new(lua_State* L) {
    i32 w = (i32)luaL_checkinteger(L, 1), h = (i32)luaL_checkinteger(L, 2);
    u8 c[4] = {channel(L, 3, 0), channel(L, 4, 0), channel(L, 5, 0), channel(L, 6, 0)};
    Image* m = push_image(L, w, h);
    u8* p = image_px(m);
    for (usize i = 0; i < (usize)w * (usize)h; ++i) memcpy(p + i * 4, c, 4);
    return 1;
}

// PNG (any the image module reads) or binary PPM, by the bytes.
int decode_bytes(lua_State* L, const u8* data, usize size, const char* what) {
    const char* why = nullptr;
    if (size >= 8 && !memcmp(data, "\x89PNG", 4)) {
        PngImage png;
        if (!png_decode(data, size, mem_general(), png, &why)) return luaL_error(L, "image: %s: %s", what, why);
        Image* m = push_image(L, (i32)png.width, (i32)png.height);
        memcpy(image_px(m), png.rgba, (usize)png.width * png.height * 4);
        mem_general()->free(mem_general(), png.rgba, (usize)png.width * png.height * 4);
        return 1;
    }
    PpmImage ppm;
    if (!ppm_decode(data, size, mem_general(), ppm, &why)) return luaL_error(L, "image: %s: not a PNG, and not a PPM (%s)", what, why);
    Image* m = push_image(L, (i32)ppm.width, (i32)ppm.height);
    u8* p = image_px(m);
    for (usize i = 0; i < (usize)ppm.width * ppm.height; ++i) {
        memcpy(p + i * 4, ppm.rgb + i * 3, 3);
        p[i * 4 + 3] = 255;
    }
    mem_general()->free(mem_general(), ppm.rgb, (usize)ppm.width * ppm.height * 3);
    return 1;
}
int l_decode(lua_State* L) {
    usize n = 0;
    const char* s = luaL_checklstring(L, 1, &n);
    return decode_bytes(L, (const u8*)s, n, "bytes");
}
int l_load(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    IoBytes file;
    i32 err = 0;
    if (!io_read_now(path, &file, nullptr, &err)) return luaL_error(L, "image: cannot read '%s' (errno %d)", path, err);
    // The bytes go into the new image before anything can raise past them.
    lua_pushlstring(L, (const char*)file.data, file.size);
    file.free();
    usize n = 0;
    const char* s = lua_tolstring(L, -1, &n);
    return decode_bytes(L, (const u8*)s, n, path);
}

// The PNG's bytes, as a Lua string, or an error.
int push_png(lua_State* L, Image* m) {
    Array<u8> out;
    const char* why = nullptr;
    if (!png_encode(image_px(m), (u32)m->w, (u32)m->h, 4, out, &why)) { out.free(); return luaL_error(L, "image: cannot encode: %s", why); }
    lua_pushlstring(L, (const char*)out.data, out.len);
    out.free();
    return 1;
}
int l_encode(lua_State* L) { return push_png(L, check(L, 1)); }
int l_save(lua_State* L) {
    Image* m = check(L, 1);
    const char* path = luaL_checkstring(L, 2);
    push_png(L, m);
    usize n = 0;
    const char* s = lua_tolstring(L, -1, &n);
    i32 err = 0;
    if (!io_write_now(path, s, n, &err)) return luaL_error(L, "image: cannot write '%s' (errno %d)", path, err);
    return 0;
}

int l_index(lua_State* L) {
    Image* m = check(L, 1);
    const char* k = lua_tostring(L, 2);
    if (k && !strcmp(k, "width")) { lua_pushinteger(L, m->w); return 1; }
    if (k && !strcmp(k, "height")) { lua_pushinteger(L, m->h); return 1; }
    luaL_getmetafield(L, 1, "methods");
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);
    return 1;
}
int l_tostring(lua_State* L) {
    Image* m = check(L, 1);
    lua_pushfstring(L, "kv.image %dx%d", m->w, m->h);
    return 1;
}

int l_get(lua_State* L) {
    Image* m = check(L, 1);
    lua_Integer x = luaL_checkinteger(L, 2), y = luaL_checkinteger(L, 3);
    if (x < 0 || y < 0 || x >= m->w || y >= m->h) { for (int i = 0; i < 4; ++i) lua_pushinteger(L, 0); return 4; }
    const u8* p = at(m, (i32)x, (i32)y);
    for (int i = 0; i < 4; ++i) lua_pushinteger(L, p[i]);
    return 4;
}
// The alpha alone, the common case of a scan, at a quarter of the calls' cost.
int l_alpha(lua_State* L) {
    Image* m = check(L, 1);
    lua_Integer x = luaL_checkinteger(L, 2), y = luaL_checkinteger(L, 3);
    lua_pushinteger(L, (x < 0 || y < 0 || x >= m->w || y >= m->h) ? 0 : at(m, (i32)x, (i32)y)[3]);
    return 1;
}
// One channel (1 red .. 4 alpha) of every pixel, rows top to bottom, as a string of w*h
// bytes: what a scan over the whole picture reads at string.byte's speed.
int l_channel(lua_State* L) {
    Image* m = check(L, 1);
    lua_Integer c = luaL_checkinteger(L, 2);
    if (c < 1 || c > 4) return luaL_error(L, "image: channel %d: 1 to 4", (int)c);
    usize n = (usize)m->w * m->h;
    luaL_Buffer b;
    u8* out = (u8*)luaL_buffinitsize(L, &b, n);
    const u8* p = image_px(m) + (c - 1);
    for (usize i = 0; i < n; ++i) out[i] = p[i * 4];
    luaL_pushresultsize(&b, n);
    return 1;
}
int l_set(lua_State* L) {
    Image* m = check(L, 1);
    lua_Integer x = luaL_checkinteger(L, 2), y = luaL_checkinteger(L, 3);
    u8 c[4] = {channel(L, 4, 0), channel(L, 5, 0), channel(L, 6, 0), channel(L, 7, 255)};
    if (x >= 0 && y >= 0 && x < m->w && y < m->h) memcpy(at(m, (i32)x, (i32)y), c, 4);
    return 0;
}

int l_copy(lua_State* L) {
    Image* m = check(L, 1);
    Image* o = push_image(L, m->w, m->h);
    memcpy(image_px(o), image_px(m), (usize)m->w * m->h * 4);
    return 1;
}

// (x0, y0) to (x1, y1), x1 and y1 not included; what lies outside the picture is clear.
int l_crop(lua_State* L) {
    Image* m = check(L, 1);
    i32 x0 = (i32)luaL_checkinteger(L, 2), y0 = (i32)luaL_checkinteger(L, 3);
    i32 x1 = (i32)luaL_checkinteger(L, 4), y1 = (i32)luaL_checkinteger(L, 5);
    if (x1 < x0 || y1 < y0) return luaL_error(L, "image: crop (%d, %d, %d, %d) is not a box", x0, y0, x1, y1);
    Image* o = push_image(L, x1 - x0, y1 - y0);
    for (i32 y = y0; y < y1; ++y)
        for (i32 x = x0; x < x1; ++x)
            if (x >= 0 && y >= 0 && x < m->w && y < m->h) memcpy(at(o, x - x0, y - y0), at(m, x, y), 4);
    return 1;
}

int l_flip(lua_State* L) {
    Image* m = check(L, 1);
    const char* how = luaL_checkstring(L, 2);
    bool vertical = !strcmp(how, "vertical");
    if (!vertical && strcmp(how, "horizontal")) return luaL_error(L, "image: flip \"vertical\" or \"horizontal\", not \"%s\"", how);
    Image* o = push_image(L, m->w, m->h);
    for (i32 y = 0; y < m->h; ++y)
        for (i32 x = 0; x < m->w; ++x) memcpy(at(o, x, y), vertical ? at(m, x, m->h - 1 - y) : at(m, m->w - 1 - x, y), 4);
    return 1;
}

// A quarter turn, counter-clockwise, k times: exact.
void quarter_turns(Image* m, Image* o, i32 k) {
    for (i32 y = 0; y < o->h; ++y)
        for (i32 x = 0; x < o->w; ++x) {
            i32 sx, sy;
            if (k == 1) { sx = m->w - 1 - y; sy = x; }
            else if (k == 2) { sx = m->w - 1 - x; sy = m->h - 1 - y; }
            else { sx = y; sy = m->h - 1 - x; }
            memcpy(at(o, x, y), at(m, sx, sy), 4);
        }
}

// Pillow's bicubic sample (a = -1, its geometry.c), of premultiplied pixels; false off the
// picture.
f64 cubic(f64 v1, f64 v2, f64 v3, f64 v4, f64 d) {
    f64 p1 = v2, p2 = -v1 + v3, p3 = 2.0 * (v1 - v2) + v3 - v4, p4 = -v1 + v2 - v3 + v4;
    return p1 + d * (p2 + d * (p3 + d * p4));
}
bool sample_bicubic(const Image* m, const u8* px, f64 xin, f64 yin, u8 out[4]) {
    if (xin < 0.0 || xin >= m->w || yin < 0.0 || yin >= m->h) return false;
    xin -= 0.5;
    yin -= 0.5;
    i32 x = (i32)kv::floor(xin), y = (i32)kv::floor(yin);
    f64 dx = xin - x, dy = yin - y;
    x--;
    y--;
    auto xc = [&](i32 v) { return v < 0 ? 0 : (v >= m->w ? m->w - 1 : v); };
    auto yc = [&](i32 v) { return v < 0 ? 0 : (v >= m->h ? m->h - 1 : v); };
    for (u32 b = 0; b < 4; ++b) {
        f64 v[4];
        for (i32 j = 0; j < 4; ++j) {
            if (j > 0 && !(y + j >= 0 && y + j < m->h)) { v[j] = v[j - 1]; continue; }
            const u8* row = px + (usize)(j == 0 ? yc(y) : y + j) * (usize)m->w * 4;
            v[j] = cubic(row[xc(x) * 4 + b], row[xc(x + 1) * 4 + b], row[xc(x + 2) * 4 + b], row[xc(x + 3) * 4 + b], dx);
        }
        f64 r = cubic(v[0], v[1], v[2], v[3], dy);
        out[b] = r <= 0.0 ? 0 : (r >= 255.0 ? 255 : (u8)r);
    }
    return true;
}
bool sample_bilinear(const Image* m, const u8* px, f64 xin, f64 yin, u8 out[4]) {
    if (xin < 0.0 || xin >= m->w || yin < 0.0 || yin >= m->h) return false;
    xin -= 0.5;
    yin -= 0.5;
    i32 x = (i32)kv::floor(xin), y = (i32)kv::floor(yin);
    f64 dx = xin - x, dy = yin - y;
    auto px_at = [&](i32 xx, i32 yy, u32 b) {
        xx = xx < 0 ? 0 : (xx >= m->w ? m->w - 1 : xx);
        yy = yy < 0 ? 0 : (yy >= m->h ? m->h - 1 : yy);
        return (f64)px[((usize)yy * (usize)m->w + (usize)xx) * 4 + b];
    };
    for (u32 b = 0; b < 4; ++b) {
        f64 t = px_at(x, y, b) + dx * (px_at(x + 1, y, b) - px_at(x, y, b));
        f64 u = px_at(x, y + 1, b) + dx * (px_at(x + 1, y + 1, b) - px_at(x, y + 1, b));
        f64 r = t + dy * (u - t);
        out[b] = r <= 0.0 ? 0 : (r >= 255.0 ? 255 : (u8)r);
    }
    return true;
}
bool sample_nearest(const Image* m, const u8* px, f64 xin, f64 yin, u8 out[4]) {
    i32 x = xin < 0.0 ? -1 : (i32)xin, y = yin < 0.0 ? -1 : (i32)yin;
    if (x < 0 || y < 0 || x >= m->w || y >= m->h) return false;
    memcpy(out, px + ((usize)y * (usize)m->w + (usize)x) * 4, 4);
    return true;
}
// Python's round(v, 15), which Pillow applies to the rotation's cosine and sine.
f64 round15(f64 v) {
    char buf[64];
    format(buf, sizeof buf, "%.15f", v);
    return strtod(buf, nullptr);
}

// Degrees counter-clockwise, as Pillow's rotate: a multiple of 90 with expand (or a
// square picture) is exact; anything else is its affine transform, sampled (nearest,
// bilinear or bicubic) on premultiplied pixels, the picture grown to hold all of it when
// expand is true.
int l_rotate(lua_State* L) {
    Image* m = check(L, 1);
    f64 deg = luaL_checknumber(L, 2);
    const char* filter = luaL_optstring(L, 3, "nearest");
    bool expand = lua_toboolean(L, 4);
    bool (*sample)(const Image*, const u8*, f64, f64, u8*) = nullptr;
    if (!strcmp(filter, "nearest")) sample = sample_nearest;
    else if (!strcmp(filter, "bilinear")) sample = sample_bilinear;
    else if (!strcmp(filter, "bicubic")) sample = sample_bicubic;
    else return luaL_error(L, "image: rotate by \"nearest\", \"bilinear\" or \"bicubic\", not \"%s\"", filter);
    f64 a = kv::fmod(deg, 360.0);
    if (a < 0) a += 360.0;
    if (a == 0.0) return l_copy(L);
    if ((a == 90.0 || a == 270.0) && (expand || m->w == m->h)) {
        Image* o = push_image(L, m->h, m->w);
        quarter_turns(m, o, a == 90.0 ? 1 : 3);
        return 1;
    }
    if (a == 180.0) {
        Image* o = push_image(L, m->w, m->h);
        quarter_turns(m, o, 2);
        return 1;
    }
    f64 w = m->w, h = m->h, cx = w / 2.0, cy = h / 2.0;
    f64 r = -a * 3.14159265358979323846 / 180.0;
    f64 mt[6] = {round15(kv::cos(r)), round15(kv::sin(r)), 0.0, round15(-kv::sin(r)), round15(kv::cos(r)), 0.0};
    auto tf = [&](f64 x, f64 y, f64* ox, f64* oy) { *ox = mt[0] * x + mt[1] * y + mt[2]; *oy = mt[3] * x + mt[4] * y + mt[5]; };
    tf(-cx, -cy, &mt[2], &mt[5]);
    mt[2] += cx;
    mt[5] += cy;
    i32 nw = m->w, nh = m->h;
    if (expand) {
        f64 xs[4], ys[4];
        const f64 corners[4][2] = {{0, 0}, {w, 0}, {w, h}, {0, h}};
        for (i32 i = 0; i < 4; ++i) tf(corners[i][0], corners[i][1], &xs[i], &ys[i]);
        f64 x0 = xs[0], x1 = xs[0], y0 = ys[0], y1 = ys[0];
        for (i32 i = 1; i < 4; ++i) {
            x0 = xs[i] < x0 ? xs[i] : x0; x1 = xs[i] > x1 ? xs[i] : x1;
            y0 = ys[i] < y0 ? ys[i] : y0; y1 = ys[i] > y1 ? ys[i] : y1;
        }
        nw = (i32)(kv::ceil(x1) - kv::floor(x0));
        nh = (i32)(kv::ceil(y1) - kv::floor(y0));
        tf(-(nw - w) / 2.0, -(nh - h) / 2.0, &mt[2], &mt[5]);
    }
    // The source, premultiplied (a copy: the picture itself is unchanged).
    usize n = (usize)m->w * m->h;
    Image* src = push_image(L, m->w, m->h);
    memcpy(image_px(src), image_px(m), n * 4);
    if (sample != sample_nearest) premultiply(image_px(src), n);
    Image* o = push_image(L, nw, nh);
    // Each pixel's source as Pillow's generic transform finds it (the one its bicubic and
    // bilinear use): a0 (x + 0.5) + a1 (y + 0.5) + a2, computed afresh, so the last bits of
    // the coordinates, and of the samples, are Pillow's.
    for (i32 y = 0; y < nh; ++y)
        for (i32 x = 0; x < nw; ++x) {
            f64 xin = x + 0.5, yin = y + 0.5;
            u8 px[4];
            if (sample(src, image_px(src), mt[0] * xin + mt[1] * yin + mt[2], mt[3] * xin + mt[4] * yin + mt[5], px)) memcpy(at(o, x, y), px, 4);
        }
    if (sample != sample_nearest) unpremultiply(image_px(o), (usize)nw * nh);
    lua_remove(L, -2); // the premultiplied copy
    return 1;
}

// To w x h by "lanczos", "bicubic" (the default), "bilinear", "box" or "nearest": Pillow's
// two passes, x then y, in fixed point on premultiplied pixels; nearest is its affine.
int l_resize(lua_State* L) {
    Image* m = check(L, 1);
    i32 w = (i32)luaL_checkinteger(L, 2), h = (i32)luaL_checkinteger(L, 3);
    const char* name = luaL_optstring(L, 4, "bicubic");
    if (w <= 0 || h <= 0) return luaL_error(L, "image: resize to %dx%d", w, h);
    if (!strcmp(name, "nearest")) {
        Image* o = push_image(L, w, h);
        f64 sx = (f64)m->w / w, sy = (f64)m->h / h;
        for (i32 y = 0; y < h; ++y)
            for (i32 x = 0; x < w; ++x) {
                i32 ix = (i32)((x + 0.5) * sx), iy = (i32)((y + 0.5) * sy);
                memcpy(at(o, x, y), at(m, ix < m->w ? ix : m->w - 1, iy < m->h ? iy : m->h - 1), 4);
            }
        return 1;
    }
    Filter f;
    if (!filter_named(name, &f)) return luaL_error(L, "image: resize by \"%s\"?", name);
    usize n = (usize)m->w * m->h;
    // Scratch pictures as userdata, so nothing is lost to an error.
    Image* src = push_image(L, m->w, m->h);
    memcpy(image_px(src), image_px(m), n * 4);
    premultiply(image_px(src), n);
    Image* mid = push_image(L, w, m->h);
    Image* o = push_image(L, w, h);
    Allocator* a = mem_general();
    Coeffs cx, cy;
    coeffs(m->w, w, f, &cx, a);
    coeffs(m->h, h, f, &cy, a);
    pass_x(image_px(src), m->w, m->h, image_px(mid), w, cx);
    pass_y(image_px(mid), w, image_px(o), h, cy);
    free_coeffs(cx, w, a);
    free_coeffs(cy, h, a);
    unpremultiply(image_px(o), (usize)w * h);
    lua_replace(L, -3); // the result where src was
    lua_pop(L, 1);      // mid
    return 1;
}

// src over this picture with src's top left at (x, y): Pillow's alpha_composite, to the
// last bit (its 7-bit fixed point and divide-by-255 by shifts).
int l_composite(lua_State* L) {
    Image* d = check(L, 1);
    Image* s = check(L, 2);
    i32 ox = (i32)luaL_optinteger(L, 3, 0), oy = (i32)luaL_optinteger(L, 4, 0);
    for (i32 y = 0; y < s->h; ++y) {
        i32 dy = y + oy;
        if (dy < 0 || dy >= d->h) continue;
        for (i32 x = 0; x < s->w; ++x) {
            i32 dx = x + ox;
            if (dx < 0 || dx >= d->w) continue;
            const u8* sp = at(s, x, y);
            u8* dp = at(d, dx, dy);
            u32 blend = dp[3] * (255u - sp[3]);
            u32 outa255 = sp[3] * 255u + blend;
            if (outa255 == 0) { memset(dp, 0, 4); continue; }
            u32 coef1 = sp[3] * 255u * 255u * (1u << 7) / outa255;
            u32 coef2 = 255u * (1u << 7) - coef1;
            for (u32 c = 0; c < 3; ++c) {
                u32 t = sp[c] * coef1 + dp[c] * coef2 + (0x80u << 7);
                dp[c] = (u8)((((t >> 8) + t) >> 8) >> 7);
            }
            u32 t = outa255 + 0x80u;
            dp[3] = (u8)(((t >> 8) + t) >> 8);
        }
    }
    lua_settop(L, 1);
    return 1;
}
// src's pixels as they are, over this picture's, with src's top left at (x, y).
int l_paste(lua_State* L) {
    Image* d = check(L, 1);
    Image* s = check(L, 2);
    i32 ox = (i32)luaL_optinteger(L, 3, 0), oy = (i32)luaL_optinteger(L, 4, 0);
    for (i32 y = 0; y < s->h; ++y)
        for (i32 x = 0; x < s->w; ++x) {
            i32 dx = x + ox, dy = y + oy;
            if (dx >= 0 && dy >= 0 && dx < d->w && dy < d->h) memcpy(at(d, dx, dy), at(s, x, y), 4);
        }
    lua_settop(L, 1);
    return 1;
}

// The box of the pixels whose alpha is over `threshold` (0): x0, y0, x1, y1, the far
// edges not included; nil for a picture with none.
int l_bbox(lua_State* L) {
    Image* m = check(L, 1);
    u8 t = channel(L, 2, 0);
    i32 x0 = m->w, y0 = m->h, x1 = -1, y1 = -1;
    for (i32 y = 0; y < m->h; ++y)
        for (i32 x = 0; x < m->w; ++x)
            if (at(m, x, y)[3] > t) {
                x0 = x < x0 ? x : x0; x1 = x > x1 ? x : x1;
                y0 = y < y0 ? y : y0; y1 = y > y1 ? y : y1;
            }
    if (x1 < 0) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, x0);
    lua_pushinteger(L, y0);
    lua_pushinteger(L, x1 + 1);
    lua_pushinteger(L, y1 + 1);
    return 4;
}

// A copy with every pixel of color (r, g, b) made clear (0, 0, 0, 0): a sheet's ground.
int l_key(lua_State* L) {
    Image* m = check(L, 1);
    u8 k[3] = {channel(L, 2, 0), channel(L, 3, 0), channel(L, 4, 0)};
    Image* o = push_image(L, m->w, m->h);
    usize n = (usize)m->w * m->h;
    const u8* p = image_px(m);
    u8* q = image_px(o);
    for (usize i = 0; i < n; ++i)
        if (memcmp(p + i * 4, k, 3) != 0) memcpy(q + i * 4, p + i * 4, 4);
    return 1;
}

// A copy with each channel times its factor, truncated (Pillow's point(lambda v: int(v *
// k))): r, g, b, a; a factor left out is 1.
int l_scale(lua_State* L) {
    Image* m = check(L, 1);
    f64 k[4] = {luaL_optnumber(L, 2, 1.0), luaL_optnumber(L, 3, 1.0), luaL_optnumber(L, 4, 1.0), luaL_optnumber(L, 5, 1.0)};
    u8 lut[4][256];
    for (u32 c = 0; c < 4; ++c)
        for (u32 v = 0; v < 256; ++v) lut[c][v] = clamp8((i64)kv::floor((f64)v * k[c]));
    Image* o = push_image(L, m->w, m->h);
    usize n = (usize)m->w * m->h * 4;
    const u8* p = image_px(m);
    u8* q = image_px(o);
    for (usize i = 0; i < n; ++i) q[i] = lut[i & 3][p[i]];
    return 1;
}

constexpr luaL_Reg k_functions[] = {
    {"new", l_new}, {"load", l_load}, {"decode", l_decode}, {nullptr, nullptr},
};
constexpr luaL_Reg k_methods[] = {
    {"get", l_get},       {"alpha", l_alpha},   {"channel", l_channel},   {"set", l_set},           {"copy", l_copy},     {"crop", l_crop},
    {"flip", l_flip},     {"rotate", l_rotate}, {"resize", l_resize},     {"composite", l_composite},
    {"paste", l_paste},   {"bbox", l_bbox},     {"key", l_key},           {"scale", l_scale},   {"encode", l_encode},
    {"save", l_save},     {nullptr, nullptr},
};

} // namespace

Image* kvscript_check_image(lua_State* L, int index) { return check(L, index); }

void kvscript_open_image(lua_State* L) {
    luaL_newmetatable(L, kImageMeta);
    lua_pushcfunction(L, l_index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, l_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_newtable(L);
    luaL_setfuncs(L, k_methods, 0);
    lua_setfield(L, -2, "methods");
    lua_pop(L, 1);
    lua_newtable(L);
    luaL_setfuncs(L, k_functions, 0);
}

Image* kvscript_push_image(lua_State* L, i32 w, i32 h) { return push_image(L, w, h); }

} // namespace kv
