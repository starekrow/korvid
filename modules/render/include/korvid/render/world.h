// Korvid render: the render world, the half of the renderer that knows no GPU. The sprites
// the deltas describe and the rows they become, the materials' records, which rows changed
// since they were last uploaded, how many sprites each sort bucket holds, the layers'
// offsets. Every backend keeps one (its Renderer's `world`) and draws from it; the functions
// here are what the three backends used to carry a copy each of
// (docs/PRDs/Engine-Slice-9-PRD.md). Plain data in, plain data out: it links with any
// backend and its tests run without one.
//
// The bytes of a row, a material row and the push block are core/layout.h's.
#pragma once
#include "korvid/render/core/layout.h"
#include "korvid/core/array.h"
#include "korvid/core/map.h"
#include "korvid/core/pool.h"

namespace kv {

struct RenderWorld {
    // The outer engine's entity handle maps to a slot here, and the slot is the GPU row.
    Pool<RenderSprite>   sprites;
    Map<Handle, u32>     sprite_of_entity;
    Pool<MaterialRecord> materials;        // row = index; 0 is the default material
    Map<u64, Handle>     material_by_name; // Name::hash -> the first live material with the name
    Array<u64>           sprite_dirty;     // bit per slot: row to upload
    Array<u64>           material_dirty;   // bit per row
    u32                  bucket_counts[kBucketCount] = {};
    u32                  keyed_counts[kBucketCount] = {};  // rows whose sort is not Row: their buckets get the key sort
    // The layers' scroll offsets (world units) at the last tick and the one before, by layer
    // + 128: copied into the frame's buffer when it is recorded, interpolated by alpha.
    f32                  layer_prev[kLayerCount][2] = {};
    f32                  layer_cur[kLayerCount][2]  = {};
    u32                  tick = 0;         // Tick deltas seen; the stamp of the newest rows
    u64                  deltas_applied = 0;
};

// The pools, the maps and the bitmaps, from `general`. A backend that registers a handle
// kind for materials gives it to `materials` (use_kind) before this.
bool rw_init(RenderWorld& w, Allocator* general);
void rw_free(RenderWorld& w);

// A tick's worth (or a frame's worth) of sprite deltas: renderer_apply.
void rw_apply(RenderWorld& w, Span<const SpriteDelta> deltas);

// The sort bucket of a layer and a pipeline slot: layer major, so ascending bucket order is
// draw order.
u32  rw_bucket_of(i32 layer, u32 slot);
// Row index for a material handle: its row, or 0 (the default) when null or stale.
u32  rw_material_row_of(const RenderWorld& w, Handle material);
// The record a row draws with: its own, or the default's when the row is dead.
const MaterialRecord& rw_material_at(const RenderWorld& w, u32 row);
// A record as its GPU row; the caller resolves the texture's slot.
void rw_material_row(const MaterialRecord& m, u32 texture_slot, GpuMaterial& out);
void rw_mark_sprite_dirty(RenderWorld& w, u32 slot);
void rw_mark_material_dirty(RenderWorld& w, u32 row);
// Every sprite using material row `row` is rebucketed (its slot may have changed) and
// marked for upload. For material edits and destruction; never on the frame path.
void rw_rebind_material(RenderWorld& w, u32 row);

// Rows [a, b) as the GPU holds them: a dead slot is a zero row, not alive, never drawn.
void rw_stage_sprites(const RenderWorld& w, u32 a, u32 b, GpuSprite* dst);

// Walks a dirty bitmap over [0, rows) and calls copy_run(a, b) for every run of set bits,
// joining runs separated by fewer than `join` clean rows (uploading a clean row again is
// cheaper than another copy region). Clears the bits. Returns the rows covered.
template <class F>
u32 rw_dirty_runs(Array<u64>& bits, u32 rows, u32 join, F copy_run) {
    u32 words = (rows + 63) / 64;
    if (words > bits.len) words = bits.len;
    u32 run_a = 0, run_b = 0; // current run [run_a, run_b)
    bool open = false;
    u32 copied = 0;
    for (u32 w = 0; w < words; ++w) {
        u64 word = bits[w];
        if (!word) continue;
        bits[w] = 0;
        while (word) {
            u32 bit = KV_CTZ64(word);
            word &= word - 1;
            u32 i = w * 64 + bit;
            if (i >= rows) break;
            if (open && i <= run_b + join) { run_b = i + 1; continue; }
            if (open) { copy_run(run_a, run_b); copied += run_b - run_a; }
            run_a = i; run_b = i + 1; open = true;
        }
    }
    if (open) { copy_run(run_a, run_b); copied += run_b - run_a; }
    return copied;
}

// The draws of a frame, as runs of buckets: from a non-empty bucket to the last non-empty
// bucket of the same pipeline slot before one of another slot intervenes. Buckets in
// between with nothing in them draw zero instances. `*cursor` starts at the first bucket to
// look at and is left after the run; false when there is no run before `end`.
struct DrawRun { u32 slot, first, last; };
bool rw_next_run(const u32* bucket_counts, u32* cursor, u32 end, DrawRun* out);
// The buckets the key sort must visit: those with rows whose sort is not Row. `*bucket`
// starts at the first to look at and is left on the one found; the caller steps past it.
bool rw_next_keyed(const RenderWorld& w, u32* bucket);
// A run's label for a capture tool: the layers it covers and what draws them (a blend's
// name or a game shader's).
void rw_run_label(char* out, usize cap, const DrawRun& run, const char* what);

// The push block for a frame. World: the view's center moved to the origin, the camera's
// rotation undone, then the ortho of a rect that size about the origin; the cull rect is the
// rotated view's bounding box. Screen: a window of width x height pixels about its center,
// no camera. ortho2d targets Vulkan's clip space, y down; `clip_y_up` negates the y row of
// both projections for the backends whose clip space has y up. key_bucket is left 0.
PushData rw_push_block(const RenderWorld& w, const RenderView& view, f32 width, f32 height, f32 time, bool clip_y_up);

// --check-sort. Each row's key as a frame at `alpha` computes it (cs_key_sort's formula on
// the CPU's copy of the rows), into keys[w.sprites.len]: taken when the check is recorded,
// since the rows may change before the frame's results are back.
void rw_snapshot_keys(const RenderWorld& w, f32 alpha, f32* keys);
// Verifies what a frame's sort wrote, read back as sorted[rows], bucket_of[rows], then the
// sort meta (totals[kBucketCount], starts[kBucketCount], visible): every bucket's run holds
// exactly the rows whose bucket it is, in (key, row) order, once each, and the counts agree
// with the CPU's where nothing was culled. Logs what it finds under `tag`; true when clean.
bool rw_verify_sort(const RenderWorld& w, const u32* readback, u32 rows, const f32* keys, const char* tag);

} // namespace kv
