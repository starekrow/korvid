// Hand-written stand-in for the build-generated module list.
// kormake writes this file into the build directory; until then it lives here.
#include "korvid/core/module.h"
#include "korvid/platform/module.h"

kv::Span<const kv::ModuleDesc> kv_module_list() {
    // Built on first call, not by a static initializer. Order is startup order.
    static const kv::ModuleDesc modules[] = {
        kv::platform_module(),
    };
    return kv::Span<const kv::ModuleDesc>(modules, KV_COUNTOF(modules));
}
