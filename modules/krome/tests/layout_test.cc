// The tree and layout, headless: a fake measure (a character is half the font size wide,
// a line is the size plus 2 high) so every number below can be worked out by hand.
#include "korvid/krome/tree.h"
#include "korvid/core/test.h"
#include <string.h>

namespace {

using namespace kv;

// A few numbers for one call, as a temporary array (C99's (const f32[]){...}, which C++
// does not have and cl refuses).
template <u32 N> struct Nums { f32 v[N]; };

vec2 fake_text(void*, const char*, u32 len, Name, f32 size) { return vec2{(f32)len * size * 0.5f, size + 2.0f}; }
vec2 fake_image(void*, Name) { return vec2{32, 24}; }
const KromeMeasure k_measure{nullptr, fake_text, fake_image};

// Paragraphs the same way: a character half the size wide, lines size + 2 apart (or the
// line height times the size), broken greedily at spaces; a word longer than the width
// overflows its line.
vec2 fake_paragraph(void*, const char* s, u32 len, Name, f32 size, f32 lh, f32 width, f32* narrowest) {
    f32 ch = size * 0.5f, line = lh > 0.0f ? lh * size : size + 2.0f;
    f32 widest = 0.0f, x = 0.0f, word = 0.0f;
    u32 lines = 1;
    *narrowest = 0.0f;
    for (u32 i = 0; i <= len; ++i) {
        bool end = i == len || s[i] == ' ' || s[i] == '\n';
        if (!end) { word += ch; continue; }
        if (word > *narrowest) *narrowest = word;
        f32 need = x > 0.0f ? x + ch + word : word;
        if (width > 0.0f && x > 0.0f && need > width) { ++lines; x = word; }
        else x = need;
        if (x > widest) widest = x;
        word = 0.0f;
        if (i < len && s[i] == '\n') { ++lines; x = 0.0f; }
    }
    return vec2{widest, line * (f32)lines};
}
const KromeMeasure k_wrapping{nullptr, fake_text, fake_image, fake_paragraph};

bool near(f32 a, f32 b) { return a - b < 0.001f && b - a < 0.001f; }
bool rect_is(const rect2& r, f32 x, f32 y, f32 w, f32 h) {
    if (near(r.x, x) && near(r.y, y) && near(r.w, w) && near(r.h, h)) return true;
    KV_ERROR("test", "rect (%g, %g, %g, %g), wanted (%g, %g, %g, %g)", r.x, r.y, r.w, r.h, x, y, w, h);
    return false;
}

struct T {
    KromeTree t;
    T() { t.init(mem_general()); }
    ~T() { t.free(); }
    u32 box(u32 parent, const char* dir = "col") {
        u32 b = t.add(KromeType::Box, parent);
        t.set(b, "dir", KromeValue::str(dir));
        return b;
    }
    u32 text(u32 parent, const char* s) {
        u32 n = t.add(KromeType::Text, parent);
        t.set_text(n, s);
        return n;
    }
    void set(u32 n, const char* p, KromeValue v) {
        const char* why = t.set(n, p, v);
        if (why) KV_ERROR("test", "set %s: %s", p, why);
    }
    void flag(u32 n, const char* w) {
        const char* why = t.flag(n, w);
        if (why) KV_ERROR("test", "flag %s: %s", w, why);
    }
    void layout(f32 w = 200, f32 h = 100) { krome_layout(t, t.root, rect2{0, 0, w, h}, k_measure); }
    void wrap_layout(f32 w = 200, f32 h = 300) { krome_layout(t, t.root, rect2{0, 0, w, h}, k_wrapping); }
    const rect2& r(u32 n) { return t.nodes[n].rect; }
};

} // namespace

void test_tree(kv::TestCtx& c) {
    using namespace kv;
    c.begin("krome tree");
    T x;
    u32 root = x.box(KROME_NONE);
    KV_CHECK(c, x.t.root == root);
    u32 a = x.text(root, "hello");
    KV_CHECK(c, strcmp(x.t.text(a), "hello") == 0 && x.t.nodes[root].first == a && x.t.nodes[root].last == a);

    // Refusals say why.
    KV_CHECK(c, x.t.set(root, "nope", KromeValue::num(1)) != nullptr);
    KV_CHECK(c, x.t.set(root, "bg", KromeValue::str("red")) != nullptr);
    KV_CHECK(c, x.t.set(root, "gap", KromeValue::str("8")) != nullptr);
    KV_CHECK(c, x.t.set(root, "w", KromeValue::str("wide")) != nullptr);
    KV_CHECK(c, x.t.set(root, "pad", KromeValue::nums(Nums<3>{{1, 2, 3}}.v, 3)) != nullptr);
    KV_CHECK(c, x.t.set(root, "w", KromeValue::num(10), KromeState::Hover) != nullptr); // a state sets looks only
    KV_CHECK(c, x.t.flag(root, "shiny") != nullptr);
    KV_CHECK(c, x.t.set(root, "anchor", KromeValue::str("bottom-left")) == nullptr && x.t.nodes[root].anchor_x == 0 && x.t.nodes[root].anchor_y == 2);
    KV_CHECK(c, x.t.set(root, "anchor", KromeValue::str("left")) == nullptr && x.t.nodes[root].anchor_x == 0 && x.t.nodes[root].anchor_y == 1);
    KV_CHECK(c, x.t.set(root, "anchor", KromeValue::str("sideways")) != nullptr);

    // Colors.
    color col;
    KV_CHECK(c, krome_parse_color("#ff8000", &col) && col.r == 1.0f && near(col.g, 128.0f / 255.0f) && col.b == 0.0f && col.a == 1.0f);
    KV_CHECK(c, krome_parse_color("#f008", &col) && col.r == 1.0f && near(col.a, 136.0f / 255.0f));
    KV_CHECK(c, krome_parse_color("none", &col) && col.a == 0.0f);
    KV_CHECK(c, !krome_parse_color("#12345", &col) && !krome_parse_color("#zzzzzz", &col) && !krome_parse_color("blue", &col));

    // Looks: a state overlays only what it says; text color, font and size inherit.
    KV_CHECK(c, x.t.set(root, "bg", KromeValue::str("#102030")) == nullptr);
    KV_CHECK(c, x.t.set(root, "color", KromeValue::str("#ffffff")) == nullptr);
    KV_CHECK(c, x.t.set(root, "radius", KromeValue::num(4)) == nullptr);
    KV_CHECK(c, x.t.set(root, "bg", KromeValue::str("#405060"), KromeState::Hover) == nullptr);
    KV_CHECK(c, x.t.set(root, "color", KromeValue::str("#ff0000"), KromeState::Hover) == nullptr);
    KromeLook base = x.t.resolve_look(root, KromeState::Base, nullptr);
    KromeLook hover = x.t.resolve_look(root, KromeState::Hover, nullptr);
    KV_CHECK(c, near(base.bg.r, 16.0f / 255.0f) && near(hover.bg.r, 64.0f / 255.0f) && hover.radius[2] == 4.0f && base.radius[0] == 4.0f);
    KV_CHECK(c, base.size == KROME_DEFAULT_SIZE && base.font == name_intern(KROME_DEFAULT_FONT));
    KromeLook child = x.t.resolve_look(a, KromeState::Base, &hover);
    KV_CHECK(c, child.text_color.g == 0.0f && child.text_color.r == 1.0f && child.bg.a == 0.0f); // color inherits, bg does not
    KV_CHECK(c, x.t.set(a, "color", KromeValue::str("#00ff00")) == nullptr);
    child = x.t.resolve_look(a, KromeState::Base, &hover);
    KV_CHECK(c, child.text_color.g == 1.0f && child.text_color.r == 0.0f);
    // Radii: two numbers are a diagonal pair.
    KV_CHECK(c, x.t.set(a, "radius", KromeValue::nums(Nums<2>{{2, 6}}.v, 2)) == nullptr);
    child = x.t.resolve_look(a, KromeState::Base, nullptr);
    KV_CHECK(c, child.radius[0] == 2 && child.radius[1] == 6 && child.radius[2] == 2 && child.radius[3] == 6);
    // Selected and disabled are data: their looks apply whatever the pointer does.
    KV_CHECK(c, x.t.set(a, "bg", KromeValue::str("#0000ff"), KromeState::Selected) == nullptr);
    x.flag(a, "selected");
    KV_CHECK(c, x.t.resolve_look(a, KromeState::Hover, nullptr).bg.b == 1.0f);

    // Depth is bounded.
    T deep;
    u32 p = deep.box(KROME_NONE);
    u32 made = 1;
    for (u32 i = 0; i < 100 && p != KROME_NONE; ++i, ++made) p = deep.t.add(KromeType::Box, p);
    KV_CHECK(c, p == KROME_NONE && made == KROME_MAX_DEPTH + 1);
}

void test_layout(kv::TestCtx& c) {
    using namespace kv;
    c.begin("krome layout");
    {   // A hugging row: children, gaps and padding add up; the root fills the window.
        T x;
        u32 root = x.box(KROME_NONE);
        x.set(root, "size", KromeValue::num(10));
        u32 row = x.box(root, "row");
        x.set(row, "gap", KromeValue::num(4));
        x.set(row, "pad", KromeValue::num(2));
        u32 a = x.text(row, "ab"), b = x.text(row, "abcd");
        x.layout();
        KV_CHECK(c, rect_is(x.r(root), 0, 0, 200, 100));
        KV_CHECK(c, rect_is(x.r(row), 0, 0, 38, 16));
        KV_CHECK(c, rect_is(x.r(a), 2, 2, 10, 12) && rect_is(x.r(b), 16, 2, 20, 12));
        KV_CHECK(c, !x.t.dirty);
    }
    {   // Fills share what is left; one held to its range gives the rest back.
        T x;
        u32 root = x.box(KROME_NONE);
        u32 row = x.box(root, "row");
        x.set(row, "w", KromeValue::str("fill"));
        x.set(row, "h", KromeValue::num(20));
        u32 fixed = x.box(row);
        x.set(fixed, "w", KromeValue::num(50));
        u32 f1 = x.box(row);
        x.flag(f1, "fill");
        u32 f2 = x.box(row);
        x.flag(f2, "fill");
        x.set(f2, "w", KromeValue::range(0, 30));
        x.layout();
        KV_CHECK(c, rect_is(x.r(row), 0, 0, 200, 20));
        KV_CHECK(c, rect_is(x.r(fixed), 0, 0, 50, 0) && rect_is(x.r(f1), 50, 0, 120, 0) && rect_is(x.r(f2), 170, 0, 30, 0));
        // A minimum holds when the room runs out.
        x.set(f1, "w", KromeValue::range(140, 1000));
        x.layout();
        KV_CHECK(c, rect_is(x.r(f1), 50, 0, 140, 0) && rect_is(x.r(f2), 190, 0, 10, 0));
    }
    {   // A spacer pushes apart; a bare fill in a column fills its height, not its width.
        T x;
        u32 root = x.box(KROME_NONE);
        x.set(root, "size", KromeValue::num(10));
        u32 row = x.box(root, "row");
        x.set(row, "w", KromeValue::str("fill"));
        u32 a = x.text(row, "ab");
        u32 sp = x.box(row);
        x.flag(sp, "fill");
        u32 b = x.text(row, "abc");
        u32 col = x.box(root);
        x.flag(col, "fill");
        x.layout();
        KV_CHECK(c, rect_is(x.r(a), 0, 0, 10, 12) && rect_is(x.r(sp), 10, 0, 175, 0) && rect_is(x.r(b), 185, 0, 15, 12));
        KV_CHECK(c, rect_is(x.r(col), 0, 12, 0, 88));
    }
    {   // Alignment: along when nothing fills, and across.
        T x;
        u32 root = x.box(KROME_NONE);
        x.set(root, "size", KromeValue::num(10));
        x.flag(root, "center");
        x.flag(root, "bottom");
        u32 a = x.text(root, "abcd");
        x.layout();
        KV_CHECK(c, rect_is(x.r(a), 90, 88, 20, 12));
        u32 row = x.box(root, "row");
        x.set(row, "w", KromeValue::num(100));
        x.set(row, "h", KromeValue::num(30));
        x.flag(row, "right");
        x.flag(row, "middle");
        u32 b = x.text(row, "ab");
        x.layout();
        KV_CHECK(c, rect_is(x.r(a), 90, 58, 20, 12) && rect_is(x.r(row), 50, 70, 100, 30) && rect_is(x.r(b), 140, 79, 10, 12));
    }
    {   // A stack: every child over the whole inside, each placed by its anchor or the stack's.
        T x;
        u32 root = x.box(KROME_NONE, "stack");
        x.set(root, "pad", KromeValue::nums(Nums<2>{{10, 5}}.v, 2));
        u32 a = x.box(root);
        x.set(a, "w", KromeValue::num(20));
        x.set(a, "h", KromeValue::num(20));
        x.set(a, "anchor", KromeValue::str("bottom-right"));
        u32 b = x.box(root);
        x.flag(b, "fill");
        u32 img = x.t.add(KromeType::Image, root);
        x.set(img, "image", KromeValue::str("pic.png"));
        x.set(img, "anchor", KromeValue::str("center"));
        x.layout();
        KV_CHECK(c, rect_is(x.r(a), 170, 75, 20, 20) && rect_is(x.r(b), 10, 5, 180, 90) && rect_is(x.r(img), 84, 38, 32, 24));
    }
    {   // Ranges bound hugging content; hidden children take no room.
        T x;
        u32 root = x.box(KROME_NONE);
        x.set(root, "size", KromeValue::num(10));
        x.set(root, "gap", KromeValue::num(3));
        u32 shortest = x.text(root, "ab");
        x.set(shortest, "w", KromeValue::range(30, 40));
        u32 hidden = x.text(root, "nothing");
        x.flag(hidden, "hidden");
        u32 longest = x.text(root, "abcdefghijklmnopqrst");
        x.set(longest, "w", KromeValue::range(30, 40));
        x.layout();
        KV_CHECK(c, rect_is(x.r(shortest), 0, 0, 30, 12) && rect_is(x.r(longest), 0, 15, 40, 12));
        KV_CHECK(c, x.t.nodes[root].content.y == 27.0f);
    }
    {   // A scroll box: its content runs past it, the offset is held inside what there is.
        T x;
        u32 root = x.box(KROME_NONE);
        x.set(root, "size", KromeValue::num(10));
        u32 list = x.box(root);
        x.flag(list, "scroll");
        x.set(list, "h", KromeValue::num(50));
        u32 first = KROME_NONE, item = KROME_NONE;
        for (u32 i = 0; i < 10; ++i) { item = x.text(list, "row"); if (!i) first = item; }
        u32 filler = x.box(list);
        x.flag(filler, "fill"); // hugs in a scroll box
        x.layout();
        KV_CHECK(c, rect_is(x.r(list), 0, 0, 15, 50) && x.t.nodes[list].content.y == 120.0f && rect_is(x.r(first), 0, 0, 15, 12) && x.r(filler).h == 0);
        x.t.nodes[list].scroll.y = 100;
        x.layout();
        KV_CHECK(c, x.t.nodes[list].scroll.y == 70.0f && rect_is(x.r(first), 0, -70, 15, 12) && rect_is(x.r(item), 0, 38, 15, 12));
        x.t.nodes[list].scroll.y = -5;
        x.layout();
        KV_CHECK(c, x.t.nodes[list].scroll.y == 0.0f);
    }
    {   // Font size inherits from above, in layout too; a size below overrides it.
        T x;
        u32 root = x.box(KROME_NONE);
        x.set(root, "size", KromeValue::num(20));
        u32 inner = x.box(root);
        u32 a = x.text(inner, "ab");
        u32 b = x.text(inner, "ab");
        x.set(b, "size", KromeValue::num(10));
        x.layout();
        KV_CHECK(c, rect_is(x.r(a), 0, 0, 20, 22) && rect_is(x.r(b), 0, 22, 10, 12));
        // An overlay's subtree laid out alone still inherits from where it hangs, and takes
        // its own size at the corner it is given.
        krome_layout(x.t, inner, rect2{5, 5, 1000, 1000}, k_measure, false);
        KV_CHECK(c, rect_is(x.r(inner), 5, 5, 20, 34) && rect_is(x.r(a), 5, 5, 20, 22));
    }
    {   // Pixels: rects that share an edge share it at any scale.
        rect2 left{0, 0, 10.3f, 7.0f}, right{10.3f, 0, 5.0f, 7.0f};
        const f32 scales[] = {1.0f, 1.5f, 2.0f, 1.25f};
        bool abut = true;
        for (f32 s : scales) {
            rect2 l = krome_pixel_rect(left, s), r = krome_pixel_rect(right, s);
            if (l.x + l.w != r.x) abut = false;
            if (l.x != (f32)(i32)l.x || l.w != (f32)(i32)l.w) abut = false;
        }
        KV_CHECK(c, abut);
        KV_CHECK(c, rect_is(krome_pixel_rect(rect2{1, 1, 10, 10}, 1.5f), 2, 2, 15, 15));
    }

    {   // Wrapping (slice 3): a text gets no more width than its column, and is as tall as its
        // lines there; what follows it is placed below them. Size 10: a character is 5 wide,
        // a line 12 high.
        T x;
        u32 root = x.box(KROME_NONE);
        x.set(root, "size", KromeValue::num(10));
        u32 col = x.box(root);
        x.set(col, "w", KromeValue::num(100));
        u32 a = x.text(col, "aaaa bbbb cccc dddd"); // 95 wide on one line: two words a line at 100? no, 3 (70)
        u32 b = x.text(col, "short");
        u32 one = x.text(col, "nowrap stays on one line");
        x.flag(one, "nowrap");
        x.wrap_layout();
        // "aaaa bbbb cccc" is 70, + " dddd" is 95: one line at 100.
        KV_CHECK(c, rect_is(x.r(a), 0, 0, 95, 12));
        x.t.set_text(a, "aaaa bbbb cccc dddd eeee ffff");
        x.wrap_layout();
        KV_CHECK(c, rect_is(x.r(a), 0, 0, 100, 24) && rect_is(x.r(b), 0, 24, 25, 12));
        KV_CHECK(c, rect_is(x.r(one), 0, 36, 120, 12)); // wider than the column: it overflows
        KV_CHECK(c, x.t.nodes[a].narrowest == 20 && x.t.nodes[one].narrowest == 120);
    }
    {   // A hugging column inside a narrow one shrinks to it, and its text wraps; a fill in a
        // row wraps in its share; the column's height follows.
        T x;
        u32 root = x.box(KROME_NONE);
        x.set(root, "size", KromeValue::num(10));
        u32 outer = x.box(root);
        x.set(outer, "w", KromeValue::num(60));
        x.set(outer, "pad", KromeValue::num(5));
        u32 inner = x.box(outer);
        u32 t = x.text(inner, "one two three four");
        u32 row = x.box(root, "row");
        x.set(row, "w", KromeValue::num(100));
        x.set(row, "gap", KromeValue::num(10));
        u32 label = x.text(row, "label");
        u32 body = x.text(row, "aa bb cc dd ee ff gg");
        x.flag(body, "fill");
        u32 below = x.text(root, "below");
        x.wrap_layout();
        KV_CHECK(c, rect_is(x.r(inner), 5, 5, 50, 24) && rect_is(x.r(t), 5, 5, 50, 24)); // one two / three four
        KV_CHECK(c, rect_is(x.r(outer), 0, 0, 60, 34));
        KV_CHECK(c, rect_is(x.r(label), 0, 34, 25, 12) && rect_is(x.r(body), 35, 34, 65, 24)); // aa bb cc dd / ee ff gg
        KV_CHECK(c, rect_is(x.r(row), 0, 34, 100, 24) && rect_is(x.r(below), 0, 58, 25, 12));
        // Without a paragraph measure, text is one line, as before.
        x.layout();
        KV_CHECK(c, rect_is(x.r(t), 5, 5, 90, 12));
    }
    {   // line-height inherits, times the size.
        T x;
        u32 root = x.box(KROME_NONE);
        x.set(root, "size", KromeValue::num(10));
        x.set(root, "line-height", KromeValue::num(2));
        u32 t = x.text(root, "a\nb");
        x.wrap_layout();
        KV_CHECK(c, rect_is(x.r(t), 0, 0, 5, 40));
    }
}


// Placement by edge and offset in a stack (Krome slice 8, KR30), at three parent sizes.
void test_placement(kv::TestCtx& c) {
    using namespace kv;
    c.begin("krome placement");
    {   // The PRD's three: stretched between two edges, kept at a corner, at a third.
        T x;
        u32 root = x.box(KROME_NONE, "stack");
        x.set(root, "pad", KromeValue::num(4));
        u32 bar = x.box(root);
        x.set(bar, "l", KromeValue::num(10));
        x.set(bar, "r", KromeValue::num(10));
        x.set(bar, "h", KromeValue::num(6));
        x.set(bar, "w", KromeValue::num(999)); // ignored with both edges tied
        u32 corner = x.box(root);
        x.set(corner, "r", KromeValue::num(30));
        x.set(corner, "b", KromeValue::num(8));
        x.set(corner, "w", KromeValue::num(20));
        x.set(corner, "h", KromeValue::num(10));
        u32 third = x.box(root);
        x.set(third, "l", KromeValue::str("33.3%"));
        x.set(third, "w", KromeValue::num(5));
        x.set(third, "h", KromeValue::num(5));
        u32 near_end = x.box(root);
        x.set(near_end, "l", KromeValue::str("100%-40"));
        x.set(near_end, "t", KromeValue::str("50%+2"));
        x.set(near_end, "w", KromeValue::num(8));
        x.set(near_end, "h", KromeValue::num(8));
        const f32 sizes[3][2] = {{200, 100}, {408, 208}, {104, 64}};
        for (const auto& sz : sizes) {
            f32 W = sz[0], H = sz[1], iw = W - 8, ih = H - 8;
            x.layout(W, H);
            KV_CHECK(c, rect_is(x.r(bar), 14, 4, iw - 20, 6));
            KV_CHECK(c, rect_is(x.r(corner), 4 + iw - 30 - 20, 4 + ih - 8 - 10, 20, 10));
            KV_CHECK(c, rect_is(x.r(third), 4 + iw * 0.333f, 4, 5, 5));
            KV_CHECK(c, rect_is(x.r(near_end), 4 + iw - 40, 4 + ih * 0.5f + 2, 8, 8));
        }
    }
    {   // One edge on each axis with the anchor's place on the other; an offset moves it and
        // what is under it; edges that cross leave no width.
        T x;
        u32 root = x.box(KROME_NONE, "stack");
        x.flag(root, "center");
        x.flag(root, "middle");
        u32 a = x.box(root, "stack");
        x.set(a, "l", KromeValue::num(10));
        x.set(a, "w", KromeValue::num(20));
        x.set(a, "h", KromeValue::num(10));
        x.set(a, "x", KromeValue::num(5));
        x.set(a, "y", KromeValue::num(-3));
        u32 inner = x.box(a);
        x.set(inner, "w", KromeValue::num(4));
        x.set(inner, "h", KromeValue::num(4));
        u32 cross = x.box(root);
        x.set(cross, "l", KromeValue::str("60%"));
        x.set(cross, "r", KromeValue::str("60%"));
        x.set(cross, "h", KromeValue::num(2));
        x.layout(200, 100);
        KV_CHECK(c, rect_is(x.r(a), 15, 42, 20, 10));
        KV_CHECK(c, rect_is(x.r(inner), 15, 42, 4, 4));
        KV_CHECK(c, rect_is(x.r(cross), 120, 49, 0, 2));
        // Untied, it goes back to its anchor; the offset stays.
        x.set(a, "l", KromeValue::str("none"));
        x.layout(200, 100);
        KV_CHECK(c, rect_is(x.r(a), 95, 42, 20, 10));
    }
    {   // A hugging stack wants only what is not tied: here the 30 x 20 child.
        T x;
        u32 root = x.box(KROME_NONE);
        u32 st = x.box(root, "stack");
        u32 plain = x.box(st);
        x.set(plain, "w", KromeValue::num(30));
        x.set(plain, "h", KromeValue::num(20));
        u32 tied = x.box(st);
        x.set(tied, "r", KromeValue::num(0));
        x.set(tied, "b", KromeValue::num(0));
        x.set(tied, "w", KromeValue::num(90));
        x.set(tied, "h", KromeValue::num(70));
        u32 across = x.box(st);
        x.set(across, "l", KromeValue::num(2));
        x.set(across, "r", KromeValue::num(2));
        x.set(across, "h", KromeValue::num(40)); // only its width is tied: its height counts
        x.layout(200, 100);
        KV_CHECK(c, rect_is(x.r(st), 0, 0, 30, 40));
        KV_CHECK(c, rect_is(x.r(tied), -60, -30, 90, 70));
        KV_CHECK(c, rect_is(x.r(across), 2, 0, 26, 40));
    }
    {   // The words: what an edge takes and refuses.
        KromeEdge e;
        KV_CHECK(c, krome_parse_edge("10", &e) && e.dist == 10 && e.frac == 0);
        KV_CHECK(c, krome_parse_edge("25%", &e) && near(e.frac, 0.25f) && e.dist == 0);
        KV_CHECK(c, krome_parse_edge("100%-40", &e) && near(e.frac, 1.0f) && e.dist == -40);
        KV_CHECK(c, krome_parse_edge("50%+8.5", &e) && near(e.frac, 0.5f) && e.dist == 8.5f);
        KV_CHECK(c, !krome_parse_edge("", &e) && !krome_parse_edge("10px", &e) && !krome_parse_edge("50%-", &e) && !krome_parse_edge("50%*2", &e));
        T x;
        u32 root = x.box(KROME_NONE, "stack");
        KV_CHECK(c, x.t.set(root, "l", KromeValue::str("left")) != nullptr);
        KV_CHECK(c, x.t.set(root, "x", KromeValue::str("5%")) != nullptr);
        KV_CHECK(c, x.t.set(root, "l", KromeValue::num(3), KromeState::Hover) != nullptr);
    }
}
