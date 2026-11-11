// Module record. Startup brings up the display (the window's layer comes through SDL's video
// driver) and checks that the system has a Metal device that can do what the renderer asks
// (Apple7 or later: argument buffers tier 2, base instance in indirect draws); the renderer
// itself is created by renderer_create() once a window exists. No events, resources or
// systems: the renderer runs at frame rate from the frame loop.
#include "korvid/renderer-metal/module.h"
#include "korvid/render/renderer.h"
#include "korvid/core/log.h"
#include "korvid/platform_hid/module.h"
#import <Metal/Metal.h>

namespace kv {

namespace {

bool renderer_metal_startup(Engine& e, ModuleInstance& self) {
    if (!platform_video_up(e)) return false;
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { KV_ERROR("metal", "no Metal device"); return false; }
        bool ok = [dev supportsFamily:MTLGPUFamilyApple7] && dev.argumentBuffersSupport == MTLArgumentBuffersTier2;
        KV_INFO("metal", "device '%s'%s", dev.name.UTF8String, ok ? "" : ": not Apple7 with tier 2 argument buffers, which the renderer needs");
        [dev release];
        if (!ok) return false;
    }
    RendererMetalState* s = arena_push<RendererMetalState>(self.arena);
    if (!s) return false;
    s->general = &self.general;
    self.set_state(s);
    return true;
}

void renderer_metal_shutdown(Engine&, ModuleInstance& self) {
    RendererMetalState* s = self.state_as<RendererMetalState>();
    if (s && s->renderer) renderer_destroy(s->renderer); // clears s->renderer
}

} // namespace

KV_MODULE_DEFINE(renderer_metal_module) = {
    .name     = "renderer-metal",
    .startup  = renderer_metal_startup,
    .shutdown = renderer_metal_shutdown,
    .memory   = {.reserve = 64 * 1024},
};

} // namespace kv
