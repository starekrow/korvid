// Korvid renderer-metal: module record and state. Startup checks that the system has a
// Metal device. The renderer is created on request by renderer_create() once a window
// exists, owned here, and destroyed at module shutdown.
#pragma once
#include "korvid/core/module.h"

namespace kv {

struct Renderer;

KV_MODULE_DECLARE(renderer_metal_module);

struct RendererMetalState {
    Renderer*  renderer = nullptr; // one per process, as renderer-vulkan's
    Allocator* general  = nullptr; // the module's attributed general allocator
};

} // namespace kv
