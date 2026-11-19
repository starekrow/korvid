// asset_render: the asset module's glyph pages, drawn (AssetFontPages, korvid/asset/font.h). A
// page gets a renderer texture and a material, alpha-blended and linearly sampled because
// glyphs are drawn at any scale; its changes go up by the rectangle that changed.
#include "korvid/asset/font.h"
#include "korvid/core/format.h"
#include "korvid/core/engine.h"
#include "korvid/render/renderer.h"
#include "asset_render_internal.h"

namespace kv {

namespace {

bool page_create(void* user, const char* name, Handle* texture, Handle* material) {
    Renderer* r = renderer_get(*(Engine*)user);
    if (!r) return false;
    TextureDesc td;
    td.format = TextureFormat::RGBA8_UNORM;
    td.name   = name;
    *texture = renderer_texture_create(r, td);
    MaterialDesc md;
    md.texture = *texture;
    md.blend   = BlendMode::Alpha;
    md.filter  = TextureFilter::Linear;
    char mname[KV_ASSET_PATH_MAX + 56];
    format(mname, sizeof mname, "font:%s", name);
    md.name    = mname;
    *material = renderer_material_create(r, md);
    return true;
}

bool page_upload(void* user, Handle texture, const u8* rgba, u32 size, bool first, u32 x, u32 y, u32 w, u32 h) {
    Renderer* r = renderer_get(*(Engine*)user);
    if (!r) return false;
    if (first) return renderer_texture_upload(r, texture, size, size, rgba, (usize)size * size * 4);
    renderer_texture_update(r, texture, rgba, size, size, x, y, w, h);
    return true;
}

void page_destroy(void* user, Handle texture, Handle material) {
    Renderer* r = renderer_get(*(Engine*)user);
    if (!r) return;
    renderer_material_destroy(r, material);
    renderer_texture_destroy(r, texture);
}

} // namespace

void asset_render_fonts_init(Engine& e) {
    AssetFontPages pages;
    pages.create  = page_create;
    pages.upload  = page_upload;
    pages.destroy = page_destroy;
    pages.user    = &e;
    asset_font_pages(e, &pages);
}

void asset_render_fonts_shutdown(Engine& e) { asset_font_pages(e, nullptr); }

} // namespace kv
