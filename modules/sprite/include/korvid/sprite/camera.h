// Korvid sprite: the camera. One per process, published by the sprite module as the
// resource "sprite.camera"; a system that moves it declares the write and reaches it with
// ctx.resource<Camera2>(...). The frame loop reads it at render rate and builds the world
// view from it: the window's pixel size divided by zoom, centered on `center`, rotated by
// `rotation`. Zoom 1 is one world unit per pixel. Screen-space sprites ignore it.
#pragma once
#include "korvid/math/math.h"

namespace kv {

struct Engine;

struct Camera2 {
    point2 center;            // the world point at the window's center
    f32    zoom     = 1.0f;   // pixels per world unit; 2 shows half as much, twice as large
    f32    rotation = 0.0f;   // radians, counter-clockwise: the camera's own rotation
};

// The world rect a camera shows in a window of `width` x `height` pixels, before rotation.
inline rect2 camera_view(const Camera2& cam, u32 width, u32 height) {
    f32 z = cam.zoom > 1e-6f ? cam.zoom : 1e-6f;
    vec2 size{(f32)width / z, (f32)height / z};
    return rect2_from_center(cam.center, size);
}

// The camera, or null if the sprite module is not started. For code outside the schedule.
Camera2* sprite_camera(Engine& e);

} // namespace kv
