// Korvid platform_hid: the window, the keyboard, the mouse and gamepads: what a person
// at the machine touches and looks at. The backend is SDL3 today. Startup initializes
// SDL's event subsystem only; video and gamepads come up when a window opens (or when a
// renderer asks, platform_video_up), so a test with this module still runs without a
// display. A window is a service opened on request, up to KV_WINDOWS; the first is the main
// one, closed at module shutdown, never by the caller. "input.sample" (phase Input) samples
// the keys into "input.state" once per tick. Both are published resources: a system
// declares "input.state" or "platform_hid.window" (the main window) in its sets and reaches
// them with ctx.resource<const Input>() and ctx.resource<const HostWindow>(). The windows'
// events reach the bus from platform_pump, through a pump hook.
#pragma once
#include "korvid/core/module.h"
#include "korvid/platform_hid/actions.h"
#include "korvid/platform_hid/events.h"
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
    HostWindow   windows[KV_WINDOWS]; // by id; [0] is the main one. Each closed at module shutdown
    InputActions actions;  // what the game named, from the sampled input each tick
    u32          text_requests = 0; // platform_text_input's count
    char         text_override[KV_INPUT_TEXT_MAX] = {}; // input_text_override's, for the next sample
    u32          text_override_len = 0, text_override_backspaces = 0;
    struct KeyEdge { Key key; bool down; };
    KeyEdge      key_overrides[16] = {};   // input_key_override's, for the next sample
    u32          key_override_count = 0;
    TextComposing compose_override{};      // input_compose_override's
    bool         compose_pending = false;
    Array<char>  clipboard;                // the process's own, when the host has none to give
    bool         pointer_override = false; // input_pointer_override's, for the next sample
    f32          pointer_x = 0.0f, pointer_y = 0.0f, pointer_wheel = 0.0f;
    u8           pointer_buttons = 0;
    HidPress     presses[KV_HID_PRESSES] = {}; // the CLI's, being held
    u32          press_count = 0;
    void       (*modal_frame)(void* user) = nullptr; // platform_modal_frame's
    void*        modal_user = nullptr;
    bool         in_modal_frame = false;
};

// Opens a window: the first is the main one (id 0, platform_window), each after it the next
// free id, up to KV_WINDOWS. Null when that many are open, on a phone once one is, or when
// the system refuses. Owned by the module.
HostWindow* platform_open_window(Engine& e, const WindowDesc& desc);
// Closes a window other than the main one; its id is free again. The main one closes with
// the module.
void        platform_close_window(Engine& e, HostWindow* w);
// The open window with this id (an event's `window`), or null.
HostWindow* platform_window_by_id(Engine& e, u32 id);
// The module starts headless. This brings up video and gamepads (once; they stay up
// until the module shuts down) for whatever needs a display before a window exists,
// such as a renderer loading the Vulkan loader. Opening a window calls it. False with a
// log line when there is no video driver.
bool        platform_video_up(Engine& e);
// The main window, or null. For code outside the schedule; systems use the resource
// (platform_hid.window, the main one too).
HostWindow* platform_window(Engine& e);
// Whether the main window is seen (HostWindow::visible); true with no window.
bool        platform_window_visible(Engine& e);
// Asks the OS for typed text (InputState::text) while on; a text field turns it on while it
// has the focus. Counted: on twice needs off twice.
void platform_text_input(Engine& e, bool on);
// Types text (and Backspaces) as if from the keyboard, into the next sample (and as
// TextTyped): for demo scripts and tests.
void input_text_override(Engine& e, const char* text, u32 backspaces = 0);
// A key goes down or up as if from the keyboard, at the next sample: its state and a KeyDown
// or KeyUp. For demo scripts and tests (Shift held, then an arrow pressed).
void input_key_override(Engine& e, Key key, bool down);
// An input method's composition, as if from the OS, at the next sample (TextComposing). An
// empty text ends it.
void input_compose_override(Engine& e, const char* text, u32 caret);

// Where the focused text field's caret is, in framebuffer pixels (and the caret's x within
// the rect): the input method opens beside it.
void platform_text_area(Engine& e, rect2 r, f32 caret);
// The clipboard's text, appended to `out`: the system's, or, where the host cannot give it
// (headless, a host not yet filled in), what this process last set. False when empty.
bool platform_clipboard_get(Engine& e, Array<char>& out);
// Sets the system's clipboard and the process's own.
void platform_clipboard_set(Engine& e, const char* utf8);
// The pointer's shape over the window it is in (MouseState::window).
void platform_cursor(Engine& e, CursorShape shape);
// The pointer held to the window it is in while on, so a drag goes on outside it (Krome's presses).
void platform_capture_mouse(Engine& e, bool on);
// Puts the pointer at (x, y) (framebuffer pixels, top-left origin) with these buttons held
// (MouseButton bits) and this much wheel, as if from the mouse, into the next sample; it
// stays there until the mouse or another override moves it. For demo scripts and tests.
void input_pointer_override(Engine& e, f32 x, f32 y, u8 buttons, f32 wheel = 0.0f);
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

// The window's surface taken away and given back (HostWindow::on_surface has the terms):
// fn(user, null) from inside platform_pump before the surface is destroyed, fn(user,
// native) when there is a new one. Only Android calls it. Null turns it off. One per
// process, for whoever owns the renderer.
void platform_surface_hook(Engine& e, void (*fn)(void* user, void* native), void* user);

} // namespace kv
