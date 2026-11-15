// The krome module: its state, the pump (scripts that have landed, model values from
// outside), and the C++ API (korvid/krome/krome.h).
#include "internal.h"
#include "korvid/core/arena.h"
#include "korvid/core/engine.h"
#include "korvid/core/event.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/phase.h"
#include "korvid/platform/io.h"
#include "korvid/core/profile.h"
#include "korvid/platform_hid/input.h"
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

namespace {

void free_screen(KromeModule& s, KromeScreen* sc) {
    krome_free_overlays(s, *sc);
    if (s.under.screen == sc->id) s.under = KromeTarget{};
    if (s.captured.screen == sc->id) s.captured = KromeTarget{};
    krome_lua_release(s, *sc);
    if (sc->io.is_null() == false) if (IoQueue* io = platform_io(*s.e)) io->cancel(sc->io);
    free_in(s.general, sc->source, sc->source_len + 1);
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
    sc->loading = true;
    s.screens.push(sc);
    return sc;
}

void run(KromeModule& s, KromeScreen& sc, const char* source, usize len) {
    krome_free_overlays(s, sc);
    sc.repaint = true;
    sc.loading = false;
    sc.ready = krome_lua_run(s, sc, source, len, name_str(sc.name));
    sc.failed = !sc.ready;
    if (sc.ready) KV_INFO("krome", "screen '%s': %u nodes", name_str(sc.name), sc.tree.nodes.len);
}

// Phase PreSim, main thread, every tick: scripts that have landed run; model values from
// outside go in.
void krome_pump(TickContext& ctx) {
    KromeModule& s = ctx.state_as<KromeModule>();
    Engine& e = *ctx.engine;
    s.ctx = &ctx;
    IoQueue* io = platform_io(e);
    for (u32 i = 0; i < s.screens.len; ++i) {
        KromeScreen& sc = *s.screens[i];
        if (!sc.loading) continue;
        if (sc.source) {
            char* src = sc.source;
            usize len = sc.source_len;
            sc.source = nullptr;
            run(s, sc, src, len);
            free_in(s.general, src, len + 1);
            continue;
        }
        if (!io) continue;
        if (sc.io.is_null()) {
            IoRequest r;
            r.path = name_str(sc.name);
            r.alloc = s.general;
            sc.io = io->read_asset(r);
            if (sc.io.is_null()) { KV_ERROR("krome", "cannot read '%s'", name_str(sc.name)); sc.loading = false; sc.failed = true; }
            continue;
        }
        for (const IoCompletion& c : io->completions()) {
            if (c.request != sc.io) continue;
            IoCompletion done = c;
            io->reap(done.request);
            sc.io = Handle{};
            if (done.status != IoStatus::Done) {
                KV_ERROR("krome", "cannot read '%s' (errno %d)", name_str(sc.name), done.error);
                sc.loading = false;
                sc.failed = true;
            } else {
                run(s, sc, (const char*)done.data, done.size);
            }
            free_in(s.general, done.data, done.size ? done.size : 1);
            break;
        }
    }
    for (const KromeSet& set : ctx.received<KromeSet>()) {
        KromeScreen* sc = krome_screen(s, Name{set.screen});
        if (!sc || !sc->ready) continue;
        const char* key = name_str(Name{set.key});
        if (set.kind == 2) {
            char text[KROME_SET_TEXT + 1];
            memcpy(text, set.text, KROME_SET_TEXT);
            text[KROME_SET_TEXT] = 0;
            krome_model_set_string(s, *sc, key, text);
        } else {
            krome_model_set_number(s, *sc, key, set.number, set.kind == 1);
        }
    }
    s.ctx = nullptr;
}

// Phase PreSim, main thread, every tick, after the pump: the pointer and Escape as this
// tick's sample has them.
void krome_input(TickContext& ctx) {
    KromeModule& s = ctx.state_as<KromeModule>();
    if (!s.input_id) s.input_id = ctx.resource_id("input.state"); // per engine: a static would outlive it
    const Input* in = ctx.resource<const Input>(s.input_id);
    if (!in) return;
    s.ctx = &ctx;
    const MouseState& m = in->sampled.mouse;
    krome_pointer(s, m.x, m.y, m.down(MouseButton::Left), m.wheel, in->sampled.down(Key::Escape));
    s.ctx = nullptr;
}

// Phase PostSim, main thread, every tick: layout and paint where something changed.
void krome_paint(TickContext& ctx) {
    KV_ZONE("krome.paint");
    KromeModule& s = ctx.state_as<KromeModule>();
    krome_view_frame(s);
}

bool krome_startup(Engine& e, ModuleInstance& self) {
    KromeModule* s = arena_push<KromeModule>(self.arena);
    if (!s) return false;
    *s = KromeModule{};
    s->e = &e;
    s->general = &self.general;
    s->screens.alloc = &self.general;
    s->fonts.alloc = &self.general;
    s->images.alloc = &self.general;
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
    krome_free_view(*s);
    krome_lua_close(*s);
}

constinit const FieldDesc k_event_fields[] = {KV_FIELDS(KromeEvent, screen, node, event, value)};
constinit const FieldDesc k_set_fields[] = {KV_FIELDS(KromeSet, screen, key, number, kind, pad, text)};
constinit const EventTypeDesc k_events[] = {KV_EVENT_EX(KromeEvent, k_event_fields, 0), KV_EVENT_EX(KromeSet, k_set_fields, 0)};
constinit const SystemDesc k_systems[] = {
    {.name = ".pump", .phase = Phase::PreSim, .fn = krome_pump, .reads = "KromeSet", .emits = "KromeEvent",
     .main_thread = true, .runs = RunWhen::Always},
    {.name = ".input", .phase = Phase::PreSim, .fn = krome_input, .reads = "input.state", .emits = "KromeEvent",
     .after = ".pump", .main_thread = true, .runs = RunWhen::Always},
    {.name = ".paint", .phase = Phase::PostSim, .fn = krome_paint, .main_thread = true, .runs = RunWhen::Always},
};

} // namespace

KV_MODULE_DEFINE(krome_module) = {
    .name     = "krome",
    .events   = k_events,
    .systems  = k_systems,
    .startup  = krome_startup,
    .shutdown = krome_shutdown,
    .memory   = {.reserve = 64 * 1024},
};

// ---- the API ---------------------------------------------------------------------------------------

Name krome_open(Engine& e, const char* script, i32 layer) {
    KromeModule* s = krome_state(e);
    if (!s || !script) return Name{};
    Name n = name_intern(script);
    new_screen(*s, n, layer);
    return n;
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
        if (n > KROME_SET_TEXT) n = KROME_SET_TEXT;
        memcpy(set.text, text, n);
    }
    e.events.emit(set);
}
} // namespace

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
    for (u32 i = 0; i < t.nodes.len; ++i) if (t.nodes[i].id == id) return i;
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
    }
    return st;
}

const char* krome_model_text(Engine& e, Name screen, const char* key) {
    KromeModule* s = krome_state(e);
    KromeScreen* sc = s ? krome_screen(*s, screen) : nullptr;
    if (!sc) return "";
    return krome_model_value_text(*s, *sc, key, s->text, sizeof s->text);
}

} // namespace kv
