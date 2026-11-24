// Krome on a rebind (docs/PRDs/Krome-Slice-6-PRD.md, "Krome on a rebind"): a screen's .kimb, a
// prelude's, a screen's script, a prelude's script and a Lua screen each given again (as a
// reload does), and once read again from a file through the asset module. What survives: the
// model, the scrolls, the theme. A reload that does not check keeps the screen as it was, says
// why in the strip, and keeps the old reading alive until it is fixed.
#include "korvid/krome/krome.h"
#include "../src/internal.h"
#include "korvid/asset/module.h"
#include "korvid/asset/reload.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/kbor.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include "korvid/platform/io.h"
#include "korvid/platform/module.h"
#include "korvid/platform_hid/module.h"
#include <string.h>

namespace {

using namespace kv;

bool kbor_of(const char* json, Array<u8>& out) {
    JsonDoc doc;
    if (!json_parse(json, strlen(json), mem_scratch(), doc)) { KV_ERROR("test", "%s at line %u", doc.error, doc.error_line); return false; }
    kbor_write(doc, out);
    return out.len > 0;
}

void provide_json(Engine& e, const char* path, const char* json) {
    Array<u8> k{};
    if (kbor_of(json, k)) krome_provide(e, path, k.data, k.len);
    k.free();
}

void frames(Engine& e, u32 n) {
    for (u32 i = 0; i < n; ++i) { platform_pump(e); engine_frame(e, 1.0 / 60.0); engine_frame_cycle(e, 1.0 / 60.0); }
}

// The text a node shows, by id.
const char* shown(const KromeTree& t, const char* id) {
    u32 n = krome_find(t, name_intern(id));
    if (n == KROME_NONE) return "";
    u32 at = t.nodes[n].shown != KROME_NONE ? t.nodes[n].shown : t.nodes[n].text;
    return at == KROME_NONE ? "" : t.strings.data + at;
}

color bg_of(const KromeTree& t, const char* id) {
    u32 n = krome_find(t, name_intern(id));
    return n == KROME_NONE ? color{-1, -1, -1, -1} : t.looks[t.nodes[n].look].bg;
}

bool strip_up(KromeModule& s) {
    KromeScreen* sc = s.strip_overlay ? krome_screen_by_id(s, s.strip_screen) : nullptr;
    return sc && krome_overlay(*sc, s.strip_overlay);
}

// The prelude: a style and a theme.
const char* prelude(const char* bg) {
    static char buf[600];
    format(buf, sizeof buf, R"({"kim": 1, "file": "r/pre.kim",
      "styles": { "tile": { "p": { "bg": "%s", "pad": 4 } } },
      "themes": { "dark": { "ink": "#e0e0e0" }, "light": { "ink": "#202020" } } })", bg);
    return buf;
}

// The screen: a label reading the model, a hit that fires `bump`, a scroll box, a tile.
const char* screen(const char* label, const char* style) {
    static char buf[1400];
    format(buf, sizeof buf, R"({"kim": 1, "file": "r/s.kim", "uses": ["r/pre"], "scripts": ["r/s.lua"],
      "screens": { "main": { "w": "box", "p": { "dir": "col", "pad": 10, "gap": 6 }, "c": [
        { "w": "text", "id": "label", "t": "%s {count}" },
        { "w": "hit", "id": "bump", "e": "bump", "p": { "w": 80, "h": 20 } },
        { "w": "box", "id": "tile", "s": ["%s"], "p": { "w": 20, "h": 20 } },
        { "w": "box", "id": "list", "f": ["scroll"], "p": { "dir": "col", "w": 200, "h": 60 }, "c": [
          { "w": "box", "p": { "w": 180, "h": 400 } } ] } ] } } })", label, style);
    return buf;
}

const char* k_script_1 = R"lua(
return { model = { count = 0 }, on = { bump = function() model.count = model.count + 1 end } }
)lua";
const char* k_script_2 = R"lua(
return { model = { count = 0, extra = "new" }, on = { bump = function() model.count = model.count + 10 end } }
)lua";
const char* k_script_bad = "return { on = { bump = function() model.count = ";

char g_dir[400];

bool write_kimb(const char* rel, const char* json) {
    Array<u8> k{};
    char p[700], d[700];
    format(p, sizeof p, "%s/%s", g_dir, rel);
    format(d, sizeof d, "%s", p);
    if (char* slash = strrchr(d, '/')) { *slash = 0; io_make_dirs(d); }
    bool ok = kbor_of(json, k) && io_write_now(p, k.data, k.len);
    k.free();
    return ok;
}

} // namespace

void test_reload(kv::TestCtx& c) {
    using namespace kv;
    c.begin("krome reload");
    KV_CHECK(c, io_make_temp_dir("kv-krome-reload", g_dir, sizeof g_dir));
    const ModuleDesc* const list[] = {&platform_module, &platform_hid_module, &asset_module, &krome_module};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    KV_CHECK(c, platform_io(e)->mount_dir(g_dir));
    KromeModule& s = *krome_state(e);
    krome_viewport(e, 400, 300, 1.0f);

    provide_json(e, "r/pre.kimb", prelude("#ff0000"));
    provide_json(e, "r/s.kimb", screen("Count", "tile"));
    krome_provide(e, "r/s.lua", k_script_1, strlen(k_script_1));
    Name scn = krome_open(e, "r/s.kimb", "main");
    frames(e, 4);
    KV_CHECK(c, krome_ready(e, scn));
    KromeScreen& sc = *krome_screen(s, scn);
    KV_CHECK(c, !strcmp(shown(sc.tree, "label"), "Count 0"));
    KV_CHECK(c, bg_of(sc.tree, "tile").r == 1.0f);

    // A press, a scroll, a theme: what a person did.
    KV_CHECK(c, krome_press(e, scn, "bump"));
    frames(e, 2);
    KV_CHECK(c, !strcmp(krome_model_text(e, scn, "count"), "1"));
    u32 box = krome_find(sc.tree, name_intern("list"));
    sc.tree.nodes[box].scroll.y = 120.0f;
    sc.tree.dirty = true;
    krome_theme(e, "light");
    frames(e, 2);
    KV_CHECK(c, !strcmp(krome_current_theme(e, scn), "light"));

    // The screen's .kimb given again: the new label, the model and the scroll as they were.
    u32 reloads = s.reloads;
    provide_json(e, "r/s.kimb", screen("Total", "tile"));
    frames(e, 3);
    KV_CHECK(c, s.reloads == reloads + 1);
    KV_CHECK(c, !strcmp(shown(sc.tree, "label"), "Total 1"));
    box = krome_find(sc.tree, name_intern("list"));
    KV_CHECK(c, box != KROME_NONE && sc.tree.nodes[box].scroll.y == 120.0f);
    KV_CHECK(c, !strcmp(krome_current_theme(e, scn), "light"));
    KV_CHECK(c, !sc.held && s.retired.len == 0 && !s.reload_error[0]); // the old reading freed once the screen was built again

    // The same bytes again: nothing to do.
    reloads = s.reloads;
    provide_json(e, "r/s.kimb", screen("Total", "tile"));
    frames(e, 2);
    KV_CHECK(c, !strcmp(shown(sc.tree, "label"), "Total 1"));

    // The prelude given again: the tile in the new colour.
    provide_json(e, "r/pre.kimb", prelude("#00ff00"));
    frames(e, 3);
    KV_CHECK(c, bg_of(sc.tree, "tile").g == 1.0f && bg_of(sc.tree, "tile").r == 0.0f);

    // A screen that no longer checks (a style nobody declared): kept as it was, and said.
    provide_json(e, "r/s.kimb", screen("Broken", "nope"));
    frames(e, 3);
    KV_CHECK(c, !strcmp(shown(sc.tree, "label"), "Total 1"));
    KV_CHECK(c, strstr(s.reload_error, "nope") != nullptr);
#if KV_DEV
    KV_CHECK(c, strip_up(s));
    {
        KromeOverlay* strip = krome_overlay(*krome_screen_by_id(s, s.strip_screen), s.strip_overlay);
        KV_CHECK(c, strip && strip->passive && strip->tree.root != KROME_NONE && strip->tree.nodes[strip->tree.root].rect.w == 400.0f);
    }
#endif
    // The screen still works on what it had.
    KV_CHECK(c, krome_press(e, scn, "bump"));
    frames(e, 2);
    KV_CHECK(c, !strcmp(shown(sc.tree, "label"), "Total 2"));
    // Fixed: built from the new reading, the strip gone.
    provide_json(e, "r/s.kimb", screen("Fixed", "tile"));
    frames(e, 3);
    KV_CHECK(c, !strcmp(shown(sc.tree, "label"), "Fixed 2"));
    KV_CHECK(c, !s.reload_error[0] && !strip_up(s) && !sc.held);

    // A prelude whose change breaks the screen (its style gone): the prelude is in force, the
    // screen is held on the old reading, which is kept alive for it.
    provide_json(e, "r/pre.kimb", R"({"kim": 1, "file": "r/pre.kim", "styles": { "other": { "p": { "bg": "#0000ff" } } },
      "themes": { "dark": { "ink": "#e0e0e0" }, "light": { "ink": "#202020" } } })");
    frames(e, 3);
    KV_CHECK(c, sc.held && s.retired.len == 1 && strstr(s.reload_error, "tile") != nullptr);
    KV_CHECK(c, bg_of(sc.tree, "tile").g == 1.0f); // as it was
    krome_theme(e, "dark");                         // a rebuild while held: still the old reading
    frames(e, 2);
    KV_CHECK(c, bg_of(sc.tree, "tile").g == 1.0f && sc.held);
    provide_json(e, "r/pre.kimb", prelude("#0000ff"));
    frames(e, 3);
    KV_CHECK(c, !sc.held && s.retired.len == 0 && bg_of(sc.tree, "tile").b == 1.0f);

    // The script given again: the new handler, the model kept, a new key added.
    krome_provide(e, "r/s.lua", k_script_2, strlen(k_script_2));
    frames(e, 3);
    KV_CHECK(c, !strcmp(krome_model_text(e, scn, "count"), "2"));
    KV_CHECK(c, !strcmp(krome_model_text(e, scn, "extra"), "new"));
    KV_CHECK(c, krome_press(e, scn, "bump"));
    frames(e, 2);
    KV_CHECK(c, !strcmp(krome_model_text(e, scn, "count"), "12"));
    // A script that does not load: the handlers it had.
    krome_provide(e, "r/s.lua", k_script_bad, strlen(k_script_bad));
    frames(e, 3);
    KV_CHECK(c, s.reload_error[0] != 0);
    KV_CHECK(c, krome_press(e, scn, "bump"));
    frames(e, 2);
    KV_CHECK(c, !strcmp(krome_model_text(e, scn, "count"), "22"));
    krome_clear_errors(e);
    frames(e, 2);
    KV_CHECK(c, !s.reload_error[0] && !strip_up(s));

    // A prelude's script: its behaviour replaced, and the screens built again over it.
    {
        const char* pre_b = R"({"kim": 1, "file": "r/pb.kim", "scripts": ["r/pb.lua"]})";
        const char* scr_b = R"({"kim": 1, "file": "r/b.kim", "uses": ["r/pb"],
          "screens": { "main": { "w": "box", "c": [ { "w": "hit", "id": "knob", "b": "n", "p": { "behave": "add", "w": 40, "h": 20 } } ] } } })";
        const char* add_1 = "return { add = { press = function(w) w:set((w.value or 0) + 1) end } }";
        const char* add_2 = "return { add = { press = function(w) w:set((w.value or 0) + 100) end } }";
        provide_json(e, "r/pb.kimb", pre_b);
        provide_json(e, "r/b.kimb", scr_b);
        krome_provide(e, "r/pb.lua", add_1, strlen(add_1));
        Name bs = krome_open(e, "r/b.kimb", "main");
        frames(e, 4);
        KV_CHECK(c, krome_ready(e, bs));
        krome_set(e, bs, "n", 0.0);
        frames(e, 1);
        KromeScreen& b = *krome_screen(s, bs);
        u32 knob = krome_find(b.tree, name_intern("knob"));
        rect2 r = b.tree.nodes[knob].rect;
        auto press = [&] {
            input_pointer_override(e, r.x + 5, r.y + 5, 0); frames(e, 1);
            input_pointer_override(e, r.x + 5, r.y + 5, 1); frames(e, 1);
            input_pointer_override(e, r.x + 5, r.y + 5, 0); frames(e, 2);
        };
        press();
        KV_CHECK(c, !strcmp(krome_model_text(e, bs, "n"), "1"));
        krome_provide(e, "r/pb.lua", add_2, strlen(add_2));
        frames(e, 3);
        press();
        KV_CHECK(c, !strcmp(krome_model_text(e, bs, "n"), "101"));
        krome_close(e, bs);
        frames(e, 1);
    }

    // A Lua screen: its tree and handlers new, its model kept.
    {
        const char* l1 = R"lua(return { model = { v = 5 }, root = col { text { id = "t", "v is {v}" } } })lua";
        const char* l2 = R"lua(return { model = { v = 0 }, root = col { text { id = "t", "now {v}" } } })lua";
        krome_provide(e, "r/l.lua", l1, strlen(l1));
        Name ls = krome_open(e, "r/l.lua");
        frames(e, 3);
        KV_CHECK(c, krome_ready(e, ls));
        krome_set(e, ls, "v", 7.0);
        frames(e, 1);
        KV_CHECK(c, !strcmp(shown(krome_screen(s, ls)->tree, "t"), "v is 7"));
        krome_provide(e, "r/l.lua", l2, strlen(l2));
        frames(e, 3);
        KV_CHECK(c, !strcmp(shown(krome_screen(s, ls)->tree, "t"), "now 7"));
        KV_CHECK(c, s.retired.len == 0);
        krome_close(e, ls);
        frames(e, 1);
    }

    // From a file, through the asset module: written, read, written again, asset_reload.
    {
        KV_CHECK(c, write_kimb("f/f.kimb", R"({"kim": 1, "file": "f/f.kim",
          "screens": { "main": { "w": "box", "c": [ { "w": "text", "id": "t", "t": "first" } ] } } })"));
        Name fs = krome_open(e, "f/f.kimb", "main");
        for (u32 i = 0; i < 400 && !krome_ready(e, fs); ++i) { frames(e, 1); time_sleep(0.001); }
        KV_CHECK(c, krome_ready(e, fs));
        KV_CHECK(c, !strcmp(shown(krome_screen(s, fs)->tree, "t"), "first"));
        KV_CHECK(c, write_kimb("f/f.kimb", R"({"kim": 1, "file": "f/f.kim",
          "screens": { "main": { "w": "box", "c": [ { "w": "text", "id": "t", "t": "second" } ] } } })"));
        KV_CHECK(c, asset_reload(e, "f/f.kimb") == 1);
        for (u32 i = 0; i < 400 && strcmp(shown(krome_screen(s, fs)->tree, "t"), "second"); ++i) { frames(e, 1); time_sleep(0.001); }
        KV_CHECK(c, !strcmp(shown(krome_screen(s, fs)->tree, "t"), "second"));
        // A screen that could not open (its file not there) opens once the file is.
        Name late = krome_open(e, "f/late.kimb", "main");
        for (u32 i = 0; i < 400 && !krome_screen(s, late)->failed; ++i) { frames(e, 1); time_sleep(0.001); }
        KV_CHECK(c, krome_screen(s, late)->failed);
        KV_CHECK(c, write_kimb("f/late.kimb", R"({"kim": 1, "file": "f/late.kim",
          "screens": { "main": { "w": "box", "c": [ { "w": "text", "id": "t", "t": "late" } ] } } })"));
        KV_CHECK(c, asset_reload(e, "f/late.kimb") == 1);
        for (u32 i = 0; i < 400 && !krome_ready(e, late); ++i) { frames(e, 1); time_sleep(0.001); }
        KV_CHECK(c, krome_ready(e, late) && !strcmp(shown(krome_screen(s, late)->tree, "t"), "late"));
    }

    // Still: nothing reloads, nothing is rebuilt.
    {
        u64 layouts = s.layouts;
        frames(e, 30);
        KV_CHECK(c, s.layouts == layouts && !krome_reload_pending(s));
    }

    // Shut down with the strip up: it is a passive overlay, which closing "the top one"
    // never takes, and freeing the screen's overlays went round for ever.
    provide_json(e, "r/s.kimb", screen("Broken", "nope"));
    frames(e, 3);
#if KV_DEV
    KV_CHECK(c, strip_up(s));
#endif
    engine_shutdown(e);
    io_remove_tree(g_dir);
}
