// The gallery drawn by the CPU renderer with no window (docs/PRDs/CPU-Renderer-PRD.md, SR11,
// SR13 and SR18): every scene exactly as references/cpu has it, and within the cross-backend
// tolerance of Vulkan's references (the picture tool's defaults: 8 levels, 0.5% of the
// pixels), the game shaders run as Slang's C++. Then with --cpu-shaders none: the shaders
// scene exactly as references/cpu-none has it (its materials in their blends' pipelines, no
// post pass), every other scene as before. The scenes are the gallery's own: its source is
// built in here, and its shaders' generated modules are linked (kormake's shader_cpu rule).
#define main gallery_main
#include "../src/main.cc"
#undef main
#include "korvid/core/test.h"
#include "korvid/renderer-cpu/module.h"
#include "korvid/image/png.h"
#include "korvid/image/ppm.h"
#include "korvid/platform/io.h"
#include <stdio.h>

namespace {

using namespace kv;

// A whole file, or empty.
Array<u8> read_all(const char* path) {
    Array<u8> out{};
    FILE* f = fopen(path, "rb");
    if (!f) return out;
    u8 buf[65536];
    for (usize n; (n = fread(buf, 1, sizeof buf, f)) > 0;) memcpy(out.push_n((u32)n), buf, n);
    fclose(f);
    return out;
}

// references/ beside this file's directory.
void reference_path(char* out, usize cap, const char* backend, const char* scene) {
    const char* here = __FILE__;
    const char* slash = strrchr(here, '/');
    snprintf(out, cap, "%.*s/../references/%s/gallery_%s.png", slash ? (int)(slash - here) : 1, slash ? here : ".", backend, scene);
}

// The scene's picture against a reference: the pixels past `threshold` levels (any
// channel), and the largest difference.
bool compare(const char* dir, const char* backend, const char* scene, u32 threshold, f64 max_fraction) {
    char path[1024];
    snprintf(path, sizeof path, "%s/gallery_%s.ppm", dir, scene);
    Array<u8> a = read_all(path);
    reference_path(path, sizeof path, backend, scene);
    Array<u8> b = read_all(path);
    PpmImage pa;
    PngImage pb;
    const char* why = "";
    bool ok = a.len && b.len && ppm_decode(a.data, a.len, mem_general(), pa, &why) && png_decode(b.data, b.len, mem_general(), pb, &why);
    u32 off = 0, worst = 0;
    if (ok && (pa.width != pb.width || pa.height != pb.height)) { why = "sizes differ"; ok = false; }
    if (ok)
        for (u32 i = 0; i < pa.width * pa.height; ++i)
            for (u32 c = 0; c < 3; ++c) {
                i32 d = (i32)pa.rgb[i * 3 + c] - (i32)pb.rgba[i * 4 + c];
                u32 ad = (u32)(d < 0 ? -d : d);
                if (ad > worst) worst = ad;
                if (ad > threshold) { ++off; break; }
            }
    f64 fraction = ok ? (f64)off / ((f64)pa.width * pa.height) : 1.0;
    if (ok && fraction > max_fraction) ok = false;
    if (!ok) KV_ERROR("test", "%s against %s: %s%u pixels past %u (largest %u)", scene, backend, *why ? why : "", off, threshold, worst);
    if (pa.rgb) free_in(mem_general(), pa.rgb, (usize)pa.width * pa.height * 3);
    if (pb.rgba) free_in(mem_general(), pb.rgba, (usize)pb.width * pb.height * 4);
    a.free();
    b.free();
    return ok;
}

} // namespace

// A test program has no generated module list: the gallery's run needs the platform, the
// CPU renderer, headless, and its two shaders' CPU forms.
namespace kv {
extern const ModuleDesc gallery_gallery_rings_cpu_module;
extern const ModuleDesc gallery_gallery_post_cpu_module;
} // namespace kv
kv::Span<const kv::ModuleDesc* const> kv_module_list() {
    static const kv::ModuleDesc* const list[] = {&kv::platform_module, &kv::renderer_cpu_module, &kv::gallery_gallery_rings_cpu_module,
                                                 &kv::gallery_gallery_post_cpu_module};
    return kv::Span<const kv::ModuleDesc* const>(list, KV_COUNTOF(list));
}

int main() {
    TestCtx c;
    c.begin("gallery on the CPU, headless");
    char dir[512];
    KV_CHECK(c, io_make_temp_dir("kv-gallery-cpu", dir, sizeof dir));
    char a0[] = "gallery", a1[] = "--headless", a2[] = "--out", a3[] = "--cpu-shaders", a4[] = "none";
    char* argv[] = {a0, a1, a2, dir, nullptr};
    KV_CHECK(c, run(4, argv) == 0);
    // Every scene the CPU draws, exactly as before, and within the tolerance of the GPU's.
    const char* const all[] = {"blends", "transform", "filter", "sort", "interpolate_0", "interpolate_50", "interpolate_90", "camera", "shaders", "ui"};
    for (const char* s : all) KV_CHECK(c, compare(dir, "cpu", s, 0, 0.0));
    for (const char* s : all) KV_CHECK(c, compare(dir, "vulkan", s, 8, 0.005));
    io_remove_tree(dir);
    // The fast path: no game shader runs.
    KV_CHECK(c, io_make_temp_dir("kv-gallery-cpu-none", dir, sizeof dir));
    char* argv_none[] = {a0, a1, a3, a4, a2, dir, nullptr};
    KV_CHECK(c, run(6, argv_none) == 0);
    KV_CHECK(c, compare(dir, "cpu-none", "shaders", 0, 0.0));
    for (const char* s : all)
        if (strcmp(s, "shaders")) KV_CHECK(c, compare(dir, "cpu", s, 0, 0.0));
    io_remove_tree(dir);
    names_shutdown();
    mem_shutdown();
    return test_report(c, "gallery_tests");
}
