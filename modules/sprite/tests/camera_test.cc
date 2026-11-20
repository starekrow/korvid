// camera_resolve: a followed entity at the frame's alpha between its two placements; the
// clamp inside bounds, and centering when the view is larger; the pixel snap at an even
// and an odd window width; a stale follow.
#include "korvid/sprite/camera.h"
#include "korvid/sprite/module.h"
#include "korvid/sprite/transform2.h"
#include "korvid/core/engine.h"
#include "korvid/core/test.h"
#include "korvid/math/scalar.h"

namespace {

using namespace kv;
bool near(f32 a, f32 b) { return kv::fabs(a - b) < 1e-4f; }

constinit const KTableDesc k_ktables[] = {{.name = ".mover", .components = "Transform2", .capacity = 4}};
KV_MODULE_DEFINE(k_ct) = {.name = "ct", .ktables = k_ktables};

} // namespace

void test_camera(kv::TestCtx& c) {
    using namespace kv;
    c.begin("camera");
    const ModuleDesc* const list[] = {&sprite_module, &k_ct};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    KTable* t = engine_ktable(e, "ct.mover");
    Handle h = t->spawn(transform2_at(point2{100, 40}));
    *t->prev<Transform2>(h) = transform2_at(point2{90, 40});

    Camera2 cam;
    cam.follow = h;
    Camera2 r = camera_resolve(e, cam, 640, 360, 0.25f);
    KV_CHECK(c, near(r.center.x, 92.5f) && near(r.center.y, 40)); // a quarter of the way from 90 to 100
    cam.offset = vec2{0, 10};
    KV_CHECK(c, near(camera_resolve(e, cam, 640, 360, 1.0f).center.y, 50));
    cam.offset = vec2{0, 0};

    // Bounds: a 1000x200 world, a 640x360 window at zoom 2 showing 320x180 world units.
    cam.zoom   = 2.0f;
    cam.bounds = rect2{0, 0, 1000, 200};
    r = camera_resolve(e, cam, 640, 360, 1.0f);
    KV_CHECK(c, near(r.center.x, 160) && near(r.center.y, 90)); // both clamped: half the view in from the corner
    *t->get<Transform2>(h) = transform2_at(point2{990, 190});
    *t->prev<Transform2>(h) = transform2_at(point2{990, 190});
    r = camera_resolve(e, cam, 640, 360, 0.5f);
    KV_CHECK(c, near(r.center.x, 1000 - 160) && near(r.center.y, 200 - 90));
    cam.bounds = rect2{0, 0, 100, 1000}; // narrower than the view: centered on x
    KV_CHECK(c, near(camera_resolve(e, cam, 640, 360, 0.5f).center.x, 50));

    // Pixel snap: the view's corner on a world pixel (half a unit at zoom 2).
    cam.bounds = rect2{};
    cam.pixel_snap = true;
    *t->get<Transform2>(h) = transform2_at(point2{10.3f, 7.8f});
    *t->prev<Transform2>(h) = transform2_at(point2{10.3f, 7.8f});
    r = camera_resolve(e, cam, 640, 360, 1.0f);
    f32 left = r.center.x - 160.0f, bottom = r.center.y - 90.0f;
    KV_CHECK(c, near(left * 2.0f, kv::round(left * 2.0f)) && near(bottom * 2.0f, kv::round(bottom * 2.0f)) && kv::fabs(r.center.x - 10.3f) <= 0.25f);
    r = camera_resolve(e, cam, 641, 361, 1.0f); // odd: the center sits on a half pixel so the corner does not
    left = r.center.x - 641.0f / 4.0f;
    KV_CHECK(c, near(left * 2.0f, kv::round(left * 2.0f)));

    // A stale follow leaves the center alone.
    t->destroy(h);
    cam.pixel_snap = false;
    cam.center = point2{5, 6};
    r = camera_resolve(e, cam, 640, 360, 1.0f);
    KV_CHECK(c, near(r.center.x, 5) && near(r.center.y, 6));
    engine_shutdown(e);
}
