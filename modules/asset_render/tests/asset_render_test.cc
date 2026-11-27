// asset_render on the CPU renderer made headless. Reload (korvid/asset/reload.h) of a texture
// and a material, which are asset_render's kinds reached through the asset module: changed,
// they rebind under the same handle and say so; unchanged, nothing happens; broken, they keep
// what they were; a texture may change size; a material may name another texture. Then what
// else the asset module reaches through this module: the scopes' `scope load` by suffix, and a
// font's glyph pages, which get renderer textures here and none without it.
#include "korvid/asset/font.h"
#include "korvid/asset/module.h"
#include "korvid/asset/reload.h"
#include "korvid/asset/scope.h"
#include "korvid/asset_render/material.h"
#include "korvid/asset_render/module.h"
#include "korvid/asset_render/texture.h"
#include "korvid/cli/module.h"
#include "korvid/core/array.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include "korvid/image/png.h"
#include "korvid/platform/io.h"
#include "korvid/platform/module.h"
#include "korvid/render/renderer.h"
#include "korvid/renderer-cpu/module.h"
#include <string.h>

namespace {

using namespace kv;

// What the asset events said since the last look.
struct Heard {
    u32 ready = 0, failed = 0, failed_reload = 0, reloaded = 0;
    Handle last;
};
Heard g_heard;

void on_ready(TickContext&, Span<const AssetReady> evs) { for (const AssetReady& r : evs) { ++g_heard.ready; g_heard.last = r.asset; } }
void on_failed(TickContext&, Span<const AssetFailed> evs) {
    for (const AssetFailed& f : evs) { if (f.reload) ++g_heard.failed_reload; else ++g_heard.failed; g_heard.last = f.asset; }
}
void on_reloaded(TickContext&, Span<const AssetReloaded> evs) { for (const AssetReloaded& r : evs) { ++g_heard.reloaded; g_heard.last = r.asset; } }

constinit const HandlerDesc k_handlers[] = {
    {.name = ".ready", KV_ON(AssetReady, on_ready), .phase = Phase::PostSim},
    {.name = ".failed", KV_ON(AssetFailed, on_failed), .phase = Phase::PostSim},
    {.name = ".reloaded", KV_ON(AssetReloaded, on_reloaded), .phase = Phase::PostSim},
};
const ModuleDesc k_listener = {.name = "reload_listener", .handlers = k_handlers};

char g_dir[400];

bool write_file(const char* rel, const void* data, usize size) {
    char p[700];
    format(p, sizeof p, "%s/%s", g_dir, rel);
    return io_write_file(p, data, size);
}
bool write_text(const char* rel, const char* text) { return write_file(rel, text, strlen(text)); }

bool write_png(const char* rel, u32 w, u32 h, u8 r, u8 g, u8 b) {
    Array<u8> px, out;
    px.resize_fill((u32)(w * h * 4), 0);
    for (u32 i = 0; i < w * h; ++i) { px[i * 4] = r; px[i * 4 + 1] = g; px[i * 4 + 2] = b; px[i * 4 + 3] = 255; }
    const char* why = nullptr;
    bool ok = png_encode(px.data, w, h, 4, out, &why) && write_file(rel, out.data, out.len);
    px.free();
    out.free();
    return ok;
}

struct Lines { u32 n = 0; char text[8][160]; };
void collect(void* user, const char* line) { Lines& l = *(Lines*)user; if (l.n < 8) format(l.text[l.n++], 160, "%s", line); }

void frame(Engine& e) { platform_pump(e); engine_frame(e, 1.0 / 60.0); }

// Frames until `done`, or about two seconds.
template <class F> bool until(Engine& e, F done) {
    for (u32 i = 0; i < 2000; ++i) {
        frame(e);
        if (done()) return true;
        time_sleep(0.001);
    }
    return false;
}

// A few frames more, for what should not happen.
void quiet(Engine& e) { for (u32 i = 0; i < 20; ++i) { frame(e); time_sleep(0.001); } }

} // namespace

void test_asset_render(TestCtx& c) {
    c.begin("asset_render");
    KV_CHECK(c, io_make_temp_dir("kv-asset-render-test", g_dir, sizeof g_dir));
    KV_CHECK(c, write_png("a.png", 2, 2, 255, 0, 0));
    KV_CHECK(c, write_png("b.png", 4, 4, 0, 255, 0));
    KV_CHECK(c, write_text("m.material.json", "{ \"texture\": \"a.png\", \"blend\": \"alpha\" }"));
    char from[700], to[700];
    test_source_path(from, sizeof from, "modules/dejavu/assets/DejaVuSans.ttf"); // module.lua's test_files
    format(to, sizeof to, "%s/DejaVuSans.ttf", g_dir);
    KV_CHECK(c, io_copy_file(from, to));

    const ModuleDesc* const list[] = {&platform_module, &cli_module, &renderer_cpu_module, &asset_module, &asset_render_module, &k_listener};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    KV_CHECK(c, platform_io(e)->mount_dir(g_dir));
    RendererDesc rd;
    rd.backend = RendererBackend::Cpu;
    rd.width = 16;
    rd.height = 16;
    KV_CHECK(c, renderer_create(e, rd) != nullptr);

    Handle tex = asset_load_texture(e, "a.png");
    Handle mat = asset_load_material(e, "m.material.json");
    KV_CHECK(c, until(e, [&] { return g_heard.ready >= 2; }));
    const TextureAsset* t = asset_texture(e, tex);
    const MaterialAsset* m = asset_material(e, mat);
    KV_CHECK(c, t && t->state == AssetState::Ready && t->width == 2 && t->generation == 0 && t->hash != 0);
    KV_CHECK(c, m && m->state == AssetState::Ready && m->texture_asset == tex);
    Handle renderer_texture = t->texture;

    // Nothing loaded from a path: nothing asked.
    KV_CHECK(c, asset_reload(e, "nothing/here.png") == 0);

    // Unchanged: read, compared, nothing said, nothing changed.
    g_heard = Heard{};
    KV_CHECK(c, asset_reload(e, "a.png") == 1);
    KV_CHECK(c, asset_reload(e, "m.material.json") == 1);
    quiet(e);
    KV_CHECK(c, g_heard.reloaded == 0 && g_heard.ready == 0 && g_heard.failed == 0 && g_heard.failed_reload == 0);
    KV_CHECK(c, t->generation == 0 && m->generation == 0 && t->io.is_null());

    // A texture that changed, and grew: the same handle and renderer texture, the new size.
    KV_CHECK(c, write_png("a.png", 8, 4, 0, 0, 255));
    KV_CHECK(c, asset_reload(e, "a.png") == 1);
    KV_CHECK(c, until(e, [&] { return g_heard.reloaded == 1; }));
    KV_CHECK(c, g_heard.last == tex && asset_texture(e, tex) == t && t->texture == renderer_texture);
    KV_CHECK(c, t->state == AssetState::Ready && t->width == 8 && t->height == 4 && t->generation == 1);
    KV_CHECK(c, m->state == AssetState::Ready && m->generation == 0); // the material draws it; it did not change

    // A texture whose file broke, then went: kept each time, Ready throughout.
    g_heard = Heard{};
    KV_CHECK(c, write_text("a.png", "not a png"));
    KV_CHECK(c, asset_reload(e, "a.png") == 1);
    KV_CHECK(c, until(e, [&] { return g_heard.failed_reload == 1; }));
    char gone[700];
    format(gone, sizeof gone, "%s/a.png", g_dir);
    KV_CHECK(c, io_remove(gone));
    KV_CHECK(c, asset_reload(e, "a.png") == 1);
    KV_CHECK(c, until(e, [&] { return g_heard.failed_reload == 2; }));
    KV_CHECK(c, t->state == AssetState::Ready && t->width == 8 && t->generation == 1);

    // A material that names another texture: the row looks at it, the old texture is let go.
    g_heard = Heard{};
    KV_CHECK(c, write_text("m.material.json", "{ \"texture\": \"b.png\", \"blend\": \"additive\" }"));
    KV_CHECK(c, asset_reload(e, "m.material.json") == 1);
    KV_CHECK(c, until(e, [&] { return g_heard.reloaded == 1 && g_heard.ready == 1; })); // the material, then b.png
    KV_CHECK(c, m->generation == 1 && m->desc.blend == BlendMode::Additive && !strcmp(m->texture_path, "b.png"));
    const TextureAsset* tb = asset_texture(e, m->texture_asset);
    KV_CHECK(c, tb && tb->state == AssetState::Ready && tb->width == 4 && m->desc.texture == tb->texture);
    // a.png is still held by the global scope (it was loaded on its own): not retired.
    KV_CHECK(c, asset_texture(e, tex) == t);

    // A material file that does not parse keeps its look.
    g_heard = Heard{};
    KV_CHECK(c, write_text("m.material.json", "{ \"texture\": "));
    KV_CHECK(c, asset_reload(e, "m.material.json") == 1);
    KV_CHECK(c, until(e, [&] { return g_heard.failed_reload == 1; }));
    KV_CHECK(c, m->state == AssetState::Ready && m->generation == 1 && m->desc.blend == BlendMode::Additive);

    // Every asset at once: all read, nothing changed since.
    g_heard = Heard{};
    u32 asked = asset_reload_all(e);
    KV_CHECK(c, asked >= 3); // a.png, b.png, the material
    quiet(e);
    KV_CHECK(c, g_heard.reloaded == 0);

    // The CLI's `scope load` finds the kinds by the suffixes they registered.
    Lines out;
    KV_CHECK(c, cli_run(e, "scope create menu", collect, &out));
    out.n = 0;
    KV_CHECK(c, cli_run(e, "scope load menu b.png", collect, &out) && out.n == 1 && strstr(out.text[0], "b.png in 'menu'"));
    out.n = 0;
    KV_CHECK(c, cli_run(e, "scope load menu m.material.json", collect, &out) && out.n == 1 && strstr(out.text[0], "in 'menu'"));
    KV_CHECK(c, !cli_run(e, "scope load menu x.unknown", collect, &out));
    // Released, what the scope alone held retires; b.png is the material's too, and stays.
    KV_CHECK(c, cli_run(e, "scope release menu", collect, &out));
    quiet(e);
    KV_CHECK(c, asset_texture(e, m->texture_asset) == tb);

    // A font's pages get a renderer texture and material here, and the glyphs made are uploaded.
    Handle font = asset_load_font(e, "DejaVuSans.ttf", 16.0f);
    KV_CHECK(c, until(e, [&] { const FontAsset* f = asset_font(e, font); return f && f->state == AssetState::Ready; }));
    const FontAsset* f = asset_font(e, font);
    const Glyph* ga = font_glyph(*f, 'A');
    KV_CHECK(c, !f->texture.is_null() && !f->material.is_null() && ga && ga->texture == f->texture);
    quiet(e); // asset.fonts_flush uploads the page through asset_render
    KV_CHECK(c, renderer_texture_slot(renderer_get(e), f->texture) != 0); // a live renderer texture

    // A texture retired with its scope: the renderer texture goes with it.
    Handle lvl = asset_scope_create(e, "level");
    Handle lone = asset_load_texture(e, "a.png", lvl);
    KV_CHECK(c, lone == tex && asset_refs(e, tex) == 2);
    KV_CHECK(c, asset_scope_release(e, lvl));
    quiet(e);
    KV_CHECK(c, asset_texture(e, tex) == t && asset_refs(e, tex) == 1); // the global scope still holds it

    engine_shutdown(e);
    io_remove_tree(g_dir);
}
