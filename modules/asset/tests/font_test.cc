// Fonts from files, without a renderer: glyphs made when asked and kept, pages that fill,
// faces shared by sizes, the fallback list, subpixel copies, the replacement. The fonts are
// the repository's own (DejaVu Sans, and arcfire's PublicPixel, which has CJK DejaVu lacks),
// copied to a directory mounted as the asset root.
#include "korvid/asset/font.h"
#include "korvid/asset/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include "korvid/platform/io.h"
#include "korvid/platform/module.h"
#include <string.h>

namespace {

using namespace kv;

// The repository's root, from this file's path (.../modules/asset/tests/font_test.cc).
void repo_root(char* out, usize cap) {
    format(out, cap, "%s", __FILE__);
    for (int up = 0; up < 4; ++up) if (char* slash = strrchr(out, '/')) *slash = 0;
}

bool copy_in(const char* root, const char* from, const char* dir, const char* to) {
    char src[700], dst[700], d[700];
    format(src, sizeof src, "%s/%s", root, from);
    format(d, sizeof d, "%s/%s", dir, to);
    if (char* slash = strrchr(d, '/')) { *slash = 0; io_make_dirs(d); }
    format(dst, sizeof dst, "%s/%s", dir, to);
    return io_copy_now(src, dst);
}

void frame(Engine& e) { platform_pump(e); engine_frame(e, 1.0 / 60.0); }

bool settle(Engine& e, Handle font) {
    for (u32 i = 0; i < 2000; ++i) {
        frame(e);
        const FontAsset* f = asset_font(e, font);
        if (f && f->state != AssetState::Pending) return f->state == AssetState::Ready;
        time_sleep(0.001);
    }
    return false;
}

u32 glyphs_of(const FontAsset& f) { u32 g = 0, p = 0; font_counts(f, &g, &p); return g; }
u32 pages_of(const FontAsset& f) { u32 g = 0, p = 0; font_counts(f, &g, &p); return p; }

} // namespace

void test_font(kv::TestCtx& c) {
    using namespace kv;
    c.begin("font");
    char root[600], dir[400];
    repo_root(root, sizeof root);
    KV_CHECK(c, io_make_temp_dir("kv-font-test", dir, sizeof dir));
    KV_CHECK(c, copy_in(root, "modules/text/assets/DejaVuSans.ttf", dir, "text/DejaVuSans.ttf"));
    KV_CHECK(c, copy_in(root, "examples/arcfire/assets/font/PublicPixel.ttf", dir, "pixel/PublicPixel.ttf"));
    const ModuleDesc* const list[] = {&platform_module, &asset_module};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    KV_CHECK(c, platform_io(e)->mount_dir(dir));

    // Nothing is made until asked; asked, made once, and the same glyph after.
    Handle h16 = asset_load_font(e, "text/DejaVuSans.ttf", 16.0f);
    KV_CHECK(c, settle(e, h16));
    const FontAsset& f = *asset_font(e, h16);
    KV_CHECK(c, glyphs_of(f) == 0 && pages_of(f) == 1 && f.line_height >= 16 && f.ascent == (f32)(i32)f.ascent && f.kern_count > 1000);
    const Glyph* a = font_glyph(f, 'A');
    KV_CHECK(c, a && a->size.x > 0 && a->advance > 8 && glyphs_of(f) == 1 && font_glyph(f, 'A') == a);
    font_measure(f, "Hello");
    KV_CHECK(c, glyphs_of(f) == 5); // H, e, l, o, and A before: l once
    // Outside the old fixed set, with nothing declared: Greek, Cyrillic, box drawing.
    KV_CHECK(c, font_glyph(f, 0x03A9) && font_glyph(f, 0x0416) && font_glyph(f, 0x2502));
    // What no face has: null, or U+FFFD's glyph when the caller asks for the replacement.
    const Glyph* fffd = font_glyph(f, 0xFFFD);
    KV_CHECK(c, !font_glyph(f, 0xE000) && fffd && font_glyph_or_replacement(f, 0xE000) == fffd);
    // A quarter-pixel copy is another glyph, with the same advance.
    const Glyph* i0 = font_glyph(f, 'i');
    const Glyph* i2 = font_glyph_at(f, 'i', 2);
    KV_CHECK(c, i2 && i2 != i0 && i2->advance == i0->advance && font_glyph_at(f, 'i', 2) == i2 && font_glyph_at(f, 'i', 0) == i0);

    // Another size of the same file reads nothing more and is ready at the next pump.
    Handle h20 = asset_load_font(e, "text/DejaVuSans.ttf", 20.0f);
    frame(e);
    KV_CHECK(c, asset_font(e, h20)->state == AssetState::Ready);

    // Pages: big glyphs fill one and take another.
    Handle h120 = asset_load_font(e, "text/DejaVuSans.ttf", 120.0f);
    KV_CHECK(c, settle(e, h120));
    const FontAsset& big = *asset_font(e, h120);
    for (u32 cp = 'A'; cp <= 'Z'; ++cp) font_glyph(big, cp);
    for (u32 cp = 0x0410; cp < 0x0430; ++cp) font_glyph(big, cp); // Cyrillic capitals
    KV_CHECK(c, pages_of(big) >= 2 && font_glyph(big, 'A')->uv.w > 0.1f);

    // The fallback list: what the first face lacks comes from the next.
    const char* const fallback[] = {"pixel/PublicPixel.ttf"};
    FontDesc d;
    d.px = 16.0f;
    d.fallback = Span<const char* const>(fallback, 1);
    Handle hf = asset_load_font(e, "text/DejaVuSans.ttf", d);
    KV_CHECK(c, hf != h16 && settle(e, hf));
    for (u32 i = 0; i < 20; ++i) frame(e); // PublicPixel's read lands
    const FontAsset& fb = *asset_font(e, hf);
    const Glyph* yuan = font_glyph(fb, 0x5143);
    KV_CHECK(c, !font_glyph(f, 0x5143) && yuan && yuan->size.x > 0);
    KV_CHECK(c, font_glyph(fb, 'A')->advance == a->advance); // DejaVu's own still comes first
    // The engine's list, for fonts loaded after it is set.
    asset_set_font_fallback(e, Span<const char* const>(fallback, 1));
    Handle hd = asset_load_font(e, "text/DejaVuSans.ttf", 18.0f);
    KV_CHECK(c, settle(e, hd));
    for (u32 i = 0; i < 5; ++i) frame(e);
    KV_CHECK(c, font_glyph(*asset_font(e, hd), 0x5186) != nullptr);

    // The numbers: a strike's glyphs made as asked (Latin, Greek, Cyrillic at 14 px).
    {
        Handle h14 = asset_load_font(e, "text/DejaVuSans.ttf", 14.0f);
        KV_CHECK(c, settle(e, h14));
        const FontAsset& f14 = *asset_font(e, h14);
        f64 t0 = time_now();
        u32 made = 0;
        for (u32 cp = 0x21; cp < 0x7F; ++cp) made += font_glyph(f14, cp) != nullptr;
        for (u32 cp = 0x391; cp < 0x3CA; ++cp) made += font_glyph(f14, cp) != nullptr;
        for (u32 cp = 0x410; cp < 0x450; ++cp) made += font_glyph(f14, cp) != nullptr;
        f64 t1 = time_now();
        for (u32 cp = 0x21; cp < 0x7F; ++cp) font_glyph(f14, cp);
        f64 t2 = time_now();
        KV_INFO("test", "font numbers: %u glyphs made at 14 px in %.2f ms (%.1f us each); asked again, %.3f us each",
                made, (t1 - t0) * 1e3, (t1 - t0) * 1e6 / (made ? made : 1), (t2 - t1) * 1e6 / 94.0);
    }

    // Small sizes: whole-pixel metrics.
    Handle h9 = asset_load_font(e, "text/DejaVuSans.ttf", 9.0f);
    KV_CHECK(c, settle(e, h9));
    const FontAsset& f9 = *asset_font(e, h9);
    KV_CHECK(c, f9.ascent == (f32)(i32)f9.ascent && f9.line_height == (f32)(i32)f9.line_height && font_glyph(f9, 'm'));
    engine_shutdown(e);
    io_remove_tree(dir);
}
