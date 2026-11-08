#include "korvid/text/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/hash.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/profile.h"
#include "korvid/math/fields.h"
#include "korvid/scene/parent.h"
#include "korvid/sprite/sprite.h"
#include "korvid/sprite/transform2.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace kv {

int text_set(Text& t, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(t.str, sizeof t.str, fmt, args);
    va_end(args);
    return n;
}

u32 text_layout(const FontAsset& font, const Text& text, Span<GlyphPlacement> out, f32* width) {
    if (font.state != AssetState::Ready) { if (width) *width = 0.0f; return 0; }
    f32 advance = font_measure(font, text.str, 1.0f);
    f32 x = text.align == TextAlign::Center ? -advance * 0.5f : text.align == TextAlign::Right ? -advance : 0.0f;
    u32 n = 0;
    for (const u8* p = (const u8*)text.str; *p; ++p) {
        const Glyph* g = font_glyph(font, *p);
        if (!g) continue;
        if (g->size.x > 0.0f && g->size.y > 0.0f) {
            if (n < out.len) {
                GlyphPlacement& gp = out[n];
                gp.center = vec2{(x + g->offset.x) * text.scale, g->offset.y * text.scale};
                gp.size   = g->size * text.scale;
                gp.uv     = g->uv;
            }
            ++n;
        }
        x += g->advance;
        if (p[1]) x += font_kern(font, *p, p[1]);
    }
    if (width) *width = advance * text.scale;
    return n;
}

namespace {

// What a label's glyphs were last built from, by table and slot: the entity and a hash
// of everything that shapes the glyphs. A label whose hash changed is rebuilt.
struct LabelMark { Handle entity; u64 built; };
struct LabelTable { Array<LabelMark> marks; };

struct TextState {
    Array<LabelTable> tables; // by engine table index
    TextStats         stats;
    u32               glyph_kind = 0;
};

u64 label_hash(const Text& t, const FontAsset* f) {
    u64 h = hash_bytes(t.str, strlen(t.str));
    h = hash_mix(h ^ t.font.bits);
    h = hash_mix(h ^ (u64)(f ? (u32)f->state : 0xFF));
    u32 bits[9];
    memcpy(&bits[0], &t.scale, 4);
    memcpy(&bits[1], &t.tint, 16);
    bits[5] = (u32)t.layer; bits[6] = (u32)t.space; bits[7] = (u32)t.align; bits[8] = 0;
    return hash_mix(h ^ hash_bytes(bits, sizeof bits));
}

// Phase PostSim, every tick, before scene.compose: labels whose text or look changed (or
// whose font arrived) lose their glyphs and get new ones as children.
void text_layout_system(TickContext& ctx) {
    KV_ZONE("text.layout");
    TextState& st = ctx.state_as<TextState>();
    Engine& e = *ctx.engine;
    Table* glyphs = ctx.write_kind(st.glyph_kind);
    TextStats s;

    Allocator* alloc = st.tables.alloc;
    while (st.tables.len < e.tables.len) { LabelTable lt; lt.marks.alloc = alloc; st.tables.push(lt); }

    MemScope scope;
    Array<GlyphPlacement> places = array_in<GlyphPlacement>(mem_scratch());
    for (KindRows k : ctx.view<const Text>()) {
        if (k.table == glyphs) continue;
        LabelTable& lt = st.tables[k.table->index];
        lt.marks.resize_fill(k.rows(), LabelMark{});
        Span<const Text> texts = k.col<const Text>();
        for (u32 i = 0; i < k.rows(); ++i) {
            LabelMark& mark = lt.marks[i];
            if (!k.alive(i)) { mark = LabelMark{}; continue; } // its glyphs are orphans: the scene module removes them
            ++s.labels;
            Handle label = k.handle_at(i);
            const Text& t = texts[i];
            const FontAsset* font = asset_font(e, t.font);
            u64 h = label_hash(t, font);
            if (mark.entity == label && mark.built == h) continue;
            mark.entity = label;
            mark.built  = h;
            ++s.relaid;

            // Out with the old: every glyph whose parent is this label.
            Span<const Parent> parents = glyphs->rows_of<Parent>();
            for (u32 g = 0; g < glyphs->rows(); ++g)
                if (glyphs->is_alive(g) && parents[g].parent == label) glyphs->destroy(glyphs->handle_at(g));
            if (!font || font->state != AssetState::Ready) continue;

            places.resize_fill(KV_TEXT_MAX, GlyphPlacement{});
            u32 n = text_layout(*font, t, places.span());
            for (u32 g = 0; g < n; ++g) {
                const GlyphPlacement& p = places[g];
                Sprite spr;
                spr.material = font->material;
                spr.uv       = p.uv;
                spr.size     = p.size;
                spr.tint     = t.tint;
                spr.layer    = t.layer;
                spr.space    = t.space;
                Handle gh = glyphs->spawn(Transform2{}, spr, local2_at(point2{p.center.x, p.center.y}), Parent{label});
                if (gh.is_null()) { KV_WARN("text", "glyph kind is full (%u rows)", glyphs->rows()); break; }
                ++s.glyphs;
            }
        }
    }
    st.stats = s;
}

bool text_startup(Engine& e, ModuleInstance& self) {
    TextState* s = arena_push<TextState>(self.arena);
    if (!s) return false;
    s->tables.alloc = &self.general;
    Table* glyphs = engine_table(e, "text.glyph");
    if (!glyphs) return false;
    s->glyph_kind = glyphs->index + 1;
    self.set_state(s);
    return true;
}

void text_shutdown(Engine&, ModuleInstance& self) {
    TextState* s = self.state_as<TextState>();
    if (!s) return;
    for (LabelTable& lt : s->tables) lt.marks.free();
    s->tables.free();
}

constinit const FieldDesc k_text_fields[] = {KV_FIELDS(Text, font, str, scale, tint, layer, space, align)};
constinit const ComponentDesc k_components[] = {KV_COMPONENT(Text, k_text_fields)};

constinit const KindDesc k_kinds[] = {
    {.name = ".label", .components = "Transform2, Text", .capacity = 4096},
    {.name = ".glyph", .components = "Transform2, Sprite, Local2, Parent", .capacity = 65536, .cosmetic = true},
};

constinit const SystemDesc k_systems[] = {
    {.name = ".layout", .phase = Phase::PostSim, .fn = text_layout_system, .reads = "*.Text",
     .writes = ".glyph", .runs = RunWhen::Always},
};

} // namespace

KV_MODULE_DEFINE(text_module) = {
    .name       = "text",
    .components = k_components,
    .kinds      = k_kinds,
    .systems    = k_systems,
    .startup    = text_startup,
    .shutdown   = text_shutdown,
    .memory     = {.reserve = 64 * 1024},
};

const TextStats* text_stats(Engine& e) {
    TextState* s = engine_module_state_as<TextState>(e, "text");
    return s ? &s->stats : nullptr;
}

} // namespace kv
