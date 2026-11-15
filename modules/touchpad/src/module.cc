// The touchpad module: each frame (a platform pump hook, after platform_hid's), the
// window's touches through touchpad_update into a gamepad slot of the input state; each
// tick (PostSim) the controls drawn as screen-space sprites on the top layer. The layout is
// read through the mounts as a blob and parsed when it arrives; until then, and if it does
// not parse, the built-in one.
#include "korvid/touchpad/module.h"
#include "korvid/asset/blob.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/time.h"
#include "korvid/platform/module.h"
#include "korvid/platform_hid/events.h"
#include "korvid/platform_hid/host_window.h"
#include "korvid/platform_hid/module.h"
#include "korvid/render/renderer.h"
#include "korvid/sprite/sprite.h"
#include "korvid/sprite/transform2.h"
#include "korvid/math/scalar.h"
#include <string.h>

namespace kv {

namespace {

constexpr u32 kPadId    = 0x70AD0001u; // the virtual pad's instance id, for tools
constexpr u32 kMouseId  = 1;           // the mouse's finger
constexpr u32 kTexture  = 128;         // the ring's and the disc's size, pixels

struct TouchpadModule {
    TouchpadConfig config;
    TouchpadState  state;
    TouchpadScreen screen;
    bool   enabled = false;
    bool   mouse   = false;    // the left button is a finger
    bool   mouse_was_down = false;
    i32    slot = -1;          // the pad slot it fills
    Handle layout;             // the blob being read, until it is parsed
    char   layout_path[128] = "touchpad/default.json";
    bool   layout_wanted = true;
    Handle ring_texture, disc_texture, ring_material, disc_material;
    u32    sprite_kind = 0;
    Array<Handle> rows;
};

TouchpadModule* tp_state(Engine& e) { return engine_module_state_as<TouchpadModule>(e, "touchpad"); }

void make_textures(Engine& e, TouchpadModule& s) {
    if (!s.ring_material.is_null()) return;
    Renderer* r = renderer_get(e);
    if (!r) return;
    MemScope scope;
    u8* ring = (u8*)alloc_in(mem_scratch(), kTexture * kTexture * 4);
    u8* disc = (u8*)alloc_in(mem_scratch(), kTexture * kTexture * 4);
    for (u32 y = 0; y < kTexture; ++y)
        for (u32 x = 0; x < kTexture; ++x) {
            f32 dx = ((f32)x + 0.5f) / (f32)kTexture - 0.5f, dy = ((f32)y + 0.5f) / (f32)kTexture - 0.5f;
            f32 d = kv::sqrt(dx * dx + dy * dy) * (f32)kTexture; // pixels from the centre
            f32 outer = (f32)kTexture * 0.5f - 1.0f;
            f32 in_disc = d < outer - 0.5f ? 1.0f : d > outer + 0.5f ? 0.0f : outer + 0.5f - d;
            f32 inner = outer - (f32)kTexture * 0.06f; // the ring: 6% of the width, a pixel of edge both sides
            f32 in_ring = in_disc * (d > inner + 0.5f ? 1.0f : d < inner - 0.5f ? 0.0f : d - (inner - 0.5f));
            u8* p = ring + ((usize)y * kTexture + x) * 4;
            u8* q = disc + ((usize)y * kTexture + x) * 4;
            p[0] = p[1] = p[2] = q[0] = q[1] = q[2] = 255;
            p[3] = (u8)(in_ring * 255.0f + 0.5f);
            q[3] = (u8)(in_disc * 255.0f + 0.5f);
        }
    TextureDesc td;
    td.name = "touchpad.ring";
    s.ring_texture = renderer_texture_create(r, td);
    renderer_texture_upload(r, s.ring_texture, kTexture, kTexture, ring, (usize)kTexture * kTexture * 4);
    td.name = "touchpad.disc";
    s.disc_texture = renderer_texture_create(r, td);
    renderer_texture_upload(r, s.disc_texture, kTexture, kTexture, disc, (usize)kTexture * kTexture * 4);
    MaterialDesc md;
    md.filter = TextureFilter::Linear;
    md.name = "touchpad.ring";
    md.texture = s.ring_texture;
    s.ring_material = renderer_material_create(r, md);
    md.name = "touchpad.disc";
    md.texture = s.disc_texture;
    s.disc_material = renderer_material_create(r, md);
}

// The layout: asked for once enabled, parsed when it arrives.
void pump_layout(Engine& e, TouchpadModule& s) {
    if (s.layout_wanted) {
        s.layout_wanted = false;
        s.layout = asset_load_blob(e, s.layout_path);
    }
    if (s.layout.is_null()) return;
    const BlobAsset* b = asset_blob(e, s.layout);
    if (!b || b->state == AssetState::Pending) return;
    if (b->state == AssetState::Ready) {
        TouchpadConfig c = touchpad_default();
        const char* why = "";
        if (touchpad_parse((const char*)b->data, b->size, c, &why)) {
            s.config = c;
            KV_INFO("touchpad", "layout '%s': %u buttons, stick %s", s.layout_path, c.button_count, c.drag ? "drag" : "clamp");
        } else {
            KV_WARN("touchpad", "layout '%s': %s; the built-in one stands", s.layout_path, why);
        }
    } else {
        KV_WARN("touchpad", "layout '%s' could not be read; the built-in one stands", s.layout_path);
    }
    s.layout = Handle{};
}

// From platform_pump, after platform_hid's: this frame's touches into the pad.
void tp_pump(Engine& e, void* user) {
    TouchpadModule& s = *(TouchpadModule*)user;
    HostWindow* w = platform_window(e);
    Input* in = platform_input(e);
    if (!w || !w->is_open() || !in) return;
    if (!s.enabled && w->touch_screen()) {
        s.enabled = true;
        KV_INFO("touchpad", "on: the screen is touched");
    }
    if (!s.enabled) return;
    pump_layout(e, s);

    s.screen.width = (f32)w->width();
    s.screen.height = (f32)w->height();
    w->safe_insets(s.screen.insets);
    // Points: a desktop's window units, a phone's points; pixel_scale is both.
    s.screen.scale = w->pixel_scale();

    TouchPoint touches[KV_MAX_TOUCHES + 1];
    u32 n = 0;
    for (const TouchPoint& t : in->raw.touches) if (t.id) touches[n++] = t;
    if (s.mouse) {
        bool down = in->raw.mouse.down(MouseButton::Left);
        if (down || s.mouse_was_down) touches[n++] = TouchPoint{kMouseId, in->raw.mouse.x, in->raw.mouse.y, !down};
        s.mouse_was_down = down;
    }
    touchpad_update(s.config, s.state, touches, n, s.screen, time_now());

    // The pad: a slot of its own, connected while the touchpad is on.
    if (s.slot < 0) {
        for (i32 i = (i32)KV_MAX_GAMEPADS - 1; i >= 0 && s.slot < 0; --i)
            if (!in->raw.pads[i].connected) s.slot = i;
        if (s.slot < 0) { KV_WARN("touchpad", "every pad slot is taken; the touchpad is off"); s.enabled = false; return; }
        KV_INFO("touchpad", "pad slot %d", s.slot);
    }
    GamepadState& pad = in->raw.pads[s.slot];
    pad.connected = true;
    pad.id = kPadId;
    // The edges as a real pad's are: PadButtonDown and PadButtonUp on the bus.
    u32 changed = pad.buttons ^ s.state.pad_buttons;
    for (u32 b = 0; changed && b < (u32)PadButton::Count; ++b) {
        if (!((changed >> b) & 1u)) continue;
        if ((s.state.pad_buttons >> b) & 1u) e.events.emit(PadButtonDown{(PadButton)b, (u8)s.slot});
        else e.events.emit(PadButtonUp{(PadButton)b, (u8)s.slot});
    }
    pad.buttons = s.state.pad_buttons;
    pad.axes[(u32)PadAxis::LeftX] = s.state.axis.x;
    pad.axes[(u32)PadAxis::LeftY] = s.state.axis.y;
}

// Phase PostSim: the controls as sprites, rows reused in order.
void tp_draw(TickContext& ctx) {
    TouchpadModule& s = ctx.state_as<TouchpadModule>();
    if (!s.enabled) return;
    make_textures(*ctx.engine, s);
    if (s.ring_material.is_null()) return;
    struct Quad { Handle material; f32 x, y, d, alpha; };
    Quad quads[KV_TOUCHPAD_BUTTONS * 2 + 2];
    u32 n = 0;
    f32 a = s.config.opacity;
    for (u32 i = 0; i < s.config.button_count; ++i) {
        vec2 p = touchpad_button_center(s.config, i, s.screen);
        f32 d = touchpad_button_radius(s.config, i, s.screen) * 2.0f;
        bool down = (s.state.down >> i) & 1u;
        quads[n++] = Quad{s.disc_material, p.x, p.y, d, down ? a * 0.9f : a * 0.25f};
        quads[n++] = Quad{s.ring_material, p.x, p.y, d, down ? 1.0f : a};
    }
    if (s.state.stick_touch) {
        f32 r = s.config.radius * s.screen.scale;
        quads[n++] = Quad{s.ring_material, s.state.center.x, s.state.center.y, r * 2.0f, a};
        quads[n++] = Quad{s.disc_material, s.state.knob.x, s.state.knob.y, r * 0.9f, a * 1.6f > 1.0f ? 1.0f : a * 1.6f};
    }
    Table* rows = ctx.write_kind(s.sprite_kind);
    for (u32 i = 0; i < n; ++i) {
        const Quad& q = quads[i];
        Sprite sp;
        sp.material = q.material;
        sp.size = vec2{q.d, q.d};
        sp.tint = color{1.0f, 1.0f, 1.0f, q.alpha};
        sp.layer = 127;
        sp.space = SpriteSpace::Screen;
        sp.anchor = ScreenAnchor::TopLeft;
        sp.sort = SpriteSort::Key;
        sp.order = 1.0e6f + (f32)i; // over the UI's, which counts from 0 on the same layer
        Transform2 t = transform2_at(point2{q.x, -q.y});
        if (i < s.rows.len && rows->valid(s.rows[i])) {
            *rows->get<Sprite>(s.rows[i]) = sp;
            *rows->get<Transform2>(s.rows[i]) = t;
            *rows->prev<Transform2>(s.rows[i]) = t; // controls do not interpolate
        } else {
            Handle h = rows->spawn(t, sp);
            if (i < s.rows.len) s.rows[i] = h; else s.rows.push(h);
        }
    }
    for (u32 i = n; i < s.rows.len; ++i) rows->destroy(s.rows[i]);
    if (s.rows.len > n) s.rows.len = n;
}

bool tp_startup(Engine& e, ModuleInstance& self) {
    TouchpadModule* s = arena_push<TouchpadModule>(self.arena);
    if (!s) return false;
    s->config = touchpad_default();
    s->rows.alloc = &self.general;
    s->sprite_kind = engine_kind_id(e, "touchpad.sprite");
    self.set_state(s);
    return platform_pump_add(e, tp_pump, s);
}

void tp_shutdown(Engine& e, ModuleInstance& self) {
    TouchpadModule* s = self.state_as<TouchpadModule>();
    if (!s) return;
    platform_pump_remove(e, tp_pump, s);
    if (Renderer* r = renderer_get(e)) {
        if (!s->ring_material.is_null()) renderer_material_destroy(r, s->ring_material);
        if (!s->disc_material.is_null()) renderer_material_destroy(r, s->disc_material);
        if (!s->ring_texture.is_null()) renderer_texture_destroy(r, s->ring_texture);
        if (!s->disc_texture.is_null()) renderer_texture_destroy(r, s->disc_texture);
    }
    s->rows.free();
}

constinit const KindDesc k_kinds[] = {{.name = ".sprite", .components = "Transform2, Sprite", .capacity = 64, .cosmetic = true}};
constinit const SystemDesc k_systems[] = {
    {.name = ".draw", .phase = Phase::Frame, .fn = tp_draw, .writes = ".sprite", .after = "?ui.draw", .main_thread = true},
};

} // namespace

KV_MODULE_DEFINE(touchpad_module) = {
    .name     = "touchpad",
    .kinds    = k_kinds,
    .systems  = k_systems,
    .startup  = tp_startup,
    .shutdown = tp_shutdown,
    .memory   = {.reserve = 16 * 1024},
};

void touchpad_enable(Engine& e, bool mouse) {
    TouchpadModule* s = tp_state(e);
    if (!s) return;
    if (!s->enabled) KV_INFO("touchpad", "on%s", mouse ? ": the left mouse button is a finger" : "");
    s->enabled = true;
    s->mouse = mouse;
}

bool touchpad_enabled(Engine& e) {
    TouchpadModule* s = tp_state(e);
    return s && s->enabled;
}

void touchpad_layout(Engine& e, const char* path) {
    TouchpadModule* s = tp_state(e);
    if (!s || !path) return;
    strncpy(s->layout_path, path, sizeof s->layout_path - 1);
    s->layout_wanted = true;
}

const TouchpadState* touchpad_state(Engine& e) {
    TouchpadModule* s = tp_state(e);
    return s ? &s->state : nullptr;
}

} // namespace kv
