// edit, headless (docs/PRDs/Krome-Slice-3-PRD.md, milestone 4): focus by a press, typed text
// and keys through the platform's events, the binding both ways, submit, change and blur,
// many lines, the clipboard, read-only, undo, a double press, and the focus kept across a
// rebuild. DejaVu Sans is copied to a mounted directory: a caret needs real glyphs.
#include "korvid/krome/krome.h"
#include "../src/internal.h"
#include "korvid/asset/module.h"
#include "korvid/core/arena.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/kbor.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include "korvid/platform/io.h"
#include "korvid/platform/module.h"
#include "korvid/platform_hid/module.h"
#include <stdlib.h>
#include <string.h>

namespace {

using namespace kv;

const char* k_screen = R"lua(
return {
  model = { name = "Ada", notes = "", submitted = 0, changes = 0, blurs = 0 },
  on = {
    go = function() model.submitted = model.submitted + 1 end,
    ch = function() model.changes = model.changes + 1 end,
    bl = function() model.blurs = model.blurs + 1 end,
  },
  root = col { pad = 10, gap = 10, font = "text/DejaVuSans.ttf",
    edit { id = "name", bind = "name", event = "go", change = "ch", blur = "bl", w = 200, h = 24, pad = 4 },
    edit { id = "notes", bind = "notes", multiline = true, w = 200, h = 60, pad = 4 },
    edit { id = "ro", readonly = true, w = 200, h = 24, pad = 4, "fixed text" },
    hit { id = "other", event = "x", w = 50, h = 20 },
  },
}
)lua";

void frames(Engine& e, u32 n) {
    for (u32 i = 0; i < n; ++i) { platform_pump(e); engine_frame(e, 1.0 / 60.0); engine_frame_cycle(e, 1.0 / 60.0); }
}

// A press and a release at a point (logical = physical at scale 1).
void click(Engine& e, f32 x, f32 y) {
    input_pointer_override(e, x, y, 0); frames(e, 1);
    input_pointer_override(e, x, y, 1); frames(e, 1);
    input_pointer_override(e, x, y, 0); frames(e, 1);
}
void click_id(Engine& e, Name screen, const char* id, f32 dx = -1.0f) {
    rect2 r;
    if (!krome_find_rect(e, screen, id, &r)) return;
    click(e, dx >= 0.0f ? r.x + dx : r.x + r.w * 0.5f, r.y + r.h * 0.5f);
}
void key(Engine& e, Key k, bool ctrl = false, bool shift = false) {
    if (ctrl) input_key_override(e, Key::LCtrl, true);
    if (shift) input_key_override(e, Key::LShift, true);
    frames(e, 1);
    input_key_override(e, k, true);
    input_key_override(e, k, false);
    frames(e, 1);
    if (ctrl) input_key_override(e, Key::LCtrl, false);
    if (shift) input_key_override(e, Key::LShift, false);
    frames(e, 1);
}
void type(Engine& e, const char* s) { input_text_override(e, s); frames(e, 2); }

bool text_is(Engine& e, Name screen, const char* key, const char* want) {
    const char* got = krome_model_text(e, screen, key);
    if (!strcmp(got, want)) return true;
    KV_ERROR("test", "model %s is '%s', wanted '%s'", key, got, want);
    return false;
}

bool clip_is(Engine& e, const char* want) {
    Array<char> c{};
    platform_clipboard_get(e, c);
    c.push(0);
    bool ok = !strcmp(c.data, want);
    if (!ok) KV_ERROR("test", "clipboard '%s', wanted '%s'", c.data, want);
    c.free();
    return ok;
}

} // namespace

void test_edit(kv::TestCtx& c) {
    using namespace kv;
    c.begin("krome edit");
    char root[600], dir[400], src[700], dst[700];
    format(root, sizeof root, "%s", __FILE__);
    for (int up = 0; up < 4; ++up) if (char* slash = strrchr(root, '/')) *slash = 0;
    KV_CHECK(c, io_make_temp_dir("kv-krome-edit", dir, sizeof dir));
    format(src, sizeof src, "%s/modules/text/assets/DejaVuSans.ttf", root);
    format(dst, sizeof dst, "%s/text", dir);
    io_make_dirs(dst);
    format(dst, sizeof dst, "%s/text/DejaVuSans.ttf", dir);
    KV_CHECK(c, io_copy_now(src, dst));
    const ModuleDesc* const list[] = {&platform_module, &platform_hid_module, &asset_module, &krome_module};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    KV_CHECK(c, platform_io(e)->mount_dir(dir));
    KromeModule* s = krome_state(e);
    krome_viewport(e, 400, 300, 1.0f);
    Name sc = krome_open_source(e, "t/edit", k_screen);
    for (u32 i = 0; i < 400 && !(krome_ready(e, sc) && krome_settled(e)); ++i) { frames(e, 1); time_sleep(0.002); }
    frames(e, 3);
    KV_CHECK(c, krome_ready(e, sc));
    const KromeTree* t = krome_tree(e, sc);
    u32 name = krome_find(*t, name_intern("name"));
    KV_CHECK(c, name != KROME_NONE && t->nodes[name].type == KromeType::Edit && !strcmp(t->text(name), "Ada"));

    // A press gives it the keyboard, the caret where it was pressed (past the text: its end).
    click_id(e, sc, "name");
    KV_CHECK(c, krome_is_focused(*s, krome_screen(*s, sc)->id, 0, name) && s->focus.buf.caret == 3);
    type(e, "Bob");
    KV_CHECK(c, text_is(e, sc, "name", "AdaBob") && !strcmp(krome_tree(e, sc)->text(name), "AdaBob"));
    key(e, Key::Backspace);
    KV_CHECK(c, text_is(e, sc, "name", "AdaBo"));
    key(e, Key::Left);
    key(e, Key::Left, false, true); // a selection, one character back
    KV_CHECK(c, s->focus.buf.selected() && s->focus.buf.sel_begin() == 3 && s->focus.buf.sel_end() == 4);
    type(e, "-");
    KV_CHECK(c, text_is(e, sc, "name", "Ada-o"));
    key(e, Key::Z, true);
    KV_CHECK(c, text_is(e, sc, "name", "AdaBo"));
    // Enter submits a line; change fired once a frame it changed in; a press elsewhere blurs.
    key(e, Key::Enter);
    KV_CHECK(c, text_is(e, sc, "submitted", "1") && atoi(krome_model_text(e, sc, "changes")) >= 3);
    click_id(e, sc, "other");
    KV_CHECK(c, s->focus.node == KROME_NONE && text_is(e, sc, "blurs", "1"));
    // The model in: an edit without the keyboard shows it.
    krome_set(e, sc, "name", "Eve");
    frames(e, 1);
    KV_CHECK(c, !strcmp(krome_tree(e, sc)->text(name), "Eve"));

    // Many lines: Enter breaks one; the clipboard takes the whole; one line pastes it with a
    // space for its break.
    click_id(e, sc, "notes");
    type(e, "a\nb");
    KV_CHECK(c, text_is(e, sc, "notes", "a\nb"));
    key(e, Key::Up);
    KV_CHECK(c, s->focus.buf.caret == 1);
    key(e, Key::A, true);
    key(e, Key::C, true);
    KV_CHECK(c, clip_is(e, "a\nb"));
    click_id(e, sc, "name");
    key(e, Key::End);
    key(e, Key::V, true);
    KV_CHECK(c, text_is(e, sc, "name", "Evea b"));
    // A double press selects a word.
    rect2 r;
    krome_find_rect(e, sc, "name", &r);
    click(e, r.x + 10, r.y + 12);
    click(e, r.x + 10, r.y + 12);
    KV_CHECK(c, s->focus.buf.sel_begin() == 0 && s->focus.buf.sel_end() == 4);
    key(e, Key::Escape);
    KV_CHECK(c, s->focus.node == KROME_NONE);

    // Read-only: selected and copied, never changed.
    click_id(e, sc, "ro");
    type(e, "zz");
    key(e, Key::Backspace);
    key(e, Key::A, true);
    key(e, Key::C, true);
    KV_CHECK(c, !strcmp(krome_tree(e, sc)->text(krome_find(*krome_tree(e, sc), name_intern("ro"))), "fixed text") && clip_is(e, "fixed text"));

    // (The numbers below run before the focus test: it ends in a theme switch.)
    // The focus kept, by its id, when the tree is built again (a theme switch), with what was
    // being typed.
    click_id(e, sc, "name");
    type(e, "!");
    krome_theme(e, "light");
    frames(e, 2);
    t = krome_tree(e, sc);
    KV_CHECK(c, s->focus.node == krome_find(*t, name_intern("name")) && !strcmp(t->text(s->focus.node), "Evea b!"));
    // The numbers: a 10,000-character paragraph laid out, and a keystroke in a 1,000-line edit.
    {
        KromeModule& km = *s;
        const FontAsset* fa = nullptr;
        for (const KromeFont& kf : km.fonts) if (const FontAsset* a = asset_font(e, kf.asset)) if (a->state == AssetState::Ready) fa = a;
        Array<char> big{};
        for (u32 i = 0; i < 1000; ++i) {
            char line[64];
            i32 n = format(line, sizeof line, "line %u of the notes, written out at length\n", i);
            memcpy(big.push_n((u32)n), line, (usize)n);
        }
        big.push(0);
        if (fa) {
            Typeset ts;
            f64 t0 = time_now();
            typeset_paragraph(*fa, big.data, 10000, TypesetParams{600.0f}, ts);
            f64 t1 = time_now();
            KV_INFO("test", "edit numbers: a 10,000-character paragraph laid out at 600 px in %.2f ms (%u lines)", (t1 - t0) * 1e3, ts.lines.len);
            ts.free();
        }
        {   // Past KromeSet's 56 bytes: a KBOR document of one string.
            MemScope scope;
            JsonBuilder jb(mem_scratch());
            JsonDoc doc;
            jb.finish(jb.string(big.data, big.len - 1), doc);
            Array<u8> k{};
            kbor_write(doc, k);
            krome_set_doc(e, sc, "notes", k.data, k.len);
            k.free();
            jb.free();
        }
        frames(e, 2);
        click_id(e, sc, "notes");
        key(e, Key::End, true);
        f64 t0 = time_now();
        type(e, "x");
        f64 t1 = time_now();
        KV_CHECK(c, s->focus.buf.len == big.len); // the 1000 lines and the x (the NUL not counted, the x is)
        KV_INFO("test", "edit numbers: a keystroke in a %u-byte, 1,000-line edit: %.2f ms over the two frames it took",
                big.len - 1, (t1 - t0) * 1e3);
        big.free();
        key(e, Key::Escape);
    }

    // Closing the screen takes the keyboard back.
    krome_close(e, sc);
    KV_CHECK(c, s->focus.node == KROME_NONE);
    engine_shutdown(e);
    io_remove_tree(dir);
}
