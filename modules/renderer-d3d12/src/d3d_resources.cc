// Korvid renderer-d3d12: buffers and their states, the frame ring, deferred destruction,
// textures, materials, the handle kinds and pools, the GPU tables, the descriptor heaps and
// the root signature. Every resource is committed (its own heap): the renderer makes few,
// and a suballocator is a later step, not a different design.
#include "renderer_internal.h"
#include "korvid/core/format.h"
#include <string.h>

namespace kv {

// ---- buffers ------------------------------------------------------------------------------

// A buffer on a heap: DEFAULT (the GPU's; `uav` lets compute write it), UPLOAD (the CPU
// writes, mapped for good, always GENERIC_READ) or READBACK (the GPU copies in, mapped,
// always COPY_DEST).
bool rb_create_buffer(Renderer* r, D3D12_HEAP_TYPE heap, u64 size, bool uav, Buffer& out, const char* name) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = size ? size : 4;
    rd.Height           = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags            = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    D3D12_RESOURCE_STATES state = heap == D3D12_HEAP_TYPE_UPLOAD   ? D3D12_RESOURCE_STATE_GENERIC_READ
                                : heap == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
                                                                   : D3D12_RESOURCE_STATE_COMMON;
    D3D_CHECK(r->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&out.resource)));
    out.size  = size;
    out.state = state;
    out.mapped = nullptr;
    if (heap != D3D12_HEAP_TYPE_DEFAULT) {
        D3D12_RANGE none{0, 0}; // the CPU reads nothing of an upload buffer
        D3D_CHECK(out.resource->Map(0, heap == D3D12_HEAP_TYPE_UPLOAD ? &none : nullptr, &out.mapped));
    }
    rb_name(out.resource, name);
    return true;
}

void rb_destroy_buffer(Buffer& b) {
    rb_release(b.resource);
    b = Buffer{};
}

void rb_transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    if (from == to) {
        if (to != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) return;
        b.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = res;
    } else {
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter  = to;
    }
    cmd->ResourceBarrier(1, &b);
}

void rb_transition(ID3D12GraphicsCommandList* cmd, Buffer& b, D3D12_RESOURCE_STATES to) {
    if (!b.resource) return;
    rb_transition(cmd, b.resource, b.state, to);
    b.state = to;
}

// ---- deferred destruction -----------------------------------------------------------------
// As renderer-vulkan's: queued on the slot of the next frame to be submitted, released after
// that frame's fence passes; what is queued between frames waits for the frame after.

void rb_defer_release(Renderer* r, IUnknown* object) {
    if (object) rc_defer(r, Garbage{object});
}

void rb_defer_destroy_buffer(Renderer* r, Buffer& b) {
    rb_defer_release(r, b.resource);
    b = Buffer{};
}

// ---- the frame ring -----------------------------------------------------------------------
// renderer-vulkan's ring, on an upload heap.

// ---- descriptors ----------------------------------------------------------------------------

D3D12_CPU_DESCRIPTOR_HANDLE rb_heap_cpu(Renderer* r, u32 index) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = r->heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)index * r->heap_step;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE rb_heap_gpu(Renderer* r, u32 index) {
    D3D12_GPU_DESCRIPTOR_HANDLE h = r->heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += (UINT64)index * r->heap_step;
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE rb_rtv(Renderer* r, u32 index) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = r->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)index * r->rtv_step;
    return h;
}

// ---- textures -------------------------------------------------------------------------------

DXGI_FORMAT rb_dxgi_format(TextureFormat f) {
    return f == TextureFormat::RGBA8_UNORM ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}

void rb_defer_destroy_texture(Renderer* r, Texture& t) {
    rb_defer_release(r, t.resource);
    t.resource = nullptr;
    t.resident = false;
}

// The view a slot shows in a frame's table: its own texture, or the default's. Written once
// that frame's fence has passed, so no list in flight sees it change.
void rb_write_texture_slot(Renderer* r, u32 frame, u32 slot) {
    const Texture* t = slot < r->textures.len && r->textures.is_alive(slot) ? &r->textures.items[slot] : nullptr;
    if (!t || !t->resource) t = &r->textures.items[0];
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format                  = rb_dxgi_format(t->format);
    sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels     = 1;
    r->device->CreateShaderResourceView(t->resource, &sd, rb_heap_cpu(r, kHeapTextures + frame * kMaxTextures + slot));
}

// A texture for the record's size and format, waiting for its copy. Any previous one is
// released after the frames in flight.
bool rb_texture_create(Renderer* r, Texture& t) {
    if (t.resource) rb_defer_destroy_texture(r, t);
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = t.width;
    rd.Height           = t.height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = rb_dxgi_format(t.format);
    rd.SampleDesc.Count = 1;
    D3D_CHECK(r->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&t.resource)));
    rb_name(t.resource, name_str(t.name));
    return true;
}

// Copies tightly packed pixels into the ring (or a one-off staging buffer) at the pitch a
// placed footprint needs, and queues the copy for the next frame's command list.
bool rb_stage_texture(Renderer* r, u32 slot, Texture& t, const u8* pixels) {
    u32 row   = t.width * 4;
    u32 pitch = (row + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    u64 bytes = (u64)pitch * t.height;
    PendingUpload up{};
    up.slot     = slot;
    up.resource = t.resource;
    up.width    = t.width;
    up.height   = t.height;
    up.pitch    = pitch;
    u8* dst = nullptr;
    if (bytes <= r->ring.size / 2) {
        RingSpan span = rc_ring_alloc(r, bytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        if (!span.ptr) return false;
        dst       = (u8*)span.ptr;
        up.src    = span.buffer.resource;
        up.offset = span.offset;
    } else {
        Buffer staging;
        if (!rb_create_buffer(r, D3D12_HEAP_TYPE_UPLOAD, bytes, false, staging, "texture staging")) return false;
        dst       = (u8*)staging.mapped;
        up.src    = staging.resource;
        up.offset = 0;
        rb_defer_destroy_buffer(r, staging); // alive until this frame's fence, after the copy
    }
    for (u32 y = 0; y < t.height; ++y) memcpy(dst + (usize)y * pitch, pixels + (usize)y * row, row);
    r->uploads.push(up);
    t.resident = true;
    return true;
}

// ---- the GPU tables ------------------------------------------------------------------------------

// Sprite tables for at least `rows` rows (whole blocks). The old ones retire with the frame;
// every row is marked for upload. They are root descriptors, so nothing is rewritten: the
// next frame binds the new addresses.
bool rb_create_tables(Renderer* r, u32 rows) {
    SpriteTables& t = r->tables;
    u32 cap = kBlockRows;
    while (cap < rows) cap *= 2;
    if (cap > kMaxSprites) cap = kMaxSprites;
    if (t.capacity) {
        rb_defer_destroy_buffer(r, t.sprites);
        rb_defer_destroy_buffer(r, t.bucket_of);
        rb_defer_destroy_buffer(r, t.block_hist);
        rb_defer_destroy_buffer(r, t.sorted);
    }
    t.capacity = cap;
    const D3D12_HEAP_TYPE dh = D3D12_HEAP_TYPE_DEFAULT;
    if (!rb_create_buffer(r, dh, sizeof(GpuSprite) * (u64)cap, false, t.sprites, "sprite rows") ||
        !rb_create_buffer(r, dh, sizeof(u32) * (u64)cap, true, t.bucket_of, "sprite buckets") ||
        !rb_create_buffer(r, dh, sizeof(u32) * (u64)t.blocks() * kBucketCount, true, t.block_hist, "sort block counts") ||
        !rb_create_buffer(r, dh, sizeof(u32) * (u64)cap, true, t.sorted, "sorted rows"))
        return false;
    r->world.sprite_dirty.resize_fill(cap / 64, 0);
    for (u32 w = 0; w < (r->world.sprites.len + 63) / 64; ++w) r->world.sprite_dirty[w] = ~(u64)0;
    KV_INFO("d3d12", "sprite tables: %u rows, %u blocks, %llu KB", cap, t.blocks(),
            (unsigned long long)((t.sprites.size + t.bucket_of.size + t.block_hist.size + t.sorted.size) / 1024));
    return true;
}

bool rb_create_static_buffers(Renderer* r) {
    for (FrameInFlight& f : r->frames) {
        if (!rb_create_buffer(r, D3D12_HEAP_TYPE_UPLOAD, sizeof(f32) * 4 * kLayerCount, false, f.layers, "layer offsets")) return false;
        memset(f.layers.mapped, 0, sizeof(f32) * 4 * kLayerCount);
        if (!rb_create_buffer(r, D3D12_HEAP_TYPE_UPLOAD, sizeof(f32) * 4 * KV_POST_PARAMS, false, f.post_params, "post params")) return false;
        memset(f.post_params.mapped, 0, sizeof(f32) * 4 * KV_POST_PARAMS);
    }
    const D3D12_HEAP_TYPE dh = D3D12_HEAP_TYPE_DEFAULT;
    return rb_create_buffer(r, dh, sizeof(GpuMaterial) * kMaxMaterials, false, r->material_table, "material rows") &&
           rb_create_buffer(r, dh, sizeof(u32) * (2 * kBucketCount + 4), true, r->sort_meta, "sort totals") &&
           rb_create_buffer(r, dh, sizeof(DrawArgs) * kBucketCount, true, r->indirect, "indirect draws");
}

// ---- heaps and the root signature -----------------------------------------------------------

ID3D12RootSignature* rb_make_root_signature(Renderer* r, const D3D12_ROOT_SIGNATURE_DESC1& desc, const char* name) {
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC vd{};
    vd.Version  = D3D_ROOT_SIGNATURE_VERSION_1_1;
    vd.Desc_1_1 = desc;
    ID3DBlob* blob = nullptr;
    ID3DBlob* error = nullptr;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&vd, &blob, &error);
    if (FAILED(hr)) {
        KV_ERROR("d3d12", "root signature '%s': %s", name, error ? (const char*)error->GetBufferPointer() : hr_name(hr));
        rb_release(error);
        return nullptr;
    }
    ID3D12RootSignature* out = nullptr;
    hr = r->device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&out));
    blob->Release();
    if (FAILED(hr)) { KV_ERROR("d3d12", "CreateRootSignature '%s': %s", name, hr_name(hr)); return nullptr; }
    rb_name(out, name);
    return out;
}

// The shader-visible heap (each frame's bindless table and post input), the render target
// heap, the one root signature of the sort and the sprites (renderer_internal.h's RootParam,
// binding.slangh's registers), and the command signature of an indirect draw.
bool rb_create_heaps_and_root(Renderer* r) {
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kHeapSize;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    D3D_CHECK(r->device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&r->heap)));
    rb_name(r->heap, "bindless textures");
    r->heap_step = r->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_DESCRIPTOR_HEAP_DESC rh{};
    rh.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rh.NumDescriptors = kRtvSize;
    D3D_CHECK(r->device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&r->rtv_heap)));
    r->rtv_step = r->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_ROOT_PARAMETER1 p[kRootCount]{};
    p[kRootPush].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[kRootPush].Constants.ShaderRegister = 0;
    p[kRootPush].Constants.Num32BitValues = sizeof(PushData) / 4;
    auto root_view = [&](RootParam i, D3D12_ROOT_PARAMETER_TYPE type, u32 reg) {
        p[i].ParameterType             = type;
        p[i].Descriptor.ShaderRegister = reg;
        // The tables change between frames, never while one executes.
        p[i].Descriptor.Flags          = type == D3D12_ROOT_PARAMETER_TYPE_SRV ? D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE
                                                                               : D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    };
    root_view(kRootSprites, D3D12_ROOT_PARAMETER_TYPE_SRV, 0);
    root_view(kRootMaterials, D3D12_ROOT_PARAMETER_TYPE_SRV, 1);
    root_view(kRootLayers, D3D12_ROOT_PARAMETER_TYPE_SRV, 7);
    root_view(kRootSorted, D3D12_ROOT_PARAMETER_TYPE_UAV, 2);
    root_view(kRootBucketOf, D3D12_ROOT_PARAMETER_TYPE_UAV, 3);
    root_view(kRootBlockHist, D3D12_ROOT_PARAMETER_TYPE_UAV, 4);
    root_view(kRootSortMeta, D3D12_ROOT_PARAMETER_TYPE_UAV, 5);
    root_view(kRootIndirect, D3D12_ROOT_PARAMETER_TYPE_UAV, 6);
    D3D12_DESCRIPTOR_RANGE1 textures{};
    textures.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    textures.NumDescriptors     = UINT_MAX; // unbounded, as Vulkan's variable-count binding
    textures.BaseShaderRegister = 0;
    textures.RegisterSpace      = 1;
    textures.Flags              = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE; // a slot may be rewritten between frames
    p[kRootTextures].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[kRootTextures].DescriptorTable.NumDescriptorRanges = 1;
    p[kRootTextures].DescriptorTable.pDescriptorRanges   = &textures;
    p[kRootTextures].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    // The samplers: nearest and linear, repeating (uv outside [0, 1] tiles). gSamplers is an
    // array a shader indexes by the material's filter, which static samplers cannot back: they
    // are a table in a sampler heap of their own, written once.
    D3D12_DESCRIPTOR_HEAP_DESC sh{};
    sh.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    sh.NumDescriptors = 2;
    sh.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    D3D_CHECK(r->device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&r->sampler_heap)));
    rb_name(r->sampler_heap, "samplers");
    u32 sampler_step = r->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    for (u32 i = 0; i < 2; ++i) {
        D3D12_SAMPLER_DESC s{};
        s.Filter   = i ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
        s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        s.MaxLOD   = 1.0f;
        D3D12_CPU_DESCRIPTOR_HANDLE h = r->sampler_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)i * sampler_step;
        r->device->CreateSampler(&s, h);
    }
    D3D12_DESCRIPTOR_RANGE1 samplers{};
    samplers.RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    samplers.NumDescriptors = 2;
    p[kRootSamplers].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[kRootSamplers].DescriptorTable.NumDescriptorRanges = 1;
    p[kRootSamplers].DescriptorTable.pDescriptorRanges   = &samplers;
    p[kRootSamplers].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC1 rs{};
    rs.NumParameters     = kRootCount;
    rs.pParameters       = p;
    rs.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT; // the sorted rows, per instance
    r->root = rb_make_root_signature(r, rs, "sprites");
    if (!r->root) return false;

    D3D12_INDIRECT_ARGUMENT_DESC arg{};
    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC cs{};
    cs.ByteStride       = sizeof(DrawArgs);
    cs.NumArgumentDescs = 1;
    cs.pArgumentDescs   = &arg;
    D3D_CHECK(r->device->CreateCommandSignature(&cs, nullptr, IID_PPV_ARGS(&r->draw_signature)));
    return true;
}

// The GPU idle: textures, tables, the ring, heaps, the root signature, pools.
void rb_destroy_resources(Renderer* r) {
    for (u32 i = 0; i < r->textures.len; ++i)
        if (r->textures.is_alive(i)) rb_release(r->textures.items[i].resource);
    rb_destroy_buffer(r->tables.sprites);
    rb_destroy_buffer(r->tables.bucket_of);
    rb_destroy_buffer(r->tables.block_hist);
    rb_destroy_buffer(r->tables.sorted);
    rb_destroy_buffer(r->material_table);
    rb_destroy_buffer(r->sort_meta);
    rb_destroy_buffer(r->indirect);
    rb_destroy_buffer(r->ring.buf);
    rb_destroy_buffer(r->shot_buffer);
    rb_destroy_buffer(r->check_buffer);
    for (FrameInFlight& f : r->frames) { rb_destroy_buffer(f.layers); rb_destroy_buffer(f.post_params); }
    rb_release(r->draw_signature);
    rb_release(r->root);
    rb_release(r->heap);
    rb_release(r->rtv_heap);
    rb_release(r->sampler_heap);
    rc_free_pools(r);
    r->uploads.free();
}

// ---- what the render core asks of a buffer and of the garbage queue (core.h) ----------------------

void* rb_buffer_ptr(const RbBuffer& b) { return b.mapped; }
void  rb_buffer_retire(Renderer* r, RbBuffer& b) { rb_defer_destroy_buffer(r, b); }

bool rb_ring_buffer(Renderer* r, u64 bytes, RbBuffer& out) {
    return rb_create_buffer(r, D3D12_HEAP_TYPE_UPLOAD, bytes, false, out, "upload ring");
}

void rb_garbage_destroy(Renderer*, const RbGarbage& g) { g.object->Release(); }

} // namespace kv
