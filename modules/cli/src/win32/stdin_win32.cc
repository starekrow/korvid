// Standard input on Windows, without blocking, whatever it is: a pipe says how much is
// waiting (PeekNamedPipe); a file never blocks; a console is read only once a whole line
// is waiting (an Enter key is in its input buffer), since the console delivers lines.
#include "../os_stdin.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace kv {

namespace {
HANDLE stdin_handle() {
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    return h == INVALID_HANDLE_VALUE ? nullptr : h;
}

bool console_has_line(HANDLE h) {
    DWORD count = 0;
    if (!GetNumberOfConsoleInputEvents(h, &count) || count == 0) return false;
    INPUT_RECORD records[128];
    DWORD got = 0;
    if (!PeekConsoleInputW(h, records, count < 128 ? count : 128, &got)) return false;
    for (DWORD i = 0; i < got; ++i) {
        const INPUT_RECORD& r = records[i];
        if (r.EventType == KEY_EVENT && r.Event.KeyEvent.bKeyDown && r.Event.KeyEvent.wVirtualKeyCode == VK_RETURN) return true;
    }
    return false;
}
} // namespace

bool os_stdin_is_terminal() {
    HANDLE h = stdin_handle();
    DWORD mode;
    return h && GetConsoleMode(h, &mode);
}

KHandle os_stdin_fd() { return KHandle{}; } // a console's or a pipe's handle, which WSAPoll does not take

i32 os_stdin_read(void* buf, u32 cap) {
    HANDLE h = stdin_handle();
    if (!h) return -1;
    DWORD want = cap;
    DWORD mode;
    switch (GetFileType(h)) {
    case FILE_TYPE_PIPE: {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) return -1; // the writer closed it
        if (avail == 0) return 0;
        if (avail < want) want = avail;
        break;
    }
    case FILE_TYPE_CHAR:
        if (GetConsoleMode(h, &mode) && !console_has_line(h)) return 0;
        break;
    default: break; // a file: reads return at once
    }
    DWORD n = 0;
    if (!ReadFile(h, buf, want, &n, nullptr) || n == 0) return -1;
    return (i32)n;
}

} // namespace kv
