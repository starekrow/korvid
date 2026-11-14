// Module record. Nothing to load at startup: d3d12.dll and dxgi.dll are the system's, linked
// through their import libraries. The renderer is created by renderer_create() once a
// window exists. No events, resources or systems: the renderer runs at frame rate from the
// frame loop.
#include "korvid/renderer-d3d12/module.h"
#include "korvid/render/renderer.h"
#include "korvid/core/arena.h"
#include "korvid/platform_hid/module.h"

namespace kv {

namespace {

bool renderer_d3d12_startup(Engine& e, ModuleInstance& self) {
    if (!platform_video_up(e)) return false; // the window comes from the platform's video
    RendererD3d12State* s = arena_push<RendererD3d12State>(self.arena);
    if (!s) return false;
    s->general = &self.general;
    self.set_state(s);
    return true;
}

void renderer_d3d12_shutdown(Engine&, ModuleInstance& self) {
    RendererD3d12State* s = self.state_as<RendererD3d12State>();
    if (s && s->renderer) renderer_destroy(s->renderer); // clears s->renderer
}

} // namespace

KV_MODULE_DEFINE(renderer_d3d12_module) = {
    .name     = "renderer-d3d12",
    .startup  = renderer_d3d12_startup,
    .shutdown = renderer_d3d12_shutdown,
    .memory   = {.reserve = 64 * 1024},
};

} // namespace kv
