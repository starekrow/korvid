// example_rocks: the entry point. The game is the module (game.cc); the loop, window,
// renderer and options are the frameloop module's. This file is the config.
#include "korvid/frameloop/frameloop.h"

namespace {
constinit const char* const k_flags[] = {"--demo"}; // a scripted ship, for reproducible runs
constinit const kv::FrameLoopConfig k_config = {
    .title  = "korvid rocks",
    .width  = 1280,
    .height = 720,
    .flags  = k_flags,
};
}

int main(int argc, char** argv) {
    return kv::frameloop_main(argc, argv, k_config);
}
