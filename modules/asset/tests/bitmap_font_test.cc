// Bitmap fonts (docs/PRDs/Krome-Slice-7-PRD.md, KR32), without a renderer: a .kbf parsed and
// its mistakes, a font of 3 x 5 cells read from files at twice its size, the character drawn
// for one it lacks, a font with none, an image too small for its characters, and the same
// font described in code. The files are written here, into a directory mounted as the root.
#include "korvid/asset/font.h"
#include "korvid/asset/module.h"
#include "korvid/core/array.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/memory.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include "korvid/image/png.h"
#include "korvid/platform/io.h"
#include "korvid/platform/module.h"
#include <string.h>

namespace {

using namespace kv;

void frame(Engine& e) { platform_pump(e); engine_frame(e, 1.0 / 60.0); }

AssetState settle(Engine& e, Handle font) {
    for (u32 i = 0; i < 2000; ++i) {
        frame(e);
        const FontAsset* f = asset_font(e, font);
        if (f && f->state != AssetState::Pending) return f->state;
        time_sleep(0.001);
    }
    return AssetState::Pending;
}

bool write_text(const char* dir, const char* name, const char* text) {
    char path[700];
    format(path, sizeof path, "%s/%s", dir, name);
    return io_write_file(path, text, strlen(text));
}

// Thirteen cells of 3 x 5 in a 15 x 15 image (five across, three down): "0123456789:- ",
// each filled with a grey of its own (its index times 16), opaque; the last row's last two
// cells clear.
bool write_sheet(const char* dir, const char* name) {
    u8 px[15 * 15 * 4] = {};
    for (u32 y = 0; y < 15; ++y)
        for (u32 x = 0; x < 15; ++x) {
            u32 cell = (y / 5) * 5 + x / 3;
            u8* p = px + (y * 15 + x) * 4;
            if (cell >= 13) continue;
            p[0] = p[1] = p[2] = (u8)(cell * 16);
            p[3] = 255;
        }
    Array<u8> out{};
    out.alloc = mem_general();
    const char* why = nullptr;
    bool ok = png_encode(px, 15, 15, 4, out, &why);
    char path[700];
    format(path, sizeof path, "%s/%s", dir, name);
    ok = ok && io_write_file(path, out.data, out.len);
    out.free();
    return ok;
}

bool parse_fails(const char* text, const char* why_has, u32 at) {
    MemScope scope;
    BitmapFontDesc d;
    const char* why = nullptr;
    u32 line = 0;
    if (kbf_parse(text, strlen(text), mem_scratch(), d, &why, &line)) { KV_ERROR("test", "parsed: %s", text); return false; }
    if (!strstr(why, why_has) || line != at) { KV_ERROR("test", "line %u: %s, wanted line %u: ...%s...", line, why, at, why_has); return false; }
    return true;
}

} // namespace

void test_bitmap_font(kv::TestCtx& c) {
    using namespace kv;
    c.begin("bitmap font");
    {
        // The file: words, a comment, a blank line; strings with an escape.
        const char* text = "# digits\nimage \"digits.png\"\n\ncell 3 5\nchars \"0123456789:- \"\nmissing \" \"\nadvance 4\n";
        MemScope scope;
        BitmapFontDesc d;
        const char* why = nullptr;
        u32 line = 0;
        KV_CHECK(c, kbf_parse(text, strlen(text), mem_scratch(), d, &why, &line));
        KV_CHECK(c, d.image && !strcmp(d.image, "digits.png") && d.cell_w == 3 && d.cell_h == 5 && d.advance == 4);
        KV_CHECK(c, d.chars && !strcmp(d.chars, "0123456789:- ") && d.missing && !strcmp(d.missing, " "));
        const char* esc = "image \"a\\\"b.png\"\ncell 1 1\nchars \"\\\\x\"\n";
        KV_CHECK(c, kbf_parse(esc, strlen(esc), mem_scratch(), d, &why, &line) && !strcmp(d.image, "a\"b.png") && !strcmp(d.chars, "\\x"));
        KV_CHECK(c, parse_fails("image \"a.png\"\nsize 3 5\n", "not a .kbf word", 2));
        KV_CHECK(c, parse_fails("image \"a.png\"\ncell 3\nchars \"ab\"\n", "cell takes a width and a height", 2));
        KV_CHECK(c, parse_fails("image \"a.png\"\ncell 3 0\nchars \"ab\"\n", "cell takes", 2));
        KV_CHECK(c, parse_fails("image a.png\n", "image takes a path in quotes", 1));
        KV_CHECK(c, parse_fails("image \"a.png\"\ncell 3 5\nchars \"ab\"\nmissing \"xy\"\n", "missing takes one character", 4));
        KV_CHECK(c, parse_fails("image \"a.png\"\ncell 3 5\n", "no chars line", 0)); // the file's, no line's
    }

    char dir[400];
    KV_CHECK(c, io_make_temp_dir("kv-bitmap-font-test", dir, sizeof dir));
    char sub[500];
    format(sub, sizeof sub, "%s/bm", dir);
    io_make_dirs(sub);
    KV_CHECK(c, write_sheet(dir, "bm/digits.png"));
    KV_CHECK(c, write_text(dir, "bm/digits.kbf", "image \"digits.png\"\ncell 3 5\nchars \"0123456789:- \"\nmissing \" \"\n"));
    KV_CHECK(c, write_text(dir, "bm/plain.kbf", "image \"digits.png\"\ncell 3 5\nchars \"0123456789\"\nadvance 4\n"));
    KV_CHECK(c, write_text(dir, "bm/greedy.kbf", "image \"digits.png\"\ncell 3 5\nchars \"0123456789abcdefghij\"\n"));
    KV_CHECK(c, write_text(dir, "bm/lost.kbf", "image \"nowhere.png\"\ncell 3 5\nchars \"01\"\n"));
    KV_CHECK(c, write_text(dir, "bm/bad.kbf", "image \"digits.png\"\ncells 3 5\n"));
    const ModuleDesc* const list[] = {&platform_module, &asset_module};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    KV_CHECK(c, platform_io(e)->mount_dir(dir));

    // At 10 px a 5-pixel cell is twice its size: a glyph 6 x 10 on the baseline, its pen's
    // step the cell's width twice; its uv its cell in the page, a clear pixel round each.
    Handle h = asset_load_font(e, "bm/digits.kbf", 10.0f);
    KV_CHECK(c, settle(e, h) == AssetState::Ready);
    const FontAsset& f = *asset_font(e, h);
    KV_CHECK(c, f.glyph_count == 13 && f.ascent == 10.0f && f.descent == 0.0f && f.line_height == 10.0f && f.kern_count == 0);
    const Glyph* one = font_glyph(f, '1');
    KV_CHECK(c, one && one->size.x == 6.0f && one->size.y == 10.0f && one->advance == 6.0f && one->offset.x == 3.0f && one->offset.y == 5.0f);
    f32 side = (f32)f.atlas_width;
    KV_CHECK(c, side == 64.0f && f.atlas_height == 64);
    // Thirteen cells: four across in the page; '1' is the second, at (1 + 4, 1), ':' the eleventh.
    KV_CHECK(c, one && one->uv.x * side == 5.0f && one->uv.y * side == 1.0f && one->uv.w * side == 3.0f && one->uv.h * side == 5.0f);
    const Glyph* colon = font_glyph(f, ':');
    KV_CHECK(c, colon && colon->uv.x * side == 9.0f && colon->uv.y * side == 13.0f);
    KV_CHECK(c, font_measure(f, "12:34") == 30.0f);
    // A character it lacks draws its `missing`: a space, of the space's width.
    const Glyph* space = font_glyph(f, ' ');
    KV_CHECK(c, font_glyph(f, 'x') == nullptr && font_glyph_or_replacement(f, 'x') == space && font_measure(f, "1x1") == 18.0f);
    // The same file at another size is another strike; at the same, the same handle.
    KV_CHECK(c, asset_load_font(e, "bm/digits.kbf", 10.0f) == h);
    Handle h26 = asset_load_font(e, "bm/digits.kbf", 26.0f);
    KV_CHECK(c, h26 != h && settle(e, h26) == AssetState::Ready && font_glyph(*asset_font(e, h26), '0')->size.y == 26.0f);

    // No `missing`: a character it lacks draws nothing and takes no room; `advance` is the
    // pen's step.
    Handle hp = asset_load_font(e, "bm/plain.kbf", 5.0f);
    KV_CHECK(c, settle(e, hp) == AssetState::Ready);
    const FontAsset& fp = *asset_font(e, hp);
    KV_CHECK(c, font_glyph_or_replacement(fp, ':') == nullptr && font_measure(fp, "1:2") == 8.0f && font_glyph(fp, '9')->size.x == 3.0f);

    // Mistakes: more characters than cells, an image that is not there, a word that is not.
    KV_CHECK(c, settle(e, asset_load_font(e, "bm/greedy.kbf", 5.0f)) == AssetState::Failed);
    Handle lost = asset_load_font(e, "bm/lost.kbf", 5.0f);
    KV_CHECK(c, settle(e, lost) == AssetState::Failed && asset_font(e, lost)->error == AssetError::NotFound);
    Handle bad = asset_load_font(e, "bm/bad.kbf", 5.0f);
    KV_CHECK(c, settle(e, bad) == AssetState::Failed && asset_font(e, bad)->error == AssetError::Parse);

    // Described in code, as an inline font of a .kim is: the same glyphs.
    BitmapFontDesc d;
    d.image = "bm/digits.png";
    d.cell_w = 3;
    d.cell_h = 5;
    d.chars = "0123456789:- ";
    Handle hc = asset_load_bitmap_font(e, "t/inline.kim#digits", d, 10.0f);
    KV_CHECK(c, settle(e, hc) == AssetState::Ready && asset_font(e, hc)->glyph_count == 13 && asset_load_bitmap_font(e, "t/inline.kim#digits", d, 10.0f) == hc);
    KV_CHECK(c, font_measure(*asset_font(e, hc), "12:34") == 30.0f && font_glyph_or_replacement(*asset_font(e, hc), 'x') == nullptr);
    engine_shutdown(e);
    io_remove_tree(dir);
}
