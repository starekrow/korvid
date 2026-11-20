#include "korvid/sprite_text/module.h"
#include "korvid/typeset/typeset.h"
#include "korvid/math/scalar.h"
#include "korvid/core/format.h"
#include "korvid/core/engine.h"
#include "korvid/core/hash.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/profile.h"
#include "korvid/core/utf8.h"
#include "korvid/math/fields.h"
#include "korvid/scene/parent.h"
#include "korvid/sprite/sprite.h"
#include "korvid/sprite/transform2.h"
#include <stdarg.h>
#include <string.h>

namespace kv {

int sptext_set(Text& t, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vformat(t.str, sizeof t.str, fmt, args);
    va_end(args);
    if (n >= (int)sizeof t.str) utf8_trim(t.str); // never end on half a character
    return n;
}

u32 sptext_layout(const FontAsset& font, const Text& text, Span<GlyphPlacement> out, f32* width) {
    if (font.state != AssetState::Ready) { if (width) *width = 0.0f; return 0; }
    f32 advance = font_measure(font, text.str, 1.0f);
    f32 x = text.align == TextAlign::Center ? -advance * 0.5f : text.align == TextAlign::Right ? -advance : 0.0f;
    u32 n = 0;
    const char* p = text.str;
    u32 prev = 0;
    while (u32 cp = utf8_next(p)) {
        if (prev) x += font_kern(font, prev, cp);
        prev = cp;
        const Glyph* g = font_glyph_or_replacement(font, cp);
        if (!g) continue;
        if (g->size.x > 0.0f && g->size.y > 0.0f) {
            if (n < out.len) {
                GlyphPlacement& gp = out[n];
                gp.center = vec2{(x + g->offset.x) * text.scale, g->offset.y * text.scale};
                gp.size   = g->size * text.scale;
                gp.uv     = g->uv;
                gp.material = g->material.is_null() ? font.material : g->material;
            }
            ++n;
        }
        x += g->advance;
    }
    if (width) *width = advance * text.scale;
    return n;
}

namespace {

TypesetParams params_of(const Text& t) {
    f32 scale = t.scale > 0.0f ? t.scale : 1.0f;
    TypesetParams p;
    p.width = t.wrap > 0.0f ? t.wrap / scale : 0.0f;
    p.align = t.align == TextAlign::Center ? TypesetAlign::Center : t.align == TextAlign::Right ? TypesetAlign::Right : TypesetAlign::Left;
    p.line_height = t.line_height > 0.0f ? t.line_height / scale : 0.0f;
    p.subpixel = t.subpixel != 0;
    return p;
}

// A label over several lines (a wrap, a `\n`, a paragraph): the pen on the first line's
// baseline, the lines going down, aligned as one line is (Left from the pen, Center and Right
// about it). Only the first `shown` glyphs.
} // namespace

u32 sptext_layout_paragraph(const FontAsset& font, const Text& t, const char* str, u32 shown, Array<GlyphPlacement>& out) {
    if (font.state != AssetState::Ready) return 0;
    f32 scale = t.scale > 0.0f ? t.scale : 1.0f;
    TypesetParams p = params_of(t);
    Typeset ts;
    ts.lines.alloc = ts.glyphs.alloc = mem_scratch();
    typeset_paragraph(font, str, (u32)strlen(str), p, ts);
    f32 frame = p.width > 0.0f ? p.width : ts.width;
    f32 x0 = t.align == TextAlign::Center ? -frame * 0.5f : t.align == TextAlign::Right ? -frame : 0.0f;
    f32 first = ts.lines.len ? ts.lines[0].baseline : 0.0f;
    u32 n = 0, count = 0;
    for (const TypesetGlyph& g : ts.glyphs) {
        if (count++ >= shown) break;
        const Glyph* gl = g.glyph;
        if (!gl || gl->size.x <= 0.0f || gl->size.y <= 0.0f) continue;
        f32 pen = p.subpixel ? floor(g.x) : g.x;
        GlyphPlacement gp;
        gp.center = vec2{(x0 + pen + gl->offset.x) * scale, (-(ts.lines[g.line].baseline - first) + gl->offset.y) * scale};
        gp.size = gl->size * scale;
        gp.uv = gl->uv;
        gp.material = gl->material.is_null() ? font.material : gl->material;
        out.push(gp);
        ++n;
    }
    return n;
}

namespace {

// What a label's glyphs were last built from, by ktable and slot: the entity and a hash
// of everything that shapes the glyphs. A label whose hash changed is rebuilt.
struct LabelMark { Handle entity; u64 built; };
struct LabelTable { Array<LabelMark> marks; };

struct SptextState {
    Array<LabelTable> tables; // by engine ktable index
    SptextStats         stats;
    u32               glyph_ktable = 0;
    Array<char*>      strings;  // paragraphs' strings, by Paragraph::string - 1; null: free
    Array<u32>        free_ids;
    Allocator*        alloc = nullptr;
};

u64 label_hash(const Text& t, const FontAsset* f, const char* str, u32 shown) {
    u64 h = hash_bytes(str, strlen(str));
    h = hash_mix(h ^ shown);
    u32 more[3];
    memcpy(&more[0], &t.wrap, 4);
    memcpy(&more[1], &t.line_height, 4);
    more[2] = t.subpixel;
    h = hash_mix(h ^ hash_bytes(more, sizeof more));
    h = hash_mix(h ^ t.font.bits);
    h = hash_mix(h ^ (u64)(f ? (u32)f->state : 0xFF));
    h = hash_mix(h ^ (u64)(f ? font_generation(*f) : 0)); // a fallback that landed draws what was missing
    u32 bits[9];
    memcpy(&bits[0], &t.scale, 4);
    memcpy(&bits[1], &t.tint, 16);
    bits[5] = (u32)t.layer; bits[6] = (u32)t.space; bits[7] = (u32)t.align; bits[8] = (u32)t.anchor;
    h = hash_mix(h ^ hash_bytes(bits, sizeof bits));
    return hash_mix(h ^ hash_bytes(&t.shadow, sizeof t.shadow) ^ hash_bytes(&t.shadow_offset, sizeof t.shadow_offset) * 31);
}

// Phase PostSim, every tick, before scene.compose: labels whose text or look changed (or
// whose font arrived) lose their glyphs and get new ones as children.
void sptext_layout_system(TickContext& ctx) {
    KV_ZONE("sprite_text.layout");
    SptextState& st = ctx.state_as<SptextState>();
    Engine& e = *ctx.engine;
    KTable* glyphs = ctx.write_ktable(st.glyph_ktable);
    SptextStats s;

    Allocator* alloc = st.tables.alloc;
    while (st.tables.len < e.ktables.len) { LabelTable lt; lt.marks.alloc = alloc; st.tables.push(lt); }

    MemScope scope;
    Array<GlyphPlacement> places = array_in<GlyphPlacement>(mem_scratch());
    for (KTableRows k : ctx.view<const Text>()) {
        if (k.ktable == glyphs) continue;
        LabelTable& lt = st.tables[k.ktable->index];
        lt.marks.resize_fill(k.rows(), LabelMark{});
        Span<const Text> texts = k.col<const Text>();
        Span<const Paragraph> paras = k.col<const Paragraph>();
        Span<const Transform2> where = k.col<const Transform2>(); // the labels', as the tick's compose left them
        for (u32 i = 0; i < k.rows(); ++i) {
            LabelMark& mark = lt.marks[i];
            if (!k.alive(i)) { mark = LabelMark{}; continue; } // its glyphs are orphans: the scene module removes them
            ++s.labels;
            Handle label = k.handle_at(i);
            const Text& t = texts[i];
            const FontAsset* font = asset_font(e, t.font);
            const char* str = t.str;
            u32 shown = ~0u;
            if (i < paras.len && paras[i].string && paras[i].string <= st.strings.len && st.strings[paras[i].string - 1]) {
                str = st.strings[paras[i].string - 1];
                shown = paras[i].shown;
            }
            u64 h = label_hash(t, font, str, shown);
            if (mark.entity == label && mark.built == h) continue;
            mark.entity = label;
            mark.built  = h;
            ++s.relaid;

            // Out with the old: every glyph whose parent is this label.
            Span<const Parent> parents = glyphs->rows_of<Parent>();
            for (u32 g = 0; g < glyphs->rows(); ++g)
                if (glyphs->is_alive(g) && parents[g].parent == label) glyphs->destroy(glyphs->handle_at(g));
            if (!font || font->state != AssetState::Ready) continue;

            u32 n = 0;
            if (str == t.str && t.wrap <= 0.0f && !t.subpixel && t.line_height <= 0.0f && !strchr(str, '\n')) {
                // One line: as it always was.
                places.resize_fill(KV_SPTEXT_MAX, GlyphPlacement{});
                n = sptext_layout(*font, t, places.span());
            } else {
                places.clear();
                n = sptext_layout_paragraph(*font, t, str, shown, places);
            }
            // With a shadow, every glyph twice: the shadow keyed under the glyph in the same
            // layer, so all of a layer's shadows draw before its glyphs.
            bool shadowed = t.shadow.a > 0.0f;
            for (u32 pass = shadowed ? 0 : 1; pass < 2; ++pass)
                for (u32 g = 0; g < n; ++g) {
                    const GlyphPlacement& p = places[g];
                    Sprite spr;
                    spr.material = p.material;
                    spr.uv       = p.uv;
                    spr.size     = p.size;
                    spr.tint     = pass ? t.tint : t.shadow;
                    spr.layer    = t.layer;
                    spr.space    = t.space;
                    spr.anchor   = t.anchor;
                    if (shadowed) { spr.sort = SpriteSort::Key; spr.order = (f32)pass; }
                    point2 at{p.center.x + (pass ? 0.0f : t.shadow_offset.x), p.center.y + (pass ? 0.0f : t.shadow_offset.y)};
                    // Placed now, as the scene would compose it: the frame that shows a new
                    // glyph shows it in place; the scene keeps it with its label from the
                    // next tick on.
                    Local2 local = local2_at(at);
                    Transform2 placed = i < where.len ? placement_compose(where[i], local) : Transform2{};
                    Handle gh = glyphs->spawn(placed, spr, local, Parent{label});
                    if (gh.is_null()) { KV_WARN("sprite_text", "glyph ktable is full (%u rows)", glyphs->rows()); break; }
                    ++s.glyphs;
                }
        }
    }
    st.stats = s;
}

bool sptext_startup(Engine& e, ModuleInstance& self) {
    SptextState* s = arena_push<SptextState>(self.arena);
    if (!s) return false;
    s->tables.alloc = &self.general;
    s->strings.alloc = s->free_ids.alloc = &self.general;
    s->alloc = &self.general;
    KTable* glyphs = engine_ktable(e, "sprite_text.glyph");
    if (!glyphs) return false;
    s->glyph_ktable = glyphs->index + 1;
    self.set_state(s);
    return true;
}

void sptext_shutdown(Engine&, ModuleInstance& self) {
    SptextState* s = self.state_as<SptextState>();
    if (!s) return;
    for (LabelTable& lt : s->tables) lt.marks.free();
    s->tables.free();
    for (char* str : s->strings) if (str) free_in(s->alloc, str, strlen(str) + 1);
    s->strings.free();
    s->free_ids.free();
}

constinit const FieldDesc k_text_fields[] = {KV_FIELDS(Text, font, str, scale, tint, layer, space, anchor, align, shadow, shadow_offset, wrap, line_height, subpixel)};
constinit const FieldDesc k_paragraph_fields[] = {KV_FIELDS(Paragraph, string, shown)};
constinit const ComponentDesc k_components[] = {KV_COMPONENT(Text, k_text_fields), KV_COMPONENT(Paragraph, k_paragraph_fields)};

constinit const KTableDesc k_ktables[] = {
    {.name = ".label", .components = "Transform2, Text, Paragraph", .capacity = 4096},
    {.name = ".glyph", .components = "Transform2, Sprite, Local2, Parent", .capacity = 65536, .cosmetic = true},
};

constinit const SystemDesc k_systems[] = {
    // The Frame cycle (docs/design/cycles.md): glyphs are presentation.
    {.name = ".layout", .phase = Phase::Frame, .fn = sptext_layout_system, .reads = "*.Text, *.Paragraph, *.Transform2", .writes = ".glyph"},
};

} // namespace

KV_MODULE_DEFINE(sprite_text_module) = {
    .name       = "sprite_text",
    .components = k_components,
    .ktables    = k_ktables,
    .systems    = k_systems,
    .startup    = sptext_startup,
    .shutdown   = sptext_shutdown,
    .memory     = {.reserve = 64 * 1024},
};

namespace {
SptextState* sptext_state(Engine& e) { return engine_module_state_as<SptextState>(e, "sprite_text"); }
}

void sptext_paragraph_set(Engine& e, Paragraph& p, const char* utf8) {
    SptextState* s = sptext_state(e);
    if (!s || !utf8) return;
    if (!p.string || p.string > s->strings.len) {
        if (s->free_ids.len) p.string = s->free_ids.pop();
        else { s->strings.push(nullptr); p.string = s->strings.len; }
    }
    char*& slot = s->strings[p.string - 1];
    if (slot) free_in(s->alloc, slot, strlen(slot) + 1);
    usize n = strlen(utf8) + 1;
    slot = (char*)alloc_in(s->alloc, n, 1);
    memcpy(slot, utf8, n);
}

const char* sptext_paragraph_get(Engine& e, const Paragraph& p) {
    SptextState* s = sptext_state(e);
    if (!s || !p.string || p.string > s->strings.len) return "";
    const char* str = s->strings[p.string - 1];
    return str ? str : "";
}

void sptext_paragraph_free(Engine& e, Paragraph& p) {
    SptextState* s = sptext_state(e);
    if (!s || !p.string || p.string > s->strings.len) return;
    char*& slot = s->strings[p.string - 1];
    if (slot) { free_in(s->alloc, slot, strlen(slot) + 1); slot = nullptr; s->free_ids.push(p.string); }
    p.string = 0;
}

u32 sptext_paragraph_length(Engine& e, const Paragraph& p) { return (u32)utf8_count(sptext_paragraph_get(e, p)); }

u32 sptext_paragraph_fit(Engine& e, const Text& t, const Paragraph& p, f32 height) {
    const FontAsset* font = asset_font(e, t.font);
    if (!font || font->state != AssetState::Ready) return 0;
    const char* str = sptext_paragraph_get(e, p);
    f32 scale = t.scale > 0.0f ? t.scale : 1.0f;
    MemScope scope;
    Typeset ts;
    ts.lines.alloc = ts.glyphs.alloc = mem_scratch();
    typeset_paragraph(*font, str, (u32)strlen(str), params_of(t), ts);
    u32 end = typeset_fit(ts, height / scale);
    u32 count = 0;
    for (const TypesetGlyph& g : ts.glyphs) if (g.byte < end) ++count;
    return count;
}

const SptextStats* sptext_stats(Engine& e) {
    SptextState* s = engine_module_state_as<SptextState>(e, "sprite_text");
    return s ? &s->stats : nullptr;
}

} // namespace kv
