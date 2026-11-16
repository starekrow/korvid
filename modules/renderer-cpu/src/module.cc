// Module record. Nothing to find at start-up: the CPU is always there. The renderer itself is
// made by renderer_create() when render's dispatcher chooses this backend.
#include "korvid/renderer-cpu/module.h"
#include "renderer_internal.h"
#include "korvid/core/arena.h"

namespace kv {

namespace {

bool renderer_cpu_startup(Engine&, ModuleInstance& self) {
    RendererCpuState* s = arena_push<RendererCpuState>(self.arena);
    if (!s) return false;
    s->general = &self.general;
    self.set_state(s);
    return true;
}

void renderer_cpu_shutdown(Engine&, ModuleInstance& self) {
    RendererCpuState* s = self.state_as<RendererCpuState>();
    // Its own, not render's dispatcher's: a program with this backend alone (its tests) links no other.
    if (s && s->renderer) rb_cpu::renderer_destroy(reinterpret_cast<rb_cpu::Renderer*>(s->renderer)); // clears s->renderer
}

} // namespace

KV_MODULE_DEFINE(renderer_cpu_module) = {
    .name     = "renderer-cpu",
    .startup  = renderer_cpu_startup,
    .shutdown = renderer_cpu_shutdown,
    .memory   = {.reserve = 64 * 1024},
};

} // namespace kv
