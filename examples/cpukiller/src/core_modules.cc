// cpukiller's second module list: the core alone, for the runs without a window. The list
// with the window is kormake's (gen/cpukiller/modules.gen.cc, kv_module_list): it follows
// the platform's bindings, the render backend among them, which a list written here would
// have to name and keep in step by hand.
#include "korvid/core/module.h"
#include "korvid/cpukiller/module.h"

namespace {
constinit const kv::ModuleDesc* const k_core_modules[] = {&kv::cpukiller_module};
}

kv::Span<const kv::ModuleDesc* const> kv::cpukiller_core_modules() {
    return kv::Span<const kv::ModuleDesc* const>(k_core_modules, KV_COUNTOF(k_core_modules));
}
