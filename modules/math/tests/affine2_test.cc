#include "korvid/math/math.h"
#include "korvid/core/test.h"

void test_affine2(kv::TestCtx& c) {
    using namespace kv;
    c.begin("affine2");

    affine2 id = affine2_identity();
    point2 p{3, 4};
    vec2   v{1, 1};
    KV_CHECK(c, id * p == p);
    KV_CHECK(c, id * v == v);

    // Translation moves points and leaves vectors alone.
    affine2 t = affine2_translate(vec2{10, -2});
    KV_CHECK(c, t * p == point2{13, 2});
    KV_CHECK(c, t * v == v);

    // A quarter turn.
    affine2 r = affine2_rotate(KV_PI * 0.5f);
    KV_CHECK(c, nearly(r * vec2{1, 0}, vec2{0, 1}));
    KV_CHECK(c, nearly(r * point2{0, 1}, point2{-1, 0}));

    // Composition order: (t * r) rotates first, then translates.
    affine2 tr = t * r;
    KV_CHECK(c, nearly(tr * point2{1, 0}, point2{10, -1}));
    KV_CHECK(c, nearly(t * (r * point2{1, 0}), tr * point2{1, 0}));

    // TRS equals the product of its parts.
    affine2 trs = affine2_trs(point2{10, -2}, KV_PI * 0.5f, vec2{2, 3});
    KV_CHECK(c, nearly(trs, t * r * affine2_scale(vec2{2, 3})));
    KV_CHECK(c, nearly(trs * point2{1, 1}, point2{10 - 3, -2 + 2}));

    // Inverse round-trips, and m * inverse(m) is the identity.
    affine2 inv = inverse(trs);
    KV_CHECK(c, nearly(inv * (trs * p), p));
    KV_CHECK(c, nearly(trs * inv, id));
    KV_CHECK(c, nearly(inverse(id), id));
    KV_CHECK(c, nearly(inverse(affine2_scale(vec2{0, 1})), id)); // singular -> identity

    // Ortho: the view's corners land on the clip corners, with +y up on screen.
    rect2 view{-480, -270, 960, 540};
    affine2 o = ortho2d(view);
    KV_CHECK(c, nearly(o * point2{-480, -270}, point2{-1, 1}));  // bottom-left -> clip bottom-left
    KV_CHECK(c, nearly(o * point2{480, 270}, point2{1, -1}));    // top-right  -> clip top-right
    KV_CHECK(c, nearly(o * point2{0, 0}, point2{0, 0}));
    KV_CHECK(c, nearly(lerp(id, t, 0.5f).tx, 5.0f));
}
