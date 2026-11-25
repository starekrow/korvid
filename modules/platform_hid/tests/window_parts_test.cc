// A window's grips as the backend reads them (HostWindow::grip_at): the last rect that holds
// a point answers, a rect of the program's (None) over a grip keeps the point the program's,
// and a window that is not open takes no shape and no grips.
#include "korvid/platform_hid/host_window.h"
#include "korvid/core/test.h"

void test_window_parts(kv::TestCtx& c) {
    using namespace kv;
    c.begin("window parts");
    HostWindow w;
    const u8 mask[4] = {255, 0, 0, 255};
    WindowGripRect one{rect2{0, 0, 10, 10}, WindowGrip::Move};
    KV_CHECK(c, !w.shape(mask, 2, 2) && !w.grips(&one, 1));
    KV_CHECK(c, w.grip_at(5, 5) == WindowGrip::None);

    // What grips() keeps, set as it keeps it: a title bar, a button in it, a corner.
    w.grips_[0] = WindowGripRect{rect2{0, 0, 300, 20}, WindowGrip::Move};
    w.grips_[1] = WindowGripRect{rect2{280, 2, 16, 16}, WindowGrip::None};
    w.grips_[2] = WindowGripRect{rect2{288, 188, 12, 12}, WindowGrip::SE};
    w.grip_count_ = 3;
    KV_CHECK(c, w.grip_at(10, 10) == WindowGrip::Move);
    KV_CHECK(c, w.grip_at(285, 10) == WindowGrip::None);
    KV_CHECK(c, w.grip_at(299.5f, 199.5f) == WindowGrip::SE);
    KV_CHECK(c, w.grip_at(300, 10) == WindowGrip::None); // the right edge is outside
    KV_CHECK(c, w.grip_at(150, 100) == WindowGrip::None);
}
