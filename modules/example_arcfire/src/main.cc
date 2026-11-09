// example_arcfire: the entry point. The game is the module (module.cc and the files it
// names); the loop, the window, the renderer and the options are the frame loop's.
#include "korvid/frameloop/frameloop.h"

namespace {
// --demo plays the scripted minute and quits at the stage's end; --autoplay puts the bot
// on the stick at Normal; --stress turns every dial up, the ship invulnerable, the stage
// looping. --dial name=value moves one dial (repeatable).
constinit const char* const k_flags[] = {"--demo", "--autoplay", "--stress"};
constinit const char* const k_options[] = {"--dial"};
constinit const kv::FrameLoopConfig k_config = {
    .title  = "korvid arcfire",
    .width  = 1280,
    .height = 720,
    .exit_on_escape = false, // Escape pauses
    .clear_color = {0.0f, 0.0f, 0.0f, 1.0f},
    .flags   = k_flags,
    .options = k_options,
};
}

int main(int argc, char** argv) {
    return kv::frameloop_main(argc, argv, k_config);
}
