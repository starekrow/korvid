#include "korvid/tool/tool.h"

int main(int argc, char** argv) {
    kv::ToolConfig config;
    config.name = "kpack";
    return kv::tool_main(argc, argv, kv_module_list(), config);
}
