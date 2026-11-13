// Text without a renderer: the layout function on a hand-built font (fixed advances, a
// kern pair, a space with no ink), alignment, scale, truncation; and the system on an
// engine with no asset module up, where a label with a null font lays out as nothing and
// a dead label's glyphs are left to the scene module.
#include "korvid/text/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/test.h"
#include "korvid/core/utf8.h"
#include "korvid/scene/module.h"
#include "korvid/sprite/module.h"
#include "korvid/sprite/sprite.h"
#include "korvid/sprite/transform2.h"
#include "korvid/math/scalar.h"
#include <string.h>

namespace {

using namespace kv;

bool near(f32 a, f32 b, f32 eps = 1e-3f) { return kv::fabs(a - b) < eps; }

// A font where every glyph is 10 wide, 12 high, advances 11, sits with its center 5
// right of and 6 above the pen; the space has no ink; 'A' before 'V' kerns by -2. It
// covers printable ASCII, 'é', the em dash and U+FFFD, the last two with their own uv.
struct FakeFont {
    static constexpr u32 kCount = 95 + 3;
    FontAsset f{};
    u32       cps[kCount];
    Glyph     glyphs[kCount];
    u64       kern_pairs[1] = {(u64)'A' << 32 | 'V'};
    f32       kern_values[1] = {-2.0f};
    FakeFont() {
        for (u32 i = 0; i < 95; ++i) cps[i] = 32 + i;
        cps[95] = 0xE9; cps[96] = 0x2014; cps[97] = 0xFFFD;
        for (u32 i = 0; i < kCount; ++i) {
            Glyph& g = glyphs[i];
            g.uv      = rect2{(f32)i / 256.0f, 0.0f, 1.0f / 256.0f, 1.0f};
            g.size    = vec2{10, 12};
            g.offset  = vec2{5, 6};
            g.advance = 11;
        }
        glyphs[0].size = vec2{0, 0}; // the space
        f.state       = AssetState::Ready;
        f.glyph_count = kCount;
        f.codepoints  = cps;
        f.glyphs      = glyphs;
        f.kern_count  = 1;
        f.kern_pairs  = kern_pairs;
        f.kern_values = kern_values;
        f.ascent = 9; f.descent = -3; f.line_height = 14;
        font_index(f);
    }
};

} // namespace

void test_text(kv::TestCtx& c) {
    using namespace kv;
    c.begin("text");

    FakeFont ff;
    Text t;
    text_set(t, "AV %d", 7);
    KV_CHECK(c, strcmp(t.str, "AV 7") == 0);
    KV_CHECK(c, near(font_measure(ff.f, "AV 7"), 4 * 11 - 2));
    GlyphPlacement out[8];
    f32 width = 0;
    u32 n = text_layout(ff.f, t, out, &width);
    KV_CHECK(c, n == 3 && near(width, 42)); // the space has no quad
    KV_CHECK(c, near(out[0].center.x, 5) && near(out[0].center.y, 6));
    KV_CHECK(c, near(out[1].center.x, 11 - 2 + 5));      // kerned
    KV_CHECK(c, near(out[2].center.x, 11 - 2 + 22 + 5)); // after the space
    KV_CHECK(c, near(out[0].size.y, 12) && near(out[0].uv.x, (f32)('A' - 32) / 256.0f));

    // Center and Right move the whole line; scale multiplies everything.
    t.align = TextAlign::Center;
    text_layout(ff.f, t, out);
    KV_CHECK(c, near(out[0].center.x, -21 + 5));
    t.align = TextAlign::Right;
    t.scale = 2.0f;
    text_layout(ff.f, t, out, &width);
    KV_CHECK(c, near(out[0].center.x, (-42 + 5) * 2) && near(out[0].size.x, 20) && near(width, 84));

    // More glyphs than room: the count is the truth, nothing past the span is written.
    text_set(t, "ABCDEFGHIJ");
    GlyphPlacement two[2] = {};
    KV_CHECK(c, text_layout(ff.f, t, two) == 10 && two[1].size.x == 20);
    KV_CHECK(c, text_set(t, "%s", "a string longer than the sixty-four bytes a Text keeps, which is truncated") > (int)KV_TEXT_MAX);
    KV_CHECK(c, strlen(t.str) == KV_TEXT_MAX - 1);

    // UTF-8: two-byte and three-byte characters are one glyph each; what the font lacks
    // draws as U+FFFD; a malformed byte is U+FFFD too and never loses the rest.
    t = Text{};
    text_set(t, "\xc3\xa9\xe2\x80\x94\xe4\xb8\xad\xff" "A");
    KV_CHECK(c, text_layout(ff.f, t, out, &width) == 5 && near(width, 55));
    KV_CHECK(c, near(out[0].uv.x, 95 / 256.0f) && near(out[1].uv.x, 96 / 256.0f));
    KV_CHECK(c, near(out[2].uv.x, 97 / 256.0f) && near(out[3].uv.x, 97 / 256.0f)); // the CJK and the stray byte
    KV_CHECK(c, near(out[4].uv.x, ('A' - 32) / 256.0f) && ff.f.misses == 2);
    KV_CHECK(c, near(font_measure(ff.f, "\xc3\xa9"), 11) && font_glyph(ff.f, 0xE9) == &ff.glyphs[95] && font_glyph(ff.f, 0x4E2D) == nullptr);
    KV_CHECK(c, font_kern(ff.f, 'A', 'V') == -2.0f && font_kern(ff.f, 'V', 'A') == 0.0f);
    // Truncation keeps whole characters: 31 two-byte characters fill 62 of the 63 bytes,
    // and the 32nd would be cut, so it goes.
    char many[80] = {};
    for (u32 i = 0; i < 32; ++i) { many[i * 2] = '\xC3'; many[i * 2 + 1] = '\xA9'; } // é
    text_set(t, "%s", many);
    KV_CHECK(c, strlen(t.str) == 62 && utf8_valid(t.str, strlen(t.str)));

    // A font that is not ready lays out as nothing.
    ff.f.state = AssetState::Pending;
    KV_CHECK(c, text_layout(ff.f, t, out, &width) == 0 && width == 0.0f);

    // The system, with no asset module: a label with a null font makes no glyphs and is
    // counted; destroying it leaves nothing behind for the scene module to orphan.
    const ModuleDesc* const list[] = {&sprite_module, &scene_module, &text_module};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    Table* labels = engine_table(e, "text.label");
    Table* glyphs = engine_table(e, "text.glyph");
    KV_CHECK(c, labels && glyphs && glyphs->desc->cosmetic && !labels->desc->cosmetic);
    Text hud;
    text_set(hud, "SCORE 0");
    Handle label = labels->spawn(transform2_at(point2{0, 0}), hud);
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 0);
    KV_CHECK(c, text_stats(e)->labels == 1 && text_stats(e)->relaid == 1 && glyphs->live() == 0);
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 1);
    KV_CHECK(c, text_stats(e)->relaid == 0); // nothing changed, nothing redone
    labels->get<Text>(label)->str[6] = '1';
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 2);
    KV_CHECK(c, text_stats(e)->relaid == 1);
    labels->destroy(label);
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 3);
    KV_CHECK(c, text_stats(e)->labels == 0 && glyphs->live() == 0);
    u64 h = engine_hash_state(e);
    glyphs->spawn(Transform2{}, Sprite{}, Local2{}, Parent{});
    KV_CHECK(c, engine_hash_state(e) == h); // cosmetic kinds never hash
    engine_shutdown(e);
}
