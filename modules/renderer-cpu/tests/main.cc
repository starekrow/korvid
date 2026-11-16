// renderer-cpu without a window (docs/PRDs/CPU-Renderer-PRD.md, milestone 1): made headless,
// a frame clears its target to the view's color as sRGB bytes, a screenshot reads it, and
// it ends cleanly. Called through the backend's namespace: render's dispatcher is the
// program's, and this test links no other backend.
#include "../src/renderer_internal.h"
#include "korvid/renderer-cpu/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/test.h"
#include "korvid/platform/io.h"
#include "korvid/platform/module.h"
#include <stdio.h>
#include <string.h>

namespace {

using namespace kv;

void test_headless(TestCtx& c) {
    c.begin("renderer-cpu headless");
    const ModuleDesc* const list[] = {&platform_module, &renderer_cpu_module};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    RendererDesc d;
    d.backend = RendererBackend::Cpu;
    d.width = 64;
    d.height = 32;
    rb_cpu::Renderer* r = rb_cpu::renderer_create(e, d);
    KV_CHECK(c, r && r->backend_id == KV_RENDER_ID_CPU && rb_cpu::renderer_get(e) == r);
    RenderCaps caps = rb_cpu::renderer_caps(r);
    KV_CHECK(c, caps.backend && !strcmp(caps.backend, "cpu"));

    // A frame: the clear, as sRGB bytes (linear 0.5 is 188; 0.0 is 0; 1.0 is 255).
    RenderView v;
    v.clear_color = color{0.5f, 0.0f, 1.0f, 1.0f};
    char dir[512], path[600];
    KV_CHECK(c, io_make_temp_dir("kv-cpu-test", dir, sizeof dir));
    snprintf(path, sizeof path, "%s/shot.ppm", dir);
    KV_CHECK(c, rb_cpu::renderer_screenshot(r, path));
    KV_CHECK(c, rb_cpu::renderer_render(r, v));
    KV_CHECK(c, r->target_width == 64 && r->target_height == 32);
    const u8* px = r->headless.data + (31 * 64 + 63) * 4; // the last pixel, B, G, R, A
    KV_CHECK(c, px[0] == 255 && px[1] == 0 && px[2] == 188 && px[3] == 255);
    // The screenshot: a P6 of the same, in R, G, B.
    FILE* f = fopen(path, "rb");
    KV_CHECK(c, f != nullptr);
    if (f) {
        char head[32] = {};
        KV_CHECK(c, fread(head, 1, 13, f) == 13 && !memcmp(head, "P6\n64 32\n255\n", 13));
        u8 rgb[3] = {};
        KV_CHECK(c, fread(rgb, 1, 3, f) == 3 && rgb[0] == 188 && rgb[1] == 0 && rgb[2] == 255);
        fclose(f);
    }
    io_remove_tree(dir);

    // A resize takes at the next frame.
    rb_cpu::renderer_resize(r, 20, 10);
    KV_CHECK(c, rb_cpu::renderer_render(r, v) && r->target_width == 20 && r->target_height == 10);
    rb_cpu::renderer_destroy(r);
    KV_CHECK(c, rb_cpu::renderer_get(e) == nullptr);
    engine_shutdown(e);
}

} // namespace

int main() {
    kv::TestCtx c;
    test_headless(c);
    kv::names_shutdown();
    kv::mem_shutdown();
    return kv::test_report(c, "renderer_cpu_tests");
}
