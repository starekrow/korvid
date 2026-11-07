// Korvid frameloop: what every game needs and no game decides. A game declares a
// constinit FrameLoopConfig and calls frameloop_main from its main(); everything
// mechanical happens here. The game learns that someone wants out through the
// ExitRequested event and decides what to do about it; the loop ends when something
// calls engine_request_quit.
//
// Command line, overriding the config: --run-seconds N, --quiet, --validation,
// --no-validation, --no-vsync, --borderless, --hitch-ms N, --hash (log the state hash
// after every tick), --hash-systems (and after every system), --trace-events (print every
// delivered event with its emitter; dev builds), --threads N (main thread included; 1 is
// serial; the default is every core).
#pragma once
#include "korvid/core/engine.h"
#include "korvid/math/color.h"

namespace kv {

struct Engine;

struct FrameLoopConfig {
    const char* title      = "korvid";
    u32         width      = 1280;
    u32         height     = 720;
    bool        vsync      = true;
    bool        borderless = false;
    bool        validation = KV_DEBUG != 0;
    bool        exit_on_close  = true;  // CloseRequested -> ExitRequested
    bool        exit_on_escape = true;  // Escape held    -> ExitRequested
    f64         run_seconds    = -1.0;  // >= 0: ExitRequested after this long (smoke tests)
    color       clear_color{0.09f, 0.09f, 0.11f, 1.0f};
    u32         report_every_seconds = 1; // fps line; 0 is silent
    Span<const char* const> flags = {};   // boolean flags the game accepts ("--demo"); frameloop_flag says which were given
    EngineConfig engine = {};             // heaps and which module lives in which
};

enum class ExitReason : u8 { WindowClosed, Escape, Timer };
struct ExitRequested { ExitReason reason; };

// Parses argv over `config`, runs the engine and the loop, returns the process exit code.
int frameloop_main(int argc, char** argv, const FrameLoopConfig& config);

// The configuration in effect (config plus command line), once the module is up.
const FrameLoopConfig& frameloop_config(Engine& e);
// Whether one of the game's declared flags was on the command line.
bool frameloop_flag(Engine& e, const char* flag);

// For a tool that drives the loop itself: the config the module will use, set before
// engine_init, and one frame: pump the window, advance the engine by dt, render once.
void frameloop_set_config(const FrameLoopConfig& config);
void frameloop_step(Engine& e, f64 dt);

} // namespace kv
