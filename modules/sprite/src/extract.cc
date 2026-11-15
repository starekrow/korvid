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

namespace {

bool is_sprite_table(const Table& t) { return t.has(ComponentType<Transform2>::id) && t.has(ComponentType<Sprite>::id); }

// More than one tick since the last extract: every sprite the renderer knows whose previous
// copy (where it was when the last tick began) is not what the renderer was told goes
// there first, at once, so the last tick's motion is all that is interpolated.
void catch_up(Engine& e, ExtractState& st, Array<SpriteDelta>& out) {
    for (Table& table : e.tables) {
        if (!is_sprite_table(table) || table.index >= st.tables.len) continue;
        if (table.desc && table.desc->still) continue; // its rows do not move
        ExtractTable& et = st.tables[table.index];
        Span<const Transform2> prev = table.prev_rows_of<Transform2>();
        u32 n = et.sent.len < table.rows() ? et.sent.len : table.rows();
        for (u32 i = 0; i < n; ++i) {
            if (et.sent[i].is_null() || !table.is_alive(i) || table.handle_at(i) != et.sent[i]) continue;
            if (component_equal(transform2_schema(), &prev[i], &et.sent_placements[i])) continue;
            const Sprite& look = et.sent_sprites[i];
            SpriteDelta d{};
            d.kind            = SpriteDeltaKind::Set;
            d.entity          = et.sent[i];
            d.state.placement = prev[i];
            d.state.material  = look.material;
            d.state.uv        = look.uv;
            d.state.size      = look.size;
            d.state.tint      = look.tint;
            d.state.layer     = look.layer;
            d.state.space     = look.space;
            d.state.anchor    = look.anchor;
            d.state.sort      = look.sort;
            d.state.order     = look.order;
            d.state.snap      = true;
            out.push(d);
            et.sent_placements[i] = prev[i];
        }
    }
}

} // namespace

// The same for the layers: each whose offset as the last tick began is not what the renderer
// was told goes there first, so the marker makes it the previous offset.
static void catch_up_layers(ExtractState& st, SpriteLayers& layers, Array<SpriteDelta>& out) {
    for (u32 i = 0; i < 256; ++i) {
        if (layers.tick_start[i] == st.sent_layers[i]) continue;
        SpriteDelta d{};
        d.kind = SpriteDeltaKind::Layer;
        d.state.layer = (i32)i + KV_SPRITE_LAYER_MIN;
        d.state.placement.position = point2{layers.tick_start[i].x, layers.tick_start[i].y};
        out.push(d);
        st.sent_layers[i] = layers.tick_start[i];
    }
}

void extract_sprites(Engine& e, ExtractState& st, SpriteLayers* layers, Array<SpriteDelta>& out, u32 ticks) {
    if (ticks >= 2) {
        catch_up(e, st, out);
        if (layers) catch_up_layers(st, *layers, out);
    }
    if (ticks >= 1) {
        SpriteDelta tick{};
        tick.kind = SpriteDeltaKind::Tick;
        out.push(tick);
    }

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
        if (!is_sprite_table(table)) continue;
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

        // Removes first: a slot the renderer knows whose entity died or was replaced, past
        // the table's rows too (a table restored to fewer slots, a fresh sim's to none).
        for (u32 i = 0; i < et.sent.len; ++i) {
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
