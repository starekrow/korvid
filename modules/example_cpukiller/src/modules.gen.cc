// Two lists: with the window (the usual stack) and without (the core alone).
#include "korvid/core/module.h"
#include "korvid/platform/module.h"
#include "korvid/renderer-vulkan/module.h"
#include "korvid/asset/module.h"
#include "korvid/sprite/module.h"
#include "korvid/frameloop/module.h"
#include "korvid/example_cpukiller/module.h"

namespace {
constinit const kv::ModuleDesc* const k_window_modules[] = {
    &kv::platform_module,
    &kv::renderer_vulkan_module,
    &kv::asset_module,
    &kv::sprite_module,
    &kv::frameloop_module,
    &kv::example_cpukiller_module,
};
constinit const kv::ModuleDesc* const k_core_modules[] = {&kv::example_cpukiller_module};
}

kv::Span<const kv::ModuleDesc* const> kv_module_list() {
    return kv::Span<const kv::ModuleDesc* const>(k_window_modules, KV_COUNTOF(k_window_modules));
}

kv::Span<const kv::ModuleDesc* const> kv::cpukiller_core_modules() {
    return kv::Span<const kv::ModuleDesc* const>(k_core_modules, KV_COUNTOF(k_core_modules));
}
