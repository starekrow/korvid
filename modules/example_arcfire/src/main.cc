// example_arcfire: the entry point. The game is the module (module.cc and the files it
// names); the loop, the window, the renderer and the options are the frame loop's.
#include "korvid/frameloop/frameloop.h"

namespace {
// Played plainly, the game opens on its title; --arcade goes straight into arcade (the
// stages in order, round again a difficulty up, ending after Hard), --stage NAME into one
// stage; --difficulty easy|normal|hard is where either starts. --again runs a run once more
// from a fresh sim when it ends, to compare the two. --demo plays the scripted minute and quits at the stage's
// end; --autoplay puts the bot on the stick; --stress turns every dial up, the ship
// invulnerable, the stage looping. --dial name=value moves one dial (repeatable). --bench
// NAME fires one pattern from a fixed emitter, again and again, for writing and measuring
// patterns.
constinit const char* const k_flags[] = {"--demo", "--autoplay", "--stress", "--arcade", "--again"};
constinit const char* const k_options[] = {"--dial", "--bench", "--stage", "--difficulty"};
constinit const kv::FrameLoopConfig k_config = {
    .title  = "korvid arcfire",
    .width  = 1280,
    .height = 720,
    .exit_on_escape = false, // Escape pauses
    .clear_color = {0.0f, 0.0f, 0.0f, 1.0f},
    .sprite_rows = 98304, // the bullet kinds and the particles at their capacities, and the rest: no growth mid-run
    .flags   = k_flags,
    .options = k_options,
};
}

int main(int argc, char** argv) {
    return kv::frameloop_main(argc, argv, k_config);
}
