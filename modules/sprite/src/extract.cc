#include "korvid/sprite/extract.h"
#include "korvid/core/profile.h"
#include "korvid/sprite/components.h"
#include "korvid/sprite/world.h"
#include "korvid/core/engine.h"

namespace kv {

void ExtractState::free() {
    for (ExtractTable& t : tables) { t.sent.free(); t.sent_sprites.free(); t.sent_placements.free(); }
    tables.free();
}

void extract_sprites(Engine& e, ExtractState& st, SpriteLayers* layers, Array<SpriteDelta>& out) {
    SpriteDelta tick{};
    tick.kind = SpriteDeltaKind::Tick;
    out.push(tick);

    // The layers that moved (or snapped) since the renderer was last told.
    for (u32 i = 0; layers && i < 256; ++i) {
        if (layers->offset[i] == st.sent_layers[i] && !layers->snap[i]) continue;
        SpriteDelta d{};
        d.kind = SpriteDeltaKind::Layer;
        d.state.layer = (i32)i + KV_SPRITE_LAYER_MIN;
        d.state.placement.position = point2{layers->offset[i].x, layers->offset[i].y};
        d.state.snap = layers->snap[i] != 0;
        out.push(d);
        st.sent_layers[i] = layers->offset[i];
        layers->snap[i] = 0;
    }

    if (st.rows_hint && out.cap < st.rows_hint * 2) { // a frame may hold two ticks' deltas
        KV_ZONE("sprite.extract.out");
        out.reserve(st.rows_hint * 2);
    }

    Allocator* alloc = st.tables.alloc;
    while (st.tables.len < e.tables.len) {
        ExtractTable et;
        et.sent.alloc = et.sent_sprites.alloc = et.sent_placements.alloc = alloc;
        st.tables.push(et);
    }

    for (Table& table : e.tables) {
        if (!table.has(ComponentType<Transform2>::id) || !table.has(ComponentType<Sprite>::id)) continue;
        ExtractTable& et = st.tables[table.index];
        // A still kind (a map's tiles): nothing to look at unless rows came or went.
        bool still = table.desc && table.desc->still;
        if (still && table.churn == et.seen_churn) continue;
        et.seen_churn = table.churn;
        if (table.restores != et.seen_restores) { still = false; et.seen_restores = table.restores; } // values may have changed under known handles
        u32 n = table.rows();
        if (n > et.sent.len) {
            KV_ZONE("sprite.extract.grow");
            if (st.rows_hint && et.sent.cap == 0) { // once, to the size the game said
                u32 want = table.capacity < st.rows_hint ? table.capacity : st.rows_hint;
                if (want > n) { et.sent.reserve(want); et.sent_sprites.reserve(want); et.sent_placements.reserve(want); }
            }
            et.sent.resize_fill(n, Handle{});
            et.sent_sprites.resize_fill(n, Sprite{});
            et.sent_placements.resize_fill(n, Transform2{});
        }
        if (out.len + n > out.cap) { // at most a delta a row: grown once here, not push by push
            KV_ZONE("sprite.extract.out");
            out.reserve(out.len + n);
        }
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
            if (still && !is_new) continue; // it has not changed: it cannot
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
