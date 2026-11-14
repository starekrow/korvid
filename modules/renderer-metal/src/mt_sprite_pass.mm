// Korvid renderer-metal: the sprite pass. Shaders and pipelines, and the frame's commands:
// texture uploads and table copies out of the frame ring (a blit), the compute sort (count,
// scan, bucket scan, scatter, key sort), and one indirect draw per non-empty bucket.
// renderer-vulkan's vk_sprite_pass.cc with Metal's calls. Three things differ in kind:
//   - MSL has no numthreads: every dispatch names its threadgroup size (kComputeGroup).
//   - Metal's clip space has y up where ortho2d's (Vulkan's) has it down: the projections in
//     the push block are negated in y here, so the shaders and the math stay as they are.
//   - Argument-buffer entries are not resources the encoder can see: the textures a draw may
//     sample are declared resident for the pass (rb_use_textures).
// Barriers are gone: a serial compute encoder runs its dispatches in order with their
// writes visible to the next, and Metal tracks hazards between encoders and command buffers.
#include "renderer_internal.h"
#include "korvid/core/arena.h"
#include "korvid/platform/io.h"
#include "korvid/core/format.h"
#include "korvid/core/json.h"
#include "korvid/core/memory.h"
#include "korvid/core/profile.h"
#include "korvid/core/time.h"
#include "korvid/math/scalar.h"
#include <string.h>

namespace kv {

// ---- shaders --------------------------------------------------------------------------------

#if KV_DEV || KV_DEBUG

// The build writes Slang's reflection beside each library (<name>.metallib.json): every
// parameter by name with the buffer index the MSL holds. The constants in
// renderer_internal.h are checked against it, so a header that moved a declaration (and so
// renumbered every buffer after it) fails here by name instead of drawing garbage.
bool rb_check_bindings(Renderer* r, const char* lib, const MtExpected* want, u32 count) {
    char path[1024];
    kv::format(path, sizeof path, "%sshaders/%s.metallib.json", r->base_path, lib);
    if (!kv::io_is_file(path)) { KV_WARN("metal", "no reflection for '%s' (%s): bindings unchecked", lib, path); return true; }
    MemScope scope; // the text and the document, both in scratch, go with it
    kv::IoBytes file;
    bool read = kv::io_read_now(path, &file, mem_scratch()) && file.size > 0;
    const char* text = (const char*)file.data;
    usize len = file.size;
    JsonDoc doc;
    if (!read || !json_parse(text, len, mem_scratch(), doc)) { KV_WARN("metal", "cannot read the reflection '%s'", path); return true; }
    const JsonValue* params = json_get(doc, doc.root(), "parameters");
    bool ok = true;
    for (u32 i = 0; i < count; ++i) {
        bool found = false;
        for (const JsonValue* p = params ? json_first(doc, *params) : nullptr; p; p = json_next(doc, *p)) {
            const char* name = json_string(json_get(doc, *p, "name"), "");
            if (strcmp(name, want[i].name) != 0) continue;
            found = true;
            const JsonValue* binding = json_get(doc, *p, "binding");
            u64 index = binding ? json_u64(json_get(doc, *binding, "index"), ~0ull) : ~0ull;
            if (index != want[i].index) {
                KV_ERROR("metal", "'%s': %s is at buffer %llu in the shader, %u in renderer_internal.h", lib, want[i].name,
                         (unsigned long long)index, want[i].index);
                ok = false;
            }
        }
        if (!found) { KV_ERROR("metal", "'%s': the shader has no %s", lib, want[i].name); ok = false; }
    }
    return ok;
}
#endif

// Reads <exe dir>/shaders/<name>.metallib. Compiled by the build; nothing compiles here.
bool rb_load_library(Renderer* r, const char* name, id<MTLLibrary>* out) {
    char path[1024];
    kv::format(path, sizeof path, "%sshaders/%s.metallib", r->base_path, name);
    @autoreleasepool {
        NSError* err = nil;
        *out = [r->device newLibraryWithURL:[NSURL fileURLWithPath:@(path)] error:&err];
        if (!*out) {
            KV_ERROR("metal", "cannot load shader library '%s': %s", path, err ? err.localizedDescription.UTF8String : "?");
            return false;
        }
    }
    KV_INFO("metal", "shader library '%s' loaded", name);
    return true;
}

bool rb_load_shaders(Renderer* r) {
    if (!rb_load_library(r, "sprite", &r->sprite_lib) || !rb_load_library(r, "sprite_sort", &r->sort_lib)) return false;
    r->vs_main = [r->sprite_lib newFunctionWithName:@"vs_main"];
    r->fs_main = [r->sprite_lib newFunctionWithName:@"fs_main"];
    if (!r->vs_main || !r->fs_main) { KV_ERROR("metal", "sprite.metallib has no vs_main or fs_main"); return false; }
#if KV_DEV || KV_DEBUG
    static const MtExpected sprite[] = {{"gPush", kBindPush}, {"gLayers", kBindLayers}, {"gMaterials", kBindMaterials},
                                      {"gBindless", kBindBindless}, {"gSprites", kBindSprites}, {"gSorted", kBindSorted}};
    static const MtExpected sort[] = {{"gPush", kSortPush}, {"gLayers", kSortLayers}, {"gSprites", kSortSprites},
                                    {"gMaterials", kSortMaterials}, {"gSorted", kSortSorted}, {"gBucketOf", kSortBucketOf},
                                    {"gBlockHist", kSortBlockHist}, {"gSortMeta", kSortMeta}, {"gIndirect", kSortIndirect}};
    if (!rb_check_bindings(r, "sprite", sprite, KV_COUNTOF(sprite)) || !rb_check_bindings(r, "sprite_sort", sort, KV_COUNTOF(sort))) return false;
#endif
    return true;
}

// ---- pipelines ------------------------------------------------------------------------------

namespace {

// renderer-vulkan's blend states.
void set_blend(MTLRenderPipelineColorAttachmentDescriptor* c, BlendMode mode) {
    switch (mode) {
    case BlendMode::Opaque:
        c.blendingEnabled = NO;
        break;
    case BlendMode::Alpha:
        c.blendingEnabled             = YES;
        c.sourceRGBBlendFactor        = MTLBlendFactorSourceAlpha;
        c.destinationRGBBlendFactor   = MTLBlendFactorOneMinusSourceAlpha;
        c.rgbBlendOperation           = MTLBlendOperationAdd;
        c.sourceAlphaBlendFactor      = MTLBlendFactorOne;
        c.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        c.alphaBlendOperation         = MTLBlendOperationAdd;
        break;
    case BlendMode::Additive:
        c.blendingEnabled             = YES;
        c.sourceRGBBlendFactor        = MTLBlendFactorSourceAlpha;
        c.destinationRGBBlendFactor   = MTLBlendFactorOne;
        c.rgbBlendOperation           = MTLBlendOperationAdd;
        c.sourceAlphaBlendFactor      = MTLBlendFactorOne;
        c.destinationAlphaBlendFactor = MTLBlendFactorOne;
        c.alphaBlendOperation         = MTLBlendOperationAdd;
        break;
    default:
        break;
    }
}

// A sprite pipeline: the engine's vertex shader, a fragment shader (the engine's or a
// game's), a blend, the layer's format. Nil on failure, logged.
id<MTLRenderPipelineState> make_sprite_pipeline(Renderer* r, id<MTLFunction> fragment, BlendMode mode, const char* label) {
    @autoreleasepool {
        MTLRenderPipelineDescriptor* d = [[MTLRenderPipelineDescriptor new] autorelease];
        d.label            = @(label);
        d.vertexFunction   = r->vs_main;
        d.fragmentFunction = fragment;
        d.colorAttachments[0].pixelFormat = r->layer.pixelFormat;
        set_blend(d.colorAttachments[0], mode);
        NSError* err = nil;
        id<MTLRenderPipelineState> p = [r->device newRenderPipelineStateWithDescriptor:d error:&err];
        if (!p) KV_ERROR("metal", "pipeline %s: %s", label, err ? err.localizedDescription.UTF8String : "?");
        return p;
    }
}

bool create_compute(Renderer* r, const char* entry, id<MTLComputePipelineState>* out) {
    @autoreleasepool {
        id<MTLFunction> fn = [[r->sort_lib newFunctionWithName:@(entry)] autorelease];
        if (!fn) { KV_ERROR("metal", "sprite_sort.metallib has no %s", entry); return false; }
        MTLComputePipelineDescriptor* d = [[MTLComputePipelineDescriptor new] autorelease];
        d.label           = @(entry);
        d.computeFunction = fn;
        NSError* err = nil;
        *out = [r->device newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionNone reflection:nil error:&err];
        if (!*out) { KV_ERROR("metal", "compute pipeline %s: %s", entry, err ? err.localizedDescription.UTF8String : "?"); return false; }
    }
    if ((*out).maxTotalThreadsPerThreadgroup < kComputeGroup) {
        KV_ERROR("metal", "%s runs at most %lu threads a group; the sort needs %u", entry, (unsigned long)(*out).maxTotalThreadsPerThreadgroup, kComputeGroup);
        return false;
    }
    return true;
}

bool create_game_pipeline(Renderer* r, GamePipe& gp) {
    if (gp.failed || !gp.fragment) return false;
    if (gp.pipeline) return true;
    char label[96];
    kv::format(label, sizeof label, "%s %s", name_str(gp.shader), blend_mode_name(gp.blend));
    gp.pipeline = make_sprite_pipeline(r, gp.fragment, gp.blend, label);
    if (!gp.pipeline) {
        gp.failed = true;
        KV_ERROR("metal", "game shader '%s' does not link with the sprite shader: its sprites draw with the engine's", name_str(gp.shader));
    }
    return gp.pipeline != nil;
}

} // namespace

bool rb_create_pipelines(Renderer* r) {
    for (u32 i = 0; i < kBlendCount; ++i) {
        r->pipelines[i] = make_sprite_pipeline(r, r->fs_main, (BlendMode)i, blend_mode_name((BlendMode)i));
        if (!r->pipelines[i]) return false;
    }
    for (GamePipe& gp : r->game_pipes)
        if (gp.used && !gp.failed) create_game_pipeline(r, gp);
    return true;
}

bool rb_create_compute_pipelines(Renderer* r) {
    return create_compute(r, "cs_count", &r->cs_count) && create_compute(r, "cs_scan", &r->cs_scan) &&
           create_compute(r, "cs_bucket_scan", &r->cs_bucket_scan) && create_compute(r, "cs_scatter", &r->cs_scatter) &&
           create_compute(r, "cs_key_sort", &r->cs_key_sort);
}

// Nothing in flight.
void rb_destroy_pipelines(Renderer* r) {
    for (u32 i = 0; i < kBlendCount; ++i) { [r->pipelines[i] release]; r->pipelines[i] = nil; }
    id<MTLComputePipelineState>* cs[] = {&r->cs_count, &r->cs_scan, &r->cs_bucket_scan, &r->cs_scatter, &r->cs_key_sort};
    for (id<MTLComputePipelineState>* p : cs) { [*p release]; *p = nil; }
    for (GamePipe& gp : r->game_pipes) {
        [gp.pipeline release];
        [gp.fragment release];
        [gp.library release];
        gp = GamePipe{};
    }
    [r->vs_main release];    r->vs_main = nil;
    [r->fs_main release];    r->fs_main = nil;
    [r->sprite_lib release]; r->sprite_lib = nil;
    [r->sort_lib release];   r->sort_lib = nil;
}

// The pipeline slot for a material, as renderer-vulkan's: its blend's, or a game shader's
// (assigned here, its library loaded and its pipeline made at once: the layer's format is
// known from the start on Metal).
bool rb_game_pipe_load(Renderer* r, GamePipe& gp) {
    if (!rb_load_library(r, name_str(gp.shader), &gp.library) || !(gp.fragment = [gp.library newFunctionWithName:@"fs_main"])) return false;
    if (r->vs_main) create_game_pipeline(r, gp); // not before the engine's shaders are in
    return true;
}

// ---- the frame ------------------------------------------------------------------------------

namespace {

void record_texture_uploads(Renderer* r, id<MTLBlitCommandEncoder> blit) {
    for (const PendingUpload& up : r->uploads) {
        // The slot may have been destroyed or re-uploaded since: then this copy has no target.
        if (!r->textures.is_alive(up.slot) || r->textures.items[up.slot].tex != up.tex) continue;
        [blit copyFromBuffer:up.src sourceOffset:up.offset sourceBytesPerRow:up.width * 4 sourceBytesPerImage:(NSUInteger)up.width * up.height * 4
                  sourceSize:MTLSizeMake(up.width, up.height, 1) toTexture:up.tex destinationSlice:0 destinationLevel:0
           destinationOrigin:MTLOriginMake(0, 0, 0)];
    }
    r->uploads.clear();
}

bool record_table_copies(Renderer* r, id<MTLBlitCommandEncoder> blit) {
    bool ok = true;
    u32 rows = rw_dirty_runs(r->world.sprite_dirty, r->world.sprites.len, 32, [&](u32 a, u32 b) {
        usize bytes = (usize)(b - a) * sizeof(GpuSprite);
        RingSpan span = rc_ring_alloc(r, bytes, 16);
        if (!span.ptr) { ok = false; return; }
        GpuSprite* dst = (GpuSprite*)span.ptr;
        rw_stage_sprites(r->world, a, b, dst);
        [blit copyFromBuffer:span.buffer sourceOffset:span.offset toBuffer:r->tables.sprites destinationOffset:(NSUInteger)a * sizeof(GpuSprite) size:bytes];
    });
    r->rows_uploaded += rows;
    KV_COUNTER("sprite rows uploaded", rows);
    rw_dirty_runs(r->world.material_dirty, kMaxMaterials, 8, [&](u32 a, u32 b) {
        usize bytes = (usize)(b - a) * sizeof(GpuMaterial);
        RingSpan span = rc_ring_alloc(r, bytes, 16);
        if (!span.ptr) { ok = false; return; }
        GpuMaterial* dst = (GpuMaterial*)span.ptr;
        for (u32 i = a; i < b; ++i) rc_material_row(r, i, dst[i - a]);
        [blit copyFromBuffer:span.buffer sourceOffset:span.offset toBuffer:r->material_table destinationOffset:(NSUInteger)a * sizeof(GpuMaterial) size:bytes];
    });
    return ok;
}

void dispatch(id<MTLComputeCommandEncoder> enc, id<MTLComputePipelineState> pipe, u32 groups) {
    [enc setComputePipelineState:pipe];
    [enc dispatchThreadgroups:MTLSizeMake(groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(kComputeGroup, 1, 1)];
}

void record_sort(Renderer* r, FrameInFlight& f, id<MTLCommandBuffer> cb, const PushData& push) {
    u32 blocks = (r->world.sprites.len + kBlockRows - 1) / kBlockRows;
    MTLComputePassDescriptor* cp = [MTLComputePassDescriptor computePassDescriptor];
    cp.dispatchType = MTLDispatchTypeSerial; // in order, each dispatch's writes seen by the next
    rb_gpu_attach(r, f, cp);
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoderWithDescriptor:cp];
    enc.label = @"sort";
    [enc setBytes:&push length:sizeof push atIndex:kSortPush];
    [enc setBuffer:f.layers offset:0 atIndex:kSortLayers];
    [enc setBuffer:r->tables.sprites offset:0 atIndex:kSortSprites];
    [enc setBuffer:r->material_table offset:0 atIndex:kSortMaterials];
    [enc setBuffer:r->tables.sorted offset:0 atIndex:kSortSorted];
    [enc setBuffer:r->tables.bucket_of offset:0 atIndex:kSortBucketOf];
    [enc setBuffer:r->tables.block_hist offset:0 atIndex:kSortBlockHist];
    [enc setBuffer:r->sort_meta offset:0 atIndex:kSortMeta];
    [enc setBuffer:r->indirect offset:0 atIndex:kSortIndirect];
    [enc pushDebugGroup:@"count"];       dispatch(enc, r->cs_count, blocks);                        [enc popDebugGroup];
    [enc pushDebugGroup:@"scan"];        dispatch(enc, r->cs_scan, kBucketCount / kComputeGroup);    [enc popDebugGroup];
    [enc pushDebugGroup:@"bucket scan"]; dispatch(enc, r->cs_bucket_scan, 1);                       [enc popDebugGroup];
    [enc pushDebugGroup:@"scatter"];     dispatch(enc, r->cs_scatter, blocks);                      [enc popDebugGroup];
    // Buckets with keyed rows: one group each sorts its range by key, the bucket in the push block.
    bool keyed = false;
    for (u32 b = 0; b < kBucketCount; ++b) {
        if (!r->world.keyed_counts[b] || !r->world.bucket_counts[b]) continue;
        if (!keyed) { [enc pushDebugGroup:@"key sort"]; [enc setComputePipelineState:r->cs_key_sort]; keyed = true; }
        PushData p = push;
        p.key_bucket = b;
        [enc setBytes:&p length:sizeof p atIndex:kSortPush];
        [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(kComputeGroup, 1, 1)];
    }
    if (keyed) [enc popDebugGroup];
    [enc endEncoding];
}

// sorted[], bucket_of[] and the sort meta into a shared buffer, after the sort.
void record_sort_check(Renderer* r, id<MTLCommandBuffer> cb) {
    u32 rows = r->world.sprites.len;
    usize meta_bytes = sizeof(u32) * (2 * kBucketCount + 4);
    usize bytes = sizeof(u32) * (usize)rows * 2 + meta_bytes;
    rb_defer_release(r, r->check_buffer);
    r->check_buffer = rb_buffer(r, bytes, MTLResourceStorageModeShared, "sort check");
    if (!r->check_buffer) { r->check_pending = false; return; }
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    blit.label = @"sort check";
    if (rows) {
        [blit copyFromBuffer:r->tables.sorted sourceOffset:0 toBuffer:r->check_buffer destinationOffset:0 size:sizeof(u32) * rows];
        [blit copyFromBuffer:r->tables.bucket_of sourceOffset:0 toBuffer:r->check_buffer destinationOffset:sizeof(u32) * rows size:sizeof(u32) * rows];
    }
    [blit copyFromBuffer:r->sort_meta sourceOffset:0 toBuffer:r->check_buffer destinationOffset:sizeof(u32) * (usize)rows * 2 size:meta_bytes];
    [blit endEncoding];
    r->check_rows     = rows;
    r->check_pending  = false;
    r->check_recorded = true;
}

// One indirect draw per non-empty bucket in [first_bucket, end_bucket), the pipeline bound
// once per run of a slot (Metal has no multi-draw: renderer-vulkan's fallback is the path).
// A game slot whose pipeline failed draws with its blend's engine pipeline.
u32 record_draws(Renderer* r, id<MTLRenderCommandEncoder> enc, u32 first_bucket, u32 end_bucket) {
    u32 draws = 0;
    u32 b = first_bucket;
    DrawRun run;
    while (rw_next_run(r->world.bucket_counts, &b, end_bucket, &run)) {
        u32 slot = run.slot, first = run.first, last = run.last;
        const GamePipe* gp = slot >= kBlendCount ? &r->game_pipes[slot - kBlendCount] : nullptr;
        id<MTLRenderPipelineState> pipe = gp ? (gp->pipeline ? gp->pipeline : r->pipelines[(u32)gp->blend]) : r->pipelines[slot];
        char label[64];
        rw_run_label(label, sizeof label, run, gp ? name_str(gp->shader) : blend_mode_name((BlendMode)slot));
        [enc pushDebugGroup:@(label)];
        [enc setRenderPipelineState:pipe];
        for (u32 n = first; n <= last; ++n) {
            if (!r->world.bucket_counts[n]) continue;
            [enc drawPrimitives:MTLPrimitiveTypeTriangle indirectBuffer:r->indirect indirectBufferOffset:(NSUInteger)n * sizeof(MTLDrawPrimitivesIndirectArguments)];
            ++draws;
        }
        [enc popDebugGroup];
    }
    return draws;
}

} // namespace

bool rb_record_frame(Renderer* r, FrameInFlight& f, id<MTLCommandBuffer> cb, id<CAMetalDrawable> drawable, const RenderView& view) {
    KV_ZONE("record");
    // Uploads first: textures, then the rows the ticks changed.
    MTLBlitPassDescriptor* bp = [MTLBlitPassDescriptor blitPassDescriptor];
    rb_gpu_attach(r, f, bp);
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoderWithDescriptor:bp];
    blit.label = @"upload";
    record_texture_uploads(r, blit);
    bool copied = record_table_copies(r, blit);
    [blit endEncoding];
    if (!copied) return false;

    u32 width  = drawable ? (u32)drawable.texture.width : 0;
    u32 height = drawable ? (u32)drawable.texture.height : 0;
    bool draw = drawable && r->world.sprites.len > 0;
    // World: the view's center to the origin, the camera's rotation undone, the ortho of a
    // rect that size about the origin; the cull rect is the rotated view's bounding box.
    // Screen: the window in pixels about its center. Then y flipped for Metal.
    f32 sw = (f32)width, sh = (f32)height;
    PushData push = rw_push_block(r->world, view, sw, sh, (f32)(time_now() - r->start_time), KV_RB_CLIP_Y_UP != 0);

    if (draw) {
        record_sort(r, f, cb, push);
        if (r->check_pending) { rc_snapshot_keys(r, view.alpha); record_sort_check(r, cb); }
    }
    if (!drawable) return true;

    // With a post shader, the world (the layers below the post layer) goes into the target
    // first, and the window's pass begins with the post shader; the layers at and above the
    // post layer draw over it there.
    bool post = rb_post_prepare(r, width, height);
    u32 split = post ? rw_bucket_of(r->post.layer, 0) : 0;
    u32 draws = 0;
    auto bind_sprites = [&](id<MTLRenderCommandEncoder> enc) {
        [enc setViewport:(MTLViewport){0.0, 0.0, (double)width, (double)height, 0.0, 1.0}];
        [enc setVertexBytes:&push length:sizeof push atIndex:kBindPush];
        [enc setFragmentBytes:&push length:sizeof push atIndex:kBindPush];
        id<MTLBuffer> bound[] = {f.layers, r->material_table, f.bindless, r->tables.sprites, r->tables.sorted};
        NSUInteger offsets[] = {0, 0, 0, 0, 0};
        [enc setVertexBuffers:bound offsets:offsets withRange:NSMakeRange(kBindLayers, 5)];
        [enc setFragmentBuffers:bound offsets:offsets withRange:NSMakeRange(kBindLayers, 5)];
        static_assert(kBindMaterials == kBindLayers + 1 && kBindBindless == kBindLayers + 2 && kBindSprites == kBindLayers + 3 &&
                      kBindSorted == kBindLayers + 4, "the sprite buffers are bound as one range");
        rb_use_textures(r, enc);
    };
    auto pass_into = [&](id<MTLTexture> target, NSString* label, bool first, bool last) {
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rb_gpu_attach(r, f, rp, first, last);
        rp.colorAttachments[0].texture     = target;
        rp.colorAttachments[0].loadAction  = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor  = MTLClearColorMake(view.clear_color.r, view.clear_color.g, view.clear_color.b, view.clear_color.a);
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        enc.label = label;
        return enc;
    };
    @autoreleasepool {
        if (post) {
            id<MTLRenderCommandEncoder> world = pass_into(r->post.target, @"world", true, false);
            if (draw) { bind_sprites(world); draws += record_draws(r, world, 0, split); }
            [world endEncoding];
        }
        id<MTLRenderCommandEncoder> enc = pass_into(drawable.texture, @"draw", !post, true);
        if (post) rb_post_draw(r, f, enc, view);
        if (draw) { bind_sprites(enc); draws += record_draws(r, enc, split, kBucketCount); }
        KV_COUNTER("indirect draws", draws);
        [enc endEncoding];
    }
    return true;
}

} // namespace kv
