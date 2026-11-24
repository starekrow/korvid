// Korvid platform_hid: whether the window is seen (WindowVisibility), the part every
// backend shares. A backend says which of its reasons to be hidden came or went; this
// keeps them and emits the event when the answer, or the reason given, changes.
#include "korvid/platform_hid/host_window.h"
#include "korvid/core/log.h"

namespace kv {

WindowHidden HostWindow::hidden_why() const {
    if (hidden_ & HiddenMinimized) return WindowHidden::Minimized;
    if (hidden_) return WindowHidden::Occluded;
    return WindowHidden::None;
}

void HostWindow::set_hidden(EventBus& bus, u8 reason, bool on) {
    hidden_ = on ? (u8)(hidden_ | reason) : (u8)(hidden_ & ~reason);
    // A backend whose pump waits while it is hidden (a phone's) sends before it waits, so
    // the state is right whenever anyone looks.
    if (on) send_visibility(bus);
}

void HostWindow::send_visibility(EventBus& bus) {
    WindowHidden why = hidden_why();
    if (why == hidden_sent_) return;
    hidden_sent_ = why;
    KV_INFO("window", "%s", why == WindowHidden::None ? "shown" : why == WindowHidden::Minimized ? "hidden: minimized" : "hidden: covered or away");
    bus.emit(WindowVisibility{why == WindowHidden::None, why});
}

} // namespace kv
