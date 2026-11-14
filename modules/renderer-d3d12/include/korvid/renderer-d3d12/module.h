// Korvid renderer-d3d12: module record and state. Startup only claims the state; the
// renderer is created on request by renderer_create() once a window exists, owned here, and
// destroyed at module shutdown.
#pragma once
#include "korvid/core/module.h"

namespace kv {

struct Renderer;

KV_MODULE_DECLARE(renderer_d3d12_module);

struct RendererD3d12State {
    Renderer*  renderer = nullptr; // one per process, as with every backend
    Allocator* general  = nullptr; // the module's attributed general allocator
};

} // namespace kv
