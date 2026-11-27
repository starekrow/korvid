// Placed, moving, and not a rectangle (docs/PRDs/Krome-Slice-8-PRD.md), through a running
// Krome: edges and offsets written in .kim and bound to the model.
#include "korvid/krome/krome.h"
#include "../src/internal.h"
#include "korvid/asset/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/test.h"
#include "korvid/core/format.h"
#include "korvid/core/kbor.h"
#include "korvid/core/time.h"
#include "korvid/kim/kim.h"
#include "korvid/platform/module.h"
#include "korvid/platform_hid/module.h"
#include <stdlib.h>
#include <string.h>

namespace {

using namespace kv;

void frames(Engine& e, u32 n) {
    for (u32 i = 0; i < n; ++i) { platform_pump(e); engine_frame(e, 1.0 / 60.0); engine_frame_cycle(e, 1.0 / 60.0); }
}

bool has_error(Engine& e, const char* want) {
    const KimErrors* errs = krome_errors(e);
    char line[300];
    for (u32 i = 0; errs && i < errs->count; ++i) {
        errs->format_one(i, line, sizeof line);
        if (strstr(line, want)) return true;
    }
    KV_ERROR("test", "no error '%s' among:", want);
    for (u32 i = 0; errs && i < errs->count; ++i) { errs->format_one(i, line, sizeof line); KV_ERROR("test", "  %s", line); }
    return false;
}

bool rect_is(const rect2& r, f32 x, f32 y, f32 w, f32 h) {
    auto near = [](f32 a, f32 b) { return a - b < 0.001f && b - a < 0.001f; };
    if (near(r.x, x) && near(r.y, y) && near(r.w, w) && near(r.h, h)) return true;
    KV_ERROR("test", "rect (%g, %g, %g, %g), wanted (%g, %g, %g, %g)", r.x, r.y, r.w, r.h, x, y, w, h);
    return false;
}

const rect2& rect_of(Engine& e, Name screen, const char* id) {
    static const rect2 none{-1, -1, -1, -1};
    const KromeTree* t = krome_tree(e, screen);
    u32 n = t ? krome_find(*t, name_intern(id)) : KROME_NONE;
    return n == KROME_NONE ? none : t->nodes[n].rect;
}

// Compiled from .kim text, given to Krome, opened.
Name open_kim(TestCtx& c, Engine& e, const char* file, const char* text, const char* screen) {
    Array<u8> k{};
    KimErrors errs;
    KV_CHECK(c, kim_compile(text, strlen(text), file, k, errs) && errs.count == 0);
    char path[128];
    usize n = strlen(file);
    memcpy(path, file, n);
    memcpy(path + n, "b", 2); // t/x.kim -> t/x.kimb
    krome_provide(e, path, k.data, k.len);
    k.free();
    return krome_open(e, path, screen);
}

void provide_json(Engine& e, const char* path, const char* json) {
    JsonDoc doc;
    if (!json_parse(json, strlen(json), mem_scratch(), doc)) { KV_ERROR("test", "%s: %s at line %u", path, doc.error, doc.error_line); return; }
    Array<u8> k{};
    kbor_write(doc, k);
    krome_provide(e, path, k.data, k.len);
    k.free();
}

const ModuleDesc* const k_list[] = {&platform_module, &platform_hid_module, &asset_module, &krome_module};

void test_edges_kim(TestCtx& c) {
    c.begin("krome placed: edges in .kim");
    Engine e;
    KV_CHECK(c, engine_init(e, k_list));
    krome_viewport(e, 300, 200, 1.0f);
    // A drawer bound to the model, a bar across, a badge at a corner, a mark two thirds along.
    const char* text =
        "screen main { box dir=stack pad=10 {\n"
        "  box id=bar l=10 r=10 h=20\n"
        "  box id=badge r=30 b=8 w=16 h=16\n"
        "  box id=mark l=66.7% t=100%-12 w=4 h=12\n"
        "  box id=drawer w=100 h=50 x=@drawer\n"
        "} }\n";
    Name scn = open_kim(c, e, "t/edges.kim", text, "main");
    frames(e, 3);
    KV_CHECK(c, krome_ready(e, scn));
    KV_CHECK(c, rect_is(rect_of(e, scn, "bar"), 20, 10, 260, 20));
    KV_CHECK(c, rect_is(rect_of(e, scn, "badge"), 244, 166, 16, 16));
    KV_CHECK(c, rect_is(rect_of(e, scn, "mark"), 10 + 280 * 0.667f, 178, 4, 12));
    KV_CHECK(c, rect_is(rect_of(e, scn, "drawer"), 10, 10, 100, 50)); // nothing in the model: no offset
    // The window resized: each follows its edge.
    krome_viewport(e, 400, 250, 1.0f);
    frames(e, 1);
    KV_CHECK(c, rect_is(rect_of(e, scn, "bar"), 20, 10, 360, 20));
    KV_CHECK(c, rect_is(rect_of(e, scn, "badge"), 344, 216, 16, 16));
    // Bound: the model moves it.
    krome_set(e, scn, "drawer", -120.0);
    frames(e, 2);
    KV_CHECK(c, rect_is(rect_of(e, scn, "drawer"), -110, 10, 100, 50));

    // In a row or a column, or at the root, each is a mistake naming why.
    krome_clear_errors(e);
    const char* bad =
        "screen main { box dir=row {\n"
        "  box id=a l=4 w=10\n"
        "  box dir=col { box id=b y=3 }\n"
        "} }\n";
    Name b = open_kim(c, e, "t/notstack.kim", bad, "main");
    frames(e, 3);
    KV_CHECK(c, krome_ready(e, b));
    KV_CHECK(c, has_error(e, "t/notstack.kim:2: l= places a child of a stack, and this one's parent is a row"));
    KV_CHECK(c, has_error(e, "t/notstack.kim:3: y= places a child of a stack, and this one's parent is a col"));
    KV_CHECK(c, rect_is(rect_of(e, b, "a"), 0, 0, 10, 0));
    engine_shutdown(e);
}

// Easing (KR35): a drawer bound to the model slides 120 pixels in 0.2 s, a frame asked for
// each frame while it moves and none after; its handler is told once when it arrives.
const char* k_drawer = R"lua(
return {
  model = { drawer = 0, wide = 40, count = 0, who = "", snap = 0, snapped = 0 },
  on = {
    opened = function(id) model.count = model.count + 1; model.who = id end,
    snapped = function(id) model.snapped = model.snapped + 1 end,
  },
  root = box { dir = "stack",
    box { id = "panel", w = "@wide", h = 50, x = "@drawer", transition = 0.2, arrived = "opened" },
    box { id = "quick", w = 10, h = 10, y = "@snap", arrived = "snapped" },
    box { id = "bar", l = 10, r = 10, h = 4, transition = 0.2 },
  },
}
)lua";

void test_easing(TestCtx& c) {
    c.begin("krome placed: easing");
    Engine e;
    KV_CHECK(c, engine_init(e, k_list));
    KromeModule& s = *krome_state(e);
    krome_viewport(e, 300, 200, 1.0f);
    Name scn = krome_open_source(e, "t/drawer", k_drawer);
    for (u32 i = 0; i < 100 && !(krome_ready(e, scn) && krome_settled(e)); ++i) frames(e, 1);
    KV_CHECK(c, krome_ready(e, scn) && krome_settled(e));
    KV_CHECK(c, rect_is(rect_of(e, scn, "panel"), 0, 0, 40, 50));
    frames(e, 30); // the colours' transitions from the first paint, over

    // 120 pixels in 0.2 s at 60 frames a second: twelve frames that move, always further, the
    // last at 120; a frame asked for until then, none after, and nothing painted.
    krome_set(e, scn, "drawer", 120.0);
    f32 last = 0.0f;
    u32 moving = 0, asked = 0, asked_after = 0, backwards = 0;
    bool arrived = false;
    for (u32 i = 0; i < 40; ++i) {
        e.frame_wanted = false;
        frames(e, 1);
        f32 x = rect_of(e, scn, "panel").x;
        if (x < last) ++backwards;
        if (!arrived && x != last) ++moving;
        if (e.frame_wanted) (arrived ? asked_after : asked) += 1;
        last = x;
        if (!arrived && s.motions.len == 0) arrived = true;
    }
    KV_CHECK(c, last == 120.0f && backwards == 0 && moving >= 11 && moving <= 13 && asked >= 11 && asked <= 13 && asked_after == 0);
    if (moving < 11 || moving > 13 || asked_after) KV_ERROR("test", "moved on %u frames, asked %u while moving, %u after", moving, asked, asked_after);
    KV_CHECK(c, !strcmp(krome_model_text(e, scn, "count"), "1") && !strcmp(krome_model_text(e, scn, "who"), "panel"));
    KV_CHECK(c, s.motions.len == 0 && krome_settled(e));
    // Asleep: nothing laid out, painted or asked for.
    u64 paints = krome_stats(e).paints, layouts = krome_stats(e).layouts;
    u32 woke = 0;
    for (u32 i = 0; i < 30; ++i) { e.frame_wanted = false; e.frame_due = 0.0; frames(e, 1); if (e.frame_wanted || e.frame_due > 0.0) ++woke; }
    KV_CHECK(c, krome_stats(e).paints == paints && krome_stats(e).layouts == layouts && woke == 0);

    // Turned around mid-way: back to 0 from where it was, arriving once, at 0.
    krome_set(e, scn, "drawer", 0.0);
    frames(e, 6);
    f32 mid = rect_of(e, scn, "panel").x;
    KV_CHECK(c, mid > 0.0f && mid < 120.0f);
    krome_set(e, scn, "drawer", 120.0);
    frames(e, 2);
    KV_CHECK(c, rect_of(e, scn, "panel").x > mid); // from where it was, not from 0 nor 120
    krome_set(e, scn, "drawer", 0.0);
    frames(e, 40);
    KV_CHECK(c, rect_of(e, scn, "panel").x == 0.0f && !strcmp(krome_model_text(e, scn, "count"), "2"));

    // A size and an offset together, one arrival when both are there.
    krome_set(e, scn, "drawer", 60.0);
    frames(e, 3);
    krome_set(e, scn, "wide", 80.0);
    frames(e, 3);
    const rect2& p = rect_of(e, scn, "panel");
    KV_CHECK(c, p.w > 40.0f && p.w < 80.0f && p.x > 0.0f && p.x < 60.0f);
    frames(e, 40);
    KV_CHECK(c, rect_is(rect_of(e, scn, "panel"), 60, 0, 80, 50) && !strcmp(krome_model_text(e, scn, "count"), "3"));

    // With no transition it is there at once, and arrived.
    krome_set(e, scn, "snap", 30.0);
    frames(e, 1);
    KV_CHECK(c, rect_of(e, scn, "quick").y == 30.0f && !strcmp(krome_model_text(e, scn, "snapped"), "1"));

    // A change made by layout (the window resized) does not ease.
    krome_viewport(e, 500, 200, 1.0f);
    frames(e, 1);
    KV_CHECK(c, rect_is(rect_of(e, scn, "bar"), 10, 0, 480, 4) && s.motions.len == 0);
    engine_shutdown(e);
}

// Timers (KR35): a 500 ms timer costs two wakes a second in a window paced on demand, run
// here on a clock of the test's own: a frame cycle only when Krome asked for one (or a
// frame), at the time it asked for.
const char* k_timers = R"lua(
local h
return {
  model = { ticks = 0, once = 0, after_cancel = 0, bad = 0 },
  on = {
    stop = function() cancel(h) end,
    start = function() h = every(0.5, function() model.ticks = model.ticks + 1 end) end,
    once = function()
      after(0.25, function() model.once = model.once + 1 end)
      local gone = after(0.1, function() model.after_cancel = 1 end)
      cancel(gone)
      cancel(gone) -- nothing, a second time
    end,
    bad = function() every(0.1, function() model.bad = model.bad + 1; error("no") end) end,
    many = function() for i = 1, 300 do after(100, function() end) end end,
  },
  root = col {
    text { id = "shown", "{ticks}" },
    hit { id = "start", event = "start", w = 10, h = 10 },
    hit { id = "stop", event = "stop", w = 10, h = 10 },
    hit { id = "once", event = "once", w = 10, h = 10 },
    hit { id = "bad", event = "bad", w = 10, h = 10 },
    hit { id = "many", event = "many", w = 10, h = 10 },
  },
}
)lua";

struct Loop {
    Engine& e;
    u64 ms = 0;
    f64 vt = 0.0, last = 0.0, vdue = 0.0;
    u32 wakes = 0;
    void frame(f64 dt) { platform_pump(e); engine_frame(e, dt); engine_frame_cycle(e, dt); }
    void note() { vdue = e.frame_due > 0.0 ? vt + (e.frame_due - time_now()) : 0.0; e.frame_due = 0.0; }
    // `secs` of the clock at 1 ms: a frame whenever one is wanted or due.
    void run(f64 secs) {
        u64 end = ms + (u64)(secs * 1000.0 + 0.5);
        while (ms < end) {
            vt = (f64)++ms * 0.001;
            if (!e.frame_wanted && !(vdue > 0.0 && vt >= vdue - 1e-9)) continue;
            e.frame_wanted = false;
            frame(vt - last);
            last = vt;
            ++wakes;
            note();
        }
    }
    // Hidden: no frame at all for `secs`, then the one it is seen again.
    void hidden(f64 secs) { ms += (u64)(secs * 1000.0 + 0.5); vt = (f64)ms * 0.001; e.frame_wanted = false; frame(vt - last); last = vt; note(); }
};

void test_timers(TestCtx& c) {
    c.begin("krome placed: timers");
    Engine e;
    KV_CHECK(c, engine_init(e, k_list));
    KromeModule& s = *krome_state(e);
    krome_viewport(e, 300, 200, 1.0f);
    krome_provide(e, "t/timers.lua", k_timers, strlen(k_timers));
    Name scn = krome_open(e, "t/timers.lua");
    for (u32 i = 0; i < 200 && !(krome_ready(e, scn) && krome_settled(e)); ++i) frames(e, 1);
    frames(e, 30);
    KV_CHECK(c, krome_ready(e, scn) && krome_settled(e));
    Loop lp{e};
    // Nothing set: no wakes.
    e.frame_due = 0.0;
    e.frame_wanted = false;
    lp.run(2.0);
    KV_CHECK(c, lp.wakes == 0);

    // Ten seconds shown: twenty ticks, and a wake for each (the press's own frame aside).
    KV_CHECK(c, krome_press(e, scn, "start"));
    lp.wakes = 0;
    lp.run(10.0);
    u32 ticks = (u32)atoi(krome_model_text(e, scn, "ticks"));
    KV_INFO("test", "a 500 ms timer: %u wakes, %u ticks in ten seconds on demand", lp.wakes, ticks);
    KV_CHECK(c, ticks >= 19 && ticks <= 20 && lp.wakes >= ticks && lp.wakes <= ticks + 2);
    KV_CHECK(c, !strcmp(krome_tree(e, scn)->text(krome_find(*krome_tree(e, scn), name_intern("shown"))), krome_model_text(e, scn, "ticks")));
    // Ten seconds hidden: nothing runs, and once seen it fires once, not twenty times.
    lp.hidden(10.0);
    KV_CHECK(c, (u32)atoi(krome_model_text(e, scn, "ticks")) == ticks + 1);
    lp.run(1.0);
    KV_CHECK(c, (u32)atoi(krome_model_text(e, scn, "ticks")) == ticks + 3);
    // Cancelled: no more ticks, no more wakes.
    KV_CHECK(c, krome_press(e, scn, "stop"));
    ticks = (u32)atoi(krome_model_text(e, scn, "ticks"));
    lp.run(0.6); // the frame the timer had already asked for, which the loop keeps
    lp.wakes = 0;
    lp.run(3.0);
    KV_CHECK(c, (u32)atoi(krome_model_text(e, scn, "ticks")) == ticks && lp.wakes == 0 && s.timers.len == 0);

    // after: once; one cancelled before it fired never does.
    KV_CHECK(c, krome_press(e, scn, "once"));
    lp.run(1.0);
    KV_CHECK(c, !strcmp(krome_model_text(e, scn, "once"), "1") && !strcmp(krome_model_text(e, scn, "after_cancel"), "0") && s.timers.len == 0);

    // One that throws is cancelled after its first call, and named by the line it was set at.
    krome_clear_errors(e);
    KV_CHECK(c, krome_press(e, scn, "bad"));
    lp.run(1.0);
    KV_CHECK(c, !strcmp(krome_model_text(e, scn, "bad"), "1") && s.timers.len == 0);
    KV_CHECK(c, has_error(e, "a timer set at t/timers.lua:14 raised, and is cancelled"));

    // At most 256 a screen: the 257th is refused (the handler raises), the rest stand.
    KV_CHECK(c, krome_press(e, scn, "many"));
    KV_CHECK(c, s.timers.len == KROME_MAX_TIMERS);
    // A reload runs the script again: the old run's timers go.
    {
        char again[4096];
        format(again, sizeof again, "%s-- again\n", k_timers);
        krome_provide(e, "t/timers.lua", again, strlen(again));
    }
    for (u32 i = 0; i < 10; ++i) frames(e, 1);
    KV_CHECK(c, s.timers.len == 0);
    // And a screen's go when it closes.
    KV_CHECK(c, krome_press(e, scn, "start"));
    KV_CHECK(c, s.timers.len == 1);
    krome_close(e, scn);
    frames(e, 2);
    KV_CHECK(c, s.timers.len == 0);
    engine_shutdown(e);
}

// A behaviour's timer is its screen's: set from update, fired later, closed with it.
const char* k_blink_prelude = R"({"kim": 1, "file": "t/blink.kim", "scripts": ["t/blink.lua"]})";
const char* k_blink_script = R"lua(
return {
  blink = {
    update = function(w)
      if w.state.t then return end
      w.state.t = every(0.25, function() w:set((w.value or 0) + 1) end)
    end,
  },
}
)lua";
const char* k_blink_screen = R"({"kim": 1, "file": "t/bs.kim", "uses": ["t/blink"],
  "screens": { "main": { "w": "box", "c": [ { "w": "box", "id": "b", "b": "n", "p": { "behave": "blink", "w": 10, "h": 10 } } ] } } })";

void test_behaviour_timers(TestCtx& c) {
    c.begin("krome placed: a behaviour's timer");
    Engine e;
    KV_CHECK(c, engine_init(e, k_list));
    KromeModule& s = *krome_state(e);
    krome_viewport(e, 300, 200, 1.0f);
    provide_json(e, "t/blink.kimb", k_blink_prelude);
    krome_provide(e, "t/blink.lua", k_blink_script, strlen(k_blink_script));
    provide_json(e, "t/bs.kimb", k_blink_screen);
    Name scn = krome_open(e, "t/bs.kimb", "main");
    for (u32 i = 0; i < 100 && !krome_ready(e, scn); ++i) frames(e, 1);
    KV_CHECK(c, krome_ready(e, scn) && s.timers.len == 1 && s.timers.len && s.timers[0].screen == krome_screen(s, scn)->id);
    frames(e, 60);
    i32 n = atoi(krome_model_text(e, scn, "n"));
    KV_CHECK(c, n >= 3 && n <= 4);
    krome_close(e, scn);
    frames(e, 2);
    KV_CHECK(c, s.timers.len == 0);
    engine_shutdown(e);
}

// Text that scrolls (KR37): the timeline by hand, then wakes counted through a running Krome.
void test_marquee_offsets(TestCtx& c) {
    c.begin("krome placed: a marquee's timeline");
    KromeNode n;
    n.marquee = 10.0f;     // 10 pixels a second
    n.marquee_gap = 20.0f; // a 100-wide text in 60: a loop is 120 long, 12 s
    f64 next = 0.0;
    auto near = [](f64 a, f64 b) { return a - b < 1e-6 && b - a < 1e-6; };
    KV_CHECK(c, krome_marquee_offset(n, 100, 60, 0.0, 1.0f, &next) == 0.0f && near(next, 0.1));
    KV_CHECK(c, krome_marquee_offset(n, 100, 60, 2.55, 1.0f, &next) == 25.0f && near(next, 2.6));
    KV_CHECK(c, krome_marquee_offset(n, 100, 60, 11.95, 1.0f, &next) == 119.0f && near(next, 12.0));
    KV_CHECK(c, krome_marquee_offset(n, 100, 60, 12.0, 1.0f, &next) == 0.0f && near(next, 12.1)); // round again
    // It fits: still, and nothing next.
    KV_CHECK(c, krome_marquee_offset(n, 50, 60, 3.0, 1.0f, &next) == 0.0f && next == 0.0);
    // Stepped by 8: moves every 0.8 s.
    KV_CHECK(c, krome_marquee_offset(n, 100, 60, 1.7, 8.0f, &next) == 16.0f && near(next, 2.4));
    // A rest of 1.5 s before it starts and at each end of a loop.
    n.marquee_pause = 1.5f;
    KV_CHECK(c, krome_marquee_offset(n, 100, 60, 1.0, 1.0f, &next) == 0.0f && near(next, 1.6));
    KV_CHECK(c, krome_marquee_offset(n, 100, 60, 3.5, 1.0f, &next) == 20.0f && near(next, 3.6));
    KV_CHECK(c, krome_marquee_offset(n, 100, 60, 14.0, 1.0f, &next) == 0.0f && near(next, 15.1)); // resting again
    // Bounced: 40 to travel (100 in 60), 4 s each way, a rest at each end.
    n.marquee_bounce = true;
    KV_CHECK(c, krome_marquee_offset(n, 100, 60, 3.5, 1.0f, &next) == 20.0f);
    KV_CHECK(c, krome_marquee_offset(n, 100, 60, 5.6, 1.0f, &next) == 40.0f && near(next, 7.1)); // at the far end, resting
    KV_CHECK(c, krome_marquee_offset(n, 100, 60, 8.0, 1.0f, &next) == 30.0f);                   // on its way back
    KV_CHECK(c, krome_marquee_offset(n, 100, 60, 11.2, 1.0f, &next) == 0.0f);                   // home, resting
}

const char* k_marquee = R"lua(
return {
  model = { title = "A title far too long for its box, which scrolls", short = "Short" },
  root = col { gap = 4,
    text { id = "long", "{title}", nowrap = true, w = 120, marquee = 30, ["marquee-gap"] = 40 },
    text { id = "short", "{short}", nowrap = true, w = 120, marquee = 30 },
    col { id = "list", scroll = true, h = 30, w = 200,
      box { h = 60 },
      text { id = "below", "{title}", nowrap = true, w = 120, marquee = 30 },
    },
  },
}
)lua";

void test_marquee(TestCtx& c) {
    c.begin("krome placed: marquees");
    Engine e;
    KV_CHECK(c, engine_init(e, k_list));
    KromeModule& s = *krome_state(e);
    krome_viewport(e, 400, 300, 1.0f);
    krome_provide(e, "t/marquee.lua", k_marquee, strlen(k_marquee));
    Name scn = krome_open(e, "t/marquee.lua");
    for (u32 i = 0; i < 200 && !(krome_ready(e, scn) && krome_settled(e)); ++i) frames(e, 1);
    frames(e, 30);
    KV_CHECK(c, krome_ready(e, scn));
    // The long one moves; the short one fits and has no record; the one scrolled out of view
    // has one that asks for nothing.
    KromeScreen& sc = *krome_screen(s, scn);
    u32 lng = krome_find(sc.tree, name_intern("long")), below = krome_find(sc.tree, name_intern("below"));
    auto rec = [&](u32 node) -> KromeModule::Marquee* { for (KromeModule::Marquee& m : s.marquees) if (m.node == node) return &m; return nullptr; };
    KV_CHECK(c, s.marquees.len == 2 && rec(lng) && rec(below) && rec(below)->due == 0.0 && rec(lng)->due > 0.0);

    // 30 pixels a second at scale 1: about 30 wakes a second, not the display's 60.
    Loop lp{e};
    e.frame_wanted = false;
    lp.note();
    lp.run(0.1);
    lp.wakes = 0;
    f32 was = rec(lng)->off;
    lp.run(2.0);
    f32 moved = rec(lng)->off - was;
    KV_INFO("test", "a marquee at 30 px/s: %u wakes in 2 s, moved %.0f px", lp.wakes, (f64)moved);
    KV_CHECK(c, lp.wakes >= 58 && lp.wakes <= 62 && moved > 58.0f && moved < 62.0f);
    // Stepped: 8 pixels at a time, a wake each, 30 / 8 a second.
    krome_set(e, scn, "title", "Another title far too long for its box, stepped by eight");
    sc.tree.nodes[lng].marquee_step = 8.0f;
    lp.run(0.1);
    lp.wakes = 0;
    lp.run(4.0);
    KV_CHECK(c, lp.wakes >= 14 && lp.wakes <= 16 && (u32)rec(lng)->off % 8 == 0);
    // Hidden: no record, no wakes. Shown again, it begins from its rest.
    sc.tree.set_flag(lng, "hidden", true);
    e.frame_wanted = true; // as a binding or a behaviour's change asks
    lp.run(0.2);
    lp.wakes = 0;
    lp.run(3.0);
    KV_CHECK(c, lp.wakes == 0 && !rec(lng));
    sc.tree.set_flag(lng, "hidden", false);
    sc.tree.nodes[lng].marquee_step = 0.0f;
    e.frame_wanted = true;
    lp.run(0.5);
    KV_CHECK(c, rec(lng) && rec(lng)->off > 10.0f && rec(lng)->off < 20.0f);

    // Dragged (marquee-drag): held where the pointer leaves it, and on from there when released.
    sc.tree.flag(lng, "marquee-drag");
    const rect2& r = sc.tree.nodes[lng].rect;
    f32 px = r.x + 60.0f, py = r.y + r.h * 0.5f;
    krome_pointer(s, px, py, false, 0.0f, false);
    krome_pointer(s, px, py, true, 0.0f, false);
    f32 held = rec(lng)->off;
    KV_CHECK(c, rec(lng)->held);
    KV_CHECK(c, rec(lng)->due == 0.0); // asking for nothing while held
    krome_pointer(s, px - 25.0f, py, true, 0.0f, false);
    KV_CHECK(c, rec(lng)->off == held + 25.0f);
    // (No frame while it is held here: a frame's input pass reads the real pointer, which is up.)
    krome_pointer(s, px - 25.0f, py, false, 0.0f, false);
    lp.run(0.5);
    KV_CHECK(c, !rec(lng)->held && rec(lng)->off > held + 33.0f && rec(lng)->off < held + 42.0f);
    engine_shutdown(e);
}

// The mistakes: a marquee on what is not a one-line text.
void test_marquee_kim(TestCtx& c) {
    c.begin("krome placed: marquees in .kim");
    Engine e;
    KV_CHECK(c, engine_init(e, k_list));
    krome_viewport(e, 300, 200, 1.0f);
    krome_clear_errors(e);
    const char* text =
        "screen main { box dir=col {\n"
        "  text \"a long title\" nowrap marquee=30 marquee-mode=bounce marquee-pause=1.5 marquee-step=2 marquee-gap=10 marquee-drag w=40\n"
        "  text \"wraps\" marquee=30\n"
        "  box marquee=30\n"
        "} }\n";
    Name scn = open_kim(c, e, "t/mq.kim", text, "main");
    frames(e, 3);
    KV_CHECK(c, krome_ready(e, scn));
    KV_CHECK(c, has_error(e, "t/mq.kim:3: marquee= moves a one-line text, and this is a text without nowrap"));
    KV_CHECK(c, has_error(e, "t/mq.kim:4: marquee= moves a one-line text, and this is not a text"));
    const KromeTree* t = krome_tree(e, scn);
    const KromeNode& n = t->nodes[t->nodes[t->root].first];
    KV_CHECK(c, n.marquee == 30.0f && n.marquee_bounce && n.marquee_pause == 1.5f && n.marquee_step == 2.0f && n.marquee_gap == 10.0f && (n.flags & KROME_MARQUEE_DRAG));
    engine_shutdown(e);
}

} // namespace

void test_placed(TestCtx& c) {
    test_edges_kim(c);
    test_easing(c);
    test_timers(c);
    test_behaviour_timers(c);
    test_marquee_offsets(c);
    test_marquee(c);
    test_marquee_kim(c);
}
