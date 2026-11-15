// Typed text, keys and composition as events, headless: what demo scripts and tests drive a
// text field with, through the same events a host's keyboard and input method give; and the
// clipboard, which with no window is the process's own.
#include "korvid/platform_hid/module.h"
#include "korvid/platform/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/test.h"
#include <string.h>

namespace {

using namespace kv;

struct Seen { char typed[128]; u32 typed_events; u32 downs, ups; Key last_down; char composing[64]; u32 compose_caret; u32 composes; };
Seen g_seen;

void sys_read(TickContext& ctx) {
    for (const TextTyped& t : ctx.received<TextTyped>()) { strcat(g_seen.typed, t.text); ++g_seen.typed_events; }
    for (const KeyDown& k : ctx.received<KeyDown>()) { ++g_seen.downs; g_seen.last_down = k.key; }
    for (const KeyUp& k : ctx.received<KeyUp>()) { (void)k; ++g_seen.ups; }
    for (const TextComposing& c : ctx.received<TextComposing>()) { strcpy(g_seen.composing, c.text); g_seen.compose_caret = c.caret; ++g_seen.composes; }
}
constinit const SystemDesc k_systems[] = {{.name = ".read", .phase = Phase::Frame, .fn = sys_read, .reads = "TextTyped, KeyDown, KeyUp, TextComposing"}};
KV_MODULE_DEFINE(k_reader) = {.name = "rd", .systems = k_systems};

void frame(Engine& e) { platform_pump(e); engine_frame(e, 1.0 / 60.0); engine_frame_cycle(e, 1.0 / 60.0); }

} // namespace

void test_text_events(kv::TestCtx& c) {
    using namespace kv;
    c.begin("platform_hid text");
    g_seen = Seen{};
    const ModuleDesc* const list[] = {&platform_module, &platform_hid_module, &k_reader};
    Engine e;
    KV_CHECK(c, engine_init(e, list));

    // Typed text: one event for a short commit, several for a long one, split at code points.
    input_text_override(e, "h\xc3\xa9llo");
    frame(e);
    KV_CHECK(c, !strcmp(g_seen.typed, "h\xc3\xa9llo") && g_seen.typed_events == 1);
    g_seen = Seen{};
    input_text_override(e, "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9");
    frame(e);
    KV_CHECK(c, strlen(g_seen.typed) == 40 && g_seen.typed_events == 2);

    // Keys: down and up as the keyboard gives them, the state with them.
    g_seen = Seen{};
    input_key_override(e, Key::LShift, true);
    input_key_override(e, Key::Left, true);
    frame(e);
    KV_CHECK(c, g_seen.downs == 2 && g_seen.last_down == Key::Left && platform_input(e)->raw.down(Key::LShift));
    input_key_override(e, Key::Left, false);
    input_key_override(e, Key::LShift, false);
    frame(e);
    KV_CHECK(c, g_seen.ups == 2 && !platform_input(e)->raw.down(Key::LShift));

    // Composition: what the input method shows, then its end.
    input_compose_override(e, "\xe3\x81\x8b", 3);
    frame(e);
    KV_CHECK(c, !strcmp(g_seen.composing, "\xe3\x81\x8b") && g_seen.compose_caret == 3 && g_seen.composes == 1);
    input_compose_override(e, "", 0);
    frame(e);
    KV_CHECK(c, g_seen.composing[0] == 0 && g_seen.composes == 2);

    // The clipboard with no window: the process's own.
    Array<char> got{};
    KV_CHECK(c, !platform_clipboard_get(e, got));
    platform_clipboard_set(e, "copied \xc3\xa9");
    KV_CHECK(c, platform_clipboard_get(e, got) && got.len == 9 && !memcmp(got.data, "copied \xc3\xa9", 9));
    got.free();
    platform_cursor(e, CursorShape::IBeam); // nothing to show it on: does nothing
    platform_text_area(e, rect2{0, 0, 10, 10}, 3);
    engine_shutdown(e);
}
