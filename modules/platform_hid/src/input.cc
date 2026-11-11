#include "korvid/platform_hid/input.h"

namespace kv {

static const char* const g_key_names[(u32)Key::Count] = {
    "Unknown",
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
    "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
    "Escape", "Space", "Enter", "Tab", "Backspace",
    "Left", "Right", "Up", "Down",
    "LShift", "RShift", "LCtrl", "RCtrl", "LAlt", "RAlt",
    "LGui", "RGui",
};

const char* key_name(Key k) {
    u32 i = (u32)k;
    return i < (u32)Key::Count ? g_key_names[i] : "Invalid";
}

} // namespace kv

namespace kv {

static const char* const g_pad_button_names[(u32)PadButton::Count] = {
    "South", "East", "West", "North", "Back", "Guide", "Start",
    "LStick", "RStick", "LShoulder", "RShoulder", "DpadUp", "DpadDown", "DpadLeft", "DpadRight",
};
static const char* const g_pad_axis_names[(u32)PadAxis::Count] = {
    "LeftX", "LeftY", "RightX", "RightY", "LeftTrigger", "RightTrigger",
};

static const char* const g_mouse_button_names[(u32)MouseButton::Count] = {"Left", "Middle", "Right", "X1", "X2"};
const char* mouse_button_name(MouseButton b) { return (u32)b < (u32)MouseButton::Count ? g_mouse_button_names[(u32)b] : "Invalid"; }

const char* pad_button_name(PadButton b) { return (u32)b < (u32)PadButton::Count ? g_pad_button_names[(u32)b] : "Invalid"; }
const char* pad_axis_name(PadAxis a) { return (u32)a < (u32)PadAxis::Count ? g_pad_axis_names[(u32)a] : "Invalid"; }

} // namespace kv
