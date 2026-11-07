// Hand-written stand-in for the build-generated module list.
#include "korvid/core/module.h"
#include "korvid/platform/module.h"
#include "korvid/renderer-vulkan/module.h"
#include "korvid/asset/module.h"
#include "korvid/sprite/module.h"
#include "korvid/frameloop/module.h"
#include "korvid/example_stones/module.h"

namespace {
constinit const kv::ModuleDesc* const k_modules[] = {
    &kv::platform_module,
    &kv::renderer_vulkan_module,
    &kv::asset_module,
    &kv::sprite_module,
    &kv::frameloop_module,
    &kv::example_stones_module,
};
}

kv::Span<const kv::ModuleDesc* const> kv_module_list() {
    return kv::Span<const kv::ModuleDesc* const>(k_modules, KV_COUNTOF(k_modules));
}
