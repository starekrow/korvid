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
// --prof-report for the cost. --two opens a second window, a playlist, beside the form (F2
// hides it and shows it); --two-check counts what each presents while only the other changes,
// and hides and shows it (slice 13). --hover moves the pointer onto the Save button and off it
// every tick for ten seconds, then quits: the cost of a frame where one button changes (run it with
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
    // --two: a second window, the playlist (docs/PRDs/Engine-Slice-13-PRD.md, KR40); F2 hides it
    // and shows it again, and closing it hides it. --two-check drives the pointer in each
    // window, counting each one's presents, scrolls the list, hides and shows its window, and
    // says whether it held.
    bool two = false, two_check = false;
    HostWindow* list_win = nullptr;
    Name list;
    u32  check_step = 0, check_ticks = 0, check_bad = 0;
    u64  p0 = 0, p1 = 0;
    rect2 kept;
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
    {Act::Click, "skin_stop", 0, nullptr},   // a region of the skin's sheet takes a press as any node does
    {Act::Check, "last", 0, "skin_stop"},
    {Act::Click, "textures", 0, nullptr},  // closes the textures folder
    {Act::Click, "tab_table", 0, nullptr},
    {Act::Shot, "gallery_table", 0, nullptr},
    {Act::Click, "r1", 0, nullptr},
    {Act::Check, "picked_row", 0, "r1"},
    {Act::Click, "col_size", 0, nullptr},    // sorts by size: the smallest first
    {Act::Click, "r4096", 0, nullptr},
    {Act::Check, "picked_row", 0, "r4096"},
    {Act::Click, "tab_icons", 0, nullptr},
    {Act::Shot, "gallery_icons", 0, nullptr},
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
    {Act::Click, "tab_icons", 0, nullptr},
    {Act::Shot, "gallery_icons_light", 0, nullptr},
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

// A node of the playlist, in its window's pixels.
bool aim_list(Engine& e, const Demo& d, const char* id, f32* x, f32* y) {
    rect2 r;
    if (!d.list_win || !krome_find_rect(e, d.list, id, &r)) return false;
    f32 k = d.list_win->pixel_scale();
    *x = (r.x + r.w * 0.5f) * k;
    *y = (r.y + r.h * 0.5f) * k;
    return true;
}

u64 presents(Engine& e, u32 surface) {
    Renderer* r = renderer_get(e);
    return r ? renderer_surface_presents(r, surface) : 0;
}

// --two-check, a step a few ticks: what each window presents while only the other changes,
// and the list's scroll across a hide.
void two_check(Engine& e, Demo& d) {
    engine_want_frame(e);
    u32 t = d.check_ticks++;
    f32 x = 0, y = 0;
    auto next = [&]() { ++d.check_step; d.check_ticks = 0; };
    auto note = [&]() { d.p0 = presents(e, 0); d.p1 = presents(e, 1); };
    switch (d.check_step) {
    case 0: // both screens up and settled
        if (!krome_ready(e, d.form) || !krome_ready(e, d.list) || !krome_settled(e)) { d.check_ticks = 0; return; }
        if (t < 10) return;
        note();
        next();
        return;
    case 1: // the main window changes: on and off its Save button
        if (t < 40) {
            if ((t & 1) && aim(e, d, "save", &x, &y)) input_pointer_override(e, x, y, 0, 0.0f, 0);
            else input_pointer_override(e, 4.0f, 4.0f, 0, 0.0f, 0);
            return;
        }
        {
            u64 d0 = presents(e, 0) - d.p0, d1 = presents(e, 1) - d.p1;
            KV_INFO("kromedemo", "two: the main window hovered: it presented %llu, the playlist %llu", (unsigned long long)d0, (unsigned long long)d1);
            if (!d0 || d1) { KV_ERROR("kromedemo", "two: wanted the main window's presents only"); ++d.check_bad; }
        }
        next();
        return;
    case 2: // into the playlist, on its header, and settled
        input_pointer_override(e, 20.0f, 10.0f, 0, 0.0f, 1);
        if (t < 10) return;
        note();
        next();
        return;
    case 3: // the playlist changes: on and off a track
        if (t < 40) {
            if ((t & 1) && aim_list(e, d, "track3", &x, &y)) input_pointer_override(e, x, y, 0, 0.0f, 1);
            else input_pointer_override(e, 20.0f, 10.0f, 0, 0.0f, 1);
            return;
        }
        {
            u64 d0 = presents(e, 0) - d.p0, d1 = presents(e, 1) - d.p1;
            KV_INFO("kromedemo", "two: the playlist hovered: it presented %llu, the main window %llu (%llu frames left it as it was)",
                    (unsigned long long)d1, (unsigned long long)d0, (unsigned long long)frameloop_main_skipped(e));
            if (!d1 || d0) { KV_ERROR("kromedemo", "two: wanted the playlist's presents only"); ++d.check_bad; }
        }
        next();
        return;
    case 4: // the list scrolled down, and where a track is then
        if (t < 3 && aim_list(e, d, "track10", &x, &y)) { input_pointer_override(e, x, y, 0, -4.0f, 1); return; }
        if (t < 15) return;
        if (!krome_find_rect(e, d.list, "track40", &d.kept)) { KV_ERROR("kromedemo", "two: no track40"); ++d.check_bad; }
        KV_INFO("kromedemo", "two: scrolled, track40 at y %.1f", d.kept.y);
        if (d.kept.y > 300.0f) { KV_ERROR("kromedemo", "two: the list did not scroll"); ++d.check_bad; }
        input_pointer_override(e, 4.0f, 4.0f, 0, 0.0f, 0); // out of the playlist
        next();
        return;
    case 5: // hidden
        if (t == 0) { d.list_win->show(false); return; }
        if (t < 20) return;
        if (d.list_win->visible()) { KV_ERROR("kromedemo", "two: the playlist is seen after show(false)"); ++d.check_bad; }
        note();
        next();
        return;
    case 6: // shown again: drawn, its scroll where it was
        if (t == 0) { d.list_win->show(true); return; }
        if (t < 20) return;
        {
            rect2 r;
            bool found = krome_find_rect(e, d.list, "track40", &r);
            u64 d1 = presents(e, 1) - d.p1;
            KV_INFO("kromedemo", "two: shown again: presented %llu, track40 at y %.1f (was %.1f)", (unsigned long long)d1, found ? r.y : -1.0f, d.kept.y);
            if (!found || r.y != d.kept.y) { KV_ERROR("kromedemo", "two: the list's scroll moved across the hide"); ++d.check_bad; }
            if (!d1 || !d.list_win->visible()) { KV_ERROR("kromedemo", "two: the playlist was not drawn again"); ++d.check_bad; }
        }
        next();
        return;
    case 7: { // one keyboard across both: a press in each window, and what is typed after it
        // Three ticks a click (over it, down, up), then the text, in window 0 and then 1.
        bool second = t >= 6;
        u32 at = second ? t - 6 : t;
        bool aimed = second ? aim_list(e, d, "query", &x, &y) : aim(e, d, "notes", &x, &y);
        if (at < 3) {
            if (!aimed) { KV_ERROR("kromedemo", "two: no %s on screen", second ? "query" : "notes"); ++d.check_bad; d.check_ticks = 12; return; }
            input_pointer_override(e, x, y, at == 1 ? 1 : 0, 0.0f, second ? 1 : 0);
            return;
        }
        if (at == 3) { input_text_override(e, second ? "cd" : "ab"); return; }
        if (t < 16) return;
        const char* notes = krome_model_text(e, d.form, "notes");
        char notes_copy[64];
        format(notes_copy, sizeof notes_copy, "%s", notes);
        const char* query = krome_model_text(e, d.list, "query");
        KV_INFO("kromedemo", "two: typed in each: notes '%s', query '%s'", notes_copy, query);
        if (strcmp(notes_copy, "ab") != 0 || strcmp(query, "cd") != 0) { KV_ERROR("kromedemo", "two: wanted notes 'ab' and query 'cd'"); ++d.check_bad; }
        if (d.check_bad) KV_ERROR("kromedemo", "two: %u checks failed", d.check_bad);
        else KV_INFO("kromedemo", "two: passed");
        d.failures += d.check_bad;
        engine_request_quit(e);
        next();
        return;
    }
    default: return;
    }
}

// Phase PreSim, main thread: the pilot, a step every few ticks; and the tick count, every
// half second, for the form's title bar.
void kromedemo_tick(TickContext& ctx) {
    Engine& e = *ctx.engine;
    Demo& d = ctx.state_as<Demo>();
    // --two: the playlist window, opened once the loop and its renderer are up.
    if (d.two && !d.list_win && d.list.is_null()) {
        WindowDesc wd;
        wd.title  = "korvid krome demo: playlist";
        wd.width  = 360;
        wd.height = 480;
        d.list_win = frameloop_open_window(e, wd);
        if (d.list_win) d.list = krome_open(e, "kromedemo/playlist.lua", KROME_DEFAULT_LAYER, d.list_win->id());
        else {
            KV_ERROR("kromedemo", "two: the playlist window did not open");
            d.list = name_intern("none");
            if (d.two_check) engine_request_quit(e);
        }
    }
    if (d.two_check) { if (d.list_win) two_check(e, d); return; } // nothing else moves: what presents is what the check did
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
        if (s.expect && !strcmp(s.expect, "mod")) held = KV_PLATFORM_MOD_KEY;
        if (phase == 0 && held != Key::Unknown) input_key_override(e, held, true);
        if (phase == 1) { input_key_override(e, k, true); input_key_override(e, k, false); }
        if (phase == 2 && held != Key::Unknown) input_key_override(e, held, false);
        if (++phase < 3) { d.wait = 1; return; }
        phase = 0;
        break;
    }
    case Act::Quit:
        // The Icons page's 280 px icon is rasterized on a job.
        if (krome_stats(e).shape_jobs == 0) { KV_ERROR("kromedemo", "demo: no shape was rasterized on a job"); ++d.failures; }
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

// --two: closing the playlist hides it; F2 hides it or shows it again.
void kromedemo_close(TickContext& ctx, Span<const CloseRequested> evs) {
    Demo& d = ctx.state_as<Demo>();
    for (const CloseRequested& c : evs)
        if (d.list_win && c.window == d.list_win->id()) d.list_win->show(false);
}
void kromedemo_key(TickContext& ctx, Span<const KeyDown> evs) {
    Demo& d = ctx.state_as<Demo>();
    for (const KeyDown& k : evs)
        if (k.key == Key::F2 && !k.repeat && d.list_win) d.list_win->show(!d.list_win->visible());
}

bool kromedemo_startup(Engine& e, ModuleInstance& self) {
    Demo* d = arena_push<Demo>(self.arena);
    if (!d) return false;
    *d = Demo{};
    d->demo = frameloop_flag(e, "--demo");
    d->work = frameloop_flag(e, "--work");
    d->hover = frameloop_flag(e, "--hover");
    d->two_check = frameloop_flag(e, "--two-check");
    d->two = d->two_check || frameloop_flag(e, "--two");
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
    {.name = ".close", KV_ON(CloseRequested, kromedemo_close), .phase = Phase::PreSim},
    {.name = ".key", KV_ON(KeyDown, kromedemo_key), .phase = Phase::PreSim},
};

} // namespace

KV_MODULE_DEFINE(kromedemo_module) = {
    .name     = "kromedemo",
    .systems  = k_systems,
    .handlers = k_handlers,
    .startup  = kromedemo_startup,
    .memory   = {.reserve = 8 * 1024},
};

} // namespace kv

namespace {
constinit const char* const k_flags[] = {"--demo", "--bench", "--work", "--lua", "--type", "--hover", "--two", "--two-check"};
constinit const char* const k_options[] = {"--shots", "--theme"};
constinit const kv::FrameLoopConfig k_config = {
    .title          = "korvid krome demo",
    .width          = 1000,
    .height         = 640,
    .cpu_scale      = 1, // a tool's window: sharp text on the CPU renderer, and a frame costs only what changed
    .exit_on_escape = false, // Escape closes Krome's overlays
    .flags          = k_flags,
    .options        = k_options,
    .pacing         = kv::Pacing::OnDemand, // a tool's window: frames when something happens
};
} // namespace

int main(int argc, char** argv) { return kv::frameloop_main(argc, argv, k_config); }
