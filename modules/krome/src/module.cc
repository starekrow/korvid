// The krome module: its state, the pump (scripts that have landed, model values from
// outside), and the C++ API (korvid/krome/krome.h).
#include "internal.h"
#include "korvid/core/arena.h"
#include "korvid/core/cli.h"
#include "korvid/core/engine.h"
#include "korvid/core/event.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/phase.h"
#include "korvid/platform/io.h"
#include "korvid/asset_render/texture.h"
#include "korvid/core/profile.h"
#include "korvid/platform_hid/input.h"
#include "korvid/platform_hid/events.h"
#include <stdlib.h>
#include <string.h>

namespace kv {

KromeModule* krome_state(Engine& e) { return engine_module_state_as<KromeModule>(e, "krome"); }

KromeScreen* krome_screen(KromeModule& s, Name name) {
    for (KromeScreen* sc : s.screens) if (sc->name == name) return sc;
    return nullptr;
}

KromeScreen* krome_screen_by_id(KromeModule& s, u32 id) {
    for (KromeScreen* sc : s.screens) if (sc->id == id) return sc;
    return nullptr;
}

void krome_emit(KromeModule& s, const KromeEvent& ev) {
    if (s.ctx) s.ctx->emit(ev);
    else s.e->events.emit(ev);
}

void krome_emit_dragged(KromeModule& s, const KromeDragged& ev) {
    if (s.ctx) s.ctx->emit(ev);
    else s.e->events.emit(ev);
}

void krome_emit_view(KromeModule& s, const KromeView& ev) {
    if (s.ctx) s.ctx->emit(ev);
    else s.e->events.emit(ev);
}

namespace {

void free_screen(KromeModule& s, KromeScreen* sc) {
    if (s.focus.node != KROME_NONE && s.focus.screen == sc->id) krome_edit_blur(s);
    krome_free_overlays(s, *sc);
    if (s.under.screen == sc->id) s.under = KromeTarget{};
    if (s.captured.screen == sc->id) s.captured = KromeTarget{};
    krome_drop_timers(s, sc->id); // a screen's timers go when it closes
    krome_lua_release(s, *sc);
    free_in(s.general, sc->source, sc->source_len + 1);
    if (sc->arena.base) arena_destroy(sc->arena);
    if (sc->frames.base) arena_destroy(sc->frames);
    sc->regions.free();
    sc->sparse_rows.free();
    sc->slots.free();
    sc->uses.free();
    sc->tree.free();
    sc->deps.free();
    free_items(s.general, sc, 1);
}

KromeScreen* new_screen(KromeModule& s, Name name, i32 layer) {
    for (u32 i = 0; i < s.screens.len; ++i)
        if (s.screens[i]->name == name) {
            free_screen(s, s.screens[i]);
            s.screens[i] = s.screens[s.screens.len - 1];
            --s.screens.len;
            break;
        }
    KromeScreen* sc = alloc_items<KromeScreen>(s.general, 1);
    *sc = KromeScreen{};
    sc->id = s.next_id++;
    sc->name = name;
    sc->layer = layer;
    sc->tree.init(s.general);
    sc->deps.alloc = s.general;
    sc->uses.alloc = s.general;
    sc->loading = true;
    s.screens.push(sc);
    return sc;
}

void fail(KromeScreen& sc) {
    sc.loading = false;
    sc.ready = false;
    sc.failed = true;
    sc.stage = KromeStage::Failed;
}

void built(KromeModule& s, KromeScreen& sc) {
    krome_free_overlays(s, sc);
    sc.repaint = true;
    sc.file = sc.doc ? sc.doc->file : nullptr;
    sc.held = false;
    bool ok = krome_build_screen(s, sc);
    sc.built_epoch = s.epoch;
    if (sc.waiting) return; // a kim() piece's file is on its way: built again when it lands
    if (!ok) { fail(sc); return; }
    sc.loading = false;
    sc.ready = true;
    sc.failed = false;
    sc.stage = KromeStage::Built;
    KV_INFO("krome", "screen '%s': %u nodes", name_str(sc.name), sc.tree.nodes.len);
}

// One step of a screen that is loading: its file, then its scripts (a .kimb's) or its
// script run (a Lua screen's), then the preludes it uses, then its tree.
void advance(KromeModule& s, KromeScreen& sc) {
    if (sc.stage == KromeStage::Read) {
        if (sc.source) { // a Lua screen given as source
            char* src = sc.source;
            usize len = sc.source_len;
            sc.source = nullptr;
            bool ok = krome_lua_run(s, sc, src, len, name_str(sc.name));
            free_in(s.general, src, len + 1);
            if (!ok) { fail(sc); return; }
            sc.stage = KromeStage::Uses;
        } else if (sc.doc) {
            if (sc.doc->failed) { fail(sc); return; }
            if (!sc.doc->ready) return;
            // Its scripts are asked for now, and run once all are in.
            if (const JsonValue* list = sc.doc->file->scripts)
                for (const JsonValue* v = json_first(sc.doc->file->doc, *list); v; v = json_next(sc.doc->file->doc, *v))
                    if (v->str()) krome_doc(s, name_intern(v->str()), true);
            sc.stage = KromeStage::Scripts;
        } else {
            KromeDoc* d = krome_doc(s, sc.script, true);
            if (d->failed) { fail(sc); return; }
            if (!d->ready) return;
            if (!krome_lua_run(s, sc, d->source, d->source_len, name_str(sc.name))) { fail(sc); return; }
            sc.stage = KromeStage::Uses;
        }
    }
    if (sc.stage == KromeStage::Scripts) {
        KromeDoc* list[64];
        u32 n = 0;
        if (const JsonValue* scripts = sc.doc->file->scripts)
            for (const JsonValue* v = json_first(sc.doc->file->doc, *scripts); v && n < 64; v = json_next(sc.doc->file->doc, *v)) {
                if (!v->str()) continue;
                KromeDoc* d = krome_doc(s, name_intern(v->str()), true);
                if (d->loading) return;
                if (d->failed) { krome_error(s, sc.doc->file->file, v->line, nullptr, 0, "script \"%s\": cannot read it", v->str()); continue; }
                list[n++] = d;
            }
        // A script that fails names itself, and the screen opens with what the others gave.
        krome_lua_scripts(s, sc, list, n);
        sc.stage = KromeStage::Uses;
    }
    if (sc.stage == KromeStage::Uses) {
        bool failed = false;
        if (!krome_scope_ready(s, sc, &failed)) { if (failed) fail(sc); return; }
        // Its preludes' scripts, once per state: their behaviours are there before it is built.
        if (!krome_prelude_scripts(s, sc.scope)) return;
        built(s, sc);
    }
}

// The whole of a kind-3 set's string, taken: the caller frees it. Null if it is not there.
char* take_set_text(KromeModule& s, f64 index) {
    u32 i = (u32)index;
    if (index < 0.0 || i >= s.set_texts.len) return nullptr;
    char* t = s.set_texts[i];
    s.set_texts[i] = nullptr;
    return t;
}

// Phase Frame, main thread: documents that have landed, screens that can go a step
// further, rebuilds asked for (a theme switch), and model values from outside.
void krome_pump(TickContext& ctx) {
    KV_ZONE("krome.pump");
    KromeModule& s = ctx.state_as<KromeModule>();
    s.ctx = &ctx;
    // The clock moves at the frame's start, so a timer due at the time the frame was asked
    // for fires in it (and the paint sees the same time).
    s.clock += ctx.dt;
    s.clock_ahead = true;
    bool changed = krome_pump_docs(s);
    if (changed || krome_reload_pending(s)) krome_reload(s);
    krome_reload_strip(s);
    for (u32 i = 0; i < s.screens.len; ++i) {
        KromeScreen& sc = *s.screens[i];
        if (sc.loading) advance(s, sc);
        if (sc.ready && sc.rebuild) {
            sc.rebuild = false;
            krome_build_screen(s, sc);
            engine_want_frame(*s.e);
        }
    }
    for (const KromeModule::SetDoc& d : s.set_docs) {
        KromeScreen* sc = krome_screen(s, d.screen);
        if (sc && sc->ready) krome_model_set_doc(s, *sc, name_str(d.key), d.data, d.size);
        free_in(s.general, d.data, d.size ? d.size : 1);
    }
    s.set_docs.clear();
    for (const KromeSet& set : ctx.received<KromeSet>()) {
        KromeScreen* sc = krome_screen(s, Name{set.screen});
        char* whole = set.kind == 3 ? take_set_text(s, set.number) : nullptr;
        if (!sc || !sc->ready) { free_in(s.general, whole, whole ? strlen(whole) + 1 : 0); continue; }
        const char* key = name_str(Name{set.key});
        if (set.kind == 3) {
            if (whole) krome_model_set_string(s, *sc, key, whole);
            free_in(s.general, whole, whole ? strlen(whole) + 1 : 0);
        } else if (set.kind == 2) {
            char text[KROME_SET_TEXT + 1];
            memcpy(text, set.text, KROME_SET_TEXT);
            text[KROME_SET_TEXT] = 0;
            krome_model_set_string(s, *sc, key, text);
        } else {
            krome_model_set_number(s, *sc, key, set.number, set.kind == 1);
        }
    }
    // Every long string taken: the indexes start again from 0.
    bool left = false;
    for (char* t : s.set_texts) if (t) left = true;
    if (!left) s.set_texts.len = 0;
    krome_run_timers(s); // before layout, on the clock the last paint left
    s.ctx = nullptr;
}

// The Frame cycle, after the pump: what the pointer and the keys did since the last frame.
// Presses, releases, the wheel and Escape come as events, so none is lost or counted twice
// however frames and ticks fall; where the pointer is now comes from the raw state.
void krome_input(TickContext& ctx) {
    KV_ZONE("krome.input");
    KromeModule& s = ctx.state_as<KromeModule>();
    if (!s.input_id) s.input_id = ctx.resource_id("input.state"); // per engine: a static would outlive it
    const Input* in = ctx.resource<const Input>(s.input_id);
    s.ctx = &ctx;
    s.desk_ok = in && in->raw.mouse.desktop;
    if (s.desk_ok) { s.desk_x = in->raw.mouse.desktop_x; s.desk_y = in->raw.mouse.desktop_y; }
    // The left button's changes in order: from up, down first; from down, up first.
    Span<const MouseButtonDown> downs = ctx.received<MouseButtonDown>();
    Span<const MouseButtonUp> ups = ctx.received<MouseButtonUp>();
    u32 d = 0, u = 0;
    for (;;) {
        while (d < downs.len && downs[d].button != MouseButton::Left) ++d;
        while (u < ups.len && ups[u].button != MouseButton::Left) ++u;
        // Each in its window, in that window's pixels (KR40).
        if (!s.down && d < downs.len) { krome_window_enter(s, downs[d].window); krome_pointer(s, downs[d].x, downs[d].y, true, 0.0f, false); ++d; continue; }
        if (s.down && u < ups.len) { krome_window_enter(s, ups[u].window); krome_pointer(s, ups[u].x, ups[u].y, false, 0.0f, false); ++u; continue; }
        break;
    }
    // The right button presses for a node's context event.
    for (const MouseButtonDown& b : downs)
        if (b.button == MouseButton::Right) { krome_window_enter(s, b.window); krome_context_press(s, b.x, b.y); }
    f32 wheel = 0.0f, wheel_x = 0.0f;
    for (const MouseWheel& w : ctx.received<MouseWheel>()) { wheel += w.dy; wheel_x += w.dx; }
    // The keyboard follows the OS's focus between the windows: a gain names the one it is in.
    for (const FocusChanged& f : ctx.received<FocusChanged>()) {
        s.window_focused = f.focused;
        if (f.focused) krome_window_focus(s, f.window);
    }
    // Keys go to the edit with the keyboard first; what it does not take (Escape with none)
    // is the screens'.
    bool escape = false;
    bool shift = in && (in->raw.down(Key::LShift) || in->raw.down(Key::RShift));
    bool ctrl = in && (in->raw.down(Key::LCtrl) || in->raw.down(Key::RCtrl));
    bool alt = in && (in->raw.down(Key::LAlt) || in->raw.down(Key::RAlt));
    bool gui = in && (in->raw.down(Key::LGui) || in->raw.down(Key::RGui));
    u32 mods = (shift ? KROME_MOD_SHIFT : 0) | (ctrl ? KROME_MOD_CTRL : 0) | (alt ? KROME_MOD_ALT : 0) | (gui ? KROME_MOD_CMD : 0);
    for (const KeyDown& k : ctx.received<KeyDown>()) {
        // Tab moves the focus (an edit with `tabs` keeps it, as a tab typed).
        if (k.key == Key::Tab && !(mods & (KROME_MOD_CTRL | KROME_MOD_ALT | KROME_MOD_CMD))) {
            if (krome_focus_tab(s, shift)) continue;
            krome_edit_typed(s, "\t");
            continue;
        }
        if (krome_edit_key(s, k.key, shift || k.key == Key::LShift, ctrl, alt, gui)) continue;
        if (krome_focus_key(s, k.key, mods)) continue;
        // What an edit types is its own, though it took no key for it: a shortcut needs a
        // modifier while one has the keyboard (or is a function key).
        KromeTree* ft = krome_focused_tree(s, nullptr);
        bool typing = ft && ft->nodes[s.focus.node].type == KromeType::Edit && !(mods & (KROME_MOD_CTRL | KROME_MOD_ALT | KROME_MOD_CMD))
                   && !(k.key >= Key::F1 && k.key <= Key::F12) && k.key != Key::Escape && k.key != Key::Enter;
        if (!typing && !k.repeat && krome_shortcut(s, k.key, mods)) continue;
        if (k.key == Key::Escape && !k.repeat) escape = true;
    }
    for (const TextComposing& c : ctx.received<TextComposing>()) krome_edit_compose(s, c.text, c.caret);
    for (const TextTyped& t : ctx.received<TextTyped>()) {
        // A commit ends the composition it came from.
        s.focus.composing[0] = 0;
        krome_edit_typed(s, t.text);
    }
    f32 x = in ? in->raw.mouse.x : s.px, y = in ? in->raw.mouse.y : s.py;
    if (in) krome_window_enter(s, in->raw.mouse.window); // where the pointer is now: its window's screens
    // A press whose release never came (the window lost the focus, or the pointer left it
    // where the platform keeps no hold on it) ends where the pointer is.
    if (s.down && in && !in->raw.mouse.down(MouseButton::Left)) krome_pointer(s, x, y, false, 0.0f, false);
    // Shift turns the wheel across.
    if (shift && wheel != 0.0f) { wheel_x -= wheel; wheel = 0.0f; }
    krome_pointer(s, x, y, s.down, wheel, escape);
    if (wheel_x != 0.0f) krome_wheel(s, wheel_x, 0.0f);
    krome_edit_frame(s);
    s.ctx = nullptr;
}

// Phase PostSim, main thread, every tick: layout and paint where something changed.
void krome_paint(TickContext& ctx) {
    KV_ZONE("krome.paint");
    KromeModule& s = ctx.state_as<KromeModule>();
    if (!s.clock_ahead) s.clock += ctx.dt; // a paint with no pump before it
    s.clock_ahead = false;
    krome_view_frame(s);
}

bool krome_startup(Engine& e, ModuleInstance& self) {
    KromeModule* s = arena_push<KromeModule>(self.arena);
    if (!s) return false;
    *s = KromeModule{};
    s->e = &e;
    s->general = &self.general;
    s->screens.alloc = &self.general;
    s->docs.alloc = &self.general;
    s->provided.alloc = &self.general;
    s->set_docs.alloc = &self.general;
    s->set_texts.alloc = &self.general;
    s->fonts.alloc = &self.general;
    s->images.alloc = s->image_errors.alloc = &self.general;
    s->layers.alloc = &self.general;
    s->list.init(&self.general);
    if (!krome_lua_open(*s)) return false;
    self.set_state(s);
    return true;
}

void krome_shutdown(Engine&, ModuleInstance& self) {
    KromeModule* s = self.state_as<KromeModule>();
    if (!s) return;
    for (KromeScreen* sc : s->screens) free_screen(*s, sc);
    s->screens.free();
    s->focus.buf.free();
    krome_free_docs(*s);
    for (KromeModule::SetDoc& d : s->set_docs) free_in(s->general, d.data, d.size ? d.size : 1);
    s->set_docs.free();
    for (char* t : s->set_texts) if (t) free_in(s->general, t, strlen(t) + 1);
    s->set_texts.free();
    krome_free_view(*s);
    krome_lua_close(*s);
    krome_free_behaviours(*s);
    s->anims.free();
    s->plays.free();
    s->motions.free();
    s->timers.free(); // their functions went with the Lua state
    s->marquees.free();
    s->arrivals.free();
}

// ---- /krome in the CLI --------------------------------------------------------------------------
// ls /krome: the screens (their names are asset paths, so they nest); ls a screen: its nodes
// that have an id, and its overlays (overlay<N>/); print a screen or an overlay: its tree as
// laid out; print a node: its rect, flags, look and text; patch a node: a property, for the
// run (`patch /krome/kromedemo/form.lua/save bg #ff0000`).

struct CliPath { KromeScreen* screen = nullptr; KromeTree* tree = nullptr; u32 overlay = 0; u32 node = KROME_NONE; bool rest_bad = false; };

// "kromedemo/form.lua[/overlay2][/id]" (a trailing slash allowed).
CliPath resolve(KromeModule& s, const char* rel) {
    CliPath p;
    usize best = 0;
    for (KromeScreen* sc : s.screens) {
        const char* n = name_str(sc->name);
        usize len = strlen(n);
        if (len > best && strncmp(rel, n, len) == 0 && (rel[len] == 0 || rel[len] == '/')) { p.screen = sc; best = len; }
    }
    if (!p.screen) return p;
    p.tree = &p.screen->tree;
    const char* rest = rel + best;
    if (*rest == '/') ++rest;
    if (!strncmp(rest, "overlay", 7)) {
        u32 id = (u32)strtoul(rest + 7, nullptr, 10);
        KromeOverlay* o = krome_overlay(*p.screen, id);
        if (!o) { p.rest_bad = true; return p; }
        p.overlay = id;
        p.tree = &o->tree;
        rest = strchr(rest, '/');
        rest = rest ? rest + 1 : "";
    }
    char word[96];
    usize n = 0;
    while (rest[n] && rest[n] != '/' && n + 1 < sizeof word) { word[n] = rest[n]; ++n; }
    word[n] = 0;
    if (!n) return p;
    if (word[0] == '#') {
        u32 i = (u32)strtoul(word + 1, nullptr, 10);
        if (i < p.tree->nodes.len) p.node = i; else p.rest_bad = true;
    } else {
        p.node = krome_find(*p.tree, name_intern(word));
        if (p.node == KROME_NONE) p.rest_bad = true;
    }
    return p;
}

void node_line(const KromeTree& t, u32 i, char* out, usize cap) {
    const KromeNode& n = t.nodes[i];
    const char* text = t.text(i);
    format(out, cap, "%s %.0f,%.0f %.0fx%.0f%s%s%s%s%.40s%s", krome_type_name(n.type), n.rect.x, n.rect.y, n.rect.w, n.rect.h,
           n.event.is_null() ? "" : " -> ", n.event.is_null() ? "" : name_str(n.event), (n.flags & KROME_DISABLED) ? " disabled" : "",
           *text ? "  \"" : "", text, *text ? "\"" : "");
}

void krome_cli_list(CliContext& ctx, const char* dir, CliEmitFn emit, void* user) {
    KromeModule* s = krome_state(*ctx.engine);
    if (!s) return;
    char summary[160];
    if (dir[0]) {
        CliPath p = resolve(*s, dir);
        if (p.screen && !p.rest_bad && p.node == KROME_NONE) {
            for (u32 i = 0; i < p.tree->nodes.len; ++i) {
                if (p.tree->nodes[i].id.is_null()) continue;
                node_line(*p.tree, i, summary, sizeof summary);
                if (!emit(user, name_str(p.tree->nodes[i].id), summary)) return;
            }
            if (!p.overlay)
                for (KromeOverlay* o : p.screen->overlays) {
                    char name[32];
                    format(name, sizeof name, "overlay%u/", o->id);
                    format(summary, sizeof summary, "%u nodes%s", o->tree.nodes.len, o->modal ? ", modal" : "");
                    if (!emit(user, name, summary)) return;
                }
            return;
        }
    }
    CliTree tree{dir, emit, user};
    for (KromeScreen* sc : s->screens) {
        format(summary, sizeof summary, "%s, %u nodes, layer %d, %u overlays", sc->ready ? "ready" : sc->loading ? "loading" : "failed",
               sc->tree.nodes.len, sc->layer, sc->overlays.len);
        if (!tree.add(name_str(sc->name), true, summary)) return;
    }
}

bool krome_cli_describe(CliContext& ctx, const char* rel) {
    KromeModule* s = krome_state(*ctx.engine);
    if (!s) return false;
    CliPath p = resolve(*s, rel);
    if (!p.screen || p.rest_bad) return false;
    char line[200];
    const KromeTree& t = *p.tree;
    if (p.node == KROME_NONE) {
        for (u32 i = 0; i < t.nodes.len; ++i) {
            u32 depth = 0;
            for (u32 q = t.nodes[i].parent; q != KROME_NONE; q = t.nodes[q].parent) ++depth;
            node_line(t, i, line, sizeof line);
            ctx.print("%*s#%u %s%s%s", (int)depth * 2, "", i, t.nodes[i].id.is_null() ? "" : name_str(t.nodes[i].id), t.nodes[i].id.is_null() ? "" : " ", line);
        }
        return true;
    }
    const KromeNode& n = t.nodes[p.node];
    node_line(t, p.node, line, sizeof line);
    ctx.print("%s", line);
    ctx.print("line %u, dir %s, pad %g %g %g %g, gap %g, flags 0x%x", n.line, n.dir == KromeDir::Row ? "row" : n.dir == KromeDir::Col ? "col" : "stack",
              n.pad[0], n.pad[1], n.pad[2], n.pad[3], n.gap, n.flags);
    KromeLook l = t.resolve_look(p.node, KromeState::Base, nullptr);
    ctx.print("bg %.2f %.2f %.2f %.2f, radius %g %g %g %g, border %g, color %.2f %.2f %.2f %.2f, size %g, font %s",
              l.bg.r, l.bg.g, l.bg.b, l.bg.a, l.radius[0], l.radius[1], l.radius[2], l.radius[3], l.border,
              l.text_color.r, l.text_color.g, l.text_color.b, l.text_color.a, l.size, name_str(l.font));
    for (u32 st = 1; st < (u32)KromeState::Count; ++st)
        if (n.states[st]) ctx.print("has a %s look", krome_state_name((KromeState)st));
    return true;
}

bool krome_cli_patch(CliContext& ctx, const char* rel, Span<const char* const> values) {
    KromeModule* s = krome_state(*ctx.engine);
    if (!s) return false;
    CliPath p = resolve(*s, rel);
    if (!p.screen || p.rest_bad || p.node == KROME_NONE) { ctx.fail("no such node"); return false; }
    if (values.len < 1) { ctx.fail("patch NODE PROPERTY [VALUE...]"); return false; }
    const char* prop = values[0];
    const char* why = nullptr;
    if (values.len == 1) why = p.tree->flag(p.node, prop);
    else {
        // One word: a number, a range (a..b) or a string; more: numbers.
        KromeValue v;
        f32 nums[4];
        u32 count = 0;
        bool all_numbers = true;
        for (u32 i = 1; i < values.len && i <= 4; ++i) {
            char* end = nullptr;
            nums[count++] = (f32)strtod(values[i], &end);
            if (!end || *end) all_numbers = false;
        }
        const char* dots = strstr(values[1], "..");
        if (values.len == 2 && dots && dots != values[1]) v = KromeValue::range((f32)strtod(values[1], nullptr), (f32)strtod(dots + 2, nullptr));
        else if (all_numbers) v = count == 1 ? KromeValue::num(nums[0]) : KromeValue::nums(nums, count);
        else v = KromeValue::str(values[1]);
        why = p.tree->set(p.node, prop, v);
    }
    if (why) { ctx.fail("%s: %s", prop, why); return false; }
    p.tree->dirty = true;
    p.screen->repaint = true;
    if (KromeOverlay* o = p.overlay ? krome_overlay(*p.screen, p.overlay) : nullptr) o->placed = false;
    engine_want_frame(*ctx.engine);
    return true;
}

constinit const CliMountDesc k_mounts[] = {{.path = "/krome", .list = krome_cli_list, .describe = krome_cli_describe, .patch = krome_cli_patch}};

constinit const FieldDesc k_event_fields[] = {KV_FIELDS(KromeEvent, screen, node, event, value, x, y)};
constinit const FieldDesc k_set_fields[] = {KV_FIELDS(KromeSet, screen, key, number, kind, pad, text)};
constinit const FieldDesc k_view_fields[] = {KV_FIELDS(KromeView, screen, event, first, count, total, pad)};
constinit const EventTypeDesc k_events[] = {KV_EVENT_EX(KromeEvent, k_event_fields, 0), KV_EVENT_EX(KromeSet, k_set_fields, 0),
                                            KV_EVENT_EX(KromeView, k_view_fields, 0), KV_EVENT(KromeDragged)};
// A document's blob landed or reloaded (in PreSim): a frame, so the pump takes it.
void on_asset_ready(TickContext& ctx, Span<const AssetReady>) { engine_want_frame(*ctx.engine); }
void on_asset_reloaded(TickContext& ctx, Span<const AssetReloaded>) { engine_want_frame(*ctx.engine); }
void on_asset_failed(TickContext& ctx, Span<const AssetFailed>) { engine_want_frame(*ctx.engine); }
constinit const HandlerDesc k_handlers[] = {
    {.name = ".on_asset_ready", KV_ON(AssetReady, on_asset_ready), .phase = Phase::PostSim},
    {.name = ".on_asset_reloaded", KV_ON(AssetReloaded, on_asset_reloaded), .phase = Phase::PostSim},
    {.name = ".on_asset_failed", KV_ON(AssetFailed, on_asset_failed), .phase = Phase::PostSim},
};
constinit const SystemDesc k_systems[] = {
    // The Frame cycle (docs/design/cycles.md): once before each frame drawn, never in a tick.
    {.name = ".pump", .phase = Phase::Frame, .fn = krome_pump, .reads = "KromeSet", .emits = "KromeEvent", .main_thread = true},
    {.name = ".input", .phase = Phase::Frame, .fn = krome_input,
     .reads = "input.state, MouseButtonDown, MouseButtonUp, MouseWheel, KeyDown, TextTyped, TextComposing, FocusChanged", .emits = "KromeEvent, KromeDragged",
     .after = ".pump", .main_thread = true},
    {.name = ".paint", .phase = Phase::Frame, .fn = krome_paint, .after = ".input", .main_thread = true},
};

} // namespace

KV_MODULE_DEFINE(krome_module) = {
    .name     = "krome",
    .events   = k_events,
    .systems  = k_systems,
    .handlers = k_handlers,
    .mounts   = k_mounts,
    .startup  = krome_startup,
    .shutdown = krome_shutdown,
    .memory   = {.reserve = 64 * 1024},
};

// ---- the API ---------------------------------------------------------------------------------------

Name krome_open(Engine& e, const char* script, i32 layer, u32 window) {
    KromeModule* s = krome_state(e);
    if (!s || !script || window >= KV_WINDOWS) return Name{};
    Name n = name_intern(script);
    KromeScreen* sc = new_screen(*s, n, layer);
    sc->window = (u8)window;
    sc->script = n;
    return n;
}

Name krome_open(Engine& e, const char* file, const char* screen, i32 layer, u32 window) {
    KromeModule* s = krome_state(e);
    if (!s || !file || !screen || window >= KV_WINDOWS) return Name{};
    char name[400];
    format(name, sizeof name, "%s#%s", file, screen);
    Name n = name_intern(name);
    KromeScreen* sc = new_screen(*s, n, layer);
    sc->window = (u8)window;
    sc->doc = krome_doc(*s, name_intern(file), false);
    sc->screen = name_intern(screen);
    return n;
}

void krome_provide(Engine& e, const char* path, const void* data, usize size) {
    KromeModule* s = krome_state(e);
    if (!s || !path) return;
    Name n = name_intern(path);
    u32 generation = 1; // given again: a document already taken takes it as a reload
    for (u32 i = 0; i < s->provided.len; ++i)
        if (s->provided[i].path == n) {
            generation = s->provided[i].generation + 1;
            free_in(s->general, s->provided[i].data, s->provided[i].size ? s->provided[i].size : 1);
            s->provided[i] = s->provided[s->provided.len - 1];
            --s->provided.len;
            break;
        }
    engine_want_frame(e);
    KromeModule::Provided p{n, (char*)alloc_in(s->general, size ? size : 1, 16), size, generation};
    if (size) memcpy(p.data, data, size);
    s->provided.push(p);
}

void krome_theme(Engine& e, const char* theme) {
    KromeModule* s = krome_state(e);
    if (!s) return;
    s->theme = theme ? name_intern(theme) : Name{};
    for (KromeScreen* sc : s->screens) sc->rebuild = true;
    engine_want_frame(e);
}

const char* krome_current_theme(Engine& e, Name screen) {
    KromeModule* s = krome_state(e);
    KromeScreen* sc = s ? krome_screen(*s, screen) : nullptr;
    if (!sc || !sc->ready) return nullptr;
    return krome_theme_of(*s, sc->scope);
}

bool krome_settled(Engine& e) {
    KromeModule* s = krome_state(e);
    if (!s) return true;
    for (KromeScreen* sc : s->screens) {
        if (sc->loading) return false;
        if (sc->ready && (sc->tree.dirty || sc->repaint || sc->rebuild)) return false;
        for (KromeOverlay* o : sc->overlays) if (!o->placed) return false;
    }
    for (const KromeFont& f : s->fonts) if (!f.ready && !f.asset.is_null()) return false;
    for (const KromeImage& i : s->images) if (!i.ready && !i.asset.is_null()) return false;
    if (s->shape_jobs.len) return false;
    // A strip that plays some number of times is still changing until it rests; one that
    // plays for ever never would, and does not hold a picture back.
    for (const KromeModule::Play& p : s->plays) if (p.due > 0.0 && !p.forever) return false;
    if (s->motions.len || s->arrivals.len) return false; // a place easing is still moving
    return true;
}

const KimErrors* krome_errors(Engine& e) {
    KromeModule* s = krome_state(e);
    return s ? &s->errors : nullptr;
}

void krome_clear_errors(Engine& e) {
    if (KromeModule* s = krome_state(e)) { s->errors = KimErrors{}; s->reload_error[0] = 0; engine_want_frame(e); }
}

Name krome_open_source(Engine& e, const char* name, const char* source, i32 layer) {
    KromeModule* s = krome_state(e);
    if (!s || !name || !source) return Name{};
    Name n = name_intern(name);
    KromeScreen* sc = new_screen(*s, n, layer);
    usize len = strlen(source);
    sc->source = (char*)alloc_in(s->general, len + 1, 1);
    memcpy(sc->source, source, len + 1);
    sc->source_len = len;
    return n;
}

void krome_close(Engine& e, Name screen) {
    KromeModule* s = krome_state(e);
    if (!s) return;
    for (u32 i = 0; i < s->screens.len; ++i)
        if (s->screens[i]->name == screen) {
            free_screen(*s, s->screens[i]);
            s->screens[i] = s->screens[s->screens.len - 1];
            --s->screens.len;
            return;
        }
}

bool krome_ready(Engine& e, Name screen) {
    KromeModule* s = krome_state(e);
    KromeScreen* sc = s ? krome_screen(*s, screen) : nullptr;
    return sc && sc->ready;
}

namespace {
void send(Engine& e, Name screen, const char* key, f64 number, u32 kind, const char* text) {
    KromeSet set{};
    set.screen = screen.hash;
    set.key = name_intern(key).hash;
    set.number = number;
    set.kind = kind;
    if (text) {
        usize n = strlen(text);
        // Longer than the event holds: kept whole here, the event saying which (S2). The
        // event keeps its place among the other sets, so the order is the caller's.
        KromeModule* s = n > KROME_SET_TEXT ? krome_state(e) : nullptr;
        if (s) {
            char* whole = (char*)alloc_in(s->general, n + 1, 1);
            memcpy(whole, text, n + 1);
            set.kind = 3;
            set.number = (f64)s->set_texts.len;
            s->set_texts.push(whole);
        }
        memcpy(set.text, text, n < KROME_SET_TEXT ? n : KROME_SET_TEXT);
    }
    e.events.emit(set);
    // The pump that applies it runs in the Frame cycle: without a frame asked for, a loop
    // that draws only when asked (decoupled pacing) would not apply it until something
    // else drew (S1). The frame that applies it draws what it changed.
    engine_want_frame(e);
}

} // namespace

void krome_set_doc(Engine& e, Name screen, const char* key, const void* kbor, usize size) {
    KromeModule* s = krome_state(e);
    if (!s || !key || !kbor) return;
    KromeModule::SetDoc d{screen, name_intern(key), (u8*)alloc_in(s->general, size ? size : 1, 16), size};
    if (size) memcpy(d.data, kbor, size);
    s->set_docs.push(d);
    engine_want_frame(e); // applied in the Frame cycle, as krome_set's
}

void krome_set(Engine& e, Name screen, const char* key, f64 value) { send(e, screen, key, value, 0, nullptr); }
void krome_set(Engine& e, Name screen, const char* key, const char* value) { send(e, screen, key, 0.0, 2, value ? value : ""); }
void krome_set_bool(Engine& e, Name screen, const char* key, bool value) { send(e, screen, key, value ? 1.0 : 0.0, 1, nullptr); }

bool krome_press(Engine& e, Name screen, const char* id) {
    KromeModule* s = krome_state(e);
    KromeScreen* sc = s ? krome_screen(*s, screen) : nullptr;
    if (!sc || !sc->ready || !id) return false;
    u32 n = krome_find(sc->tree, name_intern(id));
    return n != KROME_NONE && krome_fire(*s, *sc, sc->tree, n, 1.0);
}

const KromeTree* krome_tree(Engine& e, Name screen) {
    KromeModule* s = krome_state(e);
    KromeScreen* sc = s ? krome_screen(*s, screen) : nullptr;
    return sc ? &sc->tree : nullptr;
}

bool krome_find_rect(Engine& e, Name screen, const char* id, rect2* out) {
    KromeModule* s = krome_state(e);
    KromeScreen* sc = s ? krome_screen(*s, screen) : nullptr;
    if (!sc || !id) return false;
    Name n = name_intern(id);
    for (u32 k = sc->overlays.len; k > 0; --k) {
        u32 i = krome_find(sc->overlays[k - 1]->tree, n);
        if (i != KROME_NONE) { *out = sc->overlays[k - 1]->tree.nodes[i].rect; return true; }
    }
    u32 i = krome_find(sc->tree, n);
    if (i == KROME_NONE) return false;
    *out = sc->tree.nodes[i].rect;
    return true;
}

u32 krome_find(const KromeTree& t, Name id) {
    if (id.is_null()) return KROME_NONE;
    for (u32 i = 0; i < t.nodes.len; ++i) if (t.nodes[i].id == id && !(t.nodes[i].flags & KROME_DEAD)) return i;
    return KROME_NONE;
}

void krome_viewport(Engine& e, f32 width, f32 height, f32 scale) {
    KromeModule* s = krome_state(e);
    if (!s) return;
    s->viewport_fixed = true;
    s->win_w = width;
    s->win_h = height;
    s->scale = scale > 0.0f ? scale : 1.0f;
    for (KromeScreen* sc : s->screens) {
        sc->tree.dirty = true;
        for (KromeOverlay* o : sc->overlays) o->placed = false;
    }
}

KromeStats krome_stats(Engine& e) {
    KromeStats st;
    if (KromeModule* s = krome_state(e)) {
        st.layouts = s->layouts; st.paints = s->paints; st.submits = s->submits;
        st.fired = s->fired; st.script_errors = s->script_errors;
        st.rebuilt_regions = s->rebuilt_regions; st.rebuilt_nodes = s->rebuilt_nodes;
        st.shapes = s->shapes_drawn; st.shape_jobs = s->shape_jobs_run;
    }
    return st;
}

const char* krome_model_text(Engine& e, Name screen, const char* key) {
    KromeModule* s = krome_state(e);
    KromeScreen* sc = s ? krome_screen(*s, screen) : nullptr;
    if (!sc) return "";
    // A dotted path ("sel.key") as a screen's texts read one.
    if (key && strchr(key, '.')) return krome_model_path_text(*s, *sc, key, s->text, sizeof s->text);
    return krome_model_value_text(*s, *sc, key, s->text, sizeof s->text);
}

} // namespace kv
