// A window and nothing else. Every event the window delivers is logged, and so is the frame
// loop's health: each second the frames run and the longest gap between two, and at once
// any gap over --hitch-ms (20 by default). The point is to move and resize the window and
// see the gaps stay small: a backend whose window blocks the loop while it is dragged
// shows a gap as long as the drag. Esc or closing the window quits; --run-seconds N quits
// by itself; --borderless opens one without a frame. Gamepad buttons and disconnections
// are logged too (platform_hid logs each pad as it connects).
#include "korvid/example_window/module.h"
#include "korvid/platform/module.h"
#include "korvid/platform_hid/events.h"
#include "korvid/platform_hid/host_window.h"
#include "korvid/platform_hid/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/time.h"
#include <stdlib.h>
#include <string.h>

namespace kv {

namespace {

void on_close(TickContext& ctx, Span<const CloseRequested>) {
    KV_INFO("window", "close requested");
    engine_request_quit(*ctx.engine);
}
void on_resized(TickContext&, Span<const WindowResized> evs) {
    for (const WindowResized& r : evs) KV_INFO("window", "resized to %ux%u px", r.width, r.height);
}
void on_focus(TickContext&, Span<const FocusChanged> evs) {
    for (const FocusChanged& f : evs) KV_INFO("window", "focus %s", f.focused ? "gained" : "lost");
}
void on_key(TickContext& ctx, Span<const KeyDown> evs) {
    for (const KeyDown& k : evs) {
        if (k.repeat) continue;
        KV_INFO("window", "key %s", key_name(k.key));
        if (k.key == Key::Escape) engine_request_quit(*ctx.engine);
    }
}
void on_button(TickContext&, Span<const MouseButtonDown> evs) {
    for (const MouseButtonDown& b : evs) KV_INFO("window", "mouse %s at %.0f, %.0f", mouse_button_name(b.button), b.x, b.y);
}
void on_wheel(TickContext&, Span<const MouseWheel> evs) {
    for (const MouseWheel& w : evs) KV_INFO("window", "wheel %.2f, %.2f", w.dx, w.dy);
}
void on_pad_button(TickContext&, Span<const PadButtonDown> evs) {
    for (const PadButtonDown& b : evs) KV_INFO("window", "pad %u %s", b.pad, pad_button_name(b.button));
}
void on_pad_gone(TickContext&, Span<const GamepadDisconnected> evs) {
    for (const GamepadDisconnected& g : evs) KV_INFO("window", "pad %u disconnected", g.pad);
}

constinit const HandlerDesc k_handlers[] = {
    {.name = ".on_pad_button", KV_ON(PadButtonDown, on_pad_button)},
    {.name = ".on_pad_gone", KV_ON(GamepadDisconnected, on_pad_gone)},
    {.name = ".on_close", KV_ON(CloseRequested, on_close)},
    {.name = ".on_resized", KV_ON(WindowResized, on_resized)},
    {.name = ".on_focus", KV_ON(FocusChanged, on_focus)},
    {.name = ".on_key", KV_ON(KeyDown, on_key)},
    {.name = ".on_button", KV_ON(MouseButtonDown, on_button)},
    {.name = ".on_wheel", KV_ON(MouseWheel, on_wheel)},
};

int run(int argc, char** argv) {
    f64 run_seconds = 0.0, hitch_ms = 20.0;
    bool borderless = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--run-seconds") == 0 && i + 1 < argc) run_seconds = atof(argv[++i]);
        else if (strcmp(argv[i], "--hitch-ms") == 0 && i + 1 < argc) hitch_ms = atof(argv[++i]);
        else if (strcmp(argv[i], "--borderless") == 0) borderless = true;
        else { KV_ERROR("window", "unknown option '%s' (--run-seconds N, --hitch-ms N, --borderless)", argv[i]); return 2; }
    }
    Engine e;
    if (!engine_init(e, kv_module_list())) return 1;
    WindowDesc d;
    d.title = "korvid window";
    d.width = 960;
    d.height = 540;
    d.borderless = borderless;
    int rc = 0;
    if (!platform_open_window(e, d)) rc = 1;
    f64 start = time_now(), last = start, second = start, worst = 0.0;
    u32 frames = 0;
    while (rc == 0 && !e.quit_requested) {
        f64 now = time_now();
        f64 gap = now - last;
        last = now;
        if (gap * 1000.0 > hitch_ms) KV_WARN("window", "frame gap %.1f ms", gap * 1000.0);
        if (gap > worst) worst = gap;
        platform_pump(e);
        engine_frame(e, gap);
        ++frames;
        if (now - second >= 1.0) {
            KV_INFO("window", "%u frames, longest gap %.1f ms", frames, worst * 1000.0);
            frames = 0;
            worst = 0.0;
            second = now;
        }
        if (run_seconds > 0.0 && now - start >= run_seconds) break;
        time_sleep(0.004); // nothing to present: pace the loop at about 250 Hz
    }
    engine_shutdown(e);
    names_shutdown();
    mem_shutdown();
    return rc;
}

} // namespace

KV_MODULE_DEFINE(example_window_module) = {
    .name     = "example_window",
    .handlers = k_handlers,
};

} // namespace kv

int main(int argc, char** argv) { return kv::run(argc, argv); }
