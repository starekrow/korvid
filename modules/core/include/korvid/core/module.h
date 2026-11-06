// Korvid core: module registration record. Modules are data, not static initializers.
// The build generates the module list (hello/src/modules.gen.cc stands in by hand).
#pragma once
#include "korvid/core/types.h"

namespace kv {

struct Engine;

struct ModuleDesc {
    const char* name;
    bool (*startup)(Engine& engine);
    void (*shutdown)(Engine& engine);
};

} // namespace kv

// Defined by the generated module list of the executable being built, in startup order.
kv::Span<const kv::ModuleDesc> kv_module_list();
