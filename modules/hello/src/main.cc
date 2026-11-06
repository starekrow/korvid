// hello: opens a HostWindow, runs the fixed-tick loop, logs platform and key events.
// Esc or closing the window quits. --run-seconds N exits by itself (smoke test).
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/time.h"
#include "korvid/platform/events.h"
#include "korvid/platform/host_window.h"
#include <stdlib.h>
#include <string.h>

namespace {

struct App {
    kv::HostWindow window;
    kv::Input      input;
    kv::u64        key_edges = 0;
};

// Phase "input": sample the raw key state once per tick for the simulation.
void sample_input(kv::TickContext& ctx) {
    ((App*)ctx.user)->input.sample();
}

// Phase "sim": react to platform events and the sampled input.
void hello_sim(kv::TickContext& ctx) {
    using namespace kv;
    App& app = *(App*)ctx.user;
    EventBus& bus = *ctx.events;

    for (const WindowResized& e : bus.read<WindowResized>())
        KV_INFO("hello", "tick %llu: resized to %ux%u", (unsigned long long)ctx.tick, e.width, e.height);
    for (const FocusChanged& e : bus.read<FocusChanged>())
        KV_INFO("hello", "tick %llu: focus %s", (unsigned long long)ctx.tick, e.focused ? "gained" : "lost");
    for (const KeyDown& e : bus.read<KeyDown>()) {
        ++app.key_edges;
        if (!e.repeat) KV_INFO("hello", "tick %llu: key down %s", (unsigned long long)ctx.tick, key_name(e.key));
    }
    for (const KeyUp& e : bus.read<KeyUp>()) {
        ++app.key_edges;
        KV_INFO("hello", "tick %llu: key up   %s", (unsigned long long)ctx.tick, key_name(e.key));
    }
    if (bus.read<CloseRequested>().len) {
        KV_INFO("hello", "close requested");
        engine_request_quit(*ctx.engine);
    }
    if (app.input.sampled.down(Key::Escape)) {
        KV_INFO("hello", "escape held, quitting");
        engine_request_quit(*ctx.engine);
    }
}

} // namespace

int main(int argc, char** argv) {
    using namespace kv;

    f64 run_seconds = -1.0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--run-seconds") == 0 && i + 1 < argc) run_seconds = atof(argv[++i]);
        else if (strcmp(argv[i], "--quiet") == 0) log_set_level(LogLevel::Warn);
        else { KV_ERROR("hello", "usage: hello [--run-seconds N] [--quiet]"); return 2; }
    }

    Engine engine;
    if (!engine_init(engine, kv_module_list())) return 1;

    App app;
    WindowDesc desc;
    desc.title = "korvid hello";
    desc.width = 960;
    desc.height = 540;
    if (!app.window.open(desc)) { engine_shutdown(engine); return 1; }

    u32 ph_input = engine.phases.add_phase("input");
    u32 ph_sim   = engine.phases.add_phase("sim");
    engine.phases.add_system(ph_input, SystemDesc{"input.sample", sample_input, &app});
    engine.phases.add_system(ph_sim,   SystemDesc{"hello.sim",    hello_sim,    &app});

    f64 start = time_now();
    f64 prev  = start;
    f64 next_report = start + 1.0;
    u32 frames_in_window = 0;

    while (!engine.quit_requested) {
        f64 now = time_now();
        f64 dt  = now - prev;
        prev = now;

        app.window.pump(engine.events, app.input);
        engine_frame(engine, dt);
        app.window.present_blank(); // no renderer yet; see HostWindow::present_blank
        ++frames_in_window;

        if (now >= next_report) {
            KV_INFO("hello", "%u fps, tick %llu, alpha %.2f, %ux%u, %s",
                    frames_in_window, (unsigned long long)engine.clock.tick_count,
                    engine.clock.alpha(), app.window.width(), app.window.height(),
                    app.window.focused() ? "focused" : "unfocused");
            frames_in_window = 0;
            next_report += 1.0;
        }
        if (run_seconds >= 0.0 && now - start >= run_seconds) {
            KV_INFO("hello", "run time reached, quitting");
            engine_request_quit(engine);
        }

        // Nothing to present yet, so pace the loop instead of spinning.
        time_sleep(0.001);
    }

    KV_INFO("hello", "%llu key edges seen", (unsigned long long)app.key_edges);
    app.window.close();
    engine_shutdown(engine);
    return 0;
}
