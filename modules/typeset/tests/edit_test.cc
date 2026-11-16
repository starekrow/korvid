// The edit buffer: changes replace the selection; undo takes back groups (typing until the
// caret moves, a pause or a word after a space; a paste alone); redo puts them back.
#include "korvid/typeset/typeset.h"
#include "korvid/core/test.h"
#include <string.h>

namespace {
using namespace kv;
bool is(const TypesetEdit& e, const char* want) {
    if (!strcmp(e.c_str(), want) && e.len == strlen(want)) return true;
    KV_ERROR("test", "edit: '%s' (%u), wanted '%s'", e.c_str(), e.len, want);
    return false;
}
void type(TypesetEdit& e, const char* s, f64& t) {
    for (const char* p = s; *p; ++p) { e.insert(p, 1, t); t += 0.1; }
}
} // namespace

void test_edit(kv::TestCtx& c) {
    c.begin("typeset edit");
    using namespace kv;
    TypesetEdit e;
    f64 t = 0.0;
    e.set("", 0);
    KV_CHECK(c, is(e, "") && e.caret == 0);

    // Typing groups by word: "hello world" is two groups ("hello ", "world").
    type(e, "hello world", t);
    KV_CHECK(c, is(e, "hello world") && e.caret == 11);
    KV_CHECK(c, e.undo_step() && is(e, "hello ") && e.caret == 6);
    KV_CHECK(c, e.undo_step() && is(e, "") && !e.undo_step());
    KV_CHECK(c, e.redo_step() && is(e, "hello ") && e.redo_step() && is(e, "hello world") && !e.redo_step());

    // A pause ends a group; moving the caret ends one.
    t += 5.0;
    type(e, "!", t);
    e.move(0, false);
    type(e, ">", t);
    KV_CHECK(c, is(e, ">hello world!"));
    KV_CHECK(c, e.undo_step() && is(e, "hello world!") && e.undo_step() && is(e, "hello world"));
    e.redo.clear();

    // A selection: replaced by what is typed; erased by Backspace; a word at a time.
    e.move(6, false);
    e.move(11, true);
    KV_CHECK(c, e.selected() && e.sel_begin() == 6 && e.sel_end() == 11);
    type(e, "there", t);
    KV_CHECK(c, is(e, "hello there") && !e.selected());
    e.erase_back(true, t);
    KV_CHECK(c, is(e, "hello "));
    e.erase_back(false, t);
    e.erase_back(false, t);
    KV_CHECK(c, is(e, "hell") && e.caret == 4);
    e.move(0, false);
    e.erase_forward(false, t);
    KV_CHECK(c, is(e, "ell"));
    // A run of Backspaces is one group.
    TypesetEdit d;
    d.set("abcdef", 6);
    f64 dt = 0.0;
    d.erase_back(false, dt); dt += 0.1; d.erase_back(false, dt); dt += 0.1; d.erase_back(false, dt);
    KV_CHECK(c, is(d, "abc") && d.undo_step() && is(d, "abcdef") && d.caret == 6);
    d.free();

    // A paste is its own step, and replaces the selection; a cut is one too.
    e.set("one two", 7);
    e.move(4, false);
    e.move(7, true);
    e.paste("2", 1);
    KV_CHECK(c, is(e, "one 2") && e.caret == 5);
    e.select_all();
    e.erase_selection();
    KV_CHECK(c, is(e, "") && e.undo_step() && is(e, "one 2") && e.sel_begin() == 0 && e.sel_end() == 5 && e.undo_step() && is(e, "one two"));
    // A new change forgets redo.
    e.redo_step();
    type(e, "x", t);
    KV_CHECK(c, !e.redo_step());

    // UTF-8: moves and erases by whole code points.
    e.set("a\xc3\xa9", 3);
    e.erase_back(false, t);
    KV_CHECK(c, is(e, "a"));
    e.set("\xc3\xa9z", 3);
    e.move(1, false); // inside the sequence: back to its start
    KV_CHECK(c, e.caret == 0);
    // Line breaks are steps of their own.
    e.set("", 0);
    type(e, "ab", t);
    e.insert("\n", 1, t);
    type(e, "cd", t);
    KV_CHECK(c, is(e, "ab\ncd") && e.undo_step() && is(e, "ab\n") && e.undo_step() && is(e, "ab"));

    // The cap: the oldest groups go, the rest still undo.
    TypesetEdit cap;
    cap.max_steps = 4;
    cap.set("", 0);
    for (u32 i = 0; i < 10; ++i) cap.paste("x", 1);
    KV_CHECK(c, cap.undo.len == 4 && cap.undo_step() && cap.len == 9 && cap.undo_step() && cap.undo_step() && cap.undo_step() && !cap.undo_step() && cap.len == 6);
    cap.free();
    e.free();
}
