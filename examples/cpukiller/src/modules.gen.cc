// Two lists: with the window (the usual stack, in the order kormake gives testbed's,
// the same closure) and without (the core alone).
#include "korvid/core/module.h"
#include "korvid/platform/module.h"
#include "korvid/platform_hid/module.h"
// The render backend is the platform's binding (renderer-metal on the Apple family, the
// toolchains' bindings); this list is written by hand, so it says so here too.
#if defined(__APPLE__)
#include "korvid/renderer-metal/module.h"
#define KV_CPUKILLER_RENDERER kv::renderer_metal_module
#else
#include "korvid/renderer-vulkan/module.h"
#define KV_CPUKILLER_RENDERER kv::renderer_vulkan_module
#endif
#include "korvid/asset/module.h"
#include "korvid/sprite/module.h"
#include "korvid/scene/module.h"
#include "korvid/text/module.h"
#include "korvid/cli/module.h"
#include "korvid/touchpad/module.h"
#include "korvid/frameloop/module.h"
#include "korvid/cpukiller/module.h"

namespace {
constinit const kv::ModuleDesc* const k_window_modules[] = {
    &kv::platform_module,
    &kv::platform_hid_module,
    &KV_CPUKILLER_RENDERER,
    &kv::asset_module,
    &kv::sprite_module,
    &kv::scene_module, // the frame loop's: its overlay is text, which is scene's, and it serves the CLI
    &kv::text_module,
    &kv::cli_module,
    &kv::touchpad_module, // the frame loop's: the on-screen pad
    &kv::frameloop_module,
    &kv::cpukiller_module,
};
constinit const kv::ModuleDesc* const k_core_modules[] = {&kv::cpukiller_module};
}

kv::Span<const kv::ModuleDesc* const> kv_module_list() {
    return kv::Span<const kv::ModuleDesc* const>(k_window_modules, KV_COUNTOF(k_window_modules));
}

kv::Span<const kv::ModuleDesc* const> kv::cpukiller_core_modules() {
    return kv::Span<const kv::ModuleDesc* const>(k_core_modules, KV_COUNTOF(k_core_modules));
}
