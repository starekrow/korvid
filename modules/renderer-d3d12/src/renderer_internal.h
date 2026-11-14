// Korvid renderer-d3d12: what the renderer's source files share. The Renderer struct, the
// GPU-side layouts the shaders mirror (the same bytes as renderer-vulkan's: one set of
// shader sources), and the private functions each file provides:
//   d3d_device.cc       factory, adapter, device, queue, the debug layer's messages
//   d3d_shader.cc       slangc's DXIL containers: <name>.dxil.zip, a blob per entry point
//   d3d_resources.cc    buffers and their states, the frame ring, garbage, textures,
//                       materials, the handle kinds and pools, descriptor heaps, the root
//                       signature
//   d3d_swapchain.cc    the DXGI flip-model swapchain, frames in flight, resizing
//   d3d_sprite_pass.cc  pipelines and the frame's commands: uploads, the compute sort, the
//                       indirect draws
//   d3d_post_pass.cc    the full-frame pass: the world's target, the post pipeline
//   d3d_gpu_profile.cc  timestamps and pipeline statistics, names and event markers
//   renderer.cc         the public API behind korvid/render/renderer.h
// This header and the sources next to it are the only place a D3D12 header is included.
//
// Where it differs from the Vulkan backend, and why:
//   - Bindings are registers: one root signature serves the sort and the sprites, with the
//     push block as root constants, the tables as root descriptors (GPU addresses: a table
//     that grows is rebound, not rewritten) and the bindless textures as one descriptor table
//     per frame in flight in a shader-visible heap. Samplers are static.
//   - The vertex shader cannot see the indirect draw's base instance (SV_StartInstanceLocation
//     is SM 6.8), so the sorted rows are also an instance-rate vertex buffer (sprite.slang).
//   - Resources have states, not layouts and stages: each buffer remembers its own, and a
//     transition is recorded where Vulkan would put a barrier.
//   - Clip space is y up, as Metal's is: the projections are flipped (d3d_sprite_pass.cc).
//   - The swapchain is flip model, so its buffers are UNORM and the render target views sRGB.
#pragma once
#include "korvid/render/renderer.h"
#include "korvid/render/material.h"
#include "korvid/core/array.h"
#include "korvid/core/log.h"
#include "korvid/core/map.h"
#include "korvid/core/name.h"
#include "korvid/core/pool.h"
#include "korvid/core/profile.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
// minwindef.h's 16-bit relics: empty macros that would eat engine names (TimerWheel::far)
// in the files the unity build puts after this one.
#undef near
#undef far
#include "korvid/render/core/layout.h"
#include "korvid/render/world.h"

// What this backend tells the render core (korvid/render/core/core.h).
#define KV_RB_NAME       "d3d12"
#define KV_RB_CLIP_Y_UP  1
#define KV_RB_SHADER_EXT ".dxil.zip"
#define KV_RB_SURFACE    RendererSurface::D3D12
#include "korvid/render/core/core.h"

namespace kv {

// The constants and the GPU-side layouts are render's (korvid/render/core/layout.h).
constexpr u32 kBackBuffers    = 3;    // flip model: one on screen, one queued, one drawing

// The shader-visible heap: each frame's bindless table, then each frame's post input.
constexpr u32 kHeapTextures = 0;
constexpr u32 kHeapPost     = kMaxTextures * kFramesInFlight;
constexpr u32 kHeapSize     = kHeapPost + kFramesInFlight;
// The render target heap: the back buffers, then the post pass's world target.
constexpr u32 kRtvPost      = kBackBuffers;
constexpr u32 kRtvSize      = kBackBuffers + 1;

// The push block (PushData, layout.h) is root constants at b0, 28 of them.

// What D3D12's DrawInstanced takes, and what cs_bucket_scan writes per bucket: the same four
// words as VkDrawIndirectCommand, so the sort is one shader for both.
struct DrawArgs { u32 vertex_count, instance_count, first_vertex, first_instance; };
static_assert(sizeof(DrawArgs) == sizeof(D3D12_DRAW_ARGUMENTS), "the sort writes D3D12_DRAW_ARGUMENTS");

// The root signature's parameters, in order. The registers are binding.slangh's.
enum RootParam : u32 {
    kRootPush,      // b0: PushData, root constants
    kRootSprites,   // t0
    kRootMaterials, // t1
    kRootLayers,    // t7
    kRootSorted,    // u2
    kRootBucketOf,  // u3
    kRootBlockHist, // u4
    kRootSortMeta,  // u5
    kRootIndirect,  // u6
    kRootTextures,  // t0, space1: the frame's bindless table
    kRootSamplers,  // s0, s1: nearest and linear, a table (an array a shader indexes cannot be static samplers)
    kRootCount
};

// ---- CPU-side records ---------------------------------------------------------------------

// A buffer: a committed resource, its heap's kind and the state it was last left in.
// Upload and readback buffers stay mapped and in the one state their heap allows.
struct Buffer {
    ID3D12Resource*       resource = nullptr;
    u64                   size     = 0;
    void*                 mapped   = nullptr;
    D3D12_RESOURCE_STATES state    = D3D12_RESOURCE_STATE_COMMON;
    D3D12_GPU_VIRTUAL_ADDRESS gpu() const { return resource ? resource->GetGPUVirtualAddress() : 0; }
};

struct Texture {
    ID3D12Resource* resource = nullptr;
    u32             width = 0, height = 0;
    TextureFormat   format = TextureFormat::RGBA8_SRGB;
    bool            resident = false;
    Name            name;
};

// A shader blob, copied out of its container: what a pipeline state is made from.
struct ShaderBlob {
    u8*   data = nullptr;
    usize size = 0;
    D3D12_SHADER_BYTECODE code() const { return D3D12_SHADER_BYTECODE{data, size}; }
};

struct GamePipe {
    Name                 shader;
    BlendMode            blend = BlendMode::Alpha;
    ShaderBlob           fragment;
    ID3D12PipelineState* pipeline = nullptr;
    bool                 used = false, failed = false;
};

// Destroyed once the frame that last used it has passed its fence: every D3D12 object is
// a COM object, so one kind does.
struct Garbage { IUnknown* object; };

// A staged texture upload, recorded into the next frame's command list.
struct PendingUpload {
    u32             slot;     // skipped if the slot's resource is no longer `resource`
    ID3D12Resource* resource;
    ID3D12Resource* src;      // the ring, or a one-off staging buffer already queued as garbage
    u64             offset;   // 512-aligned
    u32             width, height, pitch; // pitch: 256-aligned, as a placed footprint needs
};

struct RingSpan {
    ID3D12Resource* resource = nullptr;
    u64             offset = 0;
    void*           ptr    = nullptr;
};

// The frame ring, as renderer-vulkan's: one upload-heap buffer, a frame's spans released
// at its fence.
struct FrameRing {
    Buffer buf;
    u64    head = 0, tail = 0, used = 0, frame_bytes = 0, peak = 0;
    u32    grows = 0;
};

struct FrameInFlight {
    ID3D12CommandAllocator*    allocator = nullptr;
    ID3D12GraphicsCommandList* cmd       = nullptr;
    u64                        fence_value = 0; // the queue's fence reaches it when this frame is done
    Array<Garbage>             garbage;
    u32                        garbage_owned = 0;
    Array<u32>                 dirty_slots;     // texture slots to rewrite in this frame's table
    u64                        ring_end = 0, ring_bytes = 0;
    Buffer                     layers;          // the layers' offsets: upload heap, float4 each
    Buffer                     post_params;     // the post pass's parameters: upload heap
    bool                       submitted = false;
    bool                       gpu_stamped = false, gpu_counted = false;
    u64                        submit_ticks = 0;
    u64                        gpu_pixels = 0;
};

struct GpuProfiler {
    bool             timestamps = false;
    ID3D12QueryHeap* stamps = nullptr; // kGpuMarks per frame in flight
    ID3D12QueryHeap* stats  = nullptr; // one per frame in flight
    Buffer           readback;         // resolved stamps, then stats, per frame
    f64              period_ns = 1.0;
    u64              cal_gpu = 0, cal_cpu = 0;
    u32              cal_age = 0;
    u32              track = 0;
    RendererGpuStats last;
};

struct PostPass {
    Name                 shader;
    i32                  layer = 0;
    bool                 failed = false;
    ShaderBlob           vertex;   // the engine's vs_post
    ShaderBlob           fragment; // the game's fs_main
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pipeline = nullptr;
    ID3D12Resource*      target = nullptr; // the world: the swapchain's size, sRGB
    D3D12_RESOURCE_STATES target_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    u32                  width = 0, height = 0;
    f32                  params[KV_POST_PARAMS * 4] = {};
    u32                  param_count = 0;
};

struct Swapchain {
    IDXGISwapChain3* handle = nullptr;
    HANDLE           waitable = nullptr; // the frame latency object: signaled when a buffer is free
    ID3D12Resource*  buffers[kBackBuffers] = {};
    u32              width = 0, height = 0;
    bool             tearing = false;    // DXGI_FEATURE_PRESENT_ALLOW_TEARING: vsync off really off
};

struct SpriteTables {
    u32    capacity = 0;
    Buffer sprites, bucket_of, block_hist, sorted;
    u32    blocks() const { return capacity / kBlockRows; }
};

struct Renderer {
    Renderer**  owner_slot = nullptr;
    Allocator*  general    = nullptr;
    HWND        window     = nullptr;
    char        base_path[512] = {};
    bool        validation = false;
    bool        vsync      = true;
    bool        markers    = false; // event markers for PIX and the like

    IDXGIFactory4*      factory = nullptr;
    IDXGIAdapter1*      adapter = nullptr;
    ID3D12Device*       device  = nullptr;
    ID3D12CommandQueue* queue   = nullptr;
    ID3D12Fence*        fence   = nullptr;
    HANDLE              fence_event = nullptr;
    u64                 fence_next  = 1;   // the value the next submit signals
    ID3D12InfoQueue*    info_queue  = nullptr; // the debug layer's messages, drained each frame
    char                adapter_name[128] = {};
    GpuProfiler         gpu_prof;

    Swapchain swapchain;
    bool      swapchain_dirty = true;
    u32       want_width = 0, want_height = 0;

    ID3D12DescriptorHeap* heap = nullptr;     // shader visible: textures, post inputs
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    ID3D12DescriptorHeap* sampler_heap = nullptr; // shader visible: the two samplers
    u32                   heap_step = 0, rtv_step = 0;
    ID3D12RootSignature*  root = nullptr;     // the sort's and the sprites'
    ID3D12CommandSignature* draw_signature = nullptr; // ExecuteIndirect: one draw per command
    ShaderBlob            vs_sprite, fs_sprite;
    ShaderBlob            cs_blobs[5];
    ID3D12PipelineState*  pipelines[kBlendCount] = {};
    GamePipe              game_pipes[kGameSlots];
    PostPass              post;
    f64                   start_time = 0.0;
    ID3D12PipelineState*  cs_count = nullptr, *cs_scan = nullptr, *cs_bucket_scan = nullptr,
                         *cs_scatter = nullptr, *cs_key_sort = nullptr;

    RenderWorld world; // the sprites, the materials, what is dirty, the bucket counts (korvid/render/world.h)
    Pool<Texture>        textures;
    Handle               default_material;
    u16                  texture_kind = 0, material_kind = 0;
    Array<PendingUpload> uploads;

    Map<u64, Handle>     texture_by_name;
    u32                  textures_made = 0;
    f32*                 check_keys = nullptr;
    u64                  rows_uploaded  = 0;

    SpriteTables tables;
    Buffer       material_table;
    Buffer       sort_meta;
    Buffer       indirect;
    FrameRing    ring;

    FrameInFlight frames[kFramesInFlight];
    u32           frame_index = 0;
    u64           frame_count = 0;

    char   shot_path[512] = {};
    bool   shot_pending = false, shot_recorded = false;
    Buffer shot_buffer;
    u32    shot_width = 0, shot_height = 0, shot_pitch = 0;
    bool   check_pending = false, check_recorded = false;
    Buffer check_buffer;
    u32    check_rows = 0;
    u32    checks_passed = 0, checks_failed = 0;

    f64 last_wait = 0.0;
    u32 validation_errors = 0, validation_warnings = 0;
};

const char* hr_name(HRESULT hr); // "E_INVALIDARG", "DXGI_ERROR_DEVICE_REMOVED", or the number

#define D3D_CHECK_RET(call, ret)                                                          \
    do {                                                                                  \
        HRESULT kv_hr_ = (call);                                                          \
        if (FAILED(kv_hr_)) {                                                             \
            KV_ERROR("d3d12", "%s failed: %s (%s:%d)", #call, hr_name(kv_hr_), __FILE__,  \
                     __LINE__);                                                           \
            return ret;                                                                   \
        }                                                                                 \
    } while (0)
#define D3D_CHECK(call) D3D_CHECK_RET(call, false)

template <class T> void rb_release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ---- d3d_device.cc ------------------------------------------------------------------------
bool rb_create_device(Renderer* r);
void rb_destroy_device(Renderer* r);
void rb_drain_messages(Renderer* r); // the debug layer's, into the log, counted
bool rb_wait_fence(Renderer* r, u64 value); // blocks until the queue's fence reaches it
bool rb_wait_idle(Renderer* r);             // signals and waits: nothing in flight after it

// ---- d3d_shader.cc ------------------------------------------------------------------------
// <exe dir>/shaders/<file>: one entry point's blob out of slangc's container. False (logged)
// when the file or the entry is not there.
bool rb_load_shader(Renderer* r, const char* file, const char* entry, ShaderBlob* out);
bool rb_load_shader_entries(Renderer* r, const char* file, const char* const* entries, ShaderBlob* outs, u32 n);
void rb_free_shader(ShaderBlob& b);

// ---- d3d_resources.cc ---------------------------------------------------------------------
bool rb_create_buffer(Renderer* r, D3D12_HEAP_TYPE heap, u64 size, bool uav, Buffer& out, const char* name);
void rb_destroy_buffer(Buffer& b);
void rb_defer_release(Renderer* r, IUnknown* object);
void rb_defer_destroy_buffer(Renderer* r, Buffer& b);
void rb_run_garbage(Renderer* r, FrameInFlight& f);
// Records a transition when the buffer is not already in `to`; a UAV barrier when it is a
// UAV staying one (one dispatch's writes before the next one's reads).
void rb_transition(ID3D12GraphicsCommandList* cmd, Buffer& b, D3D12_RESOURCE_STATES to);
void rb_transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);

bool     rb_ring_init(Renderer* r, u64 bytes);
RingSpan rb_ring_alloc(Renderer* r, u64 bytes, u64 align);
void     rb_ring_submitted(Renderer* r, FrameInFlight& f);
void     rb_ring_release(Renderer* r, FrameInFlight& f);

DXGI_FORMAT rb_dxgi_format(TextureFormat f);
bool rb_create_texture_resource(Renderer* r, Texture& t);
bool rb_stage_texture(Renderer* r, u32 slot, Texture& t, const u8* pixels);
void rb_defer_destroy_texture(Renderer* r, Texture& t);
bool rb_create_default_texture(Renderer* r);
void rb_mark_slot_dirty(Renderer* r, u32 slot);
void rb_write_texture_slot(Renderer* r, u32 frame, u32 slot);
D3D12_GPU_DESCRIPTOR_HANDLE rb_heap_gpu(Renderer* r, u32 index);
D3D12_CPU_DESCRIPTOR_HANDLE rb_heap_cpu(Renderer* r, u32 index);
D3D12_CPU_DESCRIPTOR_HANDLE rb_rtv(Renderer* r, u32 index);


bool rb_create_resource_pools(Renderer* r);
bool rb_create_tables(Renderer* r, u32 rows);
bool rb_create_static_buffers(Renderer* r);
bool rb_create_heaps_and_root(Renderer* r);
// A root signature from its description, version 1.1. Null (logged) on failure.
ID3D12RootSignature* rb_make_root_signature(Renderer* r, const D3D12_ROOT_SIGNATURE_DESC1& desc, const char* name);
void rb_destroy_resources(Renderer* r);

// ---- d3d_swapchain.cc ---------------------------------------------------------------------
bool rb_create_frames(Renderer* r);
void rb_destroy_frames(Renderer* r);
bool rb_recreate_swapchain(Renderer* r); // waits for the GPU: DXGI resizes only idle buffers
void rb_destroy_swapchain(Renderer* r);

// ---- d3d_sprite_pass.cc -------------------------------------------------------------------
bool rb_load_shaders(Renderer* r);
bool rb_create_pipelines(Renderer* r);
bool rb_create_compute_pipelines(Renderer* r);
void rb_destroy_pipelines(Renderer* r);
bool rb_record_frame(Renderer* r, FrameInFlight& f, bool have_image, u32 image_index, const RenderView& view);
u32  rb_record_draws(Renderer* r, ID3D12GraphicsCommandList* cmd, u32 first_bucket, u32 end_bucket);
bool rb_create_game_pipeline(Renderer* r, GamePipe& gp);
bool rb_write_screenshot(Renderer* r);
bool rb_verify_sort(Renderer* r);

// ---- d3d_post_pass.cc ---------------------------------------------------------------------
bool rb_post_set(Renderer* r, i32 layer, Name shader);
bool rb_post_active(const Renderer* r);
bool rb_post_prepare(Renderer* r);
void rb_post_begin_world(Renderer* r, ID3D12GraphicsCommandList* cmd, const RenderView& view);
void rb_post_end_world(Renderer* r, ID3D12GraphicsCommandList* cmd);
void rb_post_draw(Renderer* r, FrameInFlight& f, ID3D12GraphicsCommandList* cmd, const RenderView& view);
bool rb_post_create_pipeline(Renderer* r);
void rb_post_destroy(Renderer* r);

// ---- d3d_gpu_profile.cc -------------------------------------------------------------------
void rb_name(ID3D12Object* object, const char* name);
void rb_label_begin(Renderer* r, ID3D12GraphicsCommandList* cmd, const char* name);
void rb_label_end(Renderer* r, ID3D12GraphicsCommandList* cmd);
bool rb_gpu_profile_create(Renderer* r);
void rb_gpu_profile_destroy(Renderer* r);
void rb_gpu_frame_begin(Renderer* r, FrameInFlight& f, ID3D12GraphicsCommandList* cmd);
void rb_gpu_mark(Renderer* r, FrameInFlight& f, ID3D12GraphicsCommandList* cmd, GpuMark mark);
void rb_gpu_stats_begin(Renderer* r, FrameInFlight& f, ID3D12GraphicsCommandList* cmd);
void rb_gpu_stats_end(Renderer* r, FrameInFlight& f, ID3D12GraphicsCommandList* cmd);
void rb_gpu_frame_end(Renderer* r, FrameInFlight& f, ID3D12GraphicsCommandList* cmd); // resolves the queries
void rb_gpu_collect(Renderer* r, FrameInFlight& f);

// ---- renderer.cc --------------------------------------------------------------------------
u32  rb_material_slot(Renderer* r, Name shader, BlendMode blend);

} // namespace kv
