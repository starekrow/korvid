#include "korvid/platform/stdin.h"
#include <errno.h>
#include <poll.h>
#include <unistd.h>

namespace kv {

bool io_stdin_is_terminal() { return isatty(STDIN_FILENO) != 0; }

KHandle io_stdin_handle() { return khandle_make(KHandleKind::Pipe, STDIN_FILENO); }

i32 io_stdin_read(void* buf, u32 cap) {
    for (;;) {
        pollfd p{STDIN_FILENO, POLLIN, 0};
        if (poll(&p, 1, 0) <= 0 || !(p.revents & (POLLIN | POLLHUP))) return 0;
        ssize_t n = read(STDIN_FILENO, buf, cap);
        if (n > 0) return (i32)n;
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
}

} // namespace kv
