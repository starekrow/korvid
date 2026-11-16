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

namespace kv { namespace KV_RB_NS {

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
    // The UI primitive's: an executable without ui.metallib draws no UI, and the rest as ever.
    if (rb_load_library(r, "ui", &r->ui_lib)) {
        r->vs_ui = [r->ui_lib newFunctionWithName:@"vs_ui"];
        r->fs_ui = [r->ui_lib newFunctionWithName:@"fs_ui"];
        if (!r->vs_ui || !r->fs_ui) KV_ERROR("metal", "ui.metallib has no vs_ui or fs_ui: no UI");
#if KV_DEV || KV_DEBUG
        static const MtExpected ui[] = {{"gPush", kUiPush}, {"gLayers", kUiLayers}, {"gMaterials", kUiMaterials},
                                        {"gBindless", kUiBindless}, {"gUi", kUiList}};
        if (r->vs_ui && !rb_check_bindings(r, "ui", ui, KV_COUNTOF(ui))) { [r->vs_ui release]; r->vs_ui = nil; }
#endif
    }
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
    // The UI's: its colors are premultiplied. Into the world's target or the drawable, which
    // have the layer's format.
    if (r->vs_ui && r->fs_ui) {
        @autoreleasepool {
            MTLRenderPipelineDescriptor* d = [[MTLRenderPipelineDescriptor new] autorelease];
            d.label            = @"ui";
            d.vertexFunction   = r->vs_ui;
            d.fragmentFunction = r->fs_ui;
            MTLRenderPipelineColorAttachmentDescriptor* c = d.colorAttachments[0];
            c.pixelFormat                 = r->layer.pixelFormat;
            c.blendingEnabled             = YES;
            c.sourceRGBBlendFactor        = MTLBlendFactorOne;
            c.destinationRGBBlendFactor   = MTLBlendFactorOneMinusSourceAlpha;
            c.rgbBlendOperation           = MTLBlendOperationAdd;
            c.sourceAlphaBlendFactor      = MTLBlendFactorOne;
            c.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            c.alphaBlendOperation         = MTLBlendOperationAdd;
            NSError* err = nil;
            r->ui_pipeline = [r->device newRenderPipelineStateWithDescriptor:d error:&err];
            if (!r->ui_pipeline) KV_ERROR("metal", "pipeline ui: %s: no UI", err ? err.localizedDescription.UTF8String : "?");
        }
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
    [r->ui_pipeline release]; r->ui_pipeline = nil;
    [r->vs_ui release];       r->vs_ui = nil;
    [r->fs_ui release];       r->fs_ui = nil;
    [r->ui_lib release];      r->ui_lib = nil;
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
           destinationOrigin:MTLOriginMake(up.x, up.y, 0)];
    }
    r->uploads.clear();
}

void dispatch(id<MTLComputeCommandEncoder> enc, id<MTLComputePipelineState> pipe, u32 groups) {
    [enc setComputePipelineState:pipe];
    [enc dispatchThreadgroups:MTLSizeMake(groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(kComputeGroup, 1, 1)];
}

void record_sort(Renderer* r, FrameInFlight& f, id<MTLCommandBuffer> cb, const PushData& push) {
    SortStep steps[kSortSteps];
    rc_sort_steps(r, steps);
    const id<MTLComputePipelineState> pipes[kSortSteps] = {r->cs_count, r->cs_scan, r->cs_bucket_scan, r->cs_scatter};
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
    for (u32 i = 0; i < kSortSteps; ++i) {
        [enc pushDebugGroup:@(steps[i].label)];
        dispatch(enc, pipes[i], steps[i].groups);
        [enc popDebugGroup];
    }
    // Buckets with keyed rows: one group each sorts its range by key, the bucket in the push block.
    bool keyed = false;
    for (u32 b = 0; rw_next_keyed(r->world, &b); ++b) {
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

} // namespace

// The steps of the core's frame (korvid/render/core/frame.inl), in the order it calls them.
// f.rec is the command buffer being recorded; each pass is an encoder of its own, kept in
// the frame while it is open (f.blit for the uploads, f.enc for a render pass).

bool rb_record_begin(Renderer* r, FrameInFlight& f) {
    f.rec = [r->queue commandBuffer];
    f.rec.label = @"frame";
    rb_gpu_frame_begin(r, f);
    f.world_pass = false;
    return true;
}

// Uploads are a blit: textures, then the rows the ticks changed, from the ring into the
// private tables. Metal orders them after the frame before's reads.
void rb_upload_begin(Renderer* r, FrameInFlight& f) {
    MTLBlitPassDescriptor* bp = [MTLBlitPassDescriptor blitPassDescriptor];
    rb_gpu_attach(r, f, bp);
    f.blit = [f.rec blitCommandEncoderWithDescriptor:bp];
    f.blit.label = @"upload";
    record_texture_uploads(r, f.blit);
}

void rb_copy_rows(Renderer*, FrameInFlight& f, RbBuffer& table, const RingSpan& span, u64 offset, u64 bytes) {
    [f.blit copyFromBuffer:span.buffer sourceOffset:(NSUInteger)span.offset toBuffer:table destinationOffset:(NSUInteger)offset size:(NSUInteger)bytes];
}

void rb_upload_end(Renderer*, FrameInFlight& f) {
    [f.blit endEncoding];
    f.blit = nil;
}

void rb_sort(Renderer* r, FrameInFlight& f, const PushData& push) { record_sort(r, f, f.rec, push); }
void rb_read_sort(Renderer* r, FrameInFlight& f) { record_sort_check(r, f.rec); }
void rb_sort_end(Renderer*, FrameInFlight&, bool) {}

// A render pass into the world's target or the drawable. The draw's GPU timing starts at the
// first render pass of the frame and ends at the window's.
void rb_pass_begin(Renderer* r, FrameInFlight& f, RcPass pass, const RenderView& view) {
    bool world = pass == RcPass::World;
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rb_gpu_attach(r, f, rp, world || !f.world_pass, !world);
    rp.colorAttachments[0].texture     = world ? r->post.target : f.drawable.texture;
    rp.colorAttachments[0].loadAction  = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor  = MTLClearColorMake(view.clear_color.r, view.clear_color.g, view.clear_color.b, view.clear_color.a);
    f.enc = [f.rec renderCommandEncoderWithDescriptor:rp];
    f.enc.label = world ? @"world" : @"draw";
    if (world) f.world_pass = true;
}

void rb_pass_post(Renderer* r, FrameInFlight& f, const RenderView& view) { rb_post_draw(r, f, f.enc, view); }

void rb_bind_sprites(Renderer* r, FrameInFlight& f, const PushData& push) {
    id<MTLRenderCommandEncoder> enc = f.enc;
    [enc setViewport:(MTLViewport){0.0, 0.0, (double)f.width, (double)f.height, 0.0, 1.0}];
    [enc setVertexBytes:&push length:sizeof push atIndex:kBindPush];
    [enc setFragmentBytes:&push length:sizeof push atIndex:kBindPush];
    id<MTLBuffer> bound[] = {f.layers, r->material_table, f.bindless, r->tables.sprites, r->tables.sorted};
    NSUInteger offsets[] = {0, 0, 0, 0, 0};
    [enc setVertexBuffers:bound offsets:offsets withRange:NSMakeRange(kBindLayers, 5)];
    [enc setFragmentBuffers:bound offsets:offsets withRange:NSMakeRange(kBindLayers, 5)];
    static_assert(kBindMaterials == kBindLayers + 1 && kBindBindless == kBindLayers + 2 && kBindSprites == kBindLayers + 3 &&
                  kBindSorted == kBindLayers + 4, "the sprite buffers are bound as one range");
    rb_use_textures(r, enc);
}

// One indirect draw per non-empty bucket of a run (rw_next_run), the pipeline bound once
// (Metal has no multi-draw). A game slot whose pipeline failed draws with its blend's engine
// pipeline.
u32 rb_draw_run(Renderer* r, FrameInFlight& f, const DrawRun& run) {
    id<MTLRenderCommandEncoder> enc = f.enc;
    u32 draws = 0;
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
    return draws;
}

void rb_pass_end(Renderer*, FrameInFlight& f, RcPass) {
    [f.enc endEncoding];
    f.enc = nil;
}

bool rb_record_end(Renderer*, FrameInFlight&) { return true; }

// The UI primitive (korvid/render/ui.h). The lists are a shared buffer per frame slot, which
// the core writes when they change; the old one goes with the frame. No pipeline (ui.metallib
// missing, or refused): no buffer, and the core draws no UI from this slot.
bool rb_ui_buffer(Renderer* r, FrameInFlight& f, u64 bytes) {
    if (!r->ui_pipeline) return false;
    if (f.ui) rb_buffer_retire(r, f.ui);
    f.ui = rb_buffer(r, (usize)bytes, MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined, "ui lists");
    return f.ui != nil;
}

// One layer's list: an instance an item, six vertices a quad, inside a pass whose sprite
// bindings are made. The list takes the sprite table's index for the draw and gives it back;
// the sprite draws after it set their own pipeline.
void rb_ui_draw(Renderer* r, FrameInFlight& f, const PushData& push) {
    if (!r->ui_pipeline || !f.ui || !push.ui_count) return;
    id<MTLRenderCommandEncoder> enc = f.enc;
    [enc pushDebugGroup:@"ui"];
    [enc setRenderPipelineState:r->ui_pipeline];
    [enc setVertexBytes:&push length:sizeof push atIndex:kUiPush];
    [enc setFragmentBytes:&push length:sizeof push atIndex:kUiPush];
    [enc setVertexBuffer:f.ui offset:0 atIndex:kUiList];
    [enc setFragmentBuffer:f.ui offset:0 atIndex:kUiList];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6 instanceCount:push.ui_count];
    [enc setVertexBuffer:r->tables.sprites offset:0 atIndex:kBindSprites];
    [enc setFragmentBuffer:r->tables.sprites offset:0 atIndex:kBindSprites];
    [enc popDebugGroup];
}

} } // namespace kv::KV_RB_NS
