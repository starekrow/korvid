// Korvid sprite: the camera. One per process, published by the sprite module as the
// resource "sprite.camera"; a system that moves it declares the write and reaches it with
// ctx.resource<Camera2>(...). The frame loop reads it at render rate and builds the world
// view from it: the window's pixel size divided by zoom, centered on `center`, rotated by
// `rotation`. Zoom 1 is one world unit per pixel. Screen-space sprites ignore it.
//
// Following. With `follow` set to an entity that has a Transform2, the frame loop centers
// the view on that entity's interpolated position (plus `offset`) at the same alpha the
// renderer draws the sprites with, so a followed sprite stands still on screen however
// the ticks and frames fall; then it clamps the view inside `bounds` (if it has a size;
// centered on an axis where the view is larger than the bounds), then, with `pixel_snap`,
// puts the view's corner on the world's pixel grid at the current zoom, so pixel art
// never shimmers or seams at an integer zoom. A stale `follow` leaves `center` where it
// was. The clamp ignores rotation.
#pragma once
#include "korvid/core/handle.h"
#include "korvid/math/math.h"

namespace kv {

struct Engine;

struct Camera2 {
    point2 center;            // the world point at the window's center
    f32    zoom     = 1.0f;   // pixels per world unit; 2 shows half as much, twice as large
    f32    rotation = 0.0f;   // radians, counter-clockwise: the camera's own rotation
    Handle follow;            // an entity with a Transform2, or null
    vec2   offset;            // added to the followed position
    rect2  bounds;            // world rect the view stays inside; w or h of 0: no clamp on that axis
    bool   pixel_snap = false;
};

// The world rect a camera shows in a window of `width` x `height` pixels, before rotation.
inline rect2 camera_view(const Camera2& cam, u32 width, u32 height) {
    f32 z = cam.zoom > 1e-6f ? cam.zoom : 1e-6f;
    vec2 size{(f32)width / z, (f32)height / z};
    return rect2_from_center(cam.center, size);
}

// The camera, or null if the sprite module is not started. For code outside the schedule.
Camera2* sprite_camera(Engine& e);

// The camera as a frame should draw it: `cam` with follow, bounds and pixel snap applied
// for a window of `width` x `height` pixels at interpolation point `alpha`. The frame
// loop calls it each frame; the result is not written back.
Camera2 camera_resolve(Engine& e, const Camera2& cam, u32 width, u32 height, f32 alpha);

} // namespace kv
