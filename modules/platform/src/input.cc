#include "korvid/platform/input.h"

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
};

const char* key_name(Key k) {
    u32 i = (u32)k;
    return i < (u32)Key::Count ? g_key_names[i] : "Invalid";
}

} // namespace kv
