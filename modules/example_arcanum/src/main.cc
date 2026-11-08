// example_arcanum: the entry point. The game is the module (game.cc); the loop, the window,
// the renderer and the options are the frame loop's.
#include "korvid/frameloop/frameloop.h"

namespace {
constinit const char* const k_flags[] = {"--demo"}; // a scripted walk, menu and level switch, for reproducible runs
constinit const kv::FrameLoopConfig k_config = {
    .title  = "korvid arcanum",
    .width  = 1280,
    .height = 720,
    .exit_on_escape = false, // Escape opens the menu
    .clear_color = {0.0f, 0.0f, 0.0f, 1.0f},
    .flags  = k_flags,
};
}

int main(int argc, char** argv) {
    return kv::frameloop_main(argc, argv, k_config);
}
