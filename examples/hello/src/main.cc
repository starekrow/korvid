// hello: the entry point. The window, the renderer, the loop and the command line are the
// frameloop module's; this file is the config.
#include "korvid/frameloop/frameloop.h"

namespace {
constinit const kv::FrameLoopConfig k_config = {
    .title  = "hello, korvid",
    .width  = 960,
    .height = 540,
};
}

int main(int argc, char** argv) { return kv::frameloop_main(argc, argv, k_config); }
