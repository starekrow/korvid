#include "korvid/goldberg/module.h"
#include "korvid/tool/tool.h"
#include <string.h>

int main(int argc, char** argv) {
    // With --window, the frame loop and every module; without, the tool loop and only the
    // modules a headless goldberg is: the window's (platform_hid, the renderers, Krome, the
    // frame loop) are linked and never started, so a cook runs where there is no display.
    if (kv::goldberg_wants_window(argc, argv)) return kv::goldberg_window_main(argc, argv);
    static const kv::ModuleDesc* headless[16];
    kv::u32 n = 0;
    for (const kv::ModuleDesc* m : kv_module_list()) {
        const char* name = m->name;
        if (!strcmp(name, "goldberg")) m = &kv::goldberg_headless_module;
        if (!strcmp(name, "platform") || !strcmp(name, "cli") || !strcmp(name, "tool") || !strcmp(name, "goldberg"))
            if (n < 16) headless[n++] = m;
    }
    kv::ToolConfig config;
    config.name = "goldberg";
    config.session = "watch";
    return kv::tool_main(argc, argv, kv::Span<const kv::ModuleDesc* const>(headless, n), config);
}
