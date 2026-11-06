// Korvid renderer-vulkan: module record and state. Startup checks that the Vulkan loader
// is present and loads the global entry points. The renderer is created on request by
// renderer_create() once a window exists, owned here, and destroyed at module shutdown.
#pragma once
#include "korvid/core/module.h"

namespace kv {

struct Renderer;

KV_MODULE_DECLARE(renderer_vulkan_module);

struct RendererVulkanState {
    Renderer* renderer = nullptr; // one per process: the entry point tables are global
};

} // namespace kv
