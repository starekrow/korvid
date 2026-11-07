// example_testbed: the entry point. The game is the example_testbed module (game.cc); the loop, window,
// renderer and options are the frameloop module's. This file is the config.
#include "korvid/frameloop/frameloop.h"

namespace {
constinit const kv::FrameLoopConfig k_config = {
    .title  = "korvid testbed",
    .width  = 960,
    .height = 540,
};
}

int main(int argc, char** argv) {
    return kv::frameloop_main(argc, argv, k_config);
}
