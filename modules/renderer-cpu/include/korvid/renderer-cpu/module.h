// Korvid renderer-cpu: the CPU renderer's engine module. Its renderer is made by
// renderer_create (korvid/render/renderer.h) once render's dispatcher chooses it.
#pragma once
#include "korvid/core/module.h"

namespace kv {

struct Renderer;

KV_MODULE_DECLARE(renderer_cpu_module);

struct RendererCpuState {
    Renderer*  renderer = nullptr; // the public, opaque type; one per process
    Allocator* general  = nullptr; // the module's attributed general allocator
};

} // namespace kv
