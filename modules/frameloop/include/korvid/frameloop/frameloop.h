// Korvid frameloop: what every game needs and no game decides. A game declares a
// constinit FrameLoopConfig and calls frameloop_main from its main(); everything
// mechanical happens here. The game learns that someone wants out through the
// ExitRequested event and decides what to do about it; the loop ends when something
// calls engine_request_quit.
//
// One frame: platform_pump, engine_frame (the Sim ticks owed at 60 Hz, at most four),
// engine_work (the Work cycle, in the slack the renderer measured, or until the frame's
// deadline while work is pending), engine_frame_cycle, then the render, interpolated by
// the clock's alpha. Pacing (below) decides whether a frame is drawn at all.
//
// The command line overrides the config: --size, --no-vsync, --renderer, --pacing,
// --threads, --seed, --rate, --early-reads, --hash, --record, --replay, --profile, --screenshot,
// the CLI's transports, the mounts and the rest. modules/frameloop/README.md has every
// option; docs/guides/frame-loop-and-scheduling.md walks through a frame and a tick.
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
// Decoupled: ticks and frames decided apart. Ticks at the clock's rate while a module asks
// (engine_want_ticks), while input, I/O, a load, a waker or Work is pending, and four times
// a second; frames only when something to draw changed (engine_want_frame, input, the
// window, a load, sprites a tick moved), never on the heartbeat, never while the window is
// hidden. Between ticks the loop sleeps till the next is due. For a program whose sim
// follows a device (a music player) in a window nobody may be looking at.
enum class Pacing : u8 { Continuous, OnDemand, Decoupled };

// The game's declaration: what frameloop_main opens and how it runs. Every field has a
// default, so a config names only what it changes; the command line overrides some.
struct FrameLoopConfig {
    const char* title      = "korvid";
    u32         width      = 1280;
    u32         height     = 720;
    bool        vsync      = true;
    bool        borderless = false;
    // --transparent: a window with an alpha channel and no frame (WindowDesc::transparent).
    // Where it composites, the loop clears each frame to nothing, whatever clear_color says,
    // and the renderer presents the frame's alpha.
    bool        transparent = false;
    bool        validation = KV_DEBUG != 0;
    RendererBackend renderer = RendererBackend::Auto; // --renderer auto|gpu|cpu: which bound backend draws
    bool        cpu_shaders = true; // --cpu-shaders all|none: the CPU renderer runs game shaders (RendererDesc::cpu_shaders)
    // The CPU renderer's window pixels per pixel it draws, each way (RendererDesc::cpu_scale);
    // --cpu-scale auto|N overrides it. 0 (auto): the window's units on a 2x or 3x display, a
    // game's choice: its frames redraw most of the window. 1: every pixel, a tool's: sharp
    // text, and its frames cost only what changed.
    u32         cpu_scale   = 0;
    bool        exit_on_close  = true;  // the main window's CloseRequested -> ExitRequested
    bool        exit_on_escape = true;  // Escape held    -> ExitRequested
    f64         run_seconds    = -1.0;  // >= 0: ExitRequested after this long (smoke tests)
    u64         run_ticks      = 0;     // > 0: ExitRequested at this sim tick: the same length on any machine
    color       clear_color{0.09f, 0.09f, 0.11f, 1.0f};
    u32         report_every_seconds = 1; // fps line; 0 is silent
    u32         sprite_rows = 0;          // the renderer's sprite tables' first size (0: its default); a game that knows it draws many asks
    const char* touchpad_layout = nullptr; // the on-screen pad's layout (JSON, through the mounts); null: touchpad/default.json
    Span<const char* const> flags = {};   // boolean flags the game accepts ("--demo"); frameloop_flag says which were given
    Span<const char* const> options = {}; // flags the game accepts that take a value ("--dial"), each as often as it likes; frameloop_option
    Span<const char* const> recorded_actions = {}; // the actions --record keeps and --replay drives: the gameplay ones, not pause or quit
    EngineConfig engine = {};             // heaps and which module lives in which
    Pacing      pacing = Pacing::Continuous;  // --pacing demand|continuous|decoupled
};

// Why someone wants out. frameloop.exit (PreSim, Always) emits one ExitRequested per
// reason per tick, paused or not; the game quits with engine_request_quit when it is
// ready, or ignores it. Handle it in a handler or an Always system, so a pause cannot
// hold it.
enum class ExitReason : u8 {
    WindowClosed, // the main window's CloseRequested arrived and exit_on_close is set
    Escape,       // Escape is held and exit_on_escape is set
    Timer,        // --run-seconds or --run-ticks (run_seconds, run_ticks) reached; once
};
struct ExitRequested { ExitReason reason; };

// Parses argv over `config`, runs the engine and the loop, returns the process exit code:
// 0, 2 for a bad option, 1 for a failed engine_init. Call it once, from main.
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
