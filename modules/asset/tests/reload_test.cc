// Reload (korvid/asset/reload.h), with no renderer: a shape and a blob each read again by path.
// Changed, they rebind under the same handle and say so; unchanged, nothing happens; broken,
// they keep what they were; a failed load loads when its file appears. Textures and materials
// reload in asset_render's tests.
#include "korvid/asset/blob.h"
#include "korvid/asset/module.h"
#include "korvid/asset/reload.h"
#include "korvid/asset/shape.h"
#include "korvid/core/array.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include "korvid/patherizer/patherizer.h"
#include "korvid/platform/io.h"
#include "korvid/platform/module.h"
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
    KV_CHECK(c, write_text("doc.txt", "one"));
    KV_CHECK(c, write_shape("tri.kshape", 10));

    const ModuleDesc* const list[] = {&platform_module, &asset_module, &k_listener};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    KV_CHECK(c, platform_io(e)->mount_dir(g_dir));

    Handle blob = asset_load_blob(e, "doc.txt");
    Handle shape = asset_load_shape(e, "tri.svg");
    KV_CHECK(c, until(e, [&] { return g_heard.ready >= 2; }));
    const BlobAsset* b = asset_blob(e, blob);
    const ShapeAsset* sh = asset_shape(e, shape);
    KV_CHECK(c, b && b->state == AssetState::Ready && b->size == 3 && !strcmp((const char*)b->data, "one") && b->generation == 0 && b->hash != 0);
    KV_CHECK(c, sh && sh->state == AssetState::Ready);
    u32 shape_points = sh->shape.points.len;

    // Nothing loaded from a path: nothing asked.
    KV_CHECK(c, asset_reload(e, "nothing/here.png") == 0);

    // Unchanged: read, compared, nothing said, nothing changed.
    g_heard = Heard{};
    KV_CHECK(c, asset_reload(e, "doc.txt") == 1);
    KV_CHECK(c, asset_reload(e, "tri.svg") == 1);
    quiet(e);
    KV_CHECK(c, g_heard.reloaded == 0 && g_heard.ready == 0 && g_heard.failed == 0 && g_heard.failed_reload == 0);
    KV_CHECK(c, b->generation == 0 && sh->generation == 0 && b->io.is_null());

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

    // A blob whose file went: kept, Ready throughout.
    g_heard = Heard{};
    char gone[700];
    format(gone, sizeof gone, "%s/doc.txt", g_dir);
    KV_CHECK(c, io_remove(gone));
    KV_CHECK(c, asset_reload(e, "doc.txt") == 1);
    KV_CHECK(c, until(e, [&] { return g_heard.failed_reload == 1; }));
    KV_CHECK(c, b->state == AssetState::Ready && b->size == 13 && b->generation == 1);

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
    KV_CHECK(c, asked >= 4); // doc.txt, the shape, late.txt, race.txt
    quiet(e);
    KV_CHECK(c, g_heard.reloaded == 0);

    engine_shutdown(e);
    io_remove_tree(g_dir);
}
