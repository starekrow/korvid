#include "korvid/math/math.h"
#include "korvid/core/test.h"

// point + point must not compile. Checked with a constrained template, not with a build
// that is expected to fail: has_point_sum<point2> is true only if the operator exists.
template <class P, class = void>
struct has_point_sum { static constexpr bool value = false; };
template <class P>
struct has_point_sum<P, decltype(void(P{} + P{}))> { static constexpr bool value = true; };
static_assert(!has_point_sum<kv::point2>::value, "point + point must not compile");
static_assert(has_point_sum<kv::vec2>::value, "vec + vec must compile");

void test_vec2(kv::TestCtx& c) {
    using namespace kv;
    c.begin("vec2");

    vec2 a{1, 2}, b{3, -4};
    KV_CHECK(c, a + b == vec2{4, -2});
    KV_CHECK(c, b - a == vec2{2, -6});
    KV_CHECK(c, a * 2.0f == vec2{2, 4});
    KV_CHECK(c, dot(a, b) == -5.0f);
    KV_CHECK(c, length(b) == 5.0f);
    KV_CHECK(c, nearly(length(normalize(b)), 1.0f));
    KV_CHECK(c, normalize(vec2{}) == vec2{});
    KV_CHECK(c, lerp(a, b, 0.5f) == vec2{2, -1});

    point2 p{10, 10}, q{13, 14};
    KV_CHECK(c, q - p == vec2{3, 4});
    KV_CHECK(c, p + vec2{3, 4} == q);
    KV_CHECK(c, q - vec2{3, 4} == p);
    KV_CHECK(c, distance(p, q) == 5.0f);
    KV_CHECK(c, lerp(p, q, 1.0f) == q);

    rect2 r{0, 0, 10, 5};
    KV_CHECK(c, rect_center(r) == point2{5, 2.5f});
    KV_CHECK(c, rect_contains(r, point2{10, 5}));
    KV_CHECK(c, !rect_contains(r, point2{10.1f, 5}));
    KV_CHECK(c, rect_overlaps(r, rect2{9, 4, 3, 3}));
    KV_CHECK(c, !rect_overlaps(r, rect2{10, 5, 3, 3}));

    color t = color_red() * color{0.5f, 0.5f, 0.5f, 1.0f};
    KV_CHECK(c, t == color{0.5f, 0, 0, 1});
}
