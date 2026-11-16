// Korvid render core: the frame, once. renderer_render and what it records are the order of
// work the three backends agree on; each step that touches the GPU is the bound backend's
// (the rb_ functions under "the frame" in core.h), at the height of a pass: "copy these
// rows", "run the sort", "draw this run of buckets". Included by core.inl.
//
// A frame:
//   1. Wait for this slot's last frame; release its span of the ring; run its garbage.
//   2. Grow the tables if the world outgrew them; write the layers' offsets and the texture
//      slots that changed into this frame's own.
//   3. Acquire: the backend says whether there is an image to draw into, and its size.
//   4. Record: the uploads (staged textures, then the rows the ticks changed); the sort,
//      when there is something to draw; the world's pass up to the post layer when the post
//      pass is on; the window's pass, the post shader first, the rest of the runs over it.
//   5. Submit and present. Uploads are submitted even when nothing is presented.
//   6. A requested screenshot or sort check waits for the frame and is written or verified.

namespace kv { namespace KV_RB_NS {

void rc_sort_steps(const Renderer* r, SortStep out[kSortSteps]) {
    u32 blocks = (r->world.sprites.len + kBlockRows - 1) / kBlockRows;
    out[0] = SortStep{"count", blocks};                           // cull, bucket, a histogram per block
    out[1] = SortStep{"scan", kBucketCount / kComputeGroup};      // counts into each block's offsets, and the totals
    out[2] = SortStep{"bucket scan", 1};                          // totals into starts and the indirect commands
    out[3] = SortStep{"scatter", blocks};                         // each row to its place
}

namespace rc {

// Dirty sprite rows and material rows, staged into the ring; the backend copies each run
// into its table. False when the ring could not hold a run: the frame is given up.
bool record_table_copies(Renderer* r, FrameInFlight& f) {
    bool ok = true;
    u32 rows = rw_dirty_runs(r->world.sprite_dirty, r->world.sprites.len, 32, [&](u32 a, u32 b) {
        u64 bytes = (u64)(b - a) * sizeof(GpuSprite);
        RingSpan span = rc_ring_alloc(r, bytes, 16);
        if (!span.ptr) { ok = false; return; }
        rw_stage_sprites(r->world, a, b, (GpuSprite*)span.ptr);
        rb_copy_rows(r, f, r->tables.sprites, span, (u64)a * sizeof(GpuSprite), bytes);
    });
    r->rows_uploaded += rows;
    KV_COUNTER("sprite rows uploaded", rows);

    rw_dirty_runs(r->world.material_dirty, kMaxMaterials, 8, [&](u32 a, u32 b) {
        u64 bytes = (u64)(b - a) * sizeof(GpuMaterial);
        RingSpan span = rc_ring_alloc(r, bytes, 16);
        if (!span.ptr) { ok = false; return; }
        GpuMaterial* dst = (GpuMaterial*)span.ptr;
        for (u32 i = a; i < b; ++i) rc_material_row(r, i, dst[i - a]);
        rb_copy_rows(r, f, r->material_table, span, (u64)a * sizeof(GpuMaterial), bytes);
    });
    return ok;
}

// One draw per run of buckets in [first_bucket, end_bucket). Returns the draws issued.
u32 draw_runs(Renderer* r, FrameInFlight& f, u32 first_bucket, u32 end_bucket) {
    u32 draws = 0;
    u32 b = first_bucket;
    DrawRun run;
    while (rw_next_run(r->world.bucket_counts, &b, end_bucket, &run)) draws += rb_draw_run(r, f, run);
    return draws;
}

// This slot's copy of the UI lists, brought up to the world's: rewritten whole when anything
// changed since the slot last wrote it, untouched otherwise. False: the slot has none, and
// draws no UI (logged once).
bool stage_ui(Renderer* r, FrameInFlight& f) {
    if (f.ui_failed) return false;
    if (!r->world.ui.len) return true;
    if (f.ui_version == r->world.ui_version) return true;
    u64 bytes = r->world.ui_bytes;
    if (bytes > f.ui_capacity) {
        u64 want = bytes + bytes / 2;
        if (want < 64 * 1024) want = 64 * 1024;
        if (!rb_ui_buffer(r, f, want)) {
            f.ui_failed = true;
            KV_ERROR(KV_RB_NAME, "the UI lists have no buffer: UI is not drawn");
            return false;
        }
        f.ui_capacity = want;
    }
    rw_ui_stage(r->world, (u8*)rb_buffer_ptr(f.ui));
    f.ui_version = r->world.ui_version;
    ++r->world.ui_stages;
    return true;
}

// The buckets [*from, end) of a pass, with the UI lists whose layers end in it drawn after
// their layers' sprites. `*ui` walks the lists in order across the frame's passes.
u32 draw_with_ui(Renderer* r, FrameInFlight& f, const PushData& push, bool sprites, bool ui_ok, u32 from, u32 end, u32* ui) {
    u32 draws = 0;
    while (*ui < r->world.ui.len) {
        const RwUiLayer& l = r->world.ui[*ui];
        u32 at = rw_ui_bucket_end(l.layer);
        if (at > end) break;
        if (sprites && at > from) { draws += draw_runs(r, f, from, at); }
        if (at > from) from = at;
        if (ui_ok && l.items) {
            PushData p = push;
            p.ui_base = (u32)l.base;
            p.ui_count = l.items;
            rb_ui_draw(r, f, p);
            ++draws;
        }
        ++*ui;
    }
    if (sprites && end > from) draws += draw_runs(r, f, from, end);
    return draws;
}

bool record_frame(Renderer* r, FrameInFlight& f, const RenderView& view, bool ui_ok) {
    KV_ZONE("record");
    if (!rb_record_begin(r, f)) return false;

    // Uploads first: textures, then the rows the ticks changed. The frame before read the
    // tables in its sort and its draws; the backend orders this frame's copies after them.
    rb_upload_begin(r, f);
    bool copied = record_table_copies(r, f);
    rb_upload_end(r, f);
    if (!copied) return false;

    bool draw = f.have_image && r->world.sprites.len > 0;
    PushData push = rw_push_block(r->world, view, (f32)f.width, (f32)f.height, (f32)(time_now() - r->start_time), KV_RB_CLIP_Y_UP != 0);
    if (draw) {
        rb_sort(r, f, push);
        if (r->check_pending) { rc_snapshot_keys(r, view.alpha); rb_read_sort(r, f); }
    }
    rb_sort_end(r, f, draw);

    if (f.have_image) {
        // With a post shader, the world (the layers below the post layer) goes into the
        // target first, and the window's pass begins with the post shader; the layers at and
        // above the post layer draw over it there. A UI list draws after its layer's sprites,
        // in whichever pass its layer is.
        bool ui    = r->world.ui.len && ui_ok;
        bool bind  = draw || ui;
        bool post  = rb_post_prepare(r, f.width, f.height);
        u32  split = post ? rw_bucket_of(r->post.layer, 0) : 0;
        u32  draws = 0;
        u32  ui_at = 0;
        if (post) {
            rb_pass_begin(r, f, RcPass::World, view);
            if (bind) { rb_bind_sprites(r, f, push); draws += draw_with_ui(r, f, push, draw, ui, 0, split, &ui_at); }
            rb_pass_end(r, f, RcPass::World);
        }
        rb_pass_begin(r, f, RcPass::Window, view);
        if (post) rb_pass_post(r, f, view);
        if (bind) { rb_bind_sprites(r, f, push); draws += draw_with_ui(r, f, push, draw, ui, split, kBucketCount, &ui_at); }
        KV_COUNTER("indirect draws", draws);
        rb_pass_end(r, f, RcPass::Window);
        if (r->shot_pending) rb_read_window(r, f);
    }
    return rb_record_end(r, f);
}

} // namespace rc

bool renderer_render(Renderer* r, const RenderView& view) {
    if (!r) return false;
    FrameInFlight& f = r->frames[r->frame_index];
    f64 waited = 0.0;
    KV_RB_FRAME_SCOPE {
        // This slot's previous frame: once it is done, what it queued for destruction can go,
        // its span of the ring is free, and what is its own (its sets) may be rewritten.
        if (!rb_frame_wait(r, f, &waited)) return false;
        r->last_wait = 0.0;
        rc_ring_release(r, f);
        rc_run_garbage(r, f);
        if (r->world.sprites.len > r->tables.capacity) {
            // Growth remarks every row dirty, so the next frame uploads the table whole: logged
            // with the frame, so a hitch can be found by it (FrameLoopConfig::sprite_rows avoids it).
            KV_INFO(KV_RB_NAME, "sprite tables grow at frame %llu: %u rows live, %u held", (unsigned long long)r->frame_count, r->world.sprites.len, r->tables.capacity);
            if (!rb_create_tables(r, r->world.sprites.len)) return false;
        }
        // The layers' offsets for this frame: previous and current, the shader interpolates.
        f32* lo = (f32*)rb_buffer_ptr(f.layers);
        for (u32 i = 0; i < kLayerCount; ++i) {
            lo[i * 4 + 0] = r->world.layer_prev[i][0]; lo[i * 4 + 1] = r->world.layer_prev[i][1];
            lo[i * 4 + 2] = r->world.layer_cur[i][0];  lo[i * 4 + 3] = r->world.layer_cur[i][1];
        }
        for (u32 slot : f.dirty_slots) rb_frame_slot(r, f, slot);
        f.dirty_slots.clear();

        // The UI lists before anything is recorded: a new buffer rewrites this slot's bindings.
        bool ui_ok = rc::stage_ui(r, f);

        if (!rb_frame_acquire(r, f, &waited)) return false;
        if (!rc::record_frame(r, f, view, ui_ok)) return false;
        if (!rb_frame_submit(r, f)) return false;
        f.submitted = true;
        f.garbage_owned = f.garbage.len; // destroyed when the frame is done; anything queued later waits for the next
        rc_ring_submitted(r, f);
        if (!rb_frame_present(r, f, &waited)) return false;

        if (r->shot_recorded || r->check_recorded) { // a dev stall: wait for this frame and read it back now
            if (!rb_frame_finish(r, f)) return false;
            if (r->shot_recorded) rc_write_screenshot(r);
            if (r->check_recorded) rc_verify_sort(r);
        }
    }
    r->last_wait = waited;
    r->frame_index = (r->frame_index + 1) % kFramesInFlight;
    ++r->frame_count;
    return f.have_image;
}

} } // namespace kv::KV_RB_NS
