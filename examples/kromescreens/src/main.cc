// kromescreens: goldberg's five screens in Krome (docs/PRDs/Krome-Slice-4-PRD.md, KW15), one
// page a tab: inspection, diff review, live status, reports, forms (assets/screens.kim, its
// models and handlers in assets/screens.lua). The game's side plays a cook of 10,000 steps
// into the live status: a few running at once, their progress and the counts sent to the
// screen four times a second (krome_set_doc, krome_set).
//
// --demo drives each page by pointer and keyboard as a person would and checks the model:
// "kromescreens: demo passed", or what did not hold. --shots DIR keeps a picture of each
// page as it goes (each once Krome has settled), then two in the light theme; the cook is
// held still while pictures are taken, so two runs compare. --theme light starts in it.
// --bench sends the 10,000 steps every tick for ten seconds, on the status page: run it with
// --prof-report for what that costs.
#include "korvid/kromescreens/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/json.h"
#include "korvid/core/kbor.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
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

constexpr u32 k_steps = 10000;
constexpr u32 k_running = 8; // steps running at once

struct Cook {
    u8  pct[k_steps];   // 0..100
    u8  state[k_steps]; // 0 waiting, 1 running, 2 done, 3 failed
    u32 next = 0, done = 0, failed = 0, running = 0;
};

struct Screens {
    Name screen;
    bool demo = false, bench = false;
    const char* shots = nullptr;
    u32  step = 0, wait = 0, failures = 0, settled = 0;
    Cook* cook = nullptr;
    u64  sends = 0;
    f64  encode = 0.0;   // seconds spent making the steps' KBOR
    usize bytes = 0;     // the last document's size
};

const char* const k_converters[] = {"texconv", "aseprite", "packer", "kpack", "fontc", "audioc", "kimc"};
const char* const k_states[] = {"waiting", "running", "done", "failed"};

// The cook a step further: running steps advance, the finished make room for the next.
void advance(Cook& c) {
    for (u32 i = 0; i < k_steps; ++i) {
        if (c.state[i] != 1) continue;
        u32 p = c.pct[i] + 7 + (i * 13) % 19;
        if (p >= 100) {
            c.pct[i] = 100;
            bool fail = i % 97 == 41;
            c.state[i] = fail ? 3 : 2;
            if (fail) ++c.failed; else ++c.done;
            --c.running;
        } else c.pct[i] = (u8)p;
    }
    while (c.running < k_running && c.next < k_steps) { c.state[c.next++] = 1; ++c.running; }
}

// The steps as a list of { name, conv, state, pct } and the counts, to the screen.
void send(Engine& e, Screens& s) {
    Cook& c = *s.cook;
    f64 t0 = time_now();
    MemScope scope;
    JsonBuilder b(mem_scratch());
    JsonRef list = b.array();
    for (u32 i = 0; i < k_steps; ++i) {
        JsonRef row = b.object();
        char name[48], pct[8];
        format(name, sizeof name, "assets/step_%05u.png", i + 1);
        format(pct, sizeof pct, "%u%%", c.pct[i]);
        b.set(row, "name", b.string(name));
        b.set(row, "conv", b.string(k_converters[i % KV_COUNTOF(k_converters)]));
        b.set(row, "state", b.string(k_states[c.state[i]]));
        b.set(row, "pct", b.string(pct));
        b.push(list, row);
    }
    JsonDoc doc;
    Array<u8> k{};
    if (b.finish(list, doc)) kbor_write(doc, k);
    b.free();
    s.encode += time_now() - t0;
    s.bytes = k.len;
    ++s.sends;
    krome_set_doc(e, s.screen, "steps", k.data, k.len);
    k.free();
    krome_set(e, s.screen, "done", (f64)c.done);
    krome_set(e, s.screen, "total", (f64)k_steps);
    krome_set(e, s.screen, "running", (f64)c.running);
    krome_set(e, s.screen, "failed", (f64)c.failed);
    char overall[16];
    format(overall, sizeof overall, "%.1f%%", 100.0 * (c.done + c.failed) / k_steps);
    krome_set(e, s.screen, "overall", overall);
}

enum class Act : u8 { Click, Shot, Check, Theme, Type, Key, Wait, Quit };
struct Step { Act act; const char* id; const char* mods; const char* expect; };

// Each page in turn; positions are found when the step runs.
constinit const Step k_pilot[] = {
    // Inspection: the asset's graph and what governed it; a rule opens its .berg.
    {Act::Shot, "inspect", nullptr, nullptr},
    {Act::Click, "atlas", nullptr, nullptr},
    {Act::Click, "up", nullptr, nullptr},            // closes upstream
    {Act::Click, "rule_1", nullptr, nullptr},
    {Act::Shot, "inspect_berg", nullptr, nullptr},
    {Act::Check, "berg_shown", nullptr, "assets.berg:20"},
    {Act::Key, "escape", nullptr, nullptr},          // the dialog's Cancel
    {Act::Check, "confirming", nullptr, ""},
    // Diff review: one accepted, one rejected, then all, confirmed.
    {Act::Click, "page_diff", nullptr, nullptr},
    {Act::Click, "accept_1", nullptr, nullptr},
    {Act::Click, "reject_2", nullptr, nullptr},
    {Act::Click, "accept_3", nullptr, nullptr},      // a third rebuild of the list under the pointer
    {Act::Click, "accept_4", nullptr, nullptr},
    {Act::Check, "accepted", nullptr, "3"},
    {Act::Check, "rejected", nullptr, "1"},
    {Act::Shot, "diff", nullptr, nullptr},
    {Act::Click, "accept_all", nullptr, nullptr},
    {Act::Shot, "diff_confirm", nullptr, nullptr},
    {Act::Key, "enter", nullptr, nullptr},           // the dialog's OK
    {Act::Check, "reroutes_left", nullptr, "0"},
    {Act::Check, "accepted", nullptr, "23"},  // 3 by hand, the other 20 by "all"
    // Live status: the cook as it runs.
    {Act::Click, "page_status", nullptr, nullptr},
    {Act::Wait, nullptr, nullptr, nullptr},
    {Act::Shot, "status", nullptr, nullptr},
    // Reports: a tab, a filter, a sort, a pick.
    {Act::Click, "page_reports", nullptr, nullptr},
    {Act::Click, "rep_budgets", nullptr, nullptr},
    {Act::Check, "report_count", nullptr, "40"},
    {Act::Click, "filter", nullptr, nullptr},
    {Act::Type, "big_1", nullptr, nullptr},
    {Act::Check, "shown_count", nullptr, "10"},
    {Act::Click, "col_rsize", nullptr, nullptr},
    {Act::Click, "b12", nullptr, nullptr},
    {Act::Check, "picked_warning", nullptr, "b12"},
    {Act::Shot, "reports", nullptr, nullptr},
    // Forms: a rule picked, its pattern edited, saved by its shortcut; every field by Tab.
    {Act::Click, "page_forms", nullptr, nullptr},
    {Act::Click, "rule2", nullptr, nullptr},
    {Act::Click, "f_match", nullptr, nullptr},
    {Act::Key, "a", "ctrl", nullptr},
    {Act::Type, "sounds/*.ogg", nullptr, nullptr},
    {Act::Key, "tab", nullptr, nullptr},             // the type
    {Act::Key, "tab", nullptr, nullptr},             // the quality
    {Act::Key, "right", nullptr, nullptr},
    {Act::Key, "tab", nullptr, nullptr},             // mipmaps
    {Act::Key, "space", nullptr, nullptr},
    {Act::Key, "s", "mod", nullptr},                 // the form's mod+s: Command on a Mac
    {Act::Check, "saves", nullptr, "1"},
    {Act::Check, "form_state", nullptr, "saved"},
    {Act::Shot, "forms", nullptr, nullptr},
    {Act::Click, "revert", nullptr, nullptr},
    {Act::Check, "form_state", nullptr, "reverted"},
    // The light theme.
    {Act::Theme, "light", nullptr, nullptr},
    {Act::Shot, "forms_light", nullptr, nullptr},
    {Act::Click, "page_status", nullptr, nullptr},
    {Act::Shot, "status_light", nullptr, nullptr},
    {Act::Quit, nullptr, nullptr, nullptr},
};

f32 window_scale(Engine& e) {
    HostWindow* w = platform_window(e);
    f32 k = w ? w->pixel_scale() : 1.0f;
    return k > 0.0f ? k : 1.0f;
}

bool aim(Engine& e, const Screens& s, const char* id, f32* x, f32* y) {
    rect2 r;
    if (!krome_find_rect(e, s.screen, id, &r)) return false;
    f32 k = window_scale(e);
    *x = (r.x + r.w * 0.5f) * k;
    *y = (r.y + r.h * 0.5f) * k;
    return true;
}

// Phase PreSim, main thread: the cook, and the pilot a step every few ticks.
void kromescreens_tick(TickContext& ctx) {
    Engine& e = *ctx.engine;
    Screens& s = ctx.state_as<Screens>();
    if (!krome_ready(e, s.screen)) return;
    // The cook moves unless pictures are being taken (two runs' pictures then compare).
    bool still = s.shots != nullptr;
    if (s.bench) {
        advance(*s.cook);
        send(e, s);
        engine_want_frame(e);
        // Ten seconds of it, then what making the documents cost (the rest is --prof-report's).
        if (ctx.tick >= 600 && s.sends) {
            KV_INFO("kromescreens", "bench: the 10,000 steps sent %llu times, %.2f ms each to make (%zu KB of KBOR)", (unsigned long long)s.sends,
                    s.encode * 1000.0 / (f64)s.sends, s.bytes / 1024);
            engine_request_quit(e);
        }
        return;
    }
    else if (ctx.tick % 15 == 0 && (!still || s.sends == 0)) {
        if (!still) { advance(*s.cook); advance(*s.cook); }
        else for (u32 i = 0; i < 40; ++i) advance(*s.cook);
        send(e, s);
    }
    if (!s.demo) return;
    engine_want_frame(e);
    if (s.wait) { --s.wait; return; }
    if (s.step >= KV_COUNTOF(k_pilot)) return;
    const Step& st = k_pilot[s.step];
    f32 x = 0, y = 0;
    switch (st.act) {
    case Act::Click: {
        static u32 phase = 0;
        if (!aim(e, s, st.id, &x, &y)) { KV_ERROR("kromescreens", "demo: no '%s' on screen", st.id); ++s.failures; break; }
        input_pointer_override(e, x, y, phase == 1 ? 1 : 0);
        if (++phase < 3) { s.wait = 1; return; }
        phase = 0;
        break;
    }
    case Act::Shot:
        if (s.shots && !krome_settled(e)) { s.settled = 0; return; }
        if (s.shots && s.settled++ < 3) return;
        s.settled = 0;
        if (s.shots) {
            char path[600];
            format(path, sizeof path, "%s/kromescreens_%s.ppm", s.shots, st.id);
            if (Renderer* r = renderer_get(e)) renderer_screenshot(r, path);
        }
        break;
    case Act::Check: {
        const char* got = krome_model_text(e, s.screen, st.id);
        if (strcmp(got, st.expect) != 0) { KV_ERROR("kromescreens", "demo: %s is '%s', wanted '%s'", st.id, got, st.expect); ++s.failures; }
        break;
    }
    case Act::Theme: krome_theme(e, st.id); break;
    case Act::Type: input_text_override(e, st.id); break;
    case Act::Wait: s.wait = 60; break;
    case Act::Key: {
        static u32 phase = 0;
        struct Named { const char* name; Key key; };
        static const Named keys[] = {{"enter", Key::Enter}, {"escape", Key::Escape}, {"tab", Key::Tab}, {"space", Key::Space},
                                     {"right", Key::Right}, {"left", Key::Left}, {"a", Key::A}, {"s", Key::S}};
        Key k = Key::Unknown;
        for (const Named& n : keys) if (!strcmp(n.name, st.id)) k = n.key;
        // "ctrl", or "mod": a shortcut's modifier as Krome reads `mod+`, Command on a Mac.
        Key held = Key::Unknown;
        if (st.mods && !strcmp(st.mods, "ctrl")) held = Key::LCtrl;
        if (st.mods && !strcmp(st.mods, "mod")) held = KV_DEMO_MOD;
        if (phase == 0 && held != Key::Unknown) input_key_override(e, held, true);
        if (phase == 1) { input_key_override(e, k, true); input_key_override(e, k, false); }
        if (phase == 2 && held != Key::Unknown) input_key_override(e, held, false);
        if (++phase < 3) { s.wait = 1; return; }
        phase = 0;
        break;
    }
    case Act::Quit:
        KV_INFO("kromescreens", "the steps sent %llu times, %.2f ms each to make (%zu KB of KBOR)", (unsigned long long)s.sends,
                s.sends ? s.encode * 1000.0 / (f64)s.sends : 0.0, s.bytes / 1024);
        if (s.failures) KV_ERROR("kromescreens", "demo: %u checks failed", s.failures);
        else KV_INFO("kromescreens", "demo passed");
        engine_request_quit(e);
        break;
    }
    ++s.step;
    s.wait = 6;
}

void kromescreens_events(TickContext& ctx) {
    for (const KromeEvent& ev : ctx.received<KromeEvent>())
        KV_INFO("kromescreens", "event %s from %s", name_str(Name{ev.event}), ev.node ? name_str(Name{ev.node}) : "the script");
}

void kromescreens_exit(TickContext& ctx, Span<const ExitRequested>) { engine_request_quit(*ctx.engine); }

bool kromescreens_startup(Engine& e, ModuleInstance& self) {
    Screens* s = arena_push<Screens>(self.arena);
    if (!s) return false;
    *s = Screens{};
    s->cook = arena_push<Cook>(self.arena);
    if (!s->cook) return false;
    *s->cook = Cook{};
    s->demo = frameloop_flag(e, "--demo");
    s->bench = frameloop_flag(e, "--bench");
    s->shots = frameloop_option(e, "--shots");
    s->screen = krome_open(e, "kromescreens/screens.kimb", "screens");
    if (const char* theme = frameloop_option(e, "--theme")) krome_theme(e, theme);
    if (s->bench) krome_set(e, s->screen, "page", "status");
    self.set_state(s);
    return true;
}

constinit const SystemDesc k_systems[] = {
    {.name = ".tick", .phase = Phase::PreSim, .fn = kromescreens_tick, .before = "krome.input", .main_thread = true, .runs = RunWhen::Always},
    {.name = ".events", .phase = Phase::Sim, .fn = kromescreens_events, .reads = "KromeEvent", .main_thread = true},
};
constinit const HandlerDesc k_handlers[] = {
    {.name = ".exit", KV_ON(ExitRequested, kromescreens_exit), .phase = Phase::PreSim},
};

} // namespace

KV_MODULE_DEFINE(kromescreens_module) = {
    .name     = "kromescreens",
    .systems  = k_systems,
    .handlers = k_handlers,
    .startup  = kromescreens_startup,
    .memory   = {.reserve = 64 * 1024},
};

} // namespace kv

namespace {
constinit const char* const k_flags[] = {"--demo", "--bench"};
constinit const char* const k_options[] = {"--shots", "--theme"};
constinit const kv::FrameLoopConfig k_config = {
    .title          = "korvid krome: goldberg's screens",
    .width          = 1100,
    .height         = 700,
    .exit_on_escape = false,
    .flags          = k_flags,
    .options        = k_options,
    .pacing         = kv::Pacing::OnDemand,
};
} // namespace

int main(int argc, char** argv) { return kv::frameloop_main(argc, argv, k_config); }
