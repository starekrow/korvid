// renderer-cpu without a window (docs/PRDs/CPU-Renderer-PRD.md, milestone 1): made headless,
// a frame clears its target to the view's color as sRGB bytes, a screenshot reads it, and
// it ends cleanly; the tile hashes leave a tile undrawn exactly when its draws are as they
// were (milestone 3). Called through the backend's namespace: render's dispatcher is the
// program's, and this test links no other backend.
#include "../src/renderer_internal.h"
#include "korvid/renderer-cpu/module.h"
#include "korvid/render/ui.h"
#include "korvid/renderer-cpu/shader.h"
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

// A box in one tile of a 200 x 130 target (4 x 3 tiles): a frame like the last draws no tile,
// a change draws the tiles it touches, and what they draw is what drawing every tile does.
void test_tile_cache(TestCtx& c) {
    c.begin("renderer-cpu tile hashes");
    const ModuleDesc* const list[] = {&platform_module, &renderer_cpu_module};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    RendererDesc d;
    d.backend = RendererBackend::Cpu;
    d.width = 200;
    d.height = 130;
    rb_cpu::Renderer* r = rb_cpu::renderer_create(e, d);
    KV_CHECK(c, r != nullptr);
    if (!r) { engine_shutdown(e); return; }
    KV_CHECK(c, rb_cpu::renderer_caps(r).ui && !rb_cpu::renderer_caps(r).paced);
    r->tile_cache = true;
    RenderView v;
    UiList ui;
    ui.init(mem_general());
    UiBox b;
    b.rect = rect2{70, 70, 40, 30}; // inside tile (1, 1): x 64-127, y 64-127, its grown quad too
    b.radius[0] = b.radius[1] = b.radius[2] = b.radius[3] = 6.0f;
    b.fill = color{0.9f, 0.2f, 0.1f, 1.0f};
    ui.box(b);
    rb_cpu::renderer_ui_set(r, 0, ui);
    KV_CHECK(c, rb_cpu::renderer_render(r, v) && r->tiles_drawn == 12 && r->tiles_skipped == 0);
    KV_CHECK(c, rb_cpu::renderer_render(r, v) && r->tiles_drawn == 0 && r->tiles_skipped == 12);
    // The box's color changes: its tile only.
    ui.clear();
    b.fill = color{0.1f, 0.3f, 0.9f, 0.8f};
    ui.box(b);
    rb_cpu::renderer_ui_set(r, 0, ui);
    KV_CHECK(c, rb_cpu::renderer_render(r, v) && r->tiles_drawn == 1 && r->tiles_skipped == 11);
    usize bytes = (usize)200 * 130 * 4;
    u8* cached = (u8*)alloc_in(mem_general(), bytes);
    memcpy(cached, r->headless.data, bytes);
    // Every tile drawn again: the same bytes.
    r->tile_cache = false;
    memset(r->headless.data, 0, bytes);
    KV_CHECK(c, rb_cpu::renderer_render(r, v) && r->tiles_drawn == 12);
    KV_CHECK(c, memcmp(cached, r->headless.data, bytes) == 0);
    // The clear color changes: every tile.
    r->tile_cache = true;
    KV_CHECK(c, rb_cpu::renderer_render(r, v) && r->tiles_drawn == 12); // the cache starts over
    v.clear_color = color{0.2f, 0.2f, 0.2f, 1.0f};
    KV_CHECK(c, rb_cpu::renderer_render(r, v) && r->tiles_drawn == 12);
    free_in(mem_general(), cached, bytes);
    ui.free();
    rb_cpu::renderer_destroy(r);
    engine_shutdown(e);
}

// A region of a sheet (Krome slice 7, KR28): the red middle cell of a blue, red, blue strip
// drawn at 1.5x, 2x and 2.75x, linear and nearest. Held half a texel inside its uv rect, no
// pixel of it reads the blue beside it.
void test_region(TestCtx& c) {
    c.begin("renderer-cpu a region of a sheet");
    const ModuleDesc* const list[] = {&platform_module, &renderer_cpu_module};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    RendererDesc d;
    d.backend = RendererBackend::Cpu;
    d.width = 64;
    d.height = 16;
    rb_cpu::Renderer* r = rb_cpu::renderer_create(e, d);
    KV_CHECK(c, r != nullptr);
    if (!r) { engine_shutdown(e); return; }
    u8 sheet[12 * 4 * 4];
    for (u32 y = 0; y < 4; ++y)
        for (u32 x = 0; x < 12; ++x) {
            u8* p = sheet + (y * 12 + x) * 4;
            bool mid = x >= 4 && x < 8;
            p[0] = mid ? 255 : 0; p[1] = 0; p[2] = mid ? 0 : 255; p[3] = 255;
        }
    TextureDesc td;
    td.name = "test.sheet";
    Handle t = rb_cpu::renderer_texture_create(r, td);
    KV_CHECK(c, rb_cpu::renderer_texture_upload(r, t, 12, 4, sheet, sizeof sheet));
    RenderView v;
    v.clear_color = color{0, 0, 0, 1};
    KV_CHECK(c, rb_cpu::renderer_render(r, v)); // the upload lands
    UiList ui;
    ui.init(mem_general());
    struct Draw { f32 x, size; bool nearest; };
    const Draw draws[] = {{0, 6, false}, {8, 8, false}, {18, 11, false}, {32, 6, true}, {40, 11, true}};
    for (const Draw& dr : draws) {
        UiBox b;
        b.rect = rect2{dr.x, 2, dr.size, dr.size};
        b.fill = color{1, 1, 1, 1};
        b.textured = true;
        b.nearest = dr.nearest;
        b.texture = rb_cpu::renderer_texture_slot(r, t);
        b.uv = rect2{4.0f / 12.0f, 0, 4.0f / 12.0f, 1};
        ui.box(b);
    }
    rb_cpu::renderer_ui_set(r, 0, ui);
    KV_CHECK(c, rb_cpu::renderer_render(r, v));
    u32 red = 0, blue = 0;
    for (const Draw& dr : draws)
        for (u32 y = 2; y < 2 + (u32)dr.size; ++y)
            for (u32 x = (u32)dr.x; x < (u32)(dr.x + dr.size); ++x) {
                const u8* px = r->headless.data + (y * 64 + x) * 4; // B, G, R, A
                red += px[2] == 255;
                blue += px[0] != 0;
            }
    KV_CHECK(c, red == 36 + 64 + 121 + 36 + 121 && blue == 0);
    // Tiled (KR31): two blue texels and two red, every 4 pixels from the rect's left edge,
    // the last cut: blue, blue, red, red, blue, blue, red, red, blue, blue, red.
    ui.clear();
    UiBox tb;
    tb.rect = rect2{3, 2, 11, 4};
    tb.fill = color{1, 1, 1, 1};
    tb.textured = true;
    tb.nearest = true;
    tb.texture = rb_cpu::renderer_texture_slot(r, t);
    tb.uv = rect2{2.0f / 12.0f, 0, 4.0f / 12.0f, 1};
    tb.tile = vec2{4, 4};
    ui.box(tb);
    rb_cpu::renderer_ui_set(r, 0, ui);
    KV_CHECK(c, rb_cpu::renderer_render(r, v));
    char row[12] = {};
    for (u32 x = 0; x < 11; ++x) {
        const u8* px = r->headless.data + (3 * 64 + 3 + x) * 4;
        row[x] = px[0] == 255 && px[2] == 0 ? 'b' : px[2] == 255 && px[0] == 0 ? 'r' : '?';
    }
    KV_CHECK(c, !strcmp(row, "bbrrbbrrbbr"));
    if (strcmp(row, "bbrrbbrrbbr")) KV_ERROR("test", "the tiled row: %s", row);
    const u8* past = r->headless.data + (3 * 64 + 14) * 4; // the rect ends: the clear
    KV_CHECK(c, past[0] == 0 && past[2] == 0);
    ui.free();
    rb_cpu::renderer_destroy(r);
    engine_shutdown(e);
}

// The game shaders' registry (shader.h): the game's own form before Slang's, a skipped
// shader found as skipped, an unknown one not at all.
void own_form(const CpuShaderCall&) {}
void slang_form(const CpuShaderCall&) {}

void test_shader_registry(TestCtx& c) {
    c.begin("renderer-cpu shader registry");
    bool skipped = true;
    KV_CHECK(c, rb_cpu::cpu_shader_find("test_unknown", &skipped) == nullptr && !skipped);
    renderer_cpu_shader_generated("test_rings", slang_form);
    KV_CHECK(c, rb_cpu::cpu_shader_find("test_rings", &skipped) == slang_form && !skipped);
    renderer_cpu_shader("test_rings", own_form);
    KV_CHECK(c, rb_cpu::cpu_shader_find("test_rings", &skipped) == own_form);
    renderer_cpu_shader("test_rings", nullptr);
    KV_CHECK(c, rb_cpu::cpu_shader_find("test_rings", &skipped) == slang_form);
    renderer_cpu_shader_skip("test_rings");
    KV_CHECK(c, rb_cpu::cpu_shader_find("test_rings", &skipped) == nullptr && skipped);
    renderer_cpu_shader_skip("test_rings", false);
    KV_CHECK(c, rb_cpu::cpu_shader_find("test_rings", &skipped) == slang_form && !skipped);
}

} // namespace

int main() {
    kv::TestCtx c;
    test_headless(c);
    test_tile_cache(c);
    test_region(c);
    test_shader_registry(c);
    kv::names_shutdown();
    kv::mem_shutdown();
    return kv::test_report(c, "renderer_cpu_tests");
}
