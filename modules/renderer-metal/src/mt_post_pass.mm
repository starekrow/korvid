// Korvid renderer-metal: the full-frame pass. renderer-vulkan's vk_post_pass.cc with Metal's
// calls: the world's target is a private texture that is both drawn into and sampled; there
// are no layouts to move it between, and Metal orders the world's writes before the post
// pass's read, and that read before the next frame's writes. One target serves both frames
// in flight, as Vulkan's does.
#include "renderer_internal.h"
#include "korvid/core/time.h"
#include "korvid/core/format.h"
#include <string.h>

namespace kv { namespace KV_RB_NS {

namespace {

// What vs_post and the game's fragment shader share as a push block (post_fragment.slangh).
struct PostPushData {
    f32 size[2];
    f32 time;
    u32 tick;
    u32 count;
    f32 alpha;
    u32 pad0, pad1;
};

void post_fail(Renderer* r, const char* why) {
    PostPass& p = r->post;
    p.failed = true;
    KV_ERROR("metal", "post shader '%s' %s: the frame is drawn without it", name_str(p.shader), why);
}

void release_target_later(Renderer* r) {
    PostPass& p = r->post;
    rb_defer_release(r, p.target);
    p.target = nil;
    p.width = p.height = 0;
}

// The sampler and the engine's vertex shader, on first use.
bool post_setup(Renderer* r) {
    PostPass& p = r->post;
    if (!p.sampler) {
        @autoreleasepool {
            MTLSamplerDescriptor* d = [[MTLSamplerDescriptor new] autorelease];
            d.minFilter = d.magFilter = MTLSamplerMinMagFilterLinear;
            d.mipFilter = MTLSamplerMipFilterNotMipmapped;
            d.sAddressMode = d.tAddressMode = d.rAddressMode = MTLSamplerAddressModeClampToEdge; // a bent edge repeats its own pixels
            d.label = @"post";
            p.sampler = [r->device newSamplerStateWithDescriptor:d];
        }
        if (!p.sampler) return false;
    }
    if (!p.vertex) {
        if (!p.vertex_lib && !rb_load_library(r, "post", &p.vertex_lib)) return false;
        p.vertex = [p.vertex_lib newFunctionWithName:@"vs_post"];
        if (!p.vertex) { KV_ERROR("metal", "post.metallib has no vs_post"); return false; }
    }
    return true;
}

// The pipeline: vs_post, the game's fs_main, no blending, the layer's format.
bool post_pipeline(Renderer* r) {
    PostPass& p = r->post;
    @autoreleasepool {
        MTLRenderPipelineDescriptor* d = [[MTLRenderPipelineDescriptor new] autorelease];
        char label[96];
        kv::format(label, sizeof label, "post %s", name_str(p.shader));
        d.label            = @(label);
        d.vertexFunction   = p.vertex;
        d.fragmentFunction = p.fragment;
        d.colorAttachments[0].pixelFormat = r->layer.pixelFormat;
        NSError* err = nil;
        p.pipeline = [r->device newRenderPipelineStateWithDescriptor:d error:&err];
        if (!p.pipeline) {
            KV_ERROR("metal", "post pipeline: %s", err ? err.localizedDescription.UTF8String : "?");
            post_fail(r, "does not link with vs_post");
            return false;
        }
    }
    return true;
}

} // namespace

bool rb_post_set(Renderer* r, i32 layer, Name shader) {
    PostPass& p = r->post;
    p.layer = layer;
    if (shader == p.shader) return !p.failed;
    // A different shader: the old pipeline is released after the frame; its library and
    // function now (a pipeline does not need them once made).
    rb_defer_release(r, p.pipeline);
    p.pipeline = nil;
    [p.fragment release]; p.fragment = nil;
    [p.library release];  p.library = nil;
    p.shader = shader;
    p.failed = false;
    if (shader.is_null()) { release_target_later(r); return true; }

    if (!post_setup(r)) { post_fail(r, "has no vertex shader (post.metallib) or sampler"); return false; }
    if (!rb_load_library(r, name_str(shader), &p.library)) { post_fail(r, "did not load"); return false; }
    p.fragment = [p.library newFunctionWithName:@"fs_main"];
    if (!p.fragment) { post_fail(r, "has no fs_main"); return false; }
#if KV_DEV || KV_DEBUG
    static const MtExpected want[] = {{"gPost", kPostPush}, {"gPostParams", kPostParams}, {"gWorld", 0}, {"gWorldSampler", 0}};
    if (!rb_check_bindings(r, name_str(shader), want, KV_COUNTOF(want))) { post_fail(r, "binds differently from the renderer"); return false; }
#endif
    return post_pipeline(r);
}

bool rb_post_active(const Renderer* r) {
    const PostPass& p = r->post;
    return !p.shader.is_null() && !p.failed && p.pipeline;
}

// The target for the drawable as it is now: made again when its size moved.
bool rb_post_prepare(Renderer* r, u32 width, u32 height) {
    PostPass& p = r->post;
    if (!rb_post_active(r) || !width || !height) return false;
    if (p.target && p.width == width && p.height == height) return true;
    release_target_later(r);
    @autoreleasepool {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:r->layer.pixelFormat width:width height:height mipmapped:NO];
        d.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModePrivate;
        p.target = [r->device newTextureWithDescriptor:d];
    }
    if (!p.target) { post_fail(r, "has no target"); return false; }
    p.target.label = @"world target";
    p.width  = width;
    p.height = height;
    KV_INFO("metal", "world target %ux%u for post shader '%s'", width, height, name_str(p.shader));
    return true;
}

// One triangle through the game's shader, reading the target. The parameters go in this
// frame's buffer (its slot's last frame has completed, so nothing reads it): not as bytes,
// which are read-only, where the shader's StructuredBuffer comes out of Slang as a writable
// device pointer, and Metal's validation refuses the pair.
void rb_post_draw(Renderer* r, FrameInFlight& f, id<MTLRenderCommandEncoder> enc, const RenderView& view) {
    PostPass& p = r->post;
    id<MTLBuffer>& pb = p.param_buffers[&f - r->frames];
    if (!pb) pb = rb_buffer(r, sizeof p.params, MTLResourceStorageModeShared, "post params");
    if (pb) memcpy(pb.contents, p.params, sizeof p.params);
    PostPushData push{};
    push.size[0] = (f32)p.width;
    push.size[1] = (f32)p.height;
    push.time    = (f32)(time_now() - r->start_time);
    push.tick    = r->world.tick;
    push.count   = p.param_count;
    push.alpha   = view.alpha;
    [enc pushDebugGroup:@"post"];
    [enc setRenderPipelineState:p.pipeline];
    [enc setVertexBytes:&push length:sizeof push atIndex:kPostPush];
    [enc setFragmentBytes:&push length:sizeof push atIndex:kPostPush];
    [enc setFragmentBuffer:pb offset:0 atIndex:kPostParams];
    [enc setFragmentTexture:p.target atIndex:0];
    [enc setFragmentSamplerState:p.sampler atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc popDebugGroup];
}

void rb_post_destroy(Renderer* r) {
    PostPass& p = r->post;
    [p.pipeline release];
    [p.fragment release];
    [p.library release];
    [p.vertex release];
    [p.vertex_lib release];
    [p.sampler release];
    [p.target release];
    for (id<MTLBuffer> b : p.param_buffers) [b release];
    p = PostPass{};
}

} } // namespace kv::KV_RB_NS
