// Korvid platform_hid: a window's shape, grips and bounds, the part every backend shares.
// HostWindow keeps what the program gave; the backend applies what the window holds now
// (hid_window_shape, _grips, _bounds), at the call and again whenever its size or scale
// makes the kept copy mean something new.
#include "korvid/platform_hid/host_window.h"
#include "korvid/core/log.h"
#include "hid_backend.h"
#include <string.h>

namespace kv {

bool HostWindow::shape(const u8* alpha, u32 w, u32 h) {
    if (!native_) return false;
    shape_.clear();
    shape_w_ = shape_h_ = 0;
    if (alpha && w && h) {
        memcpy(shape_.push_n(w * h), alpha, (usize)w * h);
        shape_w_ = w;
        shape_h_ = h;
    }
    return hid_window_shape(*this);
}

bool HostWindow::grips(const WindowGripRect* list, u32 count) {
    if (!native_) return false;
    if (count > KV_WINDOW_GRIPS) {
        static bool told = false;
        if (!told) KV_WARN("window", "%u grips given, %u kept: the rest are not the OS's", count, KV_WINDOW_GRIPS);
        told = true;
        count = KV_WINDOW_GRIPS;
    }
    if (count) memcpy(grips_, list, (usize)count * sizeof *list);
    grip_count_ = count;
    return hid_window_grips(*this);
}

WindowGrip HostWindow::grip_at(f32 x, f32 y) const {
    for (u32 i = grip_count_; i-- > 0;) {
        const rect2& r = grips_[i].rect;
        if (x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h) return grips_[i].grip;
    }
    return WindowGrip::None;
}

void HostWindow::bounds(u32 min_w, u32 min_h, u32 max_w, u32 max_h) {
    bounds_[0] = min_w;
    bounds_[1] = min_h;
    bounds_[2] = max_w;
    bounds_[3] = max_h;
    if (native_) hid_window_bounds(*this);
}

void hid_window_parts_reset(HostWindow& w) {
    w.shape_.free();
    w.shape_w_ = w.shape_h_ = 0;
    w.grip_count_ = 0;
    for (u32& b : w.bounds_) b = 0;
    w.bounds_scale_ = 0.0f;
    w.transparent_ = false;
}

} // namespace kv
