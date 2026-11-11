// Korvid platform_hid: the window, the keyboard, the mouse and gamepads: what a person
// at the machine touches and looks at. The backend is SDL3 today. Startup initializes
// SDL's event subsystem only; video and gamepads come up when a window opens (or when a
// renderer asks, platform_video_up), so a test with this module still runs without a
// display. The window is a service opened on request and closed at module shutdown, never
// by the caller; "input.sample" (phase Input) samples the keys into "input.state" once per
// tick. Both are published resources: a system declares "input.state" or
// "platform_hid.window" in its sets and reaches them with ctx.resource<const Input>() and
// ctx.resource<const HostWindow>(). The window's events reach the bus from platform_pump,
// through a pump hook.
#pragma once
#include "korvid/core/module.h"
#include "korvid/platform_hid/actions.h"
#include "korvid/platform_hid/host_window.h"
#include "korvid/platform_hid/input.h"

namespace kv {

struct Engine;

KV_MODULE_DECLARE(platform_hid_module);

// The module's state: for code outside the schedule (the frame loop). Systems reach the
// input and the window as resources.
// The CLI's `press`: an action held for a number of sampled ticks, as a person would.
struct HidPress {
    u32  action;
    u32  ticks;  // left
    vec2 axis;   // an Axis2's direction
};
constexpr u32 KV_HID_PRESSES = 8;

struct HidState {
    Input        input;    // raw at frame rate, sampled per tick
    HostWindow   window;   // one window per process; closed at module shutdown
    InputActions actions;  // what the game named, from the sampled input each tick
    u32          text_requests = 0; // platform_text_input's count
    char         text_override[KV_INPUT_TEXT_MAX] = {}; // input_text_override's, for the next sample
    u32          text_override_len = 0, text_override_backspaces = 0;
    HidPress     presses[KV_HID_PRESSES] = {}; // the CLI's, being held
    u32          press_count = 0;
    void       (*modal_frame)(void* user) = nullptr; // platform_modal_frame's
    void*        modal_user = nullptr;
    bool         in_modal_frame = false;
};

// Opens the window. Null if one is already open or SDL refused. Owned by the module.
HostWindow* platform_open_window(Engine& e, const WindowDesc& desc);
// The module starts headless. This brings up video and gamepads (once; they stay up
// until the module shuts down) for whatever needs a display before a window exists,
// such as a renderer loading the Vulkan loader. Opening a window calls it. False with a
// log line when there is no video driver.
bool        platform_video_up(Engine& e);
// The open window, or null. For code outside the schedule; systems use the resource.
HostWindow* platform_window(Engine& e);
// Asks the OS for typed text (InputState::text) while on; a text field turns it on while it
// has the focus. Counted: on twice needs off twice.
void platform_text_input(Engine& e, bool on);
// Types text (and Backspaces) as if from the keyboard, into the next sample: for demo
// scripts and tests.
void input_text_override(Engine& e, const char* text, u32 backspaces = 0);
// The module's input, or null if the module is not started. Likewise.
Input*      platform_input(Engine& e);
// Some OSes hold the main thread in a loop of their own while the window is resized (macOS:
// a live resize runs until the mouse is released), so platform_pump does not return and no
// frame runs for the length of the drag. While such a loop lasts, fn(user) is called about
// every 4 ms from inside platform_pump, after the window's queued events have reached the
// bus: a frame loop runs a frame there (engine_frame, render) and must not call
// platform_pump, which is still running further up the stack. The I/O queue is not pumped
// meanwhile. Null turns it off. On Windows the window has a thread of its own and fn is
// never called. One per process, for whoever drives the frames.
void platform_modal_frame(Engine& e, void (*fn)(void* user), void* user);

} // namespace kv
