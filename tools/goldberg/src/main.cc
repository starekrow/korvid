#include "korvid/tool/tool.h"

int main(int argc, char** argv) {
    kv::ToolConfig config;
    config.name = "goldberg";
    config.session = "watch";
    return kv::tool_main(argc, argv, kv_module_list(), config);
}
