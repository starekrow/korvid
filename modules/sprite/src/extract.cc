#include "korvid/sprite/extract.h"
#include "korvid/sprite/components.h"
#include "korvid/sprite/world.h"
#include "korvid/core/engine.h"

namespace kv {

void ExtractState::free() {
    for (ExtractTable& t : tables) { t.sent.free(); t.sent_sprites.free(); t.sent_placements.free(); }
    tables.free();
}

void extract_sprites(Engine& e, ExtractState& st, Array<SpriteDelta>& out) {
    SpriteDelta tick{};
    tick.kind = SpriteDeltaKind::Tick;
    out.push(tick);

    Allocator* alloc = st.tables.alloc;
    while (st.tables.len < e.tables.len) {
        ExtractTable et;
        et.sent.alloc = et.sent_sprites.alloc = et.sent_placements.alloc = alloc;
        st.tables.push(et);
    }

    for (Table& table : e.tables) {
        if (!table.has(ComponentType<Transform2>::id) || !table.has(ComponentType<Sprite>::id)) continue;
        ExtractTable& et = st.tables[table.index];
        u32 n = table.rows();
        et.sent.resize_fill(n, Handle{});
        et.sent_sprites.resize_fill(n, Sprite{});
        et.sent_placements.resize_fill(n, Transform2{});
        Span<const Transform2> cur  = table.rows_of<Transform2>();
        Span<const Transform2> prev = table.prev_rows_of<Transform2>();
        Span<const Sprite>     spr  = table.rows_of<Sprite>();

        // Removes first: a slot the renderer knows whose entity died or was replaced.
        for (u32 i = 0; i < n; ++i) {
            Handle sent = et.sent[i];
            if (sent.is_null()) continue;
            if (table.is_alive(i) && table.handle_at(i) == sent) continue;
            SpriteDelta d{};
            d.kind   = SpriteDeltaKind::Remove;
            d.entity = sent;
            out.push(d);
            et.sent[i] = Handle{};
        }
        // Then Sets: new entities, and known ones that moved or changed. A move whose
        // previous copy already equals the new transform is a teleport: the system wrote
        // both, and the renderer must not interpolate across the gap.
        for (u32 i = 0; i < n; ++i) {
            if (!table.is_alive(i)) continue;
            bool is_new  = et.sent[i].is_null();
            bool moved   = !component_equal(transform2_schema(), &cur[i], &et.sent_placements[i]);
            bool changed = is_new || !component_equal(sprite_schema(), &spr[i], &et.sent_sprites[i]);
            if (!is_new && !moved && !changed) continue;
            bool snap = moved && !is_new && component_equal(transform2_schema(), &cur[i], &prev[i]);

            SpriteDelta d{};
            d.kind            = SpriteDeltaKind::Set;
            d.entity          = table.handle_at(i);
            d.state.placement = cur[i];
            d.state.material  = spr[i].material;
            d.state.uv        = spr[i].uv;
            d.state.size      = spr[i].size;
            d.state.tint      = spr[i].tint;
            d.state.layer     = spr[i].layer;
            d.state.space     = spr[i].space;
            d.state.anchor    = spr[i].anchor;
            d.state.sort      = spr[i].sort;
            d.state.order     = spr[i].order;
            d.state.snap      = snap;
            out.push(d);
            et.sent[i]            = d.entity;
            et.sent_sprites[i]    = spr[i];
            et.sent_placements[i] = cur[i];
        }
    }
}

} // namespace kv
