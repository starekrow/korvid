// Reload (korvid/asset/reload.h), on the CPU renderer made headless: a texture, a material, a
// shape and a blob each read again by path. Changed, they rebind under the same handle and say
// so; unchanged, nothing happens; broken, they keep what they were; a texture may change size;
// a material may name another texture; a failed load loads when its file appears.
#include "korvid/asset/blob.h"
#include "korvid/asset/material.h"
#include "korvid/asset/module.h"
#include "korvid/asset/reload.h"
#include "korvid/asset/shape.h"
#include "korvid/asset/texture.h"
#include "korvid/core/array.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include "korvid/image/png.h"
#include "korvid/patherizer/patherizer.h"
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
    return io_write_now(p, data, size);
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

bool write_shape(const char* rel, f32 size) {
    PathShape s;
    s.init(mem_general());
    s.view[2] = s.view[3] = 20;
    PathBuilder b(s);
    b.move_to(0, 0); b.line_to(size, 0); b.line_to(size, size); b.close();
    b.fill(s.add_current());
    Array<u8> out;
    bool ok = patherizer_write_kshape(s, out) && write_file(rel, out.data, out.len);
    out.free();
    s.free();
    return ok;
}

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

void test_reload(TestCtx& c) {
    c.begin("reload");
    KV_CHECK(c, io_make_temp_dir("kv-reload-test", g_dir, sizeof g_dir));
    KV_CHECK(c, write_png("a.png", 2, 2, 255, 0, 0));
    KV_CHECK(c, write_png("b.png", 4, 4, 0, 255, 0));
    KV_CHECK(c, write_text("m.material.json", "{ \"texture\": \"a.png\", \"blend\": \"alpha\" }"));
    KV_CHECK(c, write_text("doc.txt", "one"));
    KV_CHECK(c, write_shape("tri.kshape", 10));

    const ModuleDesc* const list[] = {&platform_module, &renderer_cpu_module, &asset_module, &k_listener};
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
    Handle blob = asset_load_blob(e, "doc.txt");
    Handle shape = asset_load_shape(e, "tri.svg");
    KV_CHECK(c, until(e, [&] { return g_heard.ready >= 4; }));
    const TextureAsset* t = asset_texture(e, tex);
    const MaterialAsset* m = asset_material(e, mat);
    const BlobAsset* b = asset_blob(e, blob);
    const ShapeAsset* sh = asset_shape(e, shape);
    KV_CHECK(c, t && t->state == AssetState::Ready && t->width == 2 && t->generation == 0 && t->hash != 0);
    KV_CHECK(c, m && m->state == AssetState::Ready && m->texture_asset == tex);
    KV_CHECK(c, b && b->state == AssetState::Ready && b->size == 3 && !strcmp((const char*)b->data, "one"));
    KV_CHECK(c, sh && sh->state == AssetState::Ready);
    Handle renderer_texture = t->texture;
    u32 shape_points = sh->shape.points.len;

    // Nothing loaded from a path: nothing asked.
    KV_CHECK(c, asset_reload(e, "nothing/here.png") == 0);

    // Unchanged: read, compared, nothing said, nothing changed.
    g_heard = Heard{};
    KV_CHECK(c, asset_reload(e, "a.png") == 1);
    KV_CHECK(c, asset_reload(e, "doc.txt") == 1);
    quiet(e);
    KV_CHECK(c, g_heard.reloaded == 0 && g_heard.ready == 0 && g_heard.failed == 0 && g_heard.failed_reload == 0);
    KV_CHECK(c, t->generation == 0 && b->generation == 0 && t->io.is_null());

    // A texture that changed, and grew: the same handle and renderer texture, the new size.
    KV_CHECK(c, write_png("a.png", 8, 4, 0, 0, 255));
    KV_CHECK(c, asset_reload(e, "a.png") == 1);
    KV_CHECK(c, until(e, [&] { return g_heard.reloaded == 1; }));
    KV_CHECK(c, g_heard.last == tex && asset_texture(e, tex) == t && t->texture == renderer_texture);
    KV_CHECK(c, t->state == AssetState::Ready && t->width == 8 && t->height == 4 && t->generation == 1);
    KV_CHECK(c, m->state == AssetState::Ready && m->generation == 0); // the material draws it; it did not change

    // A blob: new bytes, the old ones gone.
    g_heard = Heard{};
    KV_CHECK(c, write_text("doc.txt", "two and three"));
    KV_CHECK(c, asset_reload(e, "doc.txt") == 1);
    KV_CHECK(c, until(e, [&] { return g_heard.reloaded == 1; }));
    KV_CHECK(c, b->size == 13 && !strcmp((const char*)b->data, "two and three") && b->generation == 1 && b->state == AssetState::Ready);

    // A shape, by its source's name: new paths.
    g_heard = Heard{};
    KV_CHECK(c, write_shape("tri.kshape", 16));
    KV_CHECK(c, asset_reload(e, "tri.svg") == 1);
    KV_CHECK(c, until(e, [&] { return g_heard.reloaded == 1; }));
    KV_CHECK(c, g_heard.last == shape && sh->generation == 1 && sh->shape.points.len == shape_points && sh->shape.points[2] == 16.0f);

    // A shape that no longer reads keeps the one it had.
    g_heard = Heard{};
    KV_CHECK(c, write_text("tri.kshape", "not a shape"));
    KV_CHECK(c, asset_reload(e, "tri.kshape") == 1);
    KV_CHECK(c, until(e, [&] { return g_heard.failed_reload == 1; }));
    KV_CHECK(c, g_heard.reloaded == 0 && sh->state == AssetState::Ready && sh->generation == 1 && sh->shape.points[2] == 16.0f);

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

    // A load that failed (no file), asked again once the file is there: it loads.
    g_heard = Heard{};
    Handle late = asset_load_blob(e, "late.txt");
    KV_CHECK(c, until(e, [&] { return g_heard.failed == 1; }));
    KV_CHECK(c, asset_blob(e, late)->state == AssetState::Failed);
    KV_CHECK(c, write_text("late.txt", "here now"));
    KV_CHECK(c, asset_reload(e, "late.txt") == 1);
    KV_CHECK(c, until(e, [&] { return g_heard.ready == 1; }));
    KV_CHECK(c, g_heard.last == late && asset_blob(e, late)->state == AssetState::Ready && asset_blob(e, late)->size == 8);

    // Asked while the first read is still out: read once more after it lands, and the second
    // read sees the new bytes.
    g_heard = Heard{};
    KV_CHECK(c, write_text("race.txt", "first"));
    Handle race = asset_load_blob(e, "race.txt");
    KV_CHECK(c, write_text("race.txt", "second!"));
    KV_CHECK(c, asset_reload(e, "race.txt") == 1);
    KV_CHECK(c, asset_blob(e, race)->again || asset_blob(e, race)->state == AssetState::Ready);
    KV_CHECK(c, until(e, [&] { const BlobAsset* r = asset_blob(e, race); return r->state == AssetState::Ready && r->size == 7 && r->io.is_null(); }));

    // Every asset at once: all read, nothing changed since.
    g_heard = Heard{};
    u32 asked = asset_reload_all(e);
    KV_CHECK(c, asked >= 6);
    quiet(e);
    KV_CHECK(c, g_heard.reloaded == 0);

    engine_shutdown(e);
    io_remove_tree(g_dir);
}
