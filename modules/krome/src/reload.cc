// Krome on a rebind (docs/PRDs/Krome-Slice-6-PRD.md, "Krome on a rebind"). A document that
// changed under a running screen: a .kimb's new reading checked on its own, then by every
// screen against its scope, each screen built again only if it checks (else it keeps the tree
// it had, and says why); a script run again with the model kept; a prelude's script's
// behaviours replaced. What a screen was doing survives the rebuild as it does a theme switch:
// the model, the focus and the scrolls by id, the overlays built again.
#include "internal.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include <string.h>

namespace kv {

namespace {

bool lists_script(const KimbFile* f, Name path) {
    if (!f || !f->scripts) return false;
    for (const JsonValue* v = json_first(f->doc, *f->scripts); v; v = json_next(f->doc, *v))
        if (v->str() && name_intern(v->str()) == path) return true;
    return false;
}

// The first of a check's mistakes as one line, and how many more.
void say(char* out, usize cap, const KimErrors& errs) {
    if (!errs.count) { format(out, cap, "it does not check"); return; }
    char line[300];
    errs.format_one(0, line, sizeof line);
    if (errs.count > 1) format(out, cap, "%s (and %u more)", line, errs.count - 1);
    else format(out, cap, "%s", line);
}

void log_all(KromeModule& s, const KimErrors& errs) {
    for (u32 i = 0; i < errs.count; ++i) {
        char line[300];
        errs.format_one(i, line, sizeof line);
        KV_ERROR("krome", "%s", line);
        s.errors.add(errs.list[i].file, errs.list[i].line, 0, "%s", errs.list[i].text);
        ++s.script_errors;
    }
}

// A .kimb's new reading checked in its own scope (its uses as they are now): put in force
// (1; the old one retired), or dropped with its mistakes (-1). 0 while a use is still loading.
int promote(KromeModule& s, KromeDoc& d) {
    KimbScope scope;
    bool failed = false;
    if (!krome_file_scope(s, d.next->file, scope, &failed) && !failed) return 0;
    KimErrors errs;
    if (failed || !kimb_check(scope, errs)) {
        log_all(s, errs);
        char why[400];
        say(why, sizeof why, errs);
        format(s.reload_error, sizeof s.reload_error, "%s", failed ? "a prelude it uses would not load" : why);
        KV_WARN("krome", "'%s' reloaded with mistakes: the screens keep what they had", name_str(d.path));
        arena_destroy(d.next->arena);
        free_items(s.general, d.next, 1);
        d.next = nullptr;
        return -1;
    }
    d.cur->retired_at = ++s.epoch;
    s.retired.alloc = s.general;
    s.retired.push(d.cur);
    d.cur = d.next;
    d.file = &d.cur->file;
    d.next = nullptr;
    s.reload_error[0] = 0;
    return 1;
}

// A screen's scripts run again, the model kept. False while one is still loading.
bool rescript(KromeModule& s, KromeScreen& sc) {
    if (!sc.doc) {
        KromeDoc* d = krome_doc(s, sc.script, true);
        if (d->loading) return false;
        if (!d->ready) return true;
        if (!krome_lua_run(s, sc, d->source, d->source_len, name_str(sc.name), true))
            format(s.reload_error, sizeof s.reload_error, "%s: the script failed; the screen keeps what it had", name_str(sc.script));
        return true;
    }
    const KimbFile* f = sc.doc->file;
    KromeDoc* list[64];
    u32 n = 0;
    if (f->scripts)
        for (const JsonValue* v = json_first(f->doc, *f->scripts); v && n < 64; v = json_next(f->doc, *v)) {
            if (!v->str()) continue;
            KromeDoc* d = krome_doc(s, name_intern(v->str()), true);
            if (d->loading) return false;
            if (d->ready) list[n++] = d;
        }
    if (!krome_lua_scripts(s, sc, list, n, true))
        format(s.reload_error, sizeof s.reload_error, "%s: a script failed; the screen keeps the handlers it had", name_str(sc.name));
    return true;
}

// A screen checked against its documents as they are now, and built again if it checks.
// False while something it needs is still loading.
bool recheck(KromeModule& s, KromeScreen& sc) {
    const KimbFile* f = sc.doc ? sc.doc->file : sc.has_lua_file ? &sc.lua_file : nullptr;
    if (!f) return true;
    KimbScope scope;
    bool failed = false;
    if (!krome_file_scope(s, *f, scope, &failed)) {
        if (!failed) return false;
        sc.held = true;
        format(s.reload_error, sizeof s.reload_error, "%s: a prelude it uses would not load; it keeps what it had", name_str(sc.name));
        return true;
    }
    KimErrors errs;
    if (!kimb_check(scope, errs)) {
        log_all(s, errs);
        say(s.reload_error, sizeof s.reload_error, errs);
        if (!sc.held) KV_WARN("krome", "screen '%s' does not check after a reload: it keeps what it had", name_str(sc.name));
        sc.held = true;
        return true;
    }
    if (!krome_prelude_scripts(s, scope)) return false;
    sc.scope = scope;
    sc.file = sc.doc ? sc.doc->file : nullptr;
    sc.held = false;
    krome_build_screen(s, sc);
    sc.built_epoch = s.epoch;
    sc.repaint = true;
    KV_INFO("krome", "screen '%s' rebuilt: %u nodes", name_str(sc.name), sc.tree.nodes.len);
    return true;
}

// Which screens a script that changed belongs to: its screens' to run again; a prelude's run
// again now, its behaviours replacing the old, and every screen built again over them.
void script_changed(KromeModule& s, KromeDoc& d) {
    bool prelude = false;
    for (Name r : s.ran_scripts) if (r == d.path) prelude = true;
    if (prelude) {
        if (!krome_lua_prelude(s, d.source, d.source_len, name_str(d.path), true))
            format(s.reload_error, sizeof s.reload_error, "%s: the prelude's script failed; its widgets keep what they had", name_str(d.path));
        for (KromeScreen* sc : s.screens) if (sc->ready) sc->recheck = true;
    }
    for (KromeScreen* sc : s.screens) {
        if (!sc->ready) continue;
        if (sc->doc ? lists_script(sc->doc->file, d.path) : sc->script == d.path) sc->rescript = true;
    }
}

} // namespace

void krome_free_retired(KromeModule& s, bool all) {
    u32 w = 0;
    for (u32 i = 0; i < s.retired.len; ++i) {
        KromeDocVersion* v = s.retired[i];
        bool needed = false;
        if (!all)
            for (KromeScreen* sc : s.screens)
                if (sc->ready && sc->built_epoch < v->retired_at && (!v->owner || v->owner == sc->id)) { needed = true; break; }
        if (needed) { s.retired[w++] = v; continue; }
        if (v->arena.base) arena_destroy(v->arena);
        free_items(s.general, v, 1);
    }
    s.retired.len = w;
}

bool krome_reload_pending(const KromeModule& s) {
    for (const KromeDoc* d : s.docs) if (d->next || d->changed) return true;
    for (const KromeScreen* sc : s.screens) if (sc->ready && (sc->recheck || sc->rescript)) return true;
    return false;
}

void krome_reload(KromeModule& s) {
    // New readings of .kimb files: each checked on its own before any screen sees it.
    bool kimb = false;
    for (KromeDoc* d : s.docs) {
        if (!d->next) continue;
        if (promote(s, *d) > 0) kimb = true;
    }
    // Scripts taken again: a prelude's behaviours replaced at once; a screen's handlers when
    // that screen comes to it.
    for (KromeDoc* d : s.docs) {
        if (!d->script || !d->changed) continue;
        d->changed = false;
        script_changed(s, *d);
    }
    for (KromeScreen* sc : s.screens) {
        if (sc->failed && kimb) { // what it failed on may be there now
            sc->failed = false;
            sc->loading = true;
            sc->stage = KromeStage::Read;
        }
        if (sc->ready && kimb) sc->recheck = true;
    }
    for (KromeScreen* sc : s.screens) {
        if (!sc->ready) continue;
        if (sc->rescript) {
            if (!rescript(s, *sc)) continue;
            sc->rescript = false;
            sc->recheck = true;
        }
        if (sc->recheck && recheck(s, *sc)) sc->recheck = false;
    }
    krome_free_retired(s, false);
    engine_want_frame(*s.e);
}

// ---- the strip --------------------------------------------------------------------------------------

void krome_reload_strip(KromeModule& s) {
#if KV_DEV
    // Gone with its screen, or closed with the screen's overlays: shown again if still wanted.
    if (s.strip_overlay) {
        KromeScreen* on = krome_screen_by_id(s, s.strip_screen);
        if (!on || !krome_overlay(*on, s.strip_overlay)) { s.strip_overlay = 0; s.strip_screen = 0; s.strip_shown[0] = 0; }
    }
    if (!strcmp(s.reload_error, s.strip_shown) && (s.strip_overlay || !s.reload_error[0])) return;
    format(s.strip_shown, sizeof s.strip_shown, "%s", s.reload_error);
    if (KromeScreen* was = s.strip_overlay ? krome_screen_by_id(s, s.strip_screen) : nullptr) krome_close_overlay(s, *was, s.strip_overlay);
    s.strip_overlay = 0;
    s.strip_screen = 0;
    if (!s.reload_error[0]) return;
    KromeScreen* top = krome_top_screen(s);
    if (!top) return;
    u32 id = krome_open_overlay(s, *top);
    KromeOverlay* o = krome_overlay(*top, id);
    o->passive = true;
    o->side = KromeSide::Strip;
    if (!krome_strip_overlay(s, *top, *o, s.reload_error)) { krome_close_overlay(s, *top, id); return; }
    s.strip_screen = top->id;
    s.strip_overlay = id;
    engine_want_frame(*s.e);
#else
    (void)s;
#endif
}

} // namespace kv
