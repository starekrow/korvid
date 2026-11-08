#include "korvid/asset/font.h"
#include "korvid/asset/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/time.h"
#include "korvid/platform/io.h"
#include "korvid/render/renderer.h"
#include "asset_internal.h"
#include "vendor/stb_truetype.h"
#include <stdio.h>
#include <string.h>

namespace kv {

namespace {

void* font_resolve(kv_handle h, void* ctx) { return ((AssetState_*)ctx)->fonts.get(Handle{h}); }
int font_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    FontAsset* f = ((AssetState_*)ctx)->fonts.get(Handle{h});
    if (!f) return snprintf(out, cap, "stale");
    const char* st = f->state == AssetState::Ready ? "ready" : f->state == AssetState::Failed ? "failed" : "pending";
    return snprintf(out, cap, "'%s' %gpx %s atlas %ux%u", f->path, f->px, st, f->atlas_width, f->atlas_height);
}

void fail_font(Engine& e, AssetState_& s, Handle h, FontAsset& f, AssetError err, const char* detail) {
    f.state = AssetState::Failed;
    f.error = err;
    f.io    = Handle{};
    ++s.fonts_failed;
    KV_WARN("asset", "font '%s' %gpx: %s%s%s", f.path, f.px, asset_error_name(err), detail ? ": " : "", detail ? detail : "");
    e.events.emit(AssetFailed{h, err});
}

// Rasterizes every covered code point at f.px into the smallest atlas of a fixed ladder
// that holds them, fills the glyph and kerning tables, and uploads the atlas. False with
// the reason on a file stb_truetype cannot read.
bool build_font(Engine& e, AssetState_& s, FontAsset& f, const u8* data, usize size, const char** why) {
    KV_UNUSED(size);
    f64 t0 = time_now();
    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, data, stbtt_GetFontOffsetForIndex(data, 0))) { *why = "not a TrueType font"; return false; }
    f32 scale = stbtt_ScaleForPixelHeight(&info, f.px);
    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);

    MemScope scope;
    stbtt_packedchar* packed = (stbtt_packedchar*)alloc_in(mem_scratch(), sizeof(stbtt_packedchar) * KV_FONT_COUNT);
    static const u32 k_ladder[][2] = {{256, 128}, {256, 256}, {512, 256}, {512, 512}, {1024, 512}, {1024, 1024}, {2048, 1024}, {2048, 2048}};
    u8* pixels = nullptr;
    u32 w = 0, h = 0;
    for (const u32* dims : k_ladder) {
        w = dims[0]; h = dims[1];
        pixels = (u8*)alloc_in(mem_scratch(), (usize)w * h);
        stbtt_pack_context pc;
        if (!stbtt_PackBegin(&pc, pixels, (int)w, (int)h, 0, 1, nullptr)) { *why = "packer init failed"; return false; }
        stbtt_PackSetOversampling(&pc, 1, 1);
        int ok = stbtt_PackFontRange(&pc, data, 0, f.px, (int)KV_FONT_FIRST, (int)KV_FONT_COUNT, packed);
        stbtt_PackEnd(&pc);
        if (ok) break;
        pixels = nullptr;
    }
    if (!pixels) { *why = "glyphs do not fit a 2048x2048 atlas"; return false; }

    // White with coverage in alpha, so the material's tint (and the sprite's) is the color.
    u8* rgba = (u8*)alloc_in(mem_scratch(), (usize)w * h * 4);
    for (usize i = 0; i < (usize)w * h; ++i) { rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255; rgba[i * 4 + 3] = pixels[i]; }
    Renderer* r = renderer_get(e);
    if (!r || !renderer_texture_upload(r, f.texture, w, h, rgba, (usize)w * h * 4)) { *why = "upload failed"; return false; }

    f.glyphs = (Glyph*)alloc_in(s.general, sizeof(Glyph) * KV_FONT_COUNT);
    for (u32 i = 0; i < KV_FONT_COUNT; ++i) {
        const stbtt_packedchar& p = packed[i];
        Glyph& g = f.glyphs[i];
        f32 gw = (f32)(p.x1 - p.x0), gh = (f32)(p.y1 - p.y0);
        g.uv      = rect2{(f32)p.x0 / (f32)w, (f32)p.y0 / (f32)h, gw / (f32)w, gh / (f32)h};
        g.size    = vec2{gw, gh};
        g.offset  = vec2{p.xoff + gw * 0.5f, -(p.yoff + gh * 0.5f)}; // stb's y is down from the baseline
        g.advance = p.xadvance;
    }
    // Pair kerning, kept only if the font has any.
    f32* kern = (f32*)alloc_in(s.general, sizeof(f32) * KV_FONT_COUNT * KV_FONT_COUNT);
    bool any = false;
    for (u32 a = 0; a < KV_FONT_COUNT; ++a)
        for (u32 b = 0; b < KV_FONT_COUNT; ++b) {
            f32 k = (f32)stbtt_GetCodepointKernAdvance(&info, (int)(a + KV_FONT_FIRST), (int)(b + KV_FONT_FIRST)) * scale;
            kern[a * KV_FONT_COUNT + b] = k;
            any |= k != 0.0f;
        }
    if (any) f.kern = kern;
    else free_in(s.general, kern, sizeof(f32) * KV_FONT_COUNT * KV_FONT_COUNT);

    f.ascent       = (f32)asc * scale;
    f.descent      = (f32)desc * scale;
    f.line_height  = (f32)(asc - desc + gap) * scale;
    f.atlas_width  = w;
    f.atlas_height = h;
    f.build_seconds = time_now() - t0;
    return true;
}

} // namespace

bool asset_fonts_init(Engine&, AssetState_& s, ModuleInstance& self) {
    s.font_by_key.init(&self.general);
    kv_handle_kind_desc desc{};
    desc.name = "asset.font"; desc.type_key = type_key<FontAsset>(); desc.ctx = &s;
    desc.resolve = font_resolve; desc.describe = font_describe; desc.flags = KV_HANDLE_GENERATIONAL;
    s.font_kind = handle_kind_register(desc);
    if (!s.font_kind) return false;
    s.fonts.use_kind(s.font_kind);
    return s.fonts.init(256, self.general.tag);
}

void asset_fonts_shutdown(Engine& e, AssetState_& s) {
    IoQueue* io = platform_io(e);
    for (u32 i = 0; i < s.fonts.len; ++i) {
        if (!s.fonts.is_alive(i)) continue;
        FontAsset& f = s.fonts.items[i];
        if (io && !f.io.is_null()) io->cancel(f.io);
        free_in(s.general, f.glyphs, sizeof(Glyph) * KV_FONT_COUNT);
        free_in(s.general, f.kern, sizeof(f32) * KV_FONT_COUNT * KV_FONT_COUNT);
    }
    KV_INFO("asset", "%u fonts loaded, %u failed", s.fonts_loaded, s.fonts_failed);
    kv_handle_kind_unregister(s.font_kind);
    s.font_by_key.free();
    s.fonts.free();
}

// Phase PreSim, main thread, after the texture pump: a font file that arrived is built.
void asset_fonts_pump(TickContext& ctx) {
    AssetState_& s = ctx.state_as<AssetState_>();
    Engine& e = *ctx.engine;
    IoQueue* io = platform_io(e);
    if (!io) return;
    for (u32 i = 0; i < io->completions().len;) {
        IoCompletion c = io->completions()[i];
        if (!asset_is_kind(s, c.user, s.font_kind)) { ++i; continue; }
        Handle h{c.user};
        FontAsset* f = s.fonts.get(h);
        if (!f || f->io != c.request) { ++i; continue; }
        io->reap(c.request);
        f->io = Handle{};
        if (c.status != IoStatus::Done) {
            fail_font(e, s, h, *f, c.status == IoStatus::Failed && c.error == 2 ? AssetError::NotFound : AssetError::Read, nullptr);
            continue;
        }
        const char* why = nullptr;
        bool ok = build_font(e, s, *f, (const u8*)c.data, c.size, &why);
        free_in(s.general, c.data, c.size);
        if (!ok) { fail_font(e, s, h, *f, AssetError::Decode, why); continue; }
        f->state = AssetState::Ready;
        ++s.fonts_loaded;
        KV_INFO("asset", "font '%s' %gpx ready: atlas %ux%u, line %.1f, %s kerning, built in %.1f ms", f->path, f->px,
                f->atlas_width, f->atlas_height, f->line_height, f->kern ? "with" : "no", f->build_seconds * 1e3);
        e.events.emit(AssetReady{h});
    }
}

Handle asset_load_font(Engine& e, const char* path, f32 px) {
    AssetState_* s = asset_state(e);
    Renderer* r = renderer_get(e);
    IoQueue* io = platform_io(e);
    if (!s || !r || !io || !path) { KV_ERROR("asset", "cannot load font '%s': asset, renderer and platform must be up", path ? path : "?"); return Handle{}; }
    if (strlen(path) >= KV_ASSET_PATH_MAX) { KV_ERROR("asset", "path too long: '%s'", path); return Handle{}; }
    if (!(px >= 4.0f && px <= 256.0f)) { KV_ERROR("asset", "font '%s': size %g is outside [4, 256]", path, px); return Handle{}; }

    char key[KV_ASSET_PATH_MAX + 16];
    snprintf(key, sizeof key, "%s@%g", path, px);
    Name name = name_intern(key);
    if (const Handle* existing = s->font_by_key.find(name)) return *existing;

    FontAsset f{};
    strncpy(f.path, path, sizeof f.path - 1);
    f.px    = px;
    f.state = AssetState::Pending;
    TextureDesc td;
    td.format = TextureFormat::RGBA8_UNORM;
    td.name   = key;
    f.texture = renderer_texture_create(r, td);
    if (f.texture.is_null()) return Handle{};
    MaterialDesc md;
    md.texture = f.texture;
    md.blend   = BlendMode::Alpha;
    f.material = renderer_material_create(r, md);
    if (f.material.is_null()) { renderer_texture_destroy(r, f.texture); return Handle{}; }
    Handle h = s->fonts.create(f);
    if (h.is_null()) { renderer_material_destroy(r, f.material); renderer_texture_destroy(r, f.texture); return Handle{}; }
    s->font_by_key.set(name, h);

    char full[512];
    snprintf(full, sizeof full, "%sassets/%s", platform_base_path(e), path);
    IoRequest req;
    req.path  = full;
    req.alloc = s->general;
    req.user  = h.bits;
    Handle iq = io->read_file(req);
    FontAsset* rec = s->fonts.get(h);
    if (iq.is_null()) { fail_font(e, *s, h, *rec, AssetError::Read, "could not submit"); return h; }
    rec->io = iq;
    return h;
}

const FontAsset* asset_font(Engine& e, Handle asset) {
    AssetState_* s = asset_state(e);
    return s ? s->fonts.get(asset) : nullptr;
}

f32 font_measure(const FontAsset& f, const char* str, f32 scale) {
    if (f.state != AssetState::Ready || !str) return 0.0f;
    f32 x = 0.0f;
    for (const u8* p = (const u8*)str; *p; ++p) {
        if (const Glyph* g = font_glyph(f, *p)) x += g->advance;
        if (p[1]) x += font_kern(f, *p, p[1]);
    }
    return x * scale;
}

} // namespace kv
