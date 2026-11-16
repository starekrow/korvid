// renderer-cpu: what a game shader's generated glue file includes before Slang's C++ of the
// shader (shader_glue.inl has the file's shape). Slang pastes its CPU prelude into each
// shader, the host's C headers in it; included here first, at file scope, their guards keep
// the pasted copies out of the shader's namespace.
#pragma once
#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if __has_include(<alloca.h>)
#include <alloca.h>
#endif
#if __has_include(<strings.h>)
#include <strings.h>
#endif
#include "korvid/core/module.h"
#include "korvid/renderer-cpu/shader.h"

// The generated module that registers a shader's Slang form (shader.h's second choice) when
// the program starts: kv::<sym>_module, listed by kormake after the module that owns the shader.
#define KV_CPU_SHADER_MODULE(sym, stem, fn)                                                             \
    namespace kv {                                                                                       \
    extern const ModuleDesc sym##_module;                                                                \
    KV_MODULE_DEFINE(sym##_module) = {                                                                   \
        .name    = stem ".cpu",                                                                          \
        .startup = [](Engine&, ModuleInstance&) { renderer_cpu_shader_generated(stem, fn); return true; }, \
    };                                                                                                   \
    }
