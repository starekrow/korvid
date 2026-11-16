// kromedemo: Krome's first screen, a form in markup (assets/form.kim, its handlers in
// assets/form.lua) out of krome/std; --lua opens the same form as slice 1 wrote it, in Lua
// (assets/form_lua.lua), which draws the same pixels. The game's side is small: it opens
// the screen, tells it the tick count now and then (krome_set), and listens for its
// events (KromeEvent): "quit" ends the run.
//
// --demo drives the pointer through the form as a person would (input_pointer_override):
// buttons, the list's wheel and an item in it, the menu and a pick from it, a press
// outside the menu that must close it and reach nothing, the About modal and a press
// outside it that must do nothing, a disabled button. Then it checks the model and quits:
// "kromedemo: demo passed", or what did not hold. --shots DIR writes screenshots of the
// form, the menu and the modal as it goes, each once Krome has settled (fonts in, painted),
// with the tick count held at 0 so two runs' pictures compare; and then, in the light
// theme, the form and the menu again. --theme light starts in it. It paces on demand: still, it draws nothing and
// ticks four times a second (--pacing continuous for every vsync). --bench opens a screen of
// two thousand nodes instead and lays it out and paints it every tick: run it with
// --prof-report for the cost. --hover moves the pointer onto the Save button and off it every
// tick for ten seconds, then quits: the cost of a frame where one button changes (run it with
// KV_CPU_STATS=1 under --renderer cpu for the backend's side).
#include "korvid/kromedemo/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/name.h"
#include "korvid/core/time.h"
#include "korvid/frameloop/frameloop.h"
#include "korvid/krome/krome.h"
#include "korvid/platform_hid/host_window.h"
#include "korvid/platform_hid/module.h"
#include "korvid/render/renderer.h"
#include <string.h>

// The key a script's "mod" holds: the one Krome's `mod+` shortcuts want here.
#if defined(__APPLE__)
#define KV_DEMO_MOD Key::LGui
#else
#define KV_DEMO_MOD Key::LCtrl
#endif

namespace kv {

namespace {

struct Demo {
    Name form;              // the page shown: the form, or the widget gallery
    bool lua = false;       // --lua: the form is form_lua.lua
    Name bench;
    bool demo = false;
    const char* shots = nullptr;
    u32  step = 0, wait = 0;
    u32  failures = 0;
    u32  settled = 0;
    bool hover = false;    // --hover: on and off a button every tick, ten seconds
    u32  hovers = 0;
    bool type = false;     // --type: the small-text specimen; with --shots, its picture, then quit
    u32  type_done = 0;
    // --work: a tool's work in the Work cycle, behind the window (docs/design/cycles.md).
    bool work = false;
    u64  next = 2, primes = 0;
    f64  started = 0.0, computing = 0.0;
    u64  frames_at_start = 0;
};


// A step of the pilot: where the pointer goes and what it does there.
enum class Act : u8 { Move, Click, Wheel, Shot, Check, Theme, Type, Key, Compose, Quit };
struct Step { Act act; const char* id; f32 value; const char* expect; };

// Positions are found when the step runs, so a reflow or a scroll is followed.
constinit const Step k_steps[] = {
    {Act::Shot, "form", 0, nullptr},
    {Act::Click, "up", 0, nullptr}, {Act::Click, "up", 0, nullptr}, {Act::Click, "down", 0, nullptr},
    {Act::Check, "level", 0, "4"},
    {Act::Click, "save", 0, nullptr}, {Act::Click, "save", 0, nullptr},
    {Act::Check, "saves", 0, "2"},
    {Act::Wheel, "list", -3, nullptr},
    {Act::Click, "item8", 0, nullptr},
    {Act::Check, "picked", 0, "item8"},
    {Act::Click, "menu", 0, nullptr},
    {Act::Shot, "menu", 0, nullptr},
    {Act::Click, "roomy", 0, nullptr},
    {Act::Check, "choice", 0, "roomy"},
    {Act::Click, "menu", 0, nullptr},
    {Act::Click, "save", 0, nullptr},  // outside the open menu: closes it, saves nothing
    {Act::Check, "saves", 0, "2"},
    {Act::Click, "about", 0, nullptr},
    {Act::Shot, "about", 0, nullptr},
    {Act::Click, "save", 0, nullptr},  // under the modal: nothing
    {Act::Check, "saves", 0, "2"},
    {Act::Click, "ok", 0, nullptr},
    {Act::Check, "abouts", 0, "1"},
    {Act::Click, "later", 0, nullptr}, // disabled
    {Act::Check, "saves", 0, "2"},
    // The notes: typed, a line broken, everything copied and pasted after itself, the paste
    // undone, a character composed through an input method and committed; then Escape.
    {Act::Click, "notes", 0, nullptr},
    {Act::Type, "first line", 0, nullptr},
    {Act::Key, "enter", 0, ""},
    {Act::Type, "second", 0, nullptr},
    {Act::Check, "notes", 0, "first line\nsecond"},
    {Act::Shot, "edit", 0, nullptr},
    {Act::Key, "a", 0, "ctrl"},
    {Act::Key, "c", 0, "ctrl"},
    {Act::Key, "right", 0, ""},
    {Act::Key, "v", 0, "ctrl"},
    {Act::Check, "notes", 0, "first line\nsecondfirst line\nsecond"},
    {Act::Key, "z", 0, "ctrl"},
    {Act::Check, "notes", 0, "first line\nsecond"},
    {Act::Compose, "\xe3\x81\x8b", 0, nullptr},
    {Act::Shot, "compose", 0, nullptr},
    {Act::Type, "\xe3\x81\x8b", 0, nullptr},
    {Act::Check, "notes", 0, "first line\nsecond\xe3\x81\x8b"},
    {Act::Key, "escape", 0, ""},
    // The other theme: every screen and overlay restyled from the documents Krome keeps.
    {Act::Theme, "light", 0, nullptr},
    {Act::Shot, "light", 0, nullptr},
    {Act::Click, "menu", 0, nullptr},
    {Act::Shot, "light_menu", 0, nullptr},
    {Act::Click, "close_menu", 0, nullptr},
    {Act::Check, "saves", 0, "2"},
    // The second page: every widget, by pointer and by keys, in both themes.
    {Act::Theme, "dark", 0, nullptr},
    {Act::Click, "nav_gallery", 0, nullptr},
    {Act::Shot, "gallery", 0, nullptr},
    {Act::Click, "careful", 0, nullptr},
    {Act::Check, "mode", 0, "careful"},
    {Act::Click, "wrap", 0, nullptr},
    {Act::Check, "wrap", 0, "false"},
    {Act::Click, "level", 0, nullptr},       // the middle of 0..100
    {Act::Check, "level", 0, "50"},
    {Act::Key, "right", 0, ""},
    {Act::Check, "level", 0, "55"},
    {Act::Click, "more", 0, nullptr},
    {Act::Check, "done", 0, "50%"},
    {Act::Click, "kind", 0, nullptr},
    {Act::Shot, "gallery_dropdown", 0, nullptr},
    {Act::Click, "font", 0, nullptr},
    {Act::Check, "kind", 0, "font"},
    {Act::Key, "enter", 0, ""},              // the dropdown has the focus: Enter opens it on its first
    {Act::Key, "down", 0, ""},
    {Act::Key, "enter", 0, ""},
    {Act::Check, "kind", 0, "sound"},
    {Act::Key, "tab", 0, ""},                // on to the name, by the keyboard
    {Act::Type, "Rowan", 0, nullptr},
    {Act::Check, "name", 0, "Rowan"},
    {Act::Click, "textures", 0, nullptr},  // closes the textures folder
    {Act::Click, "tab_table", 0, nullptr},
    {Act::Shot, "gallery_table", 0, nullptr},
    {Act::Click, "r1", 0, nullptr},
    {Act::Check, "picked_row", 0, "r1"},
    {Act::Click, "col_size", 0, nullptr},    // sorts by size: the smallest first
    {Act::Click, "r4096", 0, nullptr},
    {Act::Check, "picked_row", 0, "r4096"},
    {Act::Click, "tab_split", 0, nullptr},
    {Act::Shot, "gallery_split", 0, nullptr},
    {Act::Click, "split", 0, nullptr},       // the splitter takes the focus; the arrows move it
    {Act::Key, "right", 0, ""},
    {Act::Check, "split", 0, "256"},
    {Act::Click, "file", 0, nullptr},        // the menu bar: File opens its items below it
    {Act::Click, "save_all", 0, nullptr},
    {Act::Check, "last", 0, "file: save"},
    {Act::Key, "r", 0, "mod"},               // the Rename button's shortcut (mod+r: Command on a Mac)
    {Act::Key, "tab", 0, ""},                // into the dialog: its field first
    {Act::Type, "Ash", 0, nullptr},
    {Act::Shot, "gallery_dialog", 0, nullptr},
    {Act::Key, "enter", 0, ""},              // the dialog's OK
    {Act::Check, "renamed", 0, "renamed to Ash"},
    {Act::Theme, "light", 0, nullptr},
    {Act::Click, "tab_tree", 0, nullptr},
    {Act::Shot, "gallery_light", 0, nullptr},
    {Act::Click, "nav_form", 0, nullptr},
    {Act::Check, "saves", 0, "0"},           // the form again, as it opens
    {Act::Quit, nullptr, 0, nullptr},
    {Act::Click, "quit", 0, nullptr},  // its event ends the run
};

f32 window_scale(Engine& e) {
    HostWindow* w = platform_window(e);
    f32 k = w ? w->pixel_scale() : 1.0f;
    return k > 0.0f ? k : 1.0f;
}

// The pointer to the center of a node, in the window's pixels.
bool aim(Engine& e, const Demo& d, const char* id, f32* x, f32* y) {
    rect2 r;
    if (!krome_find_rect(e, d.form, id, &r)) return false;
    f32 k = window_scale(e);
    *x = (r.x + r.w * 0.5f) * k;
    *y = (r.y + r.h * 0.5f) * k;
    return true;
}

// Phase PreSim, main thread: the pilot, a step every few ticks; and the tick count, every
// half second, for the form's title bar.
void kromedemo_tick(TickContext& ctx) {
    Engine& e = *ctx.engine;
    Demo& d = ctx.state_as<Demo>();
    if (ctx.tick % 30 == 0 && !d.shots) krome_set(e, d.form, "ticks", (f64)ctx.tick);
    if (!d.bench.is_null()) { krome_set(e, d.bench, "tick", (f64)ctx.tick); engine_want_frame(e); return; }
    if (d.type) {
        if (!d.shots || !krome_ready(e, d.form)) return;
        engine_want_frame(e);
        if (!krome_settled(e)) { d.settled = 0; return; }
        if (d.settled++ < 3) return;
        if (d.type_done++ == 0) {
            char path[600];
            format(path, sizeof path, "%s/kromedemo_type.ppm", d.shots);
            if (Renderer* r = renderer_get(e)) renderer_screenshot(r, path);
        } else if (d.type_done > 6) engine_request_quit(e);
        return;
    }
    if (d.hover) {
        if (!krome_ready(e, d.form)) return;
        engine_want_frame(e);
        if (d.hovers == 0 && !krome_settled(e)) return;
        f32 x = 4.0f, y = 4.0f;
        if ((d.hovers & 1) && !aim(e, d, "save", &x, &y)) { KV_ERROR("kromedemo", "hover: no 'save' on screen"); engine_request_quit(e); return; }
        input_pointer_override(e, x, y, 0);
        if (++d.hovers > 600) { KV_INFO("kromedemo", "hover: %u frames, on and off 'save'", d.hovers - 1); engine_request_quit(e); }
        return;
    }
    if (!d.demo || !krome_ready(e, d.form)) return;
    engine_want_frame(e); // the pilot is input the window does not see: keep the loop awake for it
    if (d.wait) { --d.wait; return; }
    if (d.step >= KV_COUNTOF(k_steps)) return;
    const Step& s = k_steps[d.step];
    f32 x = 0, y = 0;
    switch (s.act) {
    case Act::Move:
        if (aim(e, d, s.id, &x, &y)) input_pointer_override(e, x, y, 0);
        break;
    case Act::Click: {
        // Three ticks a click: over it, down, up.
        static u32 phase = 0;
        if (!aim(e, d, s.id, &x, &y)) { KV_ERROR("kromedemo", "demo: no '%s' on screen", s.id); ++d.failures; break; }
        input_pointer_override(e, x, y, phase == 1 ? 1 : 0);
        if (++phase < 3) { d.wait = 1; return; }
        phase = 0;
        break;
    }
    case Act::Wheel:
        if (aim(e, d, s.id, &x, &y)) input_pointer_override(e, x, y, 0, s.value);
        break;
    case Act::Shot:
        // A picture of what is settled: the fonts in, the change painted, and a frame since.
        if (d.shots && !krome_settled(e)) { d.settled = 0; return; }
        if (d.shots && d.settled++ < 3) return;
        d.settled = 0;
        if (d.shots) {
            char path[600];
            format(path, sizeof path, "%s/kromedemo_%s.ppm", d.shots, s.id);
            if (Renderer* r = renderer_get(e)) renderer_screenshot(r, path);
        }
        break;
    case Act::Check: {
        const char* got = krome_model_text(e, d.form, s.id);
        if (strcmp(got, s.expect) != 0) { KV_ERROR("kromedemo", "demo: %s is '%s', wanted '%s' (on %s)", s.id, got, s.expect, name_str(d.form)); ++d.failures; }
        break;
    }
    case Act::Theme:
        krome_theme(e, s.id);
        break;
    case Act::Type:
        input_text_override(e, s.id);
        break;
    case Act::Compose:
        input_compose_override(e, s.id, (u32)strlen(s.id));
        break;
    case Act::Key: {
        // Three ticks a key: its modifier down, the key down and up, the modifier up.
        static u32 phase = 0;
        struct Named { const char* name; Key key; };
        static const Named keys[] = {{"enter", Key::Enter}, {"escape", Key::Escape}, {"right", Key::Right}, {"left", Key::Left},
                                     {"a", Key::A}, {"c", Key::C}, {"v", Key::V}, {"x", Key::X}, {"z", Key::Z}, {"backspace", Key::Backspace},
                                     {"down", Key::Down}, {"up", Key::Up}, {"tab", Key::Tab}, {"space", Key::Space}, {"r", Key::R}};
        Key k = Key::Unknown;
        for (const Named& n : keys) if (!strcmp(n.name, s.id)) k = n.key;
        // "ctrl", or "mod": a shortcut's modifier as Krome reads `mod+`, Command on a Mac.
        Key held = Key::Unknown;
        if (s.expect && !strcmp(s.expect, "ctrl")) held = Key::LCtrl;
        if (s.expect && !strcmp(s.expect, "mod")) held = KV_DEMO_MOD;
        if (phase == 0 && held != Key::Unknown) input_key_override(e, held, true);
        if (phase == 1) { input_key_override(e, k, true); input_key_override(e, k, false); }
        if (phase == 2 && held != Key::Unknown) input_key_override(e, held, false);
        if (++phase < 3) { d.wait = 1; return; }
        phase = 0;
        break;
    }
    case Act::Quit:
        if (d.failures) KV_ERROR("kromedemo", "demo: %u checks failed", d.failures);
        else KV_INFO("kromedemo", "demo passed");
        break;
    }
    ++d.step;
    d.wait = 6;
}

// The Work cycle: primes by trial division, a slice at a time, while the cycle has time.
constexpr u64 k_work_limit = 4000000;
void kromedemo_work(TickContext& ctx) {
    Demo& d = ctx.state_as<Demo>();
    if (!d.work || d.next >= k_work_limit) return;
    Engine& e = *ctx.engine;
    if (d.started == 0.0) { d.started = time_now(); d.frames_at_start = e.frame_cycles; }
    f64 t0 = time_now();
    // Slices of a few thousand numbers until the cycle's time is nearly gone.
    do {
        for (u64 end = d.next + 4000; d.next < end && d.next < k_work_limit; ++d.next) {
            u64 n = d.next;
            bool prime = n >= 2;
            for (u64 f = 2; f * f <= n && prime; ++f) if (n % f == 0) prime = false;
            if (prime) ++d.primes;
        }
    } while (d.next < k_work_limit && ctx.time_left() > 0.0005);
    d.computing += time_now() - t0;
    krome_set(e, d.form, "ticks", (f64)d.primes); // the title bar shows the count as it goes
    if (d.next < k_work_limit) { ctx.more(); return; }
    f64 wall = time_now() - d.started;
    KV_INFO("kromedemo", "work: %llu primes below %llu in %.2f s, computing %.2f s (%.0f%%), %llu frames drawn meanwhile",
            (unsigned long long)d.primes, (unsigned long long)k_work_limit, wall, d.computing, 100.0 * d.computing / wall,
            (unsigned long long)(e.frame_cycles - d.frames_at_start));
    engine_request_quit(e);
}

void kromedemo_events(TickContext& ctx) {
    Engine& e = *ctx.engine;
    Demo& d = ctx.state_as<Demo>();
    for (const KromeEvent& ev : ctx.received<KromeEvent>()) {
        KV_INFO("kromedemo", "event %s from %s, %g", name_str(Name{ev.event}), ev.node ? name_str(Name{ev.node}) : "the script", ev.value);
        if (ev.event == name_intern("quit").hash) engine_request_quit(e);
        // The header's navigation: the page it names, in place of the one shown.
        if (ev.event == name_intern("nav").hash) {
            bool gallery = ev.node == name_intern("nav_gallery").hash;
            Name want = gallery ? name_intern("kromedemo/gallery.kimb#gallery") : d.lua ? name_intern("kromedemo/form_lua.lua") : name_intern("kromedemo/form.kimb#form");
            if (want == d.form) continue;
            krome_close(e, d.form);
            d.form = gallery ? krome_open(e, "kromedemo/gallery.kimb", "gallery") : d.lua ? krome_open(e, "kromedemo/form_lua.lua") : krome_open(e, "kromedemo/form.kimb", "form");
        }
    }
}

void kromedemo_exit(TickContext& ctx, Span<const ExitRequested>) { engine_request_quit(*ctx.engine); }

bool kromedemo_startup(Engine& e, ModuleInstance& self) {
    Demo* d = arena_push<Demo>(self.arena);
    if (!d) return false;
    *d = Demo{};
    d->demo = frameloop_flag(e, "--demo");
    d->work = frameloop_flag(e, "--work");
    d->hover = frameloop_flag(e, "--hover");
    d->shots = frameloop_option(e, "--shots");
    if (frameloop_flag(e, "--bench")) d->bench = krome_open(e, "kromedemo/bench.lua");
    else if (frameloop_flag(e, "--type")) { d->form = krome_open(e, "kromedemo/type.kimb", "type"); d->type = true; }
    else if (frameloop_flag(e, "--lua")) { d->form = krome_open(e, "kromedemo/form_lua.lua"); d->lua = true; }
    else d->form = krome_open(e, "kromedemo/form.kimb", "form");
    if (const char* theme = frameloop_option(e, "--theme")) krome_theme(e, theme);
    self.set_state(d);
    return true;
}

constinit const SystemDesc k_systems[] = {
    {.name = ".tick", .phase = Phase::PreSim, .fn = kromedemo_tick, .before = "krome.input", .main_thread = true, .runs = RunWhen::Always},
    {.name = ".events", .phase = Phase::Sim, .fn = kromedemo_events, .reads = "KromeEvent", .main_thread = true},
    {.name = ".work", .phase = Phase::Work, .fn = kromedemo_work, .main_thread = true},
};
constinit const HandlerDesc k_handlers[] = {
    {.name = ".exit", KV_ON(ExitRequested, kromedemo_exit), .phase = Phase::PreSim},
};

} // namespace

KV_MODULE_DEFINE(kromedemo_module) = {
    .name     = "kromedemo",
    .systems  = k_systems,
    .handlers = k_handlers,
    .startup  = kromedemo_startup,
    .memory   = {.reserve = 4 * 1024},
};

} // namespace kv

namespace {
constinit const char* const k_flags[] = {"--demo", "--bench", "--work", "--lua", "--type", "--hover"};
constinit const char* const k_options[] = {"--shots", "--theme"};
constinit const kv::FrameLoopConfig k_config = {
    .title          = "korvid krome demo",
    .width          = 1000,
    .height         = 640,
    .exit_on_escape = false, // Escape closes Krome's overlays
    .flags          = k_flags,
    .options        = k_options,
    .pacing         = kv::Pacing::OnDemand, // a tool's window: frames when something happens
};
} // namespace

int main(int argc, char** argv) { return kv::frameloop_main(argc, argv, k_config); }
