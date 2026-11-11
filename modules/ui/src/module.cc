// The ui module: screens, input and events, the model, and the sprites it draws.
#include "ui_internal.h"
#include "korvid/asset/material.h"
#include "korvid/asset/scope.h"
#include "korvid/core/sort.h"
#include "korvid/audio/audio.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/profile.h"
#include "korvid/core/utf8.h"
#include "korvid/math/fields.h"
#include "korvid/platform_hid/actions.h"
#include "korvid/platform_hid/host_window.h"
#include "korvid/platform_hid/input.h"
#include "korvid/platform_hid/module.h"
#include "korvid/render/renderer.h"
#include "korvid/sprite/sprite.h"
#include "korvid/sprite/transform2.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace kv {

UiState_* ui_state(Engine& e) { return engine_module_state_as<UiState_>(e, "ui"); }

namespace {

void* screen_resolve(kv_handle h, void* ctx) { return ((UiState_*)ctx)->screens.get(Handle{h}); }
const char* screen_name_of(kv_handle h, void* ctx) { UiScreen* s = ((UiState_*)ctx)->screens.get(Handle{h}); return s ? s->name : nullptr; }
int screen_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    UiScreen* s = ((UiState_*)ctx)->screens.get(Handle{h});
    return s ? snprintf(out, cap, "'%s' %u nodes, layer %d%s%s", s->name, s->nodes.len, s->open.layer, s->open.input ? ", input" : "", s->open.hold ? ", holds" : "")
             : snprintf(out, cap, "stale");
}

void free_nodes(UiState_& s, UiScreen& sc) {
    for (UiNode& n : sc.nodes) ui_node_free(s, n);
    sc.nodes.clear();
}

// The node at a path from the root ("" is the root), or KV_UI_NONE.
u32 find(const UiScreen& sc, const char* path) {
    if (!sc.nodes.len) return KV_UI_NONE;
    u32 at = 0;
    const char* p = path ? path : "";
    while (*p) {
        const char* slash = strchr(p, '/');
        usize len = slash ? (usize)(slash - p) : strlen(p);
        char part[64];
        if (len >= sizeof part) return KV_UI_NONE;
        memcpy(part, p, len);
        part[len] = 0;
        Name want = name_lookup(part);
        u32 hit = KV_UI_NONE;
        if (at == 0 && p == path && !want.is_null() && sc.nodes[0].name == want) { // the root, by its own name
            p = slash ? slash + 1 : p + len;
            continue;
        }
        for (u32 k = sc.nodes[at].first; k != KV_UI_NONE && !want.is_null(); k = sc.nodes[k].next)
            if (sc.nodes[k].name == want) { hit = k; break; }
        // Not a child: anywhere below, by name, so a deep node needs only its own name.
        if (hit == KV_UI_NONE && !want.is_null() && !slash)
            for (u32 k = at + 1; k < sc.nodes.len; ++k) if (sc.nodes[k].name == want) { hit = k; break; }
        if (hit == KV_UI_NONE) return KV_UI_NONE;
        at = hit;
        p = slash ? slash + 1 : p + len;
    }
    return at;
}

UiNode* node_of(Engine& e, Handle screen, const char* path, UiScreen** out = nullptr) {
    UiState_* s = ui_state(e);
    UiScreen* sc = s ? s->screens.get(screen) : nullptr;
    if (!sc) return nullptr;
    u32 i = find(*sc, path);
    if (out) *out = sc;
    return i == KV_UI_NONE ? nullptr : &sc->nodes[i];
}

void play(TickContext& ctx, UiState_& s, const UiScreen& sc, const char* which) {
    UiDoc* d = s.docs.get(sc.doc);
    const UiSound* u = d ? ui_doc_sound(*d, which) : nullptr;
    if (!u) return;
    SoundPlay p;
    p.sound = u->sound;
    p.bus = AudioBus::Ui;
    p.pause_with_sim = false;
    p.volume = 0.6f;
    audio_play(ctx, p);
}

void emit(TickContext& ctx, Handle screen, Name node, const char* event, i32 value) {
    ctx.emit(UiEvent{screen, node.hash, name_intern(event).hash, value, 0});
}

// The list nearest the root of a screen that is visible, for focus.
u32 first_list(const UiScreen& sc) {
    for (u32 i = 0; i < sc.nodes.len; ++i) {
        if (sc.nodes[i].type != UiType::List) continue;
        bool shown = true;
        for (u32 a = i; a != KV_UI_NONE; a = sc.nodes[a].parent) if (!sc.nodes[a].visible) { shown = false; break; }
        if (shown) return i;
    }
    return KV_UI_NONE;
}

u32 child_at(const UiScreen& sc, u32 parent, u32 index) {
    u32 k = sc.nodes[parent].first;
    for (u32 i = 0; i < index && k != KV_UI_NONE; ++i) k = sc.nodes[k].next;
    return k;
}

u32 child_count(const UiScreen& sc, u32 parent) {
    u32 n = 0;
    for (u32 k = sc.nodes[parent].first; k != KV_UI_NONE; k = sc.nodes[k].next) ++n;
    return n;
}

// Phase PreSim, main thread, every tick: documents advance, reveals run, the topmost
// screen that takes input reads the actions and says what happened.
void ui_update(TickContext& ctx) {
    KV_ZONE("ui.update");
    static const u32 k_actions = ctx.resource_id("input.actions");
    UiState_& s = ctx.state_as<UiState_>();
    Engine& e = *ctx.engine;
    ui_docs_pump(e, s);
    f32 dt = (f32)ctx.dt;
    // Screens on their way in or out; the ones out are closed.
    for (u32 i = 0; i < s.screens.len; ++i) {
        if (!s.screens.is_alive(i)) continue;
        UiScreen& sc = s.screens.items[i];
        if (sc.motion_t < 1.0f) sc.motion_t = sc.motion.fx == UiFx::Cut || sc.motion.seconds <= 0.0f ? 1.0f : sc.motion_t + dt / sc.motion.seconds;
        if (sc.motion_t > 1.0f) sc.motion_t = 1.0f;
        if (sc.leaving && sc.motion_t >= 1.0f) ui_close(e, s.screens.handle_at(i));
    }
    // Covers: closing, waiting, opening, in that order, each saying so once.
    bool cover_hold = false;
    for (u32 i = 0; i < s.covers.len;) {
        UiCoverState& c = s.covers[i];
        f32 step = c.desc.seconds > 0.0f ? dt / c.desc.seconds : 1.0f;
        if (c.phase == 0) { c.amount += step; if (c.amount >= 1.0f) { c.amount = 1.0f; c.phase = 1; } }
        else if (c.phase == 1) {
            if (!c.said) { ctx.emit(UiCovered{c.id, 0, c.desc.user}); c.said = true; }
            else if (!c.wait) c.phase = 2;
        } else {
            c.amount -= step;
            if (c.amount <= 0.0f) {
                ctx.emit(UiUncovered{c.id, 0, c.desc.user});
                s.covers[i] = s.covers.last();
                --s.covers.len;
                continue;
            }
        }
        cover_hold |= c.desc.hold;
        ++i;
    }
    Handle top;
    u32 top_order = 0;
    i32 top_layer = -0x7FFFFFFF;
    bool hold = false;
    for (u32 i = 0; i < s.screens.len; ++i) {
        if (!s.screens.is_alive(i)) continue;
        UiScreen& sc = s.screens.items[i];
        sc.time += dt;
        hold |= sc.open.hold;
        if (sc.open.input && sc.built && !sc.leaving && (sc.open.layer > top_layer || (sc.open.layer == top_layer && sc.order > top_order))) {
            top = s.screens.handle_at(i); top_layer = sc.open.layer; top_order = sc.order;
        }
        // Reveals: characters appear at the label's rate; a tick sound every few; done once.
        for (UiNode& n : sc.nodes) {
            if (n.revealed || n.reveal_rate <= 0.0f || !n.visible) continue;
            n.reveal_t += dt;
            f32 total = 0.0f;
            u32 count = ui_text_chars(s, n.text, n.reveal_rate, &total);
            u32 shown = ui_text_shown(s, n.text, n.reveal_rate, n.reveal_t);
            if (shown / 2 != n.reveal_shown / 2 && shown < count) play(ctx, s, sc, "tick");
            n.reveal_shown = shown;
            if (n.reveal_t >= total) {
                n.revealed = true;
                emit(ctx, s.screens.handle_at(i), n.name, "revealed", 0);
            }
        }
    }
    hold |= cover_hold;
    if (hold != s.holding) { engine_hold(e, HoldUi, hold); s.holding = hold; }

    UiScreen* sc = s.screens.get(top);
    // A field on the topmost screen takes the keyboard's text, and the platform's text
    // input is on while it does.
    u32 field = KV_UI_NONE;
    for (u32 k = 0; sc && k < sc->nodes.len && field == KV_UI_NONE; ++k) {
        if (sc->nodes[k].type != UiType::Field) continue;
        bool shown = true;
        for (u32 p = k; p != KV_UI_NONE; p = sc->nodes[p].parent) if (!sc->nodes[p].visible) { shown = false; break; }
        if (shown) field = k;
    }
    if ((field != KV_UI_NONE) != s.text_on) { s.text_on = field != KV_UI_NONE; platform_text_input(e, s.text_on); }
    if (field != KV_UI_NONE && !s.covers.len) {
        static const u32 k_state = ctx.resource_id("input.state");
        const Input* in = ctx.resource<const Input>(k_state);
        if (!in) return;
        const InputState& is = in->sampled;
        UiNode& f = sc->nodes[field];
        char buf[256];
        utf8_copy(buf, sizeof buf, f.text ? f.text : "");
        bool changed = false, submit = false;
        for (u32 b = 0; b < is.backspaces; ++b) {
            usize n = strlen(buf);
            if (!n) break;
            while (n > 0 && ((u8)buf[n - 1] & 0xC0) == 0x80) --n; // back over continuation bytes
            buf[n ? n - 1 : 0] = 0;
            changed = true;
        }
        const char* p = is.text;
        while (*p) {
            const char* at = p;
            u32 cp = utf8_next(p);
            if (!cp) break;
            if (cp == '\n' || cp == '\r') { submit = true; break; }
            if (cp < 0x20 || cp == 0x7F) continue;
            if (utf8_count(buf) >= f.max_length) continue;
            usize n = strlen(buf), w = (usize)(p - at);
            if (n + w + 1 > sizeof buf) continue;
            memcpy(buf + n, at, w);
            buf[n + w] = 0;
            changed = true;
        }
        bool enter = is.down(Key::Enter), escape = is.down(Key::Escape);
        bool pad_ok = false;
        if (const GamepadState* pad = is.pad()) pad_ok = pad->down(PadButton::South) && !(s.keys_prev[1] & 1);
        if (enter && !(s.keys_prev[0] & 1)) submit = true;
        if (pad_ok) submit = true;
        s.keys_prev[0] = enter ? 1 : 0;
        s.keys_prev[1] = is.pad() && is.pad()->down(PadButton::South) ? 1 : 0;
        if (changed) { ui_node_set_text(s, f, buf); emit(ctx, top, f.name, "changed", (i32)utf8_count(buf)); }
        if (submit) { play(ctx, s, *sc, "select"); emit(ctx, top, f.name, "submit", (i32)utf8_count(buf)); }
        if (escape && !(s.keys_prev[0] & 2)) { play(ctx, s, *sc, "cancel"); emit(ctx, top, f.name, "cancel", 0); }
        s.keys_prev[0] |= escape ? 2 : 0;
        return; // a field takes the keyboard: no actions for this screen
    }
    const InputActions* a = ctx.resource<const InputActions>(k_actions);
    if (!sc || !a || s.covers.len) return; // nothing takes input under a cover
    UiDoc* d = s.docs.get(sc->doc);
    u32 act_move = a->find(d ? d->actions[0] : "move"), act_confirm = a->find(d ? d->actions[1] : "confirm"), act_cancel = a->find(d ? d->actions[2] : "cancel");
    bool confirm = action_pressed(*a, act_confirm), cancel = action_pressed(*a, act_cancel);
    vec2 mv = action_axis(*a, act_move);
    bool step = action_repeat(*a, act_move, 18, 6);
    // A reveal in progress takes the confirm and finishes.
    for (UiNode& n : sc->nodes)
        if (confirm && !n.revealed && n.reveal_rate > 0.0f && n.visible) {
            n.reveal_t = 1e6f;
            confirm = false;
        }
    // Scrolls that take input move with the axis.
    for (UiNode& n : sc->nodes)
        if (n.type == UiType::Scroll && n.input && n.visible && mv.y != 0.0f) n.scroll -= mv.y * 120.0f * dt;
    u32 list = first_list(*sc);
    if (list != KV_UI_NONE) {
        UiNode& l = sc->nodes[list];
        u32 count = child_count(*sc, list);
        if (count && step && fabsf(mv.y) > 0.5f) {
            l.focus = mv.y < 0.0f ? (l.focus + 1) % count : (l.focus + count - 1) % count;
            play(ctx, s, *sc, "move");
        }
        if (count && confirm) {
            u32 item = child_at(*sc, list, l.focus);
            const UiNode& it = sc->nodes[item];
            play(ctx, s, *sc, "select");
            ctx.emit(UiEvent{top, it.name.hash, it.event.is_null() ? name_intern("select").hash : it.event.hash, (i32)l.focus, 0});
            confirm = false;
        }
        if (cancel) {
            play(ctx, s, *sc, "cancel");
            emit(ctx, top, l.name, "cancel", 0);
            cancel = false;
        }
    }
    if (confirm) emit(ctx, top, sc->nodes.len ? sc->nodes[0].name : Name{}, "confirm", 0);
    if (cancel) { play(ctx, s, *sc, "cancel"); emit(ctx, top, sc->nodes.len ? sc->nodes[0].name : Name{}, "cancel", 0); }
}

int compare_screens(const void* x, const void* y) {
    const UiScreen* a = *(UiScreen* const*)x;
    const UiScreen* b = *(UiScreen* const*)y;
    if (a->open.layer != b->open.layer) return a->open.layer < b->open.layer ? -1 : 1;
    return a->order < b->order ? -1 : a->order > b->order ? 1 : 0;
}

// The flat white the panels without frames and the bars are drawn with.
void ensure_white(Engine& e, UiState_& s) {
    if (!s.white_material.is_null()) return;
    Renderer* r = renderer_get(e);
    if (!r) return;
    TextureDesc td;
    td.name = "ui.white";
    s.white_texture = renderer_texture_create(r, td);
    const u8 white[4] = {255, 255, 255, 255};
    renderer_texture_upload(r, s.white_texture, 1, 1, white, sizeof white);
    MaterialDesc md;
    md.name = "ui.white";
    md.texture = s.white_texture;
    s.white_material = renderer_material_create(r, md);
    // The iris: clear inside a circle, opaque outside, a pixel of edge, 256 square.
    constexpr u32 kIris = 256;
    MemScope scope;
    u8* px = (u8*)alloc_in(mem_scratch(), kIris * kIris * 4);
    for (u32 y = 0; y < kIris; ++y)
        for (u32 x = 0; x < kIris; ++x) {
            f32 dx = ((f32)x + 0.5f) / (f32)kIris - 0.5f, dy = ((f32)y + 0.5f) / (f32)kIris - 0.5f;
            f32 edge = (sqrtf(dx * dx + dy * dy) - 0.5f) * (f32)kIris + 0.5f;
            f32 alpha = edge < 0.0f ? 0.0f : edge > 1.0f ? 1.0f : edge;
            u8* p = px + ((usize)y * kIris + x) * 4;
            p[0] = p[1] = p[2] = 255;
            p[3] = (u8)(alpha * 255.0f + 0.5f);
        }
    TextureDesc it;
    it.name = "ui.iris";
    s.iris_texture = renderer_texture_create(r, it);
    renderer_texture_upload(r, s.iris_texture, kIris, kIris, px, (usize)kIris * kIris * 4);
    MaterialDesc im;
    im.name = "ui.iris";
    im.texture = s.iris_texture;
    im.filter = TextureFilter::Linear;
    s.iris_material = renderer_material_create(r, im);
}

// Phase PostSim, main thread, every tick: screens built as their documents arrive, laid
// out against the window, drawn into ui.sprite rows (reused in order, so only what
// changed reaches the renderer).
void ui_draw(TickContext& ctx) {
    KV_ZONE("ui.draw");
    static const u32 k_window = engine_resource_id(*ctx.engine, "platform_hid.window");
    UiState_& s = ctx.state_as<UiState_>();
    Engine& e = *ctx.engine;
    ensure_white(e, s);
    const HostWindow* w = k_window ? (const HostWindow*)e.resources[k_window - 1].ptr : nullptr;
    if (w && w->width() && w->height()) { s.win_w = (f32)w->width(); s.win_h = (f32)w->height(); }

    MemScope scope;
    Array<UiScreen*> order = array_in<UiScreen*>(mem_scratch());
    for (u32 i = 0; i < s.screens.len; ++i) {
        if (!s.screens.is_alive(i)) continue;
        UiScreen& sc = s.screens.items[i];
        if (!sc.built && !sc.code) {
            UiDoc* d = s.docs.get(sc.doc);
            if (d && d->state == AssetState::Ready) {
                // Rebuilt (a reload): the focus and scrolls come back by name.
                MemScope keep;
                Array<Name> focus_names = array_in<Name>(mem_scratch());
                for (u32 k = 0; k < sc.nodes.len; ++k)
                    if (sc.nodes[k].type == UiType::List) { u32 it = child_at(sc, k, sc.nodes[k].focus); focus_names.push(it != KV_UI_NONE ? sc.nodes[it].name : Name{}); }
                free_nodes(s, sc);
                sc.built = ui_build_from_doc(e, s, sc);
                u32 li = 0;
                for (u32 k = 0; k < sc.nodes.len && li < focus_names.len; ++k) {
                    if (sc.nodes[k].type != UiType::List) continue;
                    Name want = focus_names[li++];
                    u32 idx = 0;
                    for (u32 c = sc.nodes[k].first; c != KV_UI_NONE; c = sc.nodes[c].next, ++idx) if (sc.nodes[c].name == want) sc.nodes[k].focus = idx;
                }
                if (!sc.built) sc.code = true; // nothing to wait for: draw it empty
            }
        }
        order.push(&sc);
    }
    if (order.len > 1) kv::sort(order.data, order.len, sizeof(UiScreen*), compare_screens);
    // The scale: the tallest design that fits, whole when the topmost document wants pixels.
    UiDoc* lead = order.len ? s.docs.get(order.last()->doc) : nullptr;
    f32 design_h = lead ? lead->design.y : 270.0f;
    s.scale = s.win_h / design_h;
    if (!lead || lead->pixel) s.scale = floorf(s.scale) >= 1.0f ? floorf(s.scale) : 1.0f;

    s.quads.clear();
    UiStats st{};
    st.scale = s.scale;
    for (UiScreen* sc : order) {
        ++st.screens;
        st.nodes += sc->nodes.len;
        ui_layout(e, s, *sc);
        u32 first = s.quads.len;
        ui_emit(e, s, *sc);
        // Its motion: in as t goes 0 to 1, out as it goes 0 to 1 while leaving.
        f32 t = sc->motion_t < 1.0f ? sc->motion_t : 1.0f;
        f32 shown = sc->leaving ? 1.0f - t : t;
        shown = shown * shown * (3.0f - 2.0f * shown); // smooth at both ends
        if (sc->motion.fx == UiFx::Fade) for (u32 q = first; q < s.quads.len; ++q) s.quads[q].tint.a *= shown;
        if (sc->motion.fx == UiFx::Slide) {
            f32 ax, ay;
            static const i8 k_x[] = {0, -1, 0, 1, -1, 1, -1, 0, 1}, k_y[] = {0, -1, -1, -1, 0, 0, 1, 1, 1};
            ax = (f32)k_x[(u32)sc->motion.from % 9]; ay = (f32)k_y[(u32)sc->motion.from % 9];
            f32 dx = ax * s.win_w * (1.0f - shown), dy = ay * s.win_h * (1.0f - shown);
            for (u32 q = first; q < s.quads.len; ++q) { s.quads[q].x += floorf(dx); s.quads[q].y += floorf(dy); }
        }
    }
    // Covers, over everything.
    for (const UiCoverState& c : s.covers) {
        f32 a = c.amount * c.amount * (3.0f - 2.0f * c.amount);
        UiQuad q{};
        q.material = s.white_material;
        q.uv = rect2_unit();
        q.layer = 127;
        q.tint = c.desc.tint;
        if (q.material.is_null()) continue;
        if (c.desc.fx != UiFx::Iris) {
            q.x = 0; q.y = 0; q.w = s.win_w; q.h = s.win_h;
            q.tint.a *= a;
            s.quads.push(q);
            continue;
        }
        // The iris: a disc of clear over the focus, shrinking to nothing, and black around it.
        f32 cx = c.desc.focus.x * s.win_w, cy = c.desc.focus.y * s.win_h;
        f32 reach = sqrtf(s.win_w * s.win_w + s.win_h * s.win_h);
        f32 r = reach * (1.0f - a);
        f32 x0 = floorf(cx - r), y0 = floorf(cy - r), d = ceilf(2.0f * r);
        if (d >= 1.0f && !s.iris_material.is_null()) {
            UiQuad disc = q;
            disc.material = s.iris_material;
            disc.x = x0; disc.y = y0; disc.w = d; disc.h = d;
            s.quads.push(disc);
        }
        auto rect = [&](f32 x, f32 y, f32 w, f32 h) { if (w <= 0.0f || h <= 0.0f) return; UiQuad b = q; b.x = x; b.y = y; b.w = w; b.h = h; s.quads.push(b); };
        if (d < 1.0f) { x0 = cx; y0 = cy; d = 0.0f; }
        rect(0, 0, s.win_w, y0);                         // above
        rect(0, y0 + d, s.win_w, s.win_h - y0 - d);      // below
        rect(0, y0, x0, d);                              // left
        rect(x0 + d, y0, s.win_w - x0 - d, d);           // right
    }

    Table* rows = ctx.write_kind(s.sprite_kind);
    u32 n = s.quads.len;
    for (u32 i = 0; i < n; ++i) {
        const UiQuad& q = s.quads[i];
        Sprite sp;
        sp.material = q.material;
        sp.uv = q.uv;
        sp.size = vec2{q.w, q.h};
        sp.tint = q.tint;
        sp.layer = q.layer;
        sp.space = SpriteSpace::Screen;
        sp.anchor = ScreenAnchor::TopLeft;
        sp.sort = SpriteSort::Key;
        sp.order = (f32)i;
        Transform2 t = transform2_at(point2{q.x + q.w * 0.5f, -(q.y + q.h * 0.5f)});
        if (i < s.rows.len && rows->valid(s.rows[i])) {
            *rows->get<Sprite>(s.rows[i]) = sp;
            *rows->get<Transform2>(s.rows[i]) = t;
            *rows->prev<Transform2>(s.rows[i]) = t; // the UI does not interpolate
        } else {
            Handle h = rows->spawn(t, sp);
            if (i < s.rows.len) s.rows[i] = h; else s.rows.push(h);
        }
    }
    for (u32 i = n; i < s.rows.len; ++i) rows->destroy(s.rows[i]);
    if (s.rows.len > n) s.rows.len = n;
    st.sprites = n;
    s.stats = st;
}

bool ui_startup(Engine& e, ModuleInstance& self) {
    UiState_* s = arena_push<UiState_>(self.arena);
    if (!s) return false;
    s->general = &self.general;
    s->quads.alloc = s->rows.alloc = &self.general;
    s->covers.alloc = &self.general;
    s->model.init(&self.general);
    kv_handle_kind_desc desc{};
    desc.name = "ui.screen"; desc.type_key = type_key<UiScreen>(); desc.ctx = s;
    desc.resolve = screen_resolve; desc.describe = screen_describe; desc.flags = KV_HANDLE_GENERATIONAL;
    desc.name_of = screen_name_of;
    s->screen_kind = handle_kind_register(desc);
    if (!s->screen_kind) return false;
    s->screens.use_kind(s->screen_kind);
    if (!s->screens.init(256, self.general.tag)) return false;
    if (!ui_docs_init(e, *s, &self.general, self.general.tag)) return false;
    s->sprite_kind = engine_kind_id(e, "ui.sprite");
    self.set_state(s);
    return true;
}

void ui_shutdown(Engine& e, ModuleInstance& self) {
    UiState_* s = self.state_as<UiState_>();
    if (!s) return;
    for (u32 i = 0; i < s->screens.len; ++i)
        if (s->screens.is_alive(i)) { free_nodes(*s, s->screens.items[i]); s->screens.items[i].nodes.free(); }
    if (s->holding) engine_hold(e, HoldUi, false);
    ui_docs_shutdown(e, *s);
    kv_handle_kind_unregister(s->screen_kind);
    s->screens.free();
    s->model.free();
    s->quads.free();
    s->rows.free();
    s->covers.free();
    if (s->text_on) platform_text_input(e, false);
}

// /ui: the open screens and their nodes.
void ui_list(CliContext& ctx, const char* dir, CliEmitFn emit_, void* user) {
    UiState_* s = ui_state(*ctx.engine);
    if (!s || dir[0]) return;
    for (u32 i = 0; i < s->screens.len; ++i) {
        if (!s->screens.is_alive(i)) continue;
        char summary[96];
        screen_describe(s->screens.handle_at(i).bits, s, summary, sizeof summary);
        if (!emit_(user, s->screens.items[i].name, summary)) return;
    }
}

bool ui_describe(CliContext& ctx, const char* rel) {
    UiState_* s = ui_state(*ctx.engine);
    for (u32 i = 0; s && i < s->screens.len; ++i) {
        if (!s->screens.is_alive(i) || strcmp(s->screens.items[i].name, rel) != 0) continue;
        const UiScreen& sc = s->screens.items[i];
        for (u32 k = 0; k < sc.nodes.len; ++k) {
            const UiNode& n = sc.nodes[k];
            u32 depth = 0;
            for (u32 p = n.parent; p != KV_UI_NONE; p = sc.nodes[p].parent) ++depth;
            ctx.print("%*s%s %s  %.0f,%.0f %.0fx%.0f%s%s%.40s", (int)depth * 2, "", ui_type_name(n.type), n.name.is_null() ? "-" : name_str(n.name),
                      n.x, n.y, n.w, n.h, n.visible ? "" : " hidden", n.text && n.text[0] ? "  " : "", n.text ? n.text : "");
        }
        return true;
    }
    return false;
}

void cmd_reload(CliContext& ctx, Span<const char* const>) {
    u32 n = ui_reload(*ctx.engine);
    ctx.print("%u documents reloading", n);
}

constinit const CliMountDesc k_mounts[] = {{.path = "/ui", .list = ui_list, .describe = ui_describe}};
constinit const CliCommandDesc k_commands[] = {{.name = "ui reload", .help = "read every UI document again and rebuild its open screens", .fn = cmd_reload}};

constinit const FieldDesc k_event_fields[] = {KV_FIELDS(UiEvent, screen, node, event, value, pad)};
constinit const FieldDesc k_covered_fields[] = {KV_FIELDS(UiCovered, cover, pad, user)};
constinit const FieldDesc k_uncovered_fields[] = {KV_FIELDS(UiUncovered, cover, pad, user)};
constinit const EventTypeDesc k_events[] = {
    KV_EVENT_EX(UiEvent, k_event_fields, 0), KV_EVENT_EX(UiCovered, k_covered_fields, 0), KV_EVENT_EX(UiUncovered, k_uncovered_fields, 0),
};
constinit const KindDesc k_kinds[] = {{.name = ".sprite", .components = "Transform2, Sprite", .capacity = 16384, .cosmetic = true}};
constinit const SystemDesc k_systems[] = {
    {.name = ".update", .phase = Phase::PreSim, .fn = ui_update, .reads = "input.actions, input.state", .writes = "audio.commands",
     .after = "?asset.pump_scopes, ?asset.pump, ?asset.pump_blobs, ?asset.pump_sounds", .main_thread = true, .runs = RunWhen::Always},
    {.name = ".draw", .phase = Phase::PostSim, .fn = ui_draw, .writes = ".sprite", .after = "?anim.play", .before = "?scene.compose", .main_thread = true, .runs = RunWhen::Always},
};

} // namespace

KV_MODULE_DEFINE(ui_module) = {
    .name     = "ui",
    .events   = k_events,
    .kinds    = k_kinds,
    .systems  = k_systems,
    .commands = k_commands,
    .mounts   = k_mounts,
    .startup  = ui_startup,
    .shutdown = ui_shutdown,
    .memory   = {.reserve = 64 * 1024},
};

// ---- the API -------------------------------------------------------------------------------------

namespace {

Handle open_screen(Engine& e, Handle doc, const char* name, const UiOpen& open, bool code) {
    UiState_* s = ui_state(e);
    if (!s) return Handle{};
    UiScreen sc{};
    utf8_copy(sc.name, sizeof sc.name, name ? name : "");
    sc.doc = doc;
    sc.open = open;
    sc.code = code;
    sc.built = code;
    sc.order = s->next_order++;
    sc.nodes.alloc = s->general;
    sc.motion = open.enter;
    sc.motion_t = open.enter.fx == UiFx::Cut ? 1.0f : 0.0f;
    Handle h = s->screens.create(sc);
    if (!h.is_null() && !doc.is_null()) asset_ref(e, doc); // the screen holds its document
    // A document that is here builds the screen now, so the caller can set its nodes at
    // once; one still loading builds it at the first draw after it is Ready.
    UiDoc* d = s->docs.get(doc);
    UiScreen* rec = s->screens.get(h);
    if (rec && !code && d && d->state == AssetState::Ready) rec->built = ui_build_from_doc(e, *s, *rec) || (rec->code = true);
    return h;
}

} // namespace

Handle ui_open(Engine& e, Handle doc, const char* screen, const UiOpen& open) { return open_screen(e, doc, screen, open, false); }
Handle ui_open_empty(Engine& e, Handle doc, const char* name, const UiOpen& open) { return open_screen(e, doc, name, open, true); }

u32 ui_add(Engine& e, Handle screen, u32 parent, const UiNodeDesc& desc) {
    UiState_* s = ui_state(e);
    UiScreen* sc = s ? s->screens.get(screen) : nullptr;
    if (!sc) return KV_UI_NONE;
    return ui_node_add(*s, *sc, parent, desc);
}

bool ui_close(Engine& e, Handle screen) {
    UiState_* s = ui_state(e);
    UiScreen* sc = s ? s->screens.get(screen) : nullptr;
    if (!sc) return false;
    free_nodes(*s, *sc);
    sc->nodes.free();
    asset_unref(e, sc->doc);
    s->screens.destroy(screen);
    return true;
}

bool ui_close_with(Engine& e, Handle screen, const UiMotion& leave) {
    UiState_* s = ui_state(e);
    UiScreen* sc = s ? s->screens.get(screen) : nullptr;
    if (!sc) return false;
    if (leave.fx == UiFx::Cut || leave.seconds <= 0.0f) return ui_close(e, screen);
    sc->motion = leave;
    sc->motion_t = 0.0f;
    sc->leaving = true;
    sc->open.hold = false; // the sim goes on while it leaves
    return true;
}

u32 ui_cover(Engine& e, const UiCover& cover) {
    UiState_* s = ui_state(e);
    if (!s) return 0;
    UiCoverState c;
    c.id = s->next_cover++;
    c.desc = cover;
    s->covers.push(c);
    return c.id;
}

void ui_cover_wait(Engine& e, u32 cover, bool wait) {
    UiState_* s = ui_state(e);
    for (u32 i = 0; s && i < s->covers.len; ++i) if (s->covers[i].id == cover) s->covers[i].wait = wait;
}

bool ui_covering(Engine& e) {
    UiState_* s = ui_state(e);
    return s && s->covers.len;
}

const char* ui_field_text(Engine& e, Handle screen, const char* path) {
    UiNode* n = node_of(e, screen, path);
    return n && n->text ? n->text : "";
}

bool ui_is_open(Engine& e, Handle screen) {
    UiState_* s = ui_state(e);
    return s && s->screens.valid(screen);
}

Handle ui_top(Engine& e) {
    UiState_* s = ui_state(e);
    Handle top;
    i32 layer = -0x7FFFFFFF;
    u32 order = 0;
    for (u32 i = 0; s && i < s->screens.len; ++i) {
        if (!s->screens.is_alive(i)) continue;
        const UiScreen& sc = s->screens.items[i];
        if (sc.open.input && (sc.open.layer > layer || (sc.open.layer == layer && sc.order > order))) { top = s->screens.handle_at(i); layer = sc.open.layer; order = sc.order; }
    }
    return top;
}

u32 ui_find(Engine& e, Handle screen, const char* path) {
    UiState_* s = ui_state(e);
    UiScreen* sc = s ? s->screens.get(screen) : nullptr;
    return sc ? find(*sc, path) : KV_UI_NONE;
}

bool ui_set_text(Engine& e, Handle screen, const char* path, const char* text) {
    UiNode* n = node_of(e, screen, path);
    if (!n) return false;
    if (n->text && text && !strcmp(n->text, text)) return true;
    ui_node_set_text(*ui_state(e), *n, text);
    if (n->reveal_rate > 0.0f) { n->reveal_t = 0.0f; n->reveal_shown = 0; n->revealed = false; }
    return true;
}

bool ui_set_visible(Engine& e, Handle screen, const char* path, bool visible) {
    UiNode* n = node_of(e, screen, path);
    if (n) n->visible = visible;
    return n != nullptr;
}

bool ui_set_offset(Engine& e, Handle screen, const char* path, vec2 offset) {
    UiNode* n = node_of(e, screen, path);
    if (n) n->offset = offset;
    return n != nullptr;
}

bool ui_set_color(Engine& e, Handle screen, const char* path, const char* c) {
    UiNode* n = node_of(e, screen, path);
    if (n) n->color = c && c[0] ? name_intern(c) : Name{};
    return n != nullptr;
}

bool ui_set_image(Engine& e, Handle screen, const char* path, const char* style_image, u32 frame) {
    UiNode* n = node_of(e, screen, path);
    if (!n) return false;
    n->image = style_image && style_image[0] ? name_intern(style_image) : Name{};
    n->frame_index = frame;
    asset_unref(e, n->texture);
    asset_unref(e, n->material);
    n->texture = n->material = Handle{};
    return true;
}

bool ui_set_texture(Engine& e, Handle screen, const char* path, const char* texture, rect2 px) {
    UiNode* n = node_of(e, screen, path);
    if (!n) return false;
    Handle t = asset_load_texture(e, texture, Handle{});
    asset_ref(e, t);
    Handle m = asset_material_for_texture(e, t, BlendMode::Alpha, TextureFilter::Nearest, Handle{});
    asset_ref(e, m);
    asset_unref(e, n->texture);
    asset_unref(e, n->material);
    n->texture = t;
    n->material = m;
    n->tex_rect = px;
    return true;
}

bool ui_set_value(Engine& e, Handle screen, const char* path, f32 value, f32 max) {
    UiNode* n = node_of(e, screen, path);
    if (!n) return false;
    snprintf(n->value, sizeof n->value, "%g", value);
    snprintf(n->max, sizeof n->max, "%g", max);
    return true;
}

bool ui_focus(Engine& e, Handle screen, const char* list, const char* item) {
    UiScreen* sc = nullptr;
    UiNode* l = node_of(e, screen, list, &sc);
    if (!l || l->type != UiType::List) return false;
    Name want = name_lookup(item ? item : "");
    u32 idx = 0;
    for (u32 k = l->first; k != KV_UI_NONE; k = sc->nodes[k].next, ++idx)
        if (sc->nodes[k].name == want) { l->focus = idx; return true; }
    return false;
}

const char* ui_focused(Engine& e, Handle screen, const char* list) {
    UiScreen* sc = nullptr;
    UiNode* l = node_of(e, screen, list, &sc);
    if (!l || l->type != UiType::List) return nullptr;
    u32 i = l->first;
    for (u32 k = 0; k < l->focus && i != KV_UI_NONE; ++k) i = sc->nodes[i].next;
    return i == KV_UI_NONE ? nullptr : name_str(sc->nodes[i].name);
}

bool ui_scroll(Engine& e, Handle screen, const char* path, f32 units) {
    UiNode* n = node_of(e, screen, path);
    if (n) n->scroll += units;
    return n != nullptr;
}

bool ui_revealing(Engine& e, Handle screen, const char* path) {
    UiNode* n = node_of(e, screen, path);
    return n && !n->revealed;
}

bool ui_reveal_all(Engine& e, Handle screen, const char* path) {
    UiNode* n = node_of(e, screen, path);
    if (!n) return false;
    n->reveal_t = 1e6f;
    return true;
}

rect2 ui_rect(Engine& e, Handle screen, const char* path) {
    UiNode* n = node_of(e, screen, path);
    return n ? rect2{n->x, n->y, n->w, n->h} : rect2{};
}

void ui_model_set_int(Engine& e, const char* key, i64 v) {
    if (UiState_* s = ui_state(e)) s->model.set(name_intern(key).hash, UiModelValue{0, v, 0.0, Name{}});
}
void ui_model_set_float(Engine& e, const char* key, f64 v) {
    if (UiState_* s = ui_state(e)) s->model.set(name_intern(key).hash, UiModelValue{1, 0, v, Name{}});
}
void ui_model_set_str(Engine& e, const char* key, const char* v) {
    if (UiState_* s = ui_state(e)) s->model.set(name_intern(key).hash, UiModelValue{2, 0, 0.0, name_intern(v ? v : "")});
}

const UiStats* ui_stats(Engine& e) {
    UiState_* s = ui_state(e);
    return s ? &s->stats : nullptr;
}

} // namespace kv
