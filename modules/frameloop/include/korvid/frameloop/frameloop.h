// Korvid frameloop: what every game needs and no game decides. A game declares a
// constinit FrameLoopConfig and calls frameloop_main from its main(); everything
// mechanical happens here. The game learns that someone wants out through the
// ExitRequested event and decides what to do about it; the loop ends when something
// calls engine_request_quit.
//
// Command line, overriding the config: --run-seconds N, --quiet, --validation,
// --no-validation, --no-vsync, --renderer auto|gpu|cpu, --borderless, --hitch-ms N, --hash (log the state hash
// after every tick), --hash-systems (and after every system), --trace-events (print every
// delivered event with its emitter; dev builds), --threads N (main thread included; 1 is
// serial; the default is every core), --rate X (the developer's rate from the start, as the
// CLI's `rate X`: a recorded run or a demo four times faster where nothing can type to the
// CLI, an app on a phone), --early-reads order|snapshot|kind (every kind's
// early reads ordered, every kind's from a snapshot, or as each kind says, the default:
// the game is the same, the schedule is not), --profile FILE (a Chrome trace of every zone, mark
// and counter), --prof-report (the top zones with every fps line); --hitch-ms also dumps
// the hitching frame's zone tree. --screenshot FILE writes one presented frame as a binary
// PPM, frame 90 unless --screenshot-frame N says otherwise; --check-sort reads the GPU
// sort back every 30th frame and verifies it. --cli-port N listens for CLI sessions on
// loopback; --cli-stdin reads commands from stdin even when it is a terminal (it always
// does when stdin is not one). The Work cycle (engine_work, docs/design/cycles.md) gets the
// time the renderer blocked for last frame less --idle-margin-ms (1.5 by default), or with
// work pending until the frame's deadline; --no-idle gives it one cycle a frame and no more.
// --capacity KIND=N (repeatable) sets a kind's capacity over the game's (EngineConfig);
// --sprite-rows N the renderer's first sprite table size over the config's. The touchpad
// (an on-screen gamepad) is on where the screen is touched; --touchpad turns it on with
// the mouse as a finger, for trying a layout on the desktop.
#pragma once
#include "korvid/core/engine.h"
#include "korvid/math/color.h"
#include "korvid/render/renderer.h"

namespace kv {

struct Engine;

// Continuous: a frame every vsync, as a game wants. OnDemand: a frame when something
// happened (input, the window, I/O, a load, ticks with sprites in them, engine_want_frame), and between
// them no ticks and no drawing, the platform polled every few milliseconds and the loop
// woken at least four times a second (timers, the CLI). For a tool's window.
enum class Pacing : u8 { Continuous, OnDemand };

struct FrameLoopConfig {
    const char* title      = "korvid";
    u32         width      = 1280;
    u32         height     = 720;
    bool        vsync      = true;
    bool        borderless = false;
    bool        validation = KV_DEBUG != 0;
    RendererBackend renderer = RendererBackend::Auto; // --renderer auto|gpu|cpu: which bound backend draws
    bool        exit_on_close  = true;  // CloseRequested -> ExitRequested
    bool        exit_on_escape = true;  // Escape held    -> ExitRequested
    f64         run_seconds    = -1.0;  // >= 0: ExitRequested after this long (smoke tests)
    color       clear_color{0.09f, 0.09f, 0.11f, 1.0f};
    u32         report_every_seconds = 1; // fps line; 0 is silent
    u32         sprite_rows = 0;          // the renderer's sprite tables' first size (0: its default); a game that knows it draws many asks
    const char* touchpad_layout = nullptr; // the on-screen pad's layout (JSON, through the mounts); null: touchpad/default.json
    Span<const char* const> flags = {};   // boolean flags the game accepts ("--demo"); frameloop_flag says which were given
    Span<const char* const> options = {}; // flags the game accepts that take a value ("--dial"), each as often as it likes; frameloop_option
    Span<const char* const> recorded_actions = {}; // the actions --record keeps and --replay drives: the gameplay ones, not pause or quit
    EngineConfig engine = {};             // heaps and which module lives in which
    Pacing      pacing = Pacing::Continuous;  // --pacing demand|continuous
};

enum class ExitReason : u8 { WindowClosed, Escape, Timer };
struct ExitRequested { ExitReason reason; };

// Parses argv over `config`, runs the engine and the loop, returns the process exit code.
int frameloop_main(int argc, char** argv, const FrameLoopConfig& config);

// The configuration in effect (config plus command line), once the module is up.
const FrameLoopConfig& frameloop_config(Engine& e);
// Whether one of the game's declared flags was on the command line.
bool frameloop_flag(Engine& e, const char* flag);
// Another frame, please (engine_want_frame): for code that does not see the engine's header.
inline void frameloop_request_frame(Engine& e) { engine_want_frame(e); }
// The value given after the `index`th occurrence of one of the game's declared options
// ("--dial bullets=2" twice is index 0 and 1), or null when there are not that many.
const char* frameloop_option(Engine& e, const char* option, u32 index = 0);

// Recording and replay (--record FILE, --replay FILE; replay.cc has the file). Whether a
// replay is driving the recorded actions (a game's own pilots stand down while it does);
// the game's arguments that start the run it is about to record, when a menu chose it
// rather than the command line ("--arcade --difficulty hard"), for the recording's header.
bool frameloop_replaying(Engine& e);
void frameloop_record_note(Engine& e, const char* args);

// For a tool that drives the loop itself: the config the module will use, set before
// engine_init, and one frame: pump the window, advance the engine by dt, render once.
void frameloop_set_config(const FrameLoopConfig& config);
void frameloop_step(Engine& e, f64 dt);

} // namespace kv
