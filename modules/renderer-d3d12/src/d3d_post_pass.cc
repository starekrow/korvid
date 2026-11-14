// Korvid renderer-d3d12: the full-frame pass, as renderer-vulkan's. With a post shader set,
// the layers below the post layer draw into the world's target (the swapchain's size, sRGB,
// so the sprite pipelines draw into it unchanged); the target turns readable; the window's
// drawing begins with one triangle through the game's post shader; the layers above draw
// over it. Its own root signature (post_fragment.slangh's registers): the push block at b0,
// the world at t0 (a descriptor of the frame's in the shader-visible heap), the parameters
// at t1 (the frame's upload buffer), a clamped linear sampler at s0.
#include "renderer_internal.h"
#include "korvid/core/time.h"
#include "korvid/core/format.h"
#include <string.h>

namespace kv {

namespace {

struct PostPushData {
    f32 size[2];
    f32 time;
    u32 tick;
    u32 count;
    f32 alpha;
    u32 pad0, pad1;
};

enum PostRoot : u32 { kPostPush, kPostWorld, kPostParams, kPostRootCount };

bool post_root(Renderer* r) {
    PostPass& p = r->post;
    if (p.root) return true;
    D3D12_DESCRIPTOR_RANGE1 world{};
    world.RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    world.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER1 rp[kPostRootCount]{};
    rp[kPostPush].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[kPostPush].Constants.Num32BitValues = sizeof(PostPushData) / 4;
    rp[kPostWorld].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[kPostWorld].DescriptorTable.NumDescriptorRanges = 1;
    rp[kPostWorld].DescriptorTable.pDescriptorRanges   = &world;
    rp[kPostWorld].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[kPostParams].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rp[kPostParams].Descriptor.ShaderRegister = 1;
    rp[kPostParams].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC s{};
    s.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP; // a bent edge repeats its own pixels
    s.MaxLOD           = 1.0f;
    s.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC1 d{};
    d.NumParameters     = kPostRootCount;
    d.pParameters       = rp;
    d.NumStaticSamplers = 1;
    d.pStaticSamplers   = &s;
    p.root = rb_make_root_signature(r, d, "post");
    return p.root != nullptr;
}

void post_destroy_target_later(Renderer* r) {
    PostPass& p = r->post;
    rb_defer_release(r, p.target);
    p.target = nullptr;
    p.width = p.height = 0;
}

void post_fail(Renderer* r, const char* why) {
    PostPass& p = r->post;
    p.failed = true;
    KV_ERROR("d3d12", "post shader '%s' %s: the frame is drawn without it", name_str(p.shader), why);
}

} // namespace

bool rb_post_set(Renderer* r, i32 layer, Name shader) {
    PostPass& p = r->post;
    p.layer = layer;
    if (shader == p.shader) return !p.failed;
    rb_defer_release(r, p.pipeline);
    p.pipeline = nullptr;
    rb_free_shader(p.fragment);
    p.shader = shader;
    p.failed = false;
    if (shader.is_null()) { post_destroy_target_later(r); return true; }

    if (!post_root(r)) { post_fail(r, "has no root signature"); return false; }
    if (!p.vertex.data && !rb_load_shader(r, "post.dxil.zip", "vs_post", &p.vertex)) { post_fail(r, "has no vertex shader (post.dxil.zip)"); return false; }
    char file[96];
    format(file, sizeof file, "%s.dxil.zip", name_str(shader));
    if (!rb_load_shader(r, file, "fs_main", &p.fragment)) { post_fail(r, "did not load"); return false; }
    return rb_post_create_pipeline(r);
}

bool rb_post_active(const Renderer* r) {
    const PostPass& p = r->post;
    return !p.shader.is_null() && !p.failed && p.pipeline;
}

bool rb_post_create_pipeline(Renderer* r) {
    PostPass& p = r->post;
    if (p.shader.is_null() || p.failed || !p.fragment.data) return false;
    if (p.pipeline) return true;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature                                = p.root;
    d.VS                                            = p.vertex.code();
    d.PS                                            = p.fragment.code();
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask                                    = UINT_MAX;
    d.RasterizerState.FillMode                      = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode                      = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable               = TRUE;
    d.PrimitiveTopologyType                         = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets                              = 1;
    d.RTVFormats[0]                                 = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    d.SampleDesc.Count                              = 1;
    HRESULT hr = r->device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&p.pipeline));
    if (FAILED(hr)) {
        p.pipeline = nullptr;
        KV_ERROR("d3d12", "post pipeline: %s", hr_name(hr));
        rb_drain_messages(r);
        post_fail(r, "does not link with vs_post");
        return false;
    }
    char label[96];
    format(label, sizeof label, "post %s", name_str(p.shader));
    rb_name(p.pipeline, label);
    return true;
}

// The target for the swapchain as it is now: made again when its size moved. That happens
// only after a resize, which waited for the GPU, so every frame's descriptor of it can be
// written here.
bool rb_post_prepare(Renderer* r) {
    PostPass& p = r->post;
    if (!rb_post_active(r)) return false;
    const Swapchain& s = r->swapchain;
    if (p.target && p.width == s.width && p.height == s.height) return true;
    post_destroy_target_later(r);

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = s.width;
    rd.Height           = s.height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    rd.SampleDesc.Count = 1;
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    HRESULT hr = r->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                    IID_PPV_ARGS(&p.target));
    if (FAILED(hr)) { KV_ERROR("d3d12", "world target: %s", hr_name(hr)); post_fail(r, "has no target"); return false; }
    rb_name(p.target, "world target");
    p.target_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    p.width  = s.width;
    p.height = s.height;
    r->device->CreateRenderTargetView(p.target, nullptr, rb_rtv(r, kRtvPost));
    for (u32 f = 0; f < kFramesInFlight; ++f) r->device->CreateShaderResourceView(p.target, nullptr, rb_heap_cpu(r, kHeapPost + f));
    KV_INFO("d3d12", "world target %ux%u for post shader '%s'", p.width, p.height, name_str(p.shader));
    return true;
}

void rb_post_begin_world(Renderer* r, ID3D12GraphicsCommandList* cmd, const RenderView& view) {
    PostPass& p = r->post;
    rb_transition(cmd, p.target, p.target_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    p.target_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rb_rtv(r, kRtvPost);
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const f32 clear[4] = {view.clear_color.r, view.clear_color.g, view.clear_color.b, view.clear_color.a};
    cmd->ClearRenderTargetView(rtv, clear, 0, nullptr);
}

void rb_post_end_world(Renderer* r, ID3D12GraphicsCommandList* cmd) {
    PostPass& p = r->post;
    rb_transition(cmd, p.target, p.target_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    p.target_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}

// The frame's parameters (its fence has passed, so nothing reads them), then one triangle.
void rb_post_draw(Renderer* r, FrameInFlight& f, ID3D12GraphicsCommandList* cmd, const RenderView& view) {
    PostPass& p = r->post;
    u32 fi = (u32)(&f - r->frames);
    memcpy(f.post_params.mapped, p.params, sizeof p.params);
    PostPushData push{};
    push.size[0] = (f32)p.width;
    push.size[1] = (f32)p.height;
    push.time    = (f32)(time_now() - r->start_time);
    push.tick    = r->tick;
    push.count   = p.param_count;
    push.alpha   = view.alpha;
    rb_label_begin(r, cmd, "post");
    cmd->SetPipelineState(p.pipeline);
    cmd->SetGraphicsRootSignature(p.root);
    cmd->SetGraphicsRoot32BitConstants(kPostPush, sizeof push / 4, &push, 0);
    cmd->SetGraphicsRootDescriptorTable(kPostWorld, rb_heap_gpu(r, kHeapPost + fi));
    cmd->SetGraphicsRootShaderResourceView(kPostParams, f.post_params.gpu());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);
    rb_label_end(r, cmd);
}

// The GPU idle.
void rb_post_destroy(Renderer* r) {
    PostPass& p = r->post;
    rb_release(p.pipeline);
    rb_release(p.target);
    rb_release(p.root);
    rb_free_shader(p.vertex);
    rb_free_shader(p.fragment);
    p = PostPass{};
}

} // namespace kv
