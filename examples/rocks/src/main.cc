// example_rocks: the entry point. The game is the module (game.cc); the loop, window,
// renderer and options are the frameloop module's. This file is the config.
#include "korvid/frameloop/frameloop.h"

namespace {
constinit const char* const k_flags[] = {"--demo"}; // a scripted ship, for reproducible runs
// The game lives in its own heap: a warning past 16 MB, a refusal past 64 MB, both
// naming the module and category that asked.
constinit const kv::HeapDesc       k_heaps[]        = {{"game", 16u << 20, 64u << 20}};
constinit const kv::ModuleHeapDesc k_module_heaps[] = {{"example_rocks", "game"}};
constinit const kv::FrameLoopConfig k_config = {
    .title  = "korvid rocks",
    .width  = 1280,
    .height = 720,
    .exit_on_escape = false, // Escape pauses; Q quits
    .touchpad_layout = "example_rocks/touchpad.json", // A thrusts, B fires
    .flags  = k_flags,
    .engine = {.heaps = k_heaps, .module_heaps = k_module_heaps},
};
}

int main(int argc, char** argv) {
    return kv::frameloop_main(argc, argv, k_config);
}
