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

#ifndef KV_BIND_RENDER_COUNT
#define KV_BIND_RENDER_COUNT 1
#endif

namespace kv {

bool g_metal_ready = false;

namespace {

// Without a Metal device that can draw, the module fails, and the engine with it, unless the
// build binds another backend to fall back on (renderer-cpu): then it starts, empty, and
// renderer_probe says no. The iOS Simulator is the case: its GPU is an Apple2.
bool unavailable(const char* why) {
    if (KV_BIND_RENDER_COUNT < 2) return false;
    KV_INFO("metal", "unavailable (%s): another renderer will draw", why);
    return true;
}

bool renderer_metal_startup(Engine& e, ModuleInstance& self) {
    if (!platform_video_up(e)) return false;
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { KV_ERROR("metal", "no Metal device"); return unavailable("no Metal device"); }
        bool ok = [dev supportsFamily:MTLGPUFamilyApple7] && dev.argumentBuffersSupport == MTLArgumentBuffersTier2;
        // The iOS Simulator's GPU is an Apple2: no indirect draws or dispatches, which the GPU
        // sort and the draws are; it is refused here, as an A13 is.
        int apple = 0; // the highest Apple family it reports, for the log
        for (int f = 1; f <= 10; ++f)
            if ([dev supportsFamily:(MTLGPUFamily)((int)MTLGPUFamilyApple1 + f - 1)]) apple = f;
        KV_INFO("metal", "device '%s': Apple%d, Metal3 %s, argument buffers tier %d%s", dev.name.UTF8String, apple,
                [dev supportsFamily:MTLGPUFamilyMetal3] ? "yes" : "no", dev.argumentBuffersSupport == MTLArgumentBuffersTier2 ? 2 : 1,
                ok ? "" : "; the renderer needs Apple7 with tier 2");
        [dev release];
        if (!ok) return unavailable("the GPU is older than Apple7 with tier 2 argument buffers");
    }
    RendererMetalState* s = arena_push<RendererMetalState>(self.arena);
    if (!s) return false;
    s->general = &self.general;
    self.set_state(s);
    g_metal_ready = true;
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
