// A window and nothing else. Every event the window delivers is logged, and so is the frame
// loop's health: each second the frames run and the longest gap between two, and at once
// any gap over --hitch-ms (20 by default). The point is to move and resize the window and
// see the gaps stay small: a backend whose window blocks the loop while it is dragged
// shows a gap as long as the drag. Esc or closing the window quits; --run-seconds N quits
// by itself; --borderless opens one without a frame. Gamepad buttons and disconnections
// are logged too (platform_hid logs each pad as it connects).
//
// --round opens a transparent window and draws a disc in it, its edge soft, with a handle
// at the lower right: the desktop shows at the corners, and a click there reaches the
// window behind (HostWindow::shape). --grips gives the window no frame of its own to need:
// the disc's upper band moves it and the handle resizes it, by the OS's own move and
// resize (HostWindow::grips), between 160 and 720 pixels a side (HostWindow::bounds). A
// click below the band is the program's, and is logged.
//
// --two opens a second window beside the main one (slice 13, KR40): every event names its
// window, the pointer's moves between them are logged, H hides the second and shows it again
// (HostWindow::show), and closing it hides it; closing the main one quits. --two-script
// also hides it at 1 s and shows it at 2 s, and checks what it says it is.
#include "korvid/window/module.h"
#include "korvid/core/os_error.h"
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

// ---- the round picture (--round, --grips) ------------------------------------------------

struct Round {
    bool on = false, grips = false;
    u32  drawn_w = 0, drawn_h = 0; // the size last drawn and shaped
    Array<u8> mask;
};

// The handle: a square at the lower right, inside the window, its size a tenth of the side.
rect2 handle_rect(u32 w, u32 h) {
    f32 side = (f32)(w < h ? w : h) * 0.1f;
    if (side < 16.0f) side = 16.0f;
    return rect2{(f32)w - side, (f32)h - side, side, side};
}

// Coverage of the disc (and the handle) at a pixel's centre: 1 inside, 0 outside, a ramp
// 1.5 pixels wide at the edge so it is drawn soft.
f32 round_coverage(f32 x, f32 y, u32 w, u32 h, bool handle) {
    f32 cx = (f32)w * 0.5f, cy = (f32)h * 0.5f;
    f32 r = (w < h ? (f32)w : (f32)h) * 0.5f - 2.0f;
    f32 dx = x - cx, dy = y - cy;
    f32 d = r - __builtin_sqrtf(dx * dx + dy * dy);
    f32 c = d >= 1.5f ? 1.0f : d <= 0.0f ? 0.0f : d / 1.5f;
    if (handle) {
        rect2 hr = handle_rect(w, h);
        if (x >= hr.x && y >= hr.y) c = 1.0f;
    }
    return c;
}

// Draws the disc into the window's software frame and gives the window its shape, when the
// size is not what was last drawn. The colour is premultiplied by the coverage, so the edge
// is half alpha where the shape is in or out at a half.
void round_draw(Round& rd, HostWindow& win) {
    HostWindow::SoftwareFrame f;
    if (!win.software_frame(&f)) return;
    bool fresh = f.width != rd.drawn_w || f.height != rd.drawn_h;
    if (fresh) {
        rd.mask.clear();
        u8* m = rd.mask.push_n(f.width * f.height);
        f32 band = (f32)f.height * 0.4f;
        for (u32 y = 0; y < f.height; ++y) {
            u8* row = f.pixels + (usize)y * f.pitch;
            for (u32 x = 0; x < f.width; ++x) {
                f32 px = (f32)x + 0.5f, py = (f32)y + 0.5f;
                f32 a = round_coverage(px, py, f.width, f.height, rd.grips);
                // A warm disc, its band (what moves the window) a shade darker, the handle grey.
                f32 r = 0.95f, g = 0.62f, b = 0.18f;
                if (rd.grips && py < band) { r = 0.78f; g = 0.45f; b = 0.10f; }
                rect2 hr = handle_rect(f.width, f.height);
                if (rd.grips && px >= hr.x && py >= hr.y) { r = g = b = 0.55f; }
                if (!f.alpha) a = a >= 0.5f ? 1.0f : 0.0f; // an opaque frame: cut, not blended
                u8 R = (u8)(r * a * 255.0f + 0.5f), G = (u8)(g * a * 255.0f + 0.5f), B = (u8)(b * a * 255.0f + 0.5f);
                u8* p = row + x * 4;
                p[0] = f.rgba ? R : B;
                p[1] = G;
                p[2] = f.rgba ? B : R;
                p[3] = (u8)(a * 255.0f + 0.5f);
                m[(usize)y * f.width + x] = p[3];
            }
        }
        if (!win.shape(rd.mask.data, f.width, f.height)) KV_WARN("window", "the window took no shape: it stays a rectangle");
        if (rd.grips) {
            WindowGripRect g[2] = {{rect2{0, 0, (f32)f.width, band}, WindowGrip::Move},
                                   {handle_rect(f.width, f.height), WindowGrip::SE}};
            if (!win.grips(g, 2)) KV_WARN("window", "the window takes no grips here");
        }
        KV_INFO("window", "drew the round picture at %ux%u px, %s", f.width, f.height, f.alpha ? "with alpha" : "opaque");
        rd.drawn_w = f.width;
        rd.drawn_h = f.height;
    }
    win.present_software();
}

// --two's second window, while it is open.
HostWindow* g_second = nullptr;

void on_close(TickContext& ctx, Span<const CloseRequested> evs) {
    for (const CloseRequested& c : evs) {
        if (c.window != 0 && g_second && c.window == g_second->id()) {
            KV_INFO("window", "close requested of window %u: hidden instead", c.window);
            g_second->show(false);
            continue;
        }
        KV_INFO("window", "close requested");
        engine_request_quit(*ctx.engine);
    }
}
void on_resized(TickContext&, Span<const WindowResized> evs) {
    for (const WindowResized& r : evs) {
        KV_INFO("window", "window %u resized to %ux%u px", r.window, r.width, r.height);
        if (g_second && r.window == g_second->id()) g_second->present_blank();
    }
}
void on_focus(TickContext&, Span<const FocusChanged> evs) {
    for (const FocusChanged& f : evs) KV_INFO("window", "window %u focus %s", f.window, f.focused ? "gained" : "lost");
}
void on_moved(TickContext&, Span<const MouseMoved> evs) {
    static u32 in = ~0u;
    for (const MouseMoved& m : evs) {
        if (m.window == in) continue;
        in = m.window;
        KV_INFO("window", "the pointer is in window %u, at %.0f, %.0f", m.window, m.x, m.y);
    }
}
void on_key(TickContext& ctx, Span<const KeyDown> evs) {
    for (const KeyDown& k : evs) {
        if (k.repeat) continue;
        KV_INFO("window", "key %s", key_name(k.key));
        if (k.key == Key::Escape) engine_request_quit(*ctx.engine);
        if (k.key == Key::H && g_second) {
            bool hide = g_second->visible();
            g_second->show(!hide);
            if (!hide) g_second->present_blank();
        }
    }
}
void on_button(TickContext&, Span<const MouseButtonDown> evs) {
    for (const MouseButtonDown& b : evs) KV_INFO("window", "mouse %s at %.0f, %.0f in window %u", mouse_button_name(b.button), b.x, b.y, b.window);
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
    {.name = ".on_pad_button", KV_ON(PadButtonDown, on_pad_button), .phase = Phase::Input},
    {.name = ".on_pad_gone", KV_ON(GamepadDisconnected, on_pad_gone), .phase = Phase::Input},
    {.name = ".on_close", KV_ON(CloseRequested, on_close), .phase = Phase::Input},
    {.name = ".on_resized", KV_ON(WindowResized, on_resized), .phase = Phase::Input},
    {.name = ".on_focus", KV_ON(FocusChanged, on_focus), .phase = Phase::Input},
    {.name = ".on_key", KV_ON(KeyDown, on_key), .phase = Phase::Input},
    {.name = ".on_button", KV_ON(MouseButtonDown, on_button), .phase = Phase::Input},
    {.name = ".on_wheel", KV_ON(MouseWheel, on_wheel), .phase = Phase::Input},
    {.name = ".on_moved", KV_ON(MouseMoved, on_moved), .phase = Phase::Input},
};

// The loop's health and one frame of it, shared by the loop and by the frames run from
// inside an OS modal loop (platform_modal_frame: a live resize on macOS), so the gaps it
// reports cover both.
struct Loop {
    Engine* e = nullptr;
    f64 hitch_ms = 20.0;
    f64 last = 0.0, second = 0.0, worst = 0.0;
    u32 frames = 0, modal_frames = 0;
};

void frame(Loop& l) {
    f64 now = time_now();
    f64 gap = now - l.last;
    l.last = now;
    if (gap * 1000.0 > l.hitch_ms) KV_WARN("window", "frame gap %.1f ms", gap * 1000.0);
    if (gap > l.worst) l.worst = gap;
    engine_frame(*l.e, gap);
    ++l.frames;
    if (now - l.second >= 1.0) {
        if (l.modal_frames) KV_INFO("window", "%u frames (%u inside a live resize), longest gap %.1f ms", l.frames, l.modal_frames, l.worst * 1000.0);
        else KV_INFO("window", "%u frames, longest gap %.1f ms", l.frames, l.worst * 1000.0);
        l.frames = l.modal_frames = 0;
        l.worst = 0.0;
        l.second = now;
    }
}

void modal_frame(void* user) {
    Loop& l = *(Loop*)user;
    ++l.modal_frames;
    frame(l);
}

int run(int argc, char** argv) {
    f64 run_seconds = 0.0, hitch_ms = 20.0;
    bool borderless = false;
    const char* bundled = nullptr;
    const char* asset = nullptr;
    bool text = false, two = false, two_script = false;
    Round round;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--run-seconds") == 0 && i + 1 < argc) run_seconds = atof(argv[++i]);
        else if (strcmp(argv[i], "--hitch-ms") == 0 && i + 1 < argc) hitch_ms = atof(argv[++i]);
        else if (strcmp(argv[i], "--borderless") == 0) borderless = true;
        else if (strcmp(argv[i], "--bundled") == 0 && i + 1 < argc) bundled = argv[++i];
        else if (strcmp(argv[i], "--asset") == 0 && i + 1 < argc) asset = argv[++i];
        else if (strcmp(argv[i], "--text") == 0) text = true;
        else if (strcmp(argv[i], "--round") == 0) round.on = true;
        else if (strcmp(argv[i], "--grips") == 0) round.on = round.grips = true;
        else if (strcmp(argv[i], "--two") == 0) two = true;
        else if (strcmp(argv[i], "--two-script") == 0) two = two_script = true;
        else { KV_ERROR("window", "unknown option '%s' (--run-seconds N, --hitch-ms N, --borderless, --round, --grips, --two, --two-script, --bundled NAME, --asset PATH, --text)", argv[i]); return 2; }
    }
    Engine e;
    if (!engine_init(e, kv_module_list())) return 1;
    WindowDesc d;
    d.title = "korvid window";
    d.width = 960;
    d.height = 540;
    d.borderless = borderless;
    if (round.on) {
        d.width = d.height = 360;
        d.transparent = true;
    }
    int rc = 0;
    HostWindow* win = platform_open_window(e, d);
    if (!win) rc = 1;
    if (win && round.grips) win->bounds(160, 160, 720, 720);
    if (win && two) {
        WindowDesc d2;
        d2.title = "korvid window 2";
        d2.width = 480;
        d2.height = 320;
        g_second = platform_open_window(e, d2);
        if (!g_second) { KV_ERROR("window", "the second window did not open"); rc = 1; }
        else {
            KV_INFO("window", "a second window, id %u, %ux%u px; the main one is id %u", g_second->id(), g_second->width(), g_second->height(), win->id());
            g_second->present_blank();
        }
    }
    u32 script_step = 0, script_bad = 0;
    // Where a platform keeps a program's files, shown: a bundled file read now
    // ("shaders/sprite.spv"), an asset through the mounts ("window/hello.txt").
    if (bundled) {
        IoBytes b;
        i32 err = 0;
        if (io_read_bundled(bundled, &b, nullptr, &err)) KV_INFO("window", "bundled '%s': %llu bytes", bundled, (unsigned long long)b.size);
        else KV_ERROR("window", "bundled '%s': %s", bundled, os_error_text(err));
        b.free();
    }
    // Typed text, as a text field would ask for it: the OS's keyboard up where there is one,
    // and what arrives logged each frame.
    if (text && rc == 0) platform_text_input(e, true);
    Handle asset_read;
    if (asset) {
        IoRequest r;
        r.path = asset;
        asset_read = platform_io(e)->read_asset(r);
    }
    f64 start = time_now();
    Loop l;
    l.e = &e;
    l.hitch_ms = hitch_ms;
    l.last = l.second = start;
    platform_modal_frame(e, modal_frame, &l);
    while (rc == 0 && !e.quit_requested) {
        platform_pump(e); // on macOS a live resize runs inside this, its frames through modal_frame
        if (text) {
            if (Input* in = platform_input(e)) {
                if (in->raw.text_len || in->raw.backspaces)
                    KV_INFO("window", "typed \"%s\" (%u bytes), %u backspaces", in->raw.text, in->raw.text_len, in->raw.backspaces);
                in->sample(); // nothing ticks here: take what was typed, as a tick would
            }
        }
        if (!asset_read.is_null()) {
            IoQueue* io = platform_io(e);
            if (IoCompletion done; io->take(asset_read, &done)) {
                asset_read = Handle{};
                if (done.status == IoStatus::Done) KV_INFO("window", "asset '%s': %llu bytes, starting \"%.24s\"", asset, (unsigned long long)done.bytes.size, done.bytes.size ? (const char*)done.bytes.data : "");
                else KV_ERROR("window", "asset '%s': %s", asset, os_error_text(done.error));
                done.bytes.free();
            }
        }
        if (round.on && win) round_draw(round, *win);
        // --two-script: hide the second window at 1 s, show it at 2 s, and say whether each
        // took, as visible() has it once a pump has passed.
        if (two_script && g_second) {
            f64 t = time_now() - start;
            if (script_step == 0 && t >= 1.0) { g_second->show(false); ++script_step; }
            else if (script_step == 1 && t >= 1.5) {
                KV_INFO("window", "script: window %u hidden: visible %d", g_second->id(), (int)g_second->visible());
                script_bad += g_second->visible();
                ++script_step;
            }
            else if (script_step == 2 && t >= 2.0) { g_second->show(true); g_second->present_blank(); ++script_step; }
            else if (script_step == 3 && t >= 2.5) {
                KV_INFO("window", "script: window %u shown: visible %d, still %ux%u", g_second->id(), (int)g_second->visible(), g_second->width(), g_second->height());
                script_bad += !g_second->visible();
                ++script_step;
            }
        }
        frame(l);
        if (run_seconds > 0.0 && time_now() - start >= run_seconds) break;
        time_sleep(0.004); // nothing to present: pace the loop at about 250 Hz
    }
    platform_modal_frame(e, nullptr, nullptr);
    if (two_script) {
        if (script_step < 4) KV_ERROR("window", "script: ended at step %u of 4 (give --run-seconds 3 or more)", script_step);
        else if (script_bad) KV_ERROR("window", "script: %u of 2 checks failed", script_bad);
        else KV_INFO("window", "script: the second window hid and showed as asked");
        if (script_step < 4 || script_bad) rc = rc ? rc : 1;
    }
    g_second = nullptr;
    round.mask.free();
    engine_shutdown(e);
    names_shutdown();
    mem_shutdown();
    return rc;
}

} // namespace

KV_MODULE_DEFINE(window_module) = {
    .name     = "window",
    .handlers = k_handlers,
};

} // namespace kv

// Through platform_app_main, so it runs on iOS as well (inside the application there).
int main(int argc, char** argv) {
    return kv::platform_app_main(argc, argv, [](int c, char** v, void*) { return kv::run(c, v); }, nullptr);
}
