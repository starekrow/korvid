// Korvid renderer-d3d12: the sprite pass. Pipelines, and the frame's commands: texture
// uploads and table copies out of the frame ring, the compute sort (count, scan, bucket
// scan, scatter, key sort: the render interface's shaders, unchanged), and one
// ExecuteIndirect per run of buckets. renderer-vulkan's vk_sprite_pass.cc, with barriers as
// state transitions and bindings as root parameters.
#include "renderer_internal.h"
#include "korvid/core/arena.h"
#include "korvid/platform/io.h"
#include "korvid/core/format.h"
#include "korvid/core/profile.h"
#include "korvid/core/time.h"
#include "korvid/math/scalar.h"
#include <string.h>

namespace kv {

namespace {

constexpr D3D12_RESOURCE_STATES kShaderRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
constexpr DXGI_FORMAT kTargetFormat = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

D3D12_RENDER_TARGET_BLEND_DESC blend_state(BlendMode mode) {
    D3D12_RENDER_TARGET_BLEND_DESC s{};
    s.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    s.BlendOp = s.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    s.SrcBlend = s.SrcBlendAlpha = D3D12_BLEND_ONE;
    s.DestBlend = s.DestBlendAlpha = D3D12_BLEND_ZERO;
    s.LogicOp = D3D12_LOGIC_OP_NOOP;
    switch (mode) {
    case BlendMode::Alpha:
        s.BlendEnable    = TRUE;
        s.SrcBlend       = D3D12_BLEND_SRC_ALPHA;
        s.DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
        s.SrcBlendAlpha  = D3D12_BLEND_ONE;
        s.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        break;
    case BlendMode::Additive:
        s.BlendEnable    = TRUE;
        s.SrcBlend       = D3D12_BLEND_SRC_ALPHA;
        s.DestBlend      = D3D12_BLEND_ONE;
        s.SrcBlendAlpha  = D3D12_BLEND_ONE;
        s.DestBlendAlpha = D3D12_BLEND_ONE;
        break;
    default:
        break;
    }
    return s;
}

// Every UAV's writes before every later access: what Vulkan's compute-to-compute memory
// barrier is.
void uav_barrier(ID3D12GraphicsCommandList* cmd) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    cmd->ResourceBarrier(1, &b);
}

const char* const kComputeEntries[5] = {"cs_count", "cs_scan", "cs_bucket_scan", "cs_scatter", "cs_key_sort"};

ID3D12PipelineState* create_compute(Renderer* r, const ShaderBlob& cs, const char* label) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
    d.pRootSignature = r->root;
    d.CS             = cs.code();
    ID3D12PipelineState* out = nullptr;
    HRESULT hr = r->device->CreateComputePipelineState(&d, IID_PPV_ARGS(&out));
    if (FAILED(hr)) { KV_ERROR("d3d12", "compute pipeline %s: %s", label, hr_name(hr)); return nullptr; }
    rb_name(out, label);
    return out;
}

// ---- uploads --------------------------------------------------------------------------------

void record_texture_uploads(Renderer* r, ID3D12GraphicsCommandList* cmd) {
    for (const PendingUpload& up : r->uploads) {
        if (!r->textures.is_alive(up.slot) || r->textures.items[up.slot].resource != up.resource) continue;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource        = up.resource;
        dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = up.src;
        src.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset             = up.offset;
        src.PlacedFootprint.Footprint.Format   = rb_dxgi_format(r->textures.items[up.slot].format);
        src.PlacedFootprint.Footprint.Width    = up.width;
        src.PlacedFootprint.Footprint.Height   = up.height;
        src.PlacedFootprint.Footprint.Depth    = 1;
        src.PlacedFootprint.Footprint.RowPitch = up.pitch;
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr); // made in COPY_DEST
        rb_transition(cmd, up.resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    r->uploads.clear();
}

// Walks a dirty bitmap over [0, rows) and calls copy_run(a, b) for every run of set bits,
// joining runs separated by fewer than `join` clean rows (uploading a clean row again is
// cheaper than another copy region). Clears the bits.
template <class F>
u32 for_dirty_runs(Array<u64>& bits, u32 rows, u32 join, F copy_run) {
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

// Dirty sprite rows and material rows, staged into the ring and copied into the tables.
bool record_table_copies(Renderer* r, ID3D12GraphicsCommandList* cmd) {
    bool ok = true;
    rb_transition(cmd, r->tables.sprites, D3D12_RESOURCE_STATE_COPY_DEST);
    rb_transition(cmd, r->material_table, D3D12_RESOURCE_STATE_COPY_DEST);
    u32 rows = for_dirty_runs(r->sprite_dirty, r->sprites.len, 32, [&](u32 a, u32 b) {
        u64 bytes = (u64)(b - a) * sizeof(GpuSprite);
        RingSpan span = rb_ring_alloc(r, bytes, 16);
        if (!span.ptr) { ok = false; return; }
        GpuSprite* dst = (GpuSprite*)span.ptr;
        for (u32 i = a; i < b; ++i) {
            if (r->sprites.is_alive(i)) dst[i - a] = r->sprites.items[i].gpu;
            else dst[i - a] = GpuSprite{};
        }
        cmd->CopyBufferRegion(r->tables.sprites.resource, (u64)a * sizeof(GpuSprite), span.resource, span.offset, bytes);
    });
    r->rows_uploaded += rows;
    KV_COUNTER("sprite rows uploaded", rows);

    for_dirty_runs(r->material_dirty, kMaxMaterials, 8, [&](u32 a, u32 b) {
        u64 bytes = (u64)(b - a) * sizeof(GpuMaterial);
        RingSpan span = rb_ring_alloc(r, bytes, 16);
        if (!span.ptr) { ok = false; return; }
        GpuMaterial* dst = (GpuMaterial*)span.ptr;
        for (u32 i = a; i < b; ++i) rb_material_row(r, i, dst[i - a]);
        cmd->CopyBufferRegion(r->material_table.resource, (u64)a * sizeof(GpuMaterial), span.resource, span.offset, bytes);
    });
    rb_transition(cmd, r->tables.sprites, kShaderRead);
    rb_transition(cmd, r->material_table, kShaderRead);
    return ok;
}

// The root parameters the sort and the sprites read, on one bind point or the other.
template <class SetSrv, class SetUav>
void bind_tables(Renderer* r, FrameInFlight& f, SetSrv srv, SetUav uav, bool writes) {
    srv(kRootSprites, r->tables.sprites.gpu());
    srv(kRootMaterials, r->material_table.gpu());
    srv(kRootLayers, f.layers.gpu());
    if (!writes) return;
    uav(kRootSorted, r->tables.sorted.gpu());
    uav(kRootBucketOf, r->tables.bucket_of.gpu());
    uav(kRootBlockHist, r->tables.block_hist.gpu());
    uav(kRootSortMeta, r->sort_meta.gpu());
    uav(kRootIndirect, r->indirect.gpu());
}

// ---- the sort -------------------------------------------------------------------------------

void record_sort(Renderer* r, FrameInFlight& f, ID3D12GraphicsCommandList* cmd, const PushData& push) {
    u32 blocks = (r->sprites.len + kBlockRows - 1) / kBlockRows;
    const D3D12_RESOURCE_STATES uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    rb_transition(cmd, r->tables.sorted, uav);
    rb_transition(cmd, r->tables.bucket_of, uav);
    rb_transition(cmd, r->tables.block_hist, uav);
    rb_transition(cmd, r->sort_meta, uav);
    rb_transition(cmd, r->indirect, uav);

    cmd->SetComputeRootSignature(r->root);
    cmd->SetComputeRoot32BitConstants(kRootPush, sizeof push / 4, &push, 0);
    bind_tables(r, f,
                [&](u32 i, D3D12_GPU_VIRTUAL_ADDRESS a) { cmd->SetComputeRootShaderResourceView(i, a); },
                [&](u32 i, D3D12_GPU_VIRTUAL_ADDRESS a) { cmd->SetComputeRootUnorderedAccessView(i, a); }, true);
    rb_label_begin(r, cmd, "count");
    cmd->SetPipelineState(r->cs_count);
    cmd->Dispatch(blocks, 1, 1);
    rb_label_end(r, cmd);
    uav_barrier(cmd);
    rb_label_begin(r, cmd, "scan");
    cmd->SetPipelineState(r->cs_scan);
    cmd->Dispatch(kBucketCount / kComputeGroup, 1, 1);
    rb_label_end(r, cmd);
    uav_barrier(cmd);
    rb_label_begin(r, cmd, "bucket scan");
    cmd->SetPipelineState(r->cs_bucket_scan);
    cmd->Dispatch(1, 1, 1);
    rb_label_end(r, cmd);
    uav_barrier(cmd);
    rb_label_begin(r, cmd, "scatter");
    cmd->SetPipelineState(r->cs_scatter);
    cmd->Dispatch(blocks, 1, 1);
    rb_label_end(r, cmd);
    bool keyed = false;
    for (u32 b = 0; b < kBucketCount; ++b) {
        if (!r->keyed_counts[b] || !r->bucket_counts[b]) continue;
        if (!keyed) {
            uav_barrier(cmd);
            rb_label_begin(r, cmd, "key sort");
            cmd->SetPipelineState(r->cs_key_sort);
            keyed = true;
        }
        cmd->SetComputeRoot32BitConstant(kRootPush, b, offsetof(PushData, key_bucket) / 4);
        cmd->Dispatch(1, 1, 1);
    }
    if (keyed) rb_label_end(r, cmd);
    uav_barrier(cmd);
}

// The sort's outputs, ready to draw from: the draws' arguments and the rows as instance data.
void finish_sort(Renderer* r, ID3D12GraphicsCommandList* cmd) {
    rb_transition(cmd, r->indirect, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    rb_transition(cmd, r->tables.sorted, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
}

// ---- the sort check -------------------------------------------------------------------------------

// Copies sorted[], bucket_of[] and the sort meta into a host buffer, after the sort.
// Each row's key as this frame computes it (cs_key_sort's formula on the CPU's copy of the
// rows), kept for the check: the rows may change before the frame's fence passes.
void snapshot_keys(Renderer* r, f32 alpha) {
    u32 rows = r->sprites.len;
    free_in(mem_general(), r->check_keys, sizeof(f32) * (r->check_rows ? r->check_rows : 1));
    r->check_keys = (f32*)alloc_in(mem_general(), sizeof(f32) * (rows ? rows : 1));
    for (u32 i = 0; i < rows; ++i) {
        const GpuSprite& g = r->sprites.items[i].gpu;
        u32 mode = (g.flags >> kSpriteSortShift) & 3;
        f32 t = g.stamp == r->tick ? alpha : 1.0f;
        f32 y = g.prev[1] + (g.cur[1] - g.prev[1]) * t;
        r->check_keys[i] = !r->sprites.is_alive(i) ? 0.0f : mode == 1 ? g.order : mode == 2 ? -(y + g.order) : 0.0f;
    }
}

void record_sort_check(Renderer* r, ID3D12GraphicsCommandList* cmd) {
    u32 rows = r->sprites.len;
    u64 meta_bytes = sizeof(u32) * (2 * kBucketCount + 4);
    u64 bytes = sizeof(u32) * (u64)rows * 2 + meta_bytes;
    if (r->check_buffer.resource) rb_defer_destroy_buffer(r, r->check_buffer);
    if (!rb_create_buffer(r, D3D12_HEAP_TYPE_READBACK, bytes, false, r->check_buffer, "sort check")) { r->check_pending = false; return; }
    rb_transition(cmd, r->tables.sorted, D3D12_RESOURCE_STATE_COPY_SOURCE);
    rb_transition(cmd, r->tables.bucket_of, D3D12_RESOURCE_STATE_COPY_SOURCE);
    rb_transition(cmd, r->sort_meta, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(r->check_buffer.resource, 0, r->tables.sorted.resource, 0, sizeof(u32) * (u64)rows);
    cmd->CopyBufferRegion(r->check_buffer.resource, sizeof(u32) * (u64)rows, r->tables.bucket_of.resource, 0, sizeof(u32) * (u64)rows);
    cmd->CopyBufferRegion(r->check_buffer.resource, sizeof(u32) * (u64)rows * 2, r->sort_meta.resource, 0, meta_bytes);
    r->check_rows     = rows;
    r->check_pending  = false;
    r->check_recorded = true;
}

// ---- screenshots ---------------------------------------------------------------------------------

// Copies the finished back buffer into a readback buffer, at the pitch a footprint needs,
// and leaves it a render target again.
void record_screenshot(Renderer* r, ID3D12GraphicsCommandList* cmd, ID3D12Resource* image) {
    const Swapchain& s = r->swapchain;
    u32 pitch = (s.width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    if (r->shot_buffer.resource) rb_defer_destroy_buffer(r, r->shot_buffer);
    if (!rb_create_buffer(r, D3D12_HEAP_TYPE_READBACK, (u64)pitch * s.height, false, r->shot_buffer, "screenshot")) { r->shot_pending = false; return; }
    r->shot_width  = s.width;
    r->shot_height = s.height;
    r->shot_pitch  = pitch;
    rb_transition(cmd, image, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = image;
    src.Type      = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = r->shot_buffer.resource;
    dst.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_B8G8R8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width    = s.width;
    dst.PlacedFootprint.Footprint.Height   = s.height;
    dst.PlacedFootprint.Footprint.Depth    = 1;
    dst.PlacedFootprint.Footprint.RowPitch = pitch;
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    rb_transition(cmd, image, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    r->shot_pending  = false;
    r->shot_recorded = true;
}

} // namespace

// One ExecuteIndirect per run of buckets, as renderer-vulkan's multi-draw: from a non-empty
// bucket to the last non-empty bucket of the same pipeline slot before one of another slot
// intervenes; empty buckets between draw nothing.
u32 rb_record_draws(Renderer* r, ID3D12GraphicsCommandList* cmd, u32 first_bucket, u32 end_bucket) {
    u32 draws = 0;
    u32 b = first_bucket;
    while (b < end_bucket) {
        if (!r->bucket_counts[b]) { ++b; continue; }
        u32 slot = b % kSlotCount;
        u32 first = b, last = b;
        for (u32 n = b + 1; n < end_bucket; ++n) {
            if (!r->bucket_counts[n]) continue;
            if (n % kSlotCount != slot) break;
            last = n;
        }
        const GamePipe* gp = slot >= kBlendCount ? &r->game_pipes[slot - kBlendCount] : nullptr;
        ID3D12PipelineState* pipe = gp ? (gp->pipeline ? gp->pipeline : r->pipelines[(u32)gp->blend]) : r->pipelines[slot];
        char label[64];
        if (r->markers) {
            format(label, sizeof label, "layers %d..%d %s", (i32)(first / kSlotCount) + kLayerMin, (i32)(last / kSlotCount) + kLayerMin,
                   gp ? name_str(gp->shader) : blend_mode_name((BlendMode)slot));
            rb_label_begin(r, cmd, label);
        }
        cmd->SetPipelineState(pipe);
        cmd->ExecuteIndirect(r->draw_signature, last - first + 1, r->indirect.resource, (u64)first * sizeof(DrawArgs), nullptr, 0);
        ++draws;
        if (r->markers) rb_label_end(r, cmd);
        b = last + 1;
    }
    return draws;
}

// ---- shaders and pipelines --------------------------------------------------------------------

bool rb_load_shaders(Renderer* r) {
    const char* sprite[2] = {"vs_main", "fs_main"};
    ShaderBlob sb[2];
    if (!rb_load_shader_entries(r, "sprite.dxil.zip", sprite, sb, 2)) return false;
    r->vs_sprite = sb[0];
    r->fs_sprite = sb[1];
    return rb_load_shader_entries(r, "sprite_sort.dxil.zip", kComputeEntries, r->cs_blobs, 5);
}

bool rb_create_compute_pipelines(Renderer* r) {
    ID3D12PipelineState** out[5] = {&r->cs_count, &r->cs_scan, &r->cs_bucket_scan, &r->cs_scatter, &r->cs_key_sort};
    for (u32 i = 0; i < 5; ++i)
        if (!(*out[i] = create_compute(r, r->cs_blobs[i], kComputeEntries[i]))) return false;
    return true;
}

namespace {

// A sprite pipeline: the engine's vertex shader, a fragment shader (the engine's or a game's),
// a blend. The sorted rows come in as instance data: the ROW attribute, one u32 an instance.
ID3D12PipelineState* make_sprite_pipeline(Renderer* r, const ShaderBlob& fragment, BlendMode mode, const char* label) {
    D3D12_INPUT_ELEMENT_DESC row{"ROW", 0, DXGI_FORMAT_R32_UINT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature                  = r->root;
    d.VS                              = r->vs_sprite.code();
    d.PS                              = fragment.code();
    d.BlendState.RenderTarget[0]      = blend_state(mode);
    d.SampleMask                      = UINT_MAX;
    d.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.InputLayout                     = {&row, 1};
    d.PrimitiveTopologyType           = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets                = 1;
    d.RTVFormats[0]                   = kTargetFormat;
    d.SampleDesc.Count                = 1;
    ID3D12PipelineState* out = nullptr;
    HRESULT hr = r->device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&out));
    if (FAILED(hr)) { KV_ERROR("d3d12", "pipeline %s: %s", label, hr_name(hr)); rb_drain_messages(r); return nullptr; }
    rb_name(out, label);
    return out;
}

} // namespace

bool rb_create_pipelines(Renderer* r) {
    for (u32 i = 0; i < kBlendCount; ++i) {
        r->pipelines[i] = make_sprite_pipeline(r, r->fs_sprite, (BlendMode)i, blend_mode_name((BlendMode)i));
        if (!r->pipelines[i]) return false;
    }
    for (GamePipe& gp : r->game_pipes)
        if (gp.used && !gp.failed) rb_create_game_pipeline(r, gp);
    rb_post_create_pipeline(r);
    return true;
}

// The format is fixed, so a game pipeline is made as soon as its shader loads.
bool rb_create_game_pipeline(Renderer* r, GamePipe& gp) {
    if (gp.failed || !gp.fragment.data) return false;
    if (gp.pipeline || !r->root || !r->vs_sprite.data) return true;
    char label[96];
    format(label, sizeof label, "%s %s", name_str(gp.shader), blend_mode_name(gp.blend));
    gp.pipeline = make_sprite_pipeline(r, gp.fragment, gp.blend, label);
    if (!gp.pipeline) {
        gp.failed = true;
        KV_ERROR("d3d12", "game shader '%s' does not link with the sprite shader: its sprites draw with the engine's", name_str(gp.shader));
    }
    return gp.pipeline != nullptr;
}

u32 rb_material_slot(Renderer* r, Name shader, BlendMode blend) {
    u32 engine = (u32)blend < kBlendCount ? (u32)blend : (u32)BlendMode::Alpha;
    if (shader.is_null()) return engine;
    u32 free_slot = kGameSlots;
    for (u32 i = 0; i < kGameSlots; ++i) {
        GamePipe& gp = r->game_pipes[i];
        if (gp.used && gp.shader == shader && gp.blend == blend) return gp.failed ? engine : kBlendCount + i;
        if (!gp.used && free_slot == kGameSlots) free_slot = i;
    }
    if (free_slot == kGameSlots) {
        KV_ERROR("d3d12", "game shader '%s': all %u game pipeline slots are taken; drawn with the engine's", name_str(shader), kGameSlots);
        return engine;
    }
    GamePipe& gp = r->game_pipes[free_slot];
    gp = GamePipe{};
    gp.used   = true;
    gp.shader = shader;
    gp.blend  = blend;
    char file[96];
    format(file, sizeof file, "%s.dxil.zip", name_str(shader));
    if (!rb_load_shader(r, file, "fs_main", &gp.fragment)) {
        gp.failed = true;
        KV_ERROR("d3d12", "game shader '%s' did not load: its sprites draw with the engine's", name_str(shader));
        return engine;
    }
    rb_create_game_pipeline(r, gp);
    return gp.failed ? engine : kBlendCount + free_slot;
}

void rb_destroy_pipelines(Renderer* r) {
    for (ID3D12PipelineState*& p : r->pipelines) rb_release(p);
    ID3D12PipelineState** cs[] = {&r->cs_count, &r->cs_scan, &r->cs_bucket_scan, &r->cs_scatter, &r->cs_key_sort};
    for (ID3D12PipelineState** p : cs) rb_release(*p);
    for (GamePipe& gp : r->game_pipes) {
        rb_release(gp.pipeline);
        rb_free_shader(gp.fragment);
        gp = GamePipe{};
    }
    rb_free_shader(r->vs_sprite);
    rb_free_shader(r->fs_sprite);
    for (ShaderBlob& b : r->cs_blobs) rb_free_shader(b);
}

// Every bucket's run in sorted[] holds exactly the rows whose bucket it is, in ascending
// row order, once each; the counts agree with the CPU's where nothing was culled.
bool rb_verify_sort(Renderer* r) {
    r->check_recorded = false;
    if (!r->check_buffer.mapped) return false;
    u32 rows = r->check_rows;
    const u32* sorted    = (const u32*)r->check_buffer.mapped;
    const u32* bucket_of = sorted + rows;
    const u32* meta      = bucket_of + rows;
    const u32* totals    = meta;
    const u32* starts    = meta + kBucketCount;
    u32 visible_rows = 0, bad = 0, seen_total = 0;
    MemScope scope;
    u8* seen = arena_push_n<u8>(*scope.arena, rows ? rows : 1);
    memset(seen, 0, rows);
    for (u32 i = 0; i < rows; ++i) if (bucket_of[i] != 0xFFFFFFFFu) ++visible_rows;
    u32 expect_start = 0;
    for (u32 b = 0; b < kBucketCount; ++b) {
        if (starts[b] != expect_start) { ++bad; KV_ERROR("d3d12", "sort check: bucket %u starts at %u, expected %u", b, starts[b], expect_start); }
        expect_start += totals[b];
        if (totals[b] > r->bucket_counts[b]) { ++bad; KV_ERROR("d3d12", "sort check: bucket %u has %u rows on the GPU, %u on the CPU", b, totals[b], r->bucket_counts[b]); }
        // In (key, row) order: a bucket with no keyed rows has every key 0, so that is row
        // order. Keys within a hair of each other may come in either order (the GPU and the
        // CPU interpolate with different roundings); past kKeySortMax rows, row order resumes.
        u32 prev = 0;
        for (u32 k = 0; k < totals[b]; ++k) {
            u32 row = sorted[starts[b] + k];
            if (row >= rows || bucket_of[row] != b) { if (bad++ < 8) KV_ERROR("d3d12", "sort check: bucket %u entry %u is row %u (bucket %u)", b, k, row, row < rows ? bucket_of[row] : ~0u); continue; }
            f32 ka = r->check_keys && k ? r->check_keys[prev] : 0.0f, kb = r->check_keys ? r->check_keys[row] : 0.0f;
            bool ordered = true;
            if (!k || k == kKeySortMax) ordered = true;                          // the first, or the first unsorted
            else if (k > kKeySortMax || !r->check_keys || ka == kb) ordered = row > prev;
            else if (kv::fabs(ka - kb) <= 1e-3f * (1.0f + kv::fabs(ka))) ordered = true; // a near tie: either order
            else ordered = ka < kb;
            if (!ordered) { if (bad++ < 8) KV_ERROR("d3d12", "sort check: bucket %u entry %u row %u (key %g) after row %u (key %g)", b, k, row, kb, prev, ka); }
            if (seen[row]++) { if (bad++ < 8) KV_ERROR("d3d12", "sort check: row %u listed twice", row); }
            prev = row;
            ++seen_total;
        }
    }
    if (seen_total != visible_rows || meta[2 * kBucketCount] != visible_rows) {
        ++bad;
        KV_ERROR("d3d12", "sort check: %u rows visible, %u listed, GPU says %u", visible_rows, seen_total, meta[2 * kBucketCount]);
    }
    if (bad) ++r->checks_failed; else ++r->checks_passed;
    KV_INFO("d3d12", "sort check: %u rows, %u visible, %u buckets in use: %s", rows, visible_rows,
            [&] { u32 n = 0; for (u32 b = 0; b < kBucketCount; ++b) n += totals[b] != 0; return n; }(), bad ? "FAILED" : "ok");
    rb_defer_destroy_buffer(r, r->check_buffer);
    return bad == 0;
}

// Binary PPM, RGB, from the BGRA readback (sRGB-encoded bytes, as the window shows them).
bool rb_write_screenshot(Renderer* r) {
    r->shot_recorded = false;
    if (!r->shot_buffer.mapped) return false;
    IoFileWriter f;
    if (!f.open(r->shot_path)) { KV_ERROR("d3d12", "cannot write screenshot '%s'", r->shot_path); rb_defer_destroy_buffer(r, r->shot_buffer); return false; }
    f.print("P6\n%u %u\n255\n", r->shot_width, r->shot_height);
    const u8* px = (const u8*)r->shot_buffer.mapped;
    MemScope scope;
    u8* row = arena_push_n<u8>(*scope.arena, r->shot_width * 3);
    for (u32 y = 0; y < r->shot_height; ++y) {
        const u8* src = px + (usize)y * r->shot_pitch;
        for (u32 x = 0; x < r->shot_width; ++x, src += 4) {
            row[x * 3 + 0] = src[2];
            row[x * 3 + 1] = src[1];
            row[x * 3 + 2] = src[0];
        }
        f.write(row, r->shot_width * 3);
    }
    if (!f.close()) KV_ERROR("d3d12", "screenshot '%s': write failed (errno %d)", r->shot_path, f.error);
    else KV_INFO("d3d12", "screenshot '%s' %ux%u", r->shot_path, r->shot_width, r->shot_height);
    rb_defer_destroy_buffer(r, r->shot_buffer);
    return true;
}

// ---- the frame -----------------------------------------------------------------------------------

namespace {

// D3D12's clip space has y up, as Metal's does; ortho2d targets Vulkan's, y down. Negating
// the y row turns one into the other.
affine2 d3d_clip(affine2 m) {
    m.b = -m.b; m.d = -m.d; m.ty = -m.ty;
    return m;
}

} // namespace

bool rb_record_frame(Renderer* r, FrameInFlight& f, bool have_image, u32 image_index, const RenderView& view) {
    KV_ZONE("record");
    Swapchain& s = r->swapchain;
    ID3D12GraphicsCommandList* cmd = f.cmd;
    u32 fi = (u32)(&f - r->frames);

    D3D_CHECK(f.allocator->Reset()); // its last list has passed its fence
    D3D_CHECK(cmd->Reset(f.allocator, nullptr));
    ID3D12DescriptorHeap* heaps[] = {r->heap, r->sampler_heap};
    cmd->SetDescriptorHeaps(2, heaps);
    rb_gpu_frame_begin(r, f, cmd);
    f.gpu_pixels = have_image ? (u64)s.width * s.height : 0;

    // Uploads first: textures, then the rows the ticks changed. Queue order and the tables'
    // transitions keep this frame's copies after the last frame's reads.
    rb_label_begin(r, cmd, "upload");
    record_texture_uploads(r, cmd);
    if (!record_table_copies(r, cmd)) return false;
    rb_label_end(r, cmd);
    rb_gpu_mark(r, f, cmd, kGpuUploaded);
    rb_gpu_stats_begin(r, f, cmd);

    bool draw = have_image && r->sprites.len > 0;
    point2 center = rect_center(view.view);
    affine2 proj  = d3d_clip(ortho2d(rect2_from_center(point2{0, 0}, rect_size(view.view))) * affine2_rotate(-view.rotation) *
                             affine2_translate(vec2{-center.x, -center.y}));
    f32 cs = kv::fabs(kv::cos(view.rotation)), sn = kv::fabs(kv::sin(view.rotation));
    f32 hx = 0.5f * (view.view.w * cs + view.view.h * sn), hy = 0.5f * (view.view.w * sn + view.view.h * cs);
    f32 sw = (f32)s.width, sh = (f32)s.height;
    affine2 screen = d3d_clip(ortho2d(rect2{-sw * 0.5f, -sh * 0.5f, sw, sh}));
    PushData push{};
    push.view[0] = proj.a; push.view[1] = proj.b; push.view[2] = proj.c; push.view[3] = proj.d;
    push.view_trans[0] = proj.tx; push.view_trans[1] = proj.ty;
    push.alpha     = view.alpha;
    push.tick      = r->tick;
    push.cull[0] = center.x - hx; push.cull[1] = center.y - hy; push.cull[2] = 2.0f * hx; push.cull[3] = 2.0f * hy;
    push.screen[0] = screen.a; push.screen[1] = screen.b; push.screen[2] = screen.c; push.screen[3] = screen.d;
    push.screen_trans[0] = screen.tx; push.screen_trans[1] = screen.ty;
    push.row_count = r->sprites.len;
    push.screen_cull[0] = -sw * 0.5f; push.screen_cull[1] = -sh * 0.5f; push.screen_cull[2] = sw; push.screen_cull[3] = sh;
    push.time = (f32)(time_now() - r->start_time);

    if (draw) {
        rb_label_begin(r, cmd, "sort");
        record_sort(r, f, cmd, push);
        rb_label_end(r, cmd);
        if (r->check_pending) { snapshot_keys(r, view.alpha); record_sort_check(r, cmd); }
        finish_sort(r, cmd);
    }
    rb_gpu_mark(r, f, cmd, kGpuSorted);

    if (have_image) {
        ID3D12Resource* image = s.buffers[image_index];
        rb_transition(cmd, image, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

        bool post = rb_post_prepare(r);
        u32 split = post ? rb_bucket_of(r->post.layer, 0) : 0;
        D3D12_VIEWPORT vp{0.0f, 0.0f, sw, sh, 0.0f, 1.0f};
        D3D12_RECT scissor{0, 0, (LONG)s.width, (LONG)s.height};
        auto bind_sprites = [&]() {
            cmd->RSSetViewports(1, &vp);
            cmd->RSSetScissorRects(1, &scissor);
            cmd->SetGraphicsRootSignature(r->root);
            cmd->SetGraphicsRoot32BitConstants(kRootPush, sizeof push / 4, &push, 0);
            bind_tables(r, f,
                        [&](u32 i, D3D12_GPU_VIRTUAL_ADDRESS a) { cmd->SetGraphicsRootShaderResourceView(i, a); },
                        [&](u32, D3D12_GPU_VIRTUAL_ADDRESS) {}, false);
            cmd->SetGraphicsRootDescriptorTable(kRootTextures, rb_heap_gpu(r, kHeapTextures + fi * kMaxTextures));
            cmd->SetGraphicsRootDescriptorTable(kRootSamplers, r->sampler_heap->GetGPUDescriptorHandleForHeapStart());
            cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            D3D12_VERTEX_BUFFER_VIEW rows{r->tables.sorted.gpu(), (UINT)r->tables.sorted.size, sizeof(u32)};
            cmd->IASetVertexBuffers(0, 1, &rows);
        };
        u32 draws = 0;
        if (post) {
            rb_label_begin(r, cmd, "world");
            rb_post_begin_world(r, cmd, view);
            if (draw) { bind_sprites(); draws += rb_record_draws(r, cmd, 0, split); }
            rb_post_end_world(r, cmd);
            rb_label_end(r, cmd);
        }

        rb_label_begin(r, cmd, "draw");
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rb_rtv(r, image_index);
        cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const f32 clear[4] = {view.clear_color.r, view.clear_color.g, view.clear_color.b, view.clear_color.a};
        cmd->ClearRenderTargetView(rtv, clear, 0, nullptr);
        if (post) {
            cmd->RSSetViewports(1, &vp);
            cmd->RSSetScissorRects(1, &scissor);
            rb_post_draw(r, f, cmd, view);
        }
        if (draw) {
            bind_sprites(); // the post pass's root signature displaced the sprites'
            draws += rb_record_draws(r, cmd, split, kBucketCount);
        }
        KV_COUNTER("indirect draws", draws);
        rb_label_end(r, cmd);

        if (r->shot_pending) record_screenshot(r, cmd, image);
        rb_transition(cmd, image, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    }

    rb_gpu_stats_end(r, f, cmd);
    rb_gpu_mark(r, f, cmd, kGpuDrawn);
    rb_gpu_mark(r, f, cmd, kGpuEnd);
    rb_gpu_frame_end(r, f, cmd);
    D3D_CHECK(cmd->Close());
    return true;
}

} // namespace kv
