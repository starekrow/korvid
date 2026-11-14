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
bool rd_create_buffer(Renderer* r, D3D12_HEAP_TYPE heap, u64 size, bool uav, Buffer& out, const char* name) {
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
    rd_name(out.resource, name);
    return true;
}

void rd_destroy_buffer(Buffer& b) {
    rd_release(b.resource);
    b = Buffer{};
}

void rd_transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
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

void rd_transition(ID3D12GraphicsCommandList* cmd, Buffer& b, D3D12_RESOURCE_STATES to) {
    if (!b.resource) return;
    rd_transition(cmd, b.resource, b.state, to);
    b.state = to;
}

// ---- deferred destruction -----------------------------------------------------------------
// As renderer-vulkan's: queued on the slot of the next frame to be submitted, released after
// that frame's fence passes; what is queued between frames waits for the frame after.

void rd_defer_release(Renderer* r, IUnknown* object) {
    if (object) r->frames[r->frame_index].garbage.push(Garbage{object});
}

void rd_defer_destroy_buffer(Renderer* r, Buffer& b) {
    rd_defer_release(r, b.resource);
    b = Buffer{};
}

void rd_run_garbage(Renderer*, FrameInFlight& f) {
    u32 owned = f.garbage_owned <= f.garbage.len ? f.garbage_owned : f.garbage.len;
    for (u32 i = 0; i < owned; ++i) f.garbage[i].object->Release();
    u32 rest = f.garbage.len - owned;
    if (owned && rest) memmove(f.garbage.data, f.garbage.data + owned, sizeof(Garbage) * rest);
    f.garbage.len   = rest;
    f.garbage_owned = 0;
}

// ---- the frame ring -----------------------------------------------------------------------
// renderer-vulkan's ring, on an upload heap.

bool rd_ring_init(Renderer* r, u64 bytes) {
    FrameRing& g = r->ring;
    if (!rd_create_buffer(r, D3D12_HEAP_TYPE_UPLOAD, bytes, false, g.buf, "upload ring")) return false;
    g.head = g.tail = g.used = g.frame_bytes = 0;
    return true;
}

namespace {

bool ring_grow(Renderer* r, u64 need) {
    FrameRing& g = r->ring;
    u64 cap = g.buf.size * 2;
    while (cap < need * 2) cap *= 2;
    rd_defer_destroy_buffer(r, g.buf);
    if (!rd_create_buffer(r, D3D12_HEAP_TYPE_UPLOAD, cap, false, g.buf, "upload ring")) return false;
    g.head = g.tail = g.used = 0;
    g.frame_bytes = 0;
    for (FrameInFlight& f : r->frames) f.ring_bytes = 0;
    ++g.grows;
    KV_INFO("d3d12", "frame ring grown to %llu KB", (unsigned long long)(cap / 1024));
    return true;
}

} // namespace

RingSpan rd_ring_alloc(Renderer* r, u64 bytes, u64 align) {
    FrameRing& g = r->ring;
    RingSpan out;
    if (bytes == 0) return out;
    for (u32 attempt = 0; attempt < 2; ++attempt) {
        u64 cap  = g.buf.size;
        u64 head = (g.head + align - 1) / align * align;
        u64 gap  = head - g.head;
        bool ok = false;
        if (g.used == 0) {
            g.head = g.tail = 0; head = 0; gap = 0;
            ok = bytes <= cap;
        } else if (g.head >= g.tail) {
            if (head + bytes <= cap) ok = true;
            else if (bytes < g.tail) { gap = cap - g.head; head = 0; ok = true; }
        } else {
            ok = head + bytes < g.tail;
        }
        if (ok) {
            out.resource = g.buf.resource;
            out.offset   = head;
            out.ptr      = (u8*)g.buf.mapped + head;
            g.head       = head + bytes;
            if (g.head == cap) g.head = 0;
            g.used        += gap + bytes;
            g.frame_bytes += gap + bytes;
            if (g.used > g.peak) g.peak = g.used;
            return out;
        }
        if (!ring_grow(r, bytes + align)) return RingSpan{};
    }
    return out;
}

void rd_ring_submitted(Renderer* r, FrameInFlight& f) {
    f.ring_end   = r->ring.head;
    f.ring_bytes = r->ring.frame_bytes;
    r->ring.frame_bytes = 0;
}

void rd_ring_release(Renderer* r, FrameInFlight& f) {
    FrameRing& g = r->ring;
    if (!f.submitted || f.ring_bytes == 0) return;
    g.used -= f.ring_bytes;
    g.tail  = f.ring_end;
    f.ring_bytes = 0;
    if (g.used == 0) g.head = g.tail = 0;
}

// ---- descriptors ----------------------------------------------------------------------------

D3D12_CPU_DESCRIPTOR_HANDLE rd_heap_cpu(Renderer* r, u32 index) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = r->heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)index * r->heap_step;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE rd_heap_gpu(Renderer* r, u32 index) {
    D3D12_GPU_DESCRIPTOR_HANDLE h = r->heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += (UINT64)index * r->heap_step;
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE rd_rtv(Renderer* r, u32 index) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = r->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)index * r->rtv_step;
    return h;
}

// ---- textures -------------------------------------------------------------------------------

DXGI_FORMAT rd_dxgi_format(TextureFormat f) {
    return f == TextureFormat::RGBA8_UNORM ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}

void rd_defer_destroy_texture(Renderer* r, Texture& t) {
    rd_defer_release(r, t.resource);
    t.resource = nullptr;
    t.resident = false;
}

void rd_mark_slot_dirty(Renderer* r, u32 slot) {
    for (FrameInFlight& f : r->frames) f.dirty_slots.push(slot);
}

// The view a slot shows in a frame's table: its own texture, or the default's. Written once
// that frame's fence has passed, so no list in flight sees it change.
void rd_write_texture_slot(Renderer* r, u32 frame, u32 slot) {
    const Texture* t = slot < r->textures.len && r->textures.is_alive(slot) ? &r->textures.items[slot] : nullptr;
    if (!t || !t->resource) t = &r->textures.items[0];
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format                  = rd_dxgi_format(t->format);
    sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels     = 1;
    r->device->CreateShaderResourceView(t->resource, &sd, rd_heap_cpu(r, kHeapTextures + frame * kMaxTextures + slot));
}

// A texture for the record's size and format, waiting for its copy. Any previous one is
// released after the frames in flight.
bool rd_create_texture_resource(Renderer* r, Texture& t) {
    if (t.resource) rd_defer_destroy_texture(r, t);
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = t.width;
    rd.Height           = t.height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = rd_dxgi_format(t.format);
    rd.SampleDesc.Count = 1;
    D3D_CHECK(r->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&t.resource)));
    rd_name(t.resource, name_str(t.name));
    return true;
}

// Copies tightly packed pixels into the ring (or a one-off staging buffer) at the pitch a
// placed footprint needs, and queues the copy for the next frame's command list.
bool rd_stage_texture(Renderer* r, u32 slot, Texture& t, const u8* pixels) {
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
    if (bytes <= r->ring.buf.size / 2) {
        RingSpan span = rd_ring_alloc(r, bytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        if (!span.ptr) return false;
        dst       = (u8*)span.ptr;
        up.src    = span.resource;
        up.offset = span.offset;
    } else {
        Buffer staging;
        if (!rd_create_buffer(r, D3D12_HEAP_TYPE_UPLOAD, bytes, false, staging, "texture staging")) return false;
        dst       = (u8*)staging.mapped;
        up.src    = staging.resource;
        up.offset = 0;
        rd_defer_destroy_buffer(r, staging); // alive until this frame's fence, after the copy
    }
    for (u32 y = 0; y < t.height; ++y) memcpy(dst + (usize)y * pitch, pixels + (usize)y * row, row);
    r->uploads.push(up);
    t.resident = true;
    return true;
}

bool rd_create_default_texture(Renderer* r) {
    Texture t{};
    t.width = t.height = kDefaultTexSize;
    t.format = TextureFormat::RGBA8_SRGB;
    t.name = name_intern("default");
    Handle h = r->textures.create(t);
    if (h.index() != 0) { KV_ERROR("d3d12", "default texture is not slot 0"); return false; }
    Texture& slot0 = r->textures.items[0];
    if (!rd_create_texture_resource(r, slot0)) return false;

    u8 pixels[kDefaultTexSize * kDefaultTexSize * 4];
    for (u32 y = 0; y < kDefaultTexSize; ++y) {
        for (u32 x = 0; x < kDefaultTexSize; ++x) {
            bool red = ((x / kDefaultTexCell) + (y / kDefaultTexCell)) & 1;
            u8* p = pixels + (y * kDefaultTexSize + x) * 4;
            p[0] = 255;
            p[1] = red ? 0 : 255;
            p[2] = red ? 0 : 255;
            p[3] = 255;
        }
    }
    if (!rd_stage_texture(r, 0, slot0, pixels)) return false;
    for (u32 f = 0; f < kFramesInFlight; ++f)
        for (u32 slot = 0; slot < kMaxTextures; ++slot) rd_write_texture_slot(r, f, slot);

    MaterialRecord m{};
    m.texture = h;
    m.blend   = BlendMode::Alpha;
    m.slot    = (u32)BlendMode::Alpha;
    m.tint    = color_white();
    m.name    = name_intern("default");
    r->default_material = r->materials.create(m);
    r->texture_by_name.set(t.name.hash, h);
    r->material_by_name.set(m.name.hash, r->default_material);
    if (r->default_material.index() != 0) { KV_ERROR("d3d12", "default material is not row 0"); return false; }
    rd_mark_material_dirty(r, 0);
    return true;
}

// ---- materials --------------------------------------------------------------------------------

void rd_material_row(Renderer* r, u32 row, GpuMaterial& out) {
    out = GpuMaterial{};
    const MaterialRecord* m = r->materials.is_alive(row) ? &r->materials.items[row] : nullptr;
    if (!m) m = &r->materials.items[0];
    out.tint[0] = m->tint.r; out.tint[1] = m->tint.g; out.tint[2] = m->tint.b; out.tint[3] = m->tint.a;
    for (u32 i = 0; i < KV_MATERIAL_PARAMS; ++i) out.params[i] = m->params[i];
    out.texture = renderer_texture_slot(r, m->texture);
    out.blend   = (u32)m->blend;
    out.filter  = (u32)m->filter;
    out.slot    = m->slot;
}

void rd_mark_material_dirty(Renderer* r, u32 row) {
    r->material_dirty[row >> 6] |= (u64)1 << (row & 63);
}

void rd_rebind_material(Renderer* r, u32 row) {
    u32 slot = r->materials.is_alive(row) ? r->materials.items[row].slot : r->materials.items[0].slot;
    for (u32 i = 0; i < r->sprites.len; ++i) {
        if (!r->sprites.is_alive(i)) continue;
        RenderSprite& s = r->sprites.items[i];
        if (s.gpu.material != row) continue;
        if (!r->materials.is_alive(row)) s.gpu.material = 0; // the material is gone: the default
        u32 bucket = rd_bucket_of(s.gpu.layer, slot);
        if (bucket != s.bucket) {
            --r->bucket_counts[s.bucket];
            ++r->bucket_counts[bucket];
            if (s.keyed) { --r->keyed_counts[s.bucket]; ++r->keyed_counts[bucket]; }
            s.bucket = bucket;
        }
        r->sprite_dirty[i >> 6] |= (u64)1 << (i & 63);
    }
}

// ---- handle kinds and pools -----------------------------------------------------------------

namespace {

void* texture_resolve(kv_handle h, void* ctx) { return ((Renderer*)ctx)->textures.get(Handle{h}); }
int texture_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    Renderer* r = (Renderer*)ctx;
    Texture* t = r->textures.get(Handle{h});
    if (!t) return format(out, cap, "stale");
    return format(out, cap, "'%s' %ux%u slot %u %s", name_str(t->name), t->width, t->height, Handle{h}.index(),
                    t->resident ? "resident" : "placeholder");
}
void* material_resolve(kv_handle h, void* ctx) { return ((Renderer*)ctx)->materials.get(Handle{h}); }
int material_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    Renderer* r = (Renderer*)ctx;
    MaterialRecord* m = r->materials.get(Handle{h});
    if (!m) return format(out, cap, "stale");
    return format(out, cap, "'%s' %s texture slot %u row %u", name_str(m->name), blend_mode_name(m->blend), renderer_texture_slot(r, m->texture), Handle{h}.index());
}

const char* texture_name_of(kv_handle h, void* ctx) {
    Texture* t = ((Renderer*)ctx)->textures.get(Handle{h});
    return t ? name_str(t->name) : nullptr;
}
kv_handle texture_find_name(const char* name, void* ctx) {
    Renderer* r = (Renderer*)ctx;
    Handle* h = r->texture_by_name.find(name_lookup(name).hash);
    return h && r->textures.valid(*h) ? h->bits : 0;
}
const char* material_name_of(kv_handle h, void* ctx) {
    MaterialRecord* m = ((Renderer*)ctx)->materials.get(Handle{h});
    return m ? name_str(m->name) : nullptr;
}
kv_handle material_find_name(const char* name, void* ctx) {
    Renderer* r = (Renderer*)ctx;
    Handle* h = r->material_by_name.find(name_lookup(name).hash);
    return h && r->materials.valid(*h) ? h->bits : 0;
}

} // namespace

bool rd_create_resource_pools(Renderer* r) {
    kv_handle_kind_desc td{};
    td.name = "render.texture"; td.type_key = type_key<Texture>(); td.ctx = r;
    td.resolve = texture_resolve; td.describe = texture_describe;
    td.name_of = texture_name_of; td.find_name = texture_find_name; td.flags = KV_HANDLE_GENERATIONAL;
    r->texture_kind = handle_kind_register(td);
    kv_handle_kind_desc md{};
    md.name = "render.material"; md.type_key = type_key<MaterialRecord>(); md.ctx = r;
    md.resolve = material_resolve; md.describe = material_describe;
    md.name_of = material_name_of; md.find_name = material_find_name; md.flags = KV_HANDLE_GENERATIONAL;
    r->material_kind = handle_kind_register(md);
    if (!r->texture_kind || !r->material_kind) return false;
    r->textures.use_kind(r->texture_kind);
    r->materials.use_kind(r->material_kind);
    r->sprite_of_entity.init(r->general);
    r->texture_by_name.init(r->general);
    r->material_by_name.init(r->general);
    r->sprite_dirty.alloc = r->material_dirty.alloc = r->uploads.alloc = r->general;
    r->material_dirty.resize_fill((kMaxMaterials + 63) / 64, 0);
    return r->textures.init(kMaxTextures, r->general->tag) && r->materials.init(kMaxMaterials, r->general->tag) &&
           r->sprites.init(kMaxSprites, r->general->tag);
}

// ---- the GPU tables ------------------------------------------------------------------------------

// Sprite tables for at least `rows` rows (whole blocks). The old ones retire with the frame;
// every row is marked for upload. They are root descriptors, so nothing is rewritten: the
// next frame binds the new addresses.
bool rd_create_tables(Renderer* r, u32 rows) {
    SpriteTables& t = r->tables;
    u32 cap = kBlockRows;
    while (cap < rows) cap *= 2;
    if (cap > kMaxSprites) cap = kMaxSprites;
    if (t.capacity) {
        rd_defer_destroy_buffer(r, t.sprites);
        rd_defer_destroy_buffer(r, t.bucket_of);
        rd_defer_destroy_buffer(r, t.block_hist);
        rd_defer_destroy_buffer(r, t.sorted);
    }
    t.capacity = cap;
    const D3D12_HEAP_TYPE dh = D3D12_HEAP_TYPE_DEFAULT;
    if (!rd_create_buffer(r, dh, sizeof(GpuSprite) * (u64)cap, false, t.sprites, "sprite rows") ||
        !rd_create_buffer(r, dh, sizeof(u32) * (u64)cap, true, t.bucket_of, "sprite buckets") ||
        !rd_create_buffer(r, dh, sizeof(u32) * (u64)t.blocks() * kBucketCount, true, t.block_hist, "sort block counts") ||
        !rd_create_buffer(r, dh, sizeof(u32) * (u64)cap, true, t.sorted, "sorted rows"))
        return false;
    r->sprite_dirty.resize_fill(cap / 64, 0);
    for (u32 w = 0; w < (r->sprites.len + 63) / 64; ++w) r->sprite_dirty[w] = ~(u64)0;
    KV_INFO("d3d12", "sprite tables: %u rows, %u blocks, %llu KB", cap, t.blocks(),
            (unsigned long long)((t.sprites.size + t.bucket_of.size + t.block_hist.size + t.sorted.size) / 1024));
    return true;
}

bool rd_create_static_buffers(Renderer* r) {
    for (FrameInFlight& f : r->frames) {
        if (!rd_create_buffer(r, D3D12_HEAP_TYPE_UPLOAD, sizeof(f32) * 4 * kLayerCount, false, f.layers, "layer offsets")) return false;
        memset(f.layers.mapped, 0, sizeof(f32) * 4 * kLayerCount);
        if (!rd_create_buffer(r, D3D12_HEAP_TYPE_UPLOAD, sizeof(f32) * 4 * KV_POST_PARAMS, false, f.post_params, "post params")) return false;
        memset(f.post_params.mapped, 0, sizeof(f32) * 4 * KV_POST_PARAMS);
    }
    const D3D12_HEAP_TYPE dh = D3D12_HEAP_TYPE_DEFAULT;
    return rd_create_buffer(r, dh, sizeof(GpuMaterial) * kMaxMaterials, false, r->material_table, "material rows") &&
           rd_create_buffer(r, dh, sizeof(u32) * (2 * kBucketCount + 4), true, r->sort_meta, "sort totals") &&
           rd_create_buffer(r, dh, sizeof(DrawArgs) * kBucketCount, true, r->indirect, "indirect draws");
}

// ---- heaps and the root signature -----------------------------------------------------------

ID3D12RootSignature* rd_make_root_signature(Renderer* r, const D3D12_ROOT_SIGNATURE_DESC1& desc, const char* name) {
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC vd{};
    vd.Version  = D3D_ROOT_SIGNATURE_VERSION_1_1;
    vd.Desc_1_1 = desc;
    ID3DBlob* blob = nullptr;
    ID3DBlob* error = nullptr;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&vd, &blob, &error);
    if (FAILED(hr)) {
        KV_ERROR("d3d12", "root signature '%s': %s", name, error ? (const char*)error->GetBufferPointer() : hr_name(hr));
        rd_release(error);
        return nullptr;
    }
    ID3D12RootSignature* out = nullptr;
    hr = r->device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&out));
    blob->Release();
    if (FAILED(hr)) { KV_ERROR("d3d12", "CreateRootSignature '%s': %s", name, hr_name(hr)); return nullptr; }
    rd_name(out, name);
    return out;
}

// The shader-visible heap (each frame's bindless table and post input), the render target
// heap, the one root signature of the sort and the sprites (renderer_internal.h's RootParam,
// binding.slangh's registers), and the command signature of an indirect draw.
bool rd_create_heaps_and_root(Renderer* r) {
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kHeapSize;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    D3D_CHECK(r->device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&r->heap)));
    rd_name(r->heap, "bindless textures");
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
    rd_name(r->sampler_heap, "samplers");
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
    r->root = rd_make_root_signature(r, rs, "sprites");
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
void rd_destroy_resources(Renderer* r) {
    for (u32 i = 0; i < r->textures.len; ++i)
        if (r->textures.is_alive(i)) rd_release(r->textures.items[i].resource);
    rd_destroy_buffer(r->tables.sprites);
    rd_destroy_buffer(r->tables.bucket_of);
    rd_destroy_buffer(r->tables.block_hist);
    rd_destroy_buffer(r->tables.sorted);
    rd_destroy_buffer(r->material_table);
    rd_destroy_buffer(r->sort_meta);
    rd_destroy_buffer(r->indirect);
    rd_destroy_buffer(r->ring.buf);
    rd_destroy_buffer(r->shot_buffer);
    rd_destroy_buffer(r->check_buffer);
    for (FrameInFlight& f : r->frames) { rd_destroy_buffer(f.layers); rd_destroy_buffer(f.post_params); }
    rd_release(r->draw_signature);
    rd_release(r->root);
    rd_release(r->heap);
    rd_release(r->rtv_heap);
    rd_release(r->sampler_heap);
    if (r->texture_kind)  kv_handle_kind_unregister(r->texture_kind);
    if (r->material_kind) kv_handle_kind_unregister(r->material_kind);
    r->texture_kind = r->material_kind = 0;
    r->textures.free();
    r->materials.free();
    r->texture_by_name.free();
    r->material_by_name.free();
    r->sprites.free();
    r->sprite_of_entity.free();
    r->sprite_dirty.free();
    r->material_dirty.free();
    r->uploads.free();
}

} // namespace kv
